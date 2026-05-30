/*
 * platform_vita.c — Sony PlayStation Vita backend for the gb-recompiled runtime.
 *
 * Implements the platform_sdl.h contract + GBPlatformCallbacks (gbrt.h),
 * against the VitaSDK (arm-vita-eabi toolchain) + vita2d. Plain C.
 *
 * Vita is ARMv7 little-endian — rom.c compiles unchanged. With 512 MB of RAM
 * this is the roomiest of the console targets; no memory squeeze.
 *
 * Subsystems:
 *   vita2d       — GB image uploaded to a 160x144 texture, drawn 3x centered
 *                  on the 960x544 screen
 *   sceAudioOut  — main port, fed from a ring by a worker thread
 *   sceCtrl      — d-pad + face buttons + left analog
 *   stdio        — saves under ux0:/data/linksawakening/
 *
 * Output: a .vpk — installs via VitaShell on HENkaku/h-encore.
 */

#include "platform_sdl.h"
#include "gbrt.h"
#include "ppu.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <psp2/ctrl.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <vita2d.h>

/* --- Joypad globals (contract) ----------------------------------------- */
uint8_t g_joypad_buttons = 0xFF;
uint8_t g_joypad_dpad    = 0xFF;

#define GB_W   160
#define GB_H   144
#define SCR_W  960
#define SCR_H  544
#define GB_SCALE 3.0f
#define GB_OX  ((SCR_W - (int)(GB_W * GB_SCALE)) / 2)   /* 240 */
#define GB_OY  ((SCR_H - (int)(GB_H * GB_SCALE)) / 2)   /* 56  */

static GBContext*      g_ctx  = NULL;
static int             g_quit = 0;
static vita2d_texture* g_tex  = NULL;

/* --- Audio (sceAudioOut — 44.1 kHz stereo 16-bit, worker thread) ------- */

#define AGRAIN  1024                   /* stereo frames per output call      */
#define ARING   16384

static int          g_audio_port = -1;
static int16_t      g_aring[ARING * 2];
static volatile int g_aw = 0, g_ar = 0;
static SceUID       g_audio_thid = -1;
static volatile int g_audio_run  = 1;

static void xb_on_audio_sample(GBContext* ctx, int16_t left, int16_t right)
{
    (void)ctx;
    int nw = (g_aw + 1) & (ARING - 1);
    if (nw == g_ar) return;               /* ring full — drop */
    g_aring[g_aw * 2 + 0] = left;
    g_aring[g_aw * 2 + 1] = right;
    g_aw = nw;
}

/* sceAudioOutOutput blocks until the previous buffer drains, pacing us. */
static int audio_thread(SceSize args, void* argp)
{
    (void)args; (void)argp;
    static int16_t buf[AGRAIN * 2];
    while (g_audio_run) {
        for (int f = 0; f < AGRAIN; ++f) {
            if (g_ar != g_aw) {
                buf[f*2+0] = g_aring[g_ar*2+0];
                buf[f*2+1] = g_aring[g_ar*2+1];
                g_ar = (g_ar + 1) & (ARING - 1);
            } else {
                buf[f*2+0] = buf[f*2+1] = 0;
            }
        }
        sceAudioOutOutput(g_audio_port, buf);
    }
    return sceKernelExitDeleteThread(0);
}

static void audio_init(void)
{
    g_audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, AGRAIN,
                                       44100, SCE_AUDIO_OUT_MODE_STEREO);
    if (g_audio_port < 0) return;
    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(g_audio_port,
        SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    g_audio_thid = sceKernelCreateThread("la_audio", audio_thread,
                                         0x10000100, 0x10000, 0, 0, NULL);
    if (g_audio_thid >= 0)
        sceKernelStartThread(g_audio_thid, 0, NULL);
}

/* --- Battery RAM — ux0:/data/linksawakening/<rom>.sav ------------------ */

#define SAVE_DIR "ux0:/data/linksawakening"

static void save_path(char* out, size_t n, const char* rom, const char* suffix)
{
    const char* base = rom ? rom : "rom";
    for (const char* p = base; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(out, n, "%s/%s%s", SAVE_DIR, base, suffix);
}

static bool xb_load_battery_ram(GBContext* ctx, const char* rom, void* data, size_t size)
{
    (void)ctx;
    char path[256];
    save_path(path, sizeof(path), rom, ".sav");
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    size_t got = fread(data, 1, size, f);
    fclose(f);
    return got == size;
}

static bool xb_save_battery_ram(GBContext* ctx, const char* rom, const void* data, size_t size)
{
    (void)ctx;
    mkdir("ux0:/data", 0777);
    mkdir(SAVE_DIR, 0777);
    char path[256];
    save_path(path, sizeof(path), rom, ".sav");
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t put = fwrite(data, 1, size, f);
    fclose(f);
    return put == size;
}

/* --- Video — ARGB8888 (GB) -> A8B8G8R8 texture (swap R/B) -------------- */

static inline uint32_t argb_to_abgr(uint32_t p)
{
    uint32_t r = (p >> 16) & 0xFF;
    uint32_t g = (p >>  8) & 0xFF;
    uint32_t b =  p        & 0xFF;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

void gb_platform_render_frame(const uint32_t* framebuffer)
{
    if (!framebuffer || !g_tex) return;

    uint32_t* px = (uint32_t*)vita2d_texture_get_datap(g_tex);
    int stride = vita2d_texture_get_stride(g_tex) / 4;
    for (int gy = 0; gy < GB_H; ++gy) {
        uint32_t* dst = px + gy * stride;
        const uint32_t* src = framebuffer + gy * GB_W;
        for (int gx = 0; gx < GB_W; ++gx)
            dst[gx] = argb_to_abgr(src[gx]);
    }

    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_draw_texture_scale(g_tex, GB_OX, GB_OY, GB_SCALE, GB_SCALE);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

/* --- Lifecycle --------------------------------------------------------- */

bool gb_platform_init(int scale)
{
    (void)scale;
    vita2d_init();
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
    g_tex = vita2d_create_empty_texture_format(GB_W, GB_H,
                SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    audio_init();
    return true;
}

void gb_platform_shutdown(void)
{
    g_audio_run = 0;
    if (g_audio_thid >= 0)
        sceKernelWaitThreadEnd(g_audio_thid, NULL, NULL);
    if (g_audio_port >= 0) sceAudioOutReleasePort(g_audio_port);
    if (g_tex) vita2d_free_texture(g_tex);
    vita2d_fini();
    g_ctx = NULL;
}

void gb_platform_register_context(GBContext* ctx)
{
    g_ctx = ctx;
    GBPlatformCallbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_audio_sample  = xb_on_audio_sample;
    cbs.load_battery_ram = xb_load_battery_ram;
    cbs.save_battery_ram = xb_save_battery_ram;
    gb_set_platform_callbacks(ctx, &cbs);
}

/* --- Input ------------------------------------------------------------- */

bool gb_platform_poll_events(GBContext* ctx)
{
    (void)ctx;
    if (g_quit) return false;

    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);

    uint8_t btns = 0xFF, dpad = 0xFF;

    if (pad.buttons & SCE_CTRL_UP)     dpad &= ~0x04;
    if (pad.buttons & SCE_CTRL_DOWN)   dpad &= ~0x08;
    if (pad.buttons & SCE_CTRL_LEFT)   dpad &= ~0x02;
    if (pad.buttons & SCE_CTRL_RIGHT)  dpad &= ~0x01;
    /* left analog (center 128) */
    if (pad.ly < 96)  dpad &= ~0x04;
    if (pad.ly > 160) dpad &= ~0x08;
    if (pad.lx < 96)  dpad &= ~0x02;
    if (pad.lx > 160) dpad &= ~0x01;

    if (pad.buttons & SCE_CTRL_CROSS)  btns &= ~0x01;   /* A */
    if (pad.buttons & SCE_CTRL_CIRCLE) btns &= ~0x02;   /* B */
    if (pad.buttons & SCE_CTRL_SELECT) btns &= ~0x04;
    if (pad.buttons & SCE_CTRL_START)  btns &= ~0x08;
    /* SELECT+START quits to LiveArea */
    if ((pad.buttons & SCE_CTRL_SELECT) && (pad.buttons & SCE_CTRL_START))
        g_quit = 1;

    g_joypad_buttons = btns;
    g_joypad_dpad    = dpad;
    return !g_quit;
}

uint8_t gb_platform_get_joypad(void) { return g_joypad_buttons & g_joypad_dpad; }

void gb_platform_vsync(void)
{
    /* vita2d_swap_buffers() already syncs to vblank when vsync is enabled
     * (the default). Nothing extra to do here. */
}

void gb_platform_set_title(const char* title) { (void)title; }

/* --- Save state — ux0:/data/linksawakening/state.bin ------------------- */

#define STATE_PATH    SAVE_DIR "/state.bin"
#define STATE_MAGIC   0x4C415654u   /* "LAVT" */
#define STATE_VERSION 1u
#define SS_WRAM 0x8000
#define SS_VRAM 0x4000
#define SS_OAM  0xA0
#define SS_HRAM 0x7F
#define SS_IO   0x80

void gb_platform_save_state(GBContext* ctx)
{
    if (!ctx) return;
    mkdir("ux0:/data", 0777);
    mkdir(SAVE_DIR, 0777);
    FILE* f = fopen(STATE_PATH, "wb");
    if (!f) return;
    uint32_t magic = STATE_MAGIC, ver = STATE_VERSION;
    fwrite(&magic, 4, 1, f); fwrite(&ver, 4, 1, f);
    fwrite(&ctx->af, 2, 1, f); fwrite(&ctx->bc, 2, 1, f);
    fwrite(&ctx->de, 2, 1, f); fwrite(&ctx->hl, 2, 1, f);
    fwrite(&ctx->sp, 2, 1, f); fwrite(&ctx->pc, 2, 1, f);
    fwrite(&ctx->f_z, 1, 1, f); fwrite(&ctx->f_n, 1, 1, f);
    fwrite(&ctx->f_h, 1, 1, f); fwrite(&ctx->f_c, 1, 1, f);
    fwrite(&ctx->ime, 1, 1, f); fwrite(&ctx->halted, 1, 1, f);
    fwrite(&ctx->rom_bank, 2, 1, f);
    fwrite(&ctx->ram_bank, 1, 1, f);
    fwrite(&ctx->wram_bank, 1, 1, f);
    fwrite(&ctx->vram_bank, 1, 1, f);
    fwrite(&ctx->cycles, 4, 1, f);
    if (ctx->wram) fwrite(ctx->wram, 1, SS_WRAM, f);
    if (ctx->vram) fwrite(ctx->vram, 1, SS_VRAM, f);
    if (ctx->oam)  fwrite(ctx->oam,  1, SS_OAM,  f);
    if (ctx->hram) fwrite(ctx->hram, 1, SS_HRAM, f);
    if (ctx->io)   fwrite(ctx->io,   1, SS_IO,   f);
    uint32_t esz = ctx->eram ? (uint32_t)ctx->eram_size : 0;
    fwrite(&esz, 4, 1, f);
    if (ctx->eram && esz) fwrite(ctx->eram, 1, esz, f);
    if (ctx->ppu) fwrite(ctx->ppu, sizeof(GBPPU), 1, f);
    fclose(f);
}

void gb_platform_load_state(GBContext* ctx)
{
    if (!ctx) return;
    FILE* f = fopen(STATE_PATH, "rb");
    if (!f) return;
    uint32_t magic = 0, ver = 0;
    fread(&magic, 4, 1, f); fread(&ver, 4, 1, f);
    if (magic != STATE_MAGIC || ver != STATE_VERSION) { fclose(f); return; }
    fread(&ctx->af, 2, 1, f); fread(&ctx->bc, 2, 1, f);
    fread(&ctx->de, 2, 1, f); fread(&ctx->hl, 2, 1, f);
    fread(&ctx->sp, 2, 1, f); fread(&ctx->pc, 2, 1, f);
    fread(&ctx->f_z, 1, 1, f); fread(&ctx->f_n, 1, 1, f);
    fread(&ctx->f_h, 1, 1, f); fread(&ctx->f_c, 1, 1, f);
    fread(&ctx->ime, 1, 1, f); fread(&ctx->halted, 1, 1, f);
    fread(&ctx->rom_bank, 2, 1, f);
    fread(&ctx->ram_bank, 1, 1, f);
    fread(&ctx->wram_bank, 1, 1, f);
    fread(&ctx->vram_bank, 1, 1, f);
    fread(&ctx->cycles, 4, 1, f);
    if (ctx->wram) fread(ctx->wram, 1, SS_WRAM, f);
    if (ctx->vram) fread(ctx->vram, 1, SS_VRAM, f);
    if (ctx->oam)  fread(ctx->oam,  1, SS_OAM,  f);
    if (ctx->hram) fread(ctx->hram, 1, SS_HRAM, f);
    if (ctx->io)   fread(ctx->io,   1, SS_IO,   f);
    uint32_t esz = 0;
    fread(&esz, 4, 1, f);
    if (ctx->eram && esz <= ctx->eram_size) fread(ctx->eram, 1, esz, f);
    if (ctx->ppu) fread(ctx->ppu, sizeof(GBPPU), 1, f);
    fclose(f);
}

/* --- Debug / dev no-ops ------------------------------------------------ */
void gb_platform_set_input_script(const char* path)        { (void)path; }
void gb_platform_set_dump_frames(const char* dir)          { (void)dir; }
void gb_platform_set_screenshot_prefix(const char* prefix) { (void)prefix; }
