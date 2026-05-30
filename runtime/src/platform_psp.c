/*
 * platform_psp.c — Sony PSP backend for the gb-recompiled runtime.
 *
 * Implements the platform_sdl.h contract + GBPlatformCallbacks (gbrt.h),
 * against the PSPSDK (pspdev / psp-gcc toolchain). Plain C.
 *
 * PSP is MIPS Allegrex, little-endian — rom.c compiles unchanged.
 *
 * Subsystems:
 *   pspdisplay  — raw 480x272 framebuffer (8888), GB image blitted 1:1 centered
 *   pspaudio    — sceAudio channel, fed from a ring by a worker thread
 *   pspctrl     — d-pad + face buttons + analog nub
 *   stdio       — saves under ms0:/linksawakening/ (newlib over sceIo)
 *   pspkernel   — HOME/exit callback
 *
 * The 480x272 screen comfortably holds the 160x144 GB image at 1:1 (2x would
 * be 288 tall > 272), so we center 1:1 with a border — same call the 3DS
 * backend makes. Integer upscaling via the GU is a later optimization.
 *
 * Output: an EBOOT.PBP — runs on PPSSPP and on CFW/HEN hardware.
 */

#include "platform_sdl.h"
#include "gbrt.h"
#include "ppu.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <pspthreadman.h>

/* Module metadata — must live in one linked object; main() is in rom_main.c. */
PSP_MODULE_INFO("linksawakening", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);   /* take all RAM but leave 1 MB for thread stacks */

/* --- Joypad globals (contract) ----------------------------------------- */
uint8_t g_joypad_buttons = 0xFF;
uint8_t g_joypad_dpad    = 0xFF;

#define GB_W      160
#define GB_H      144
#define PSP_W     480
#define PSP_H     272
#define PSP_STRIDE 512                 /* framebuffer line size in pixels   */

/* VRAM aliases the same physical memory at a cached (0x04000000) and an
 * uncached (0x44000000) address. We write pixels through the uncached view
 * and hand the display hardware the cached base. */
#define VRAM_BASE_CACHED   0x04000000u
#define VRAM_BASE_UNCACHED 0x44000000u

static GBContext* g_ctx   = NULL;
static int        g_quit  = 0;
static uint32_t*  g_fb    = (uint32_t*)VRAM_BASE_UNCACHED;
static int        g_off_x = (PSP_W - GB_W) / 2;   /* 160 */
static int        g_off_y = (PSP_H - GB_H) / 2;   /* 64  */

/* --- Audio (sceAudio — 44.1 kHz stereo 16-bit, worker thread) ---------- */

#define ACHANS     1024                /* stereo frames per sceAudio buffer  */
#define ARING      16384

static int          g_audio_chan = -1;
static int16_t      g_aring[ARING * 2];
static volatile int g_aw = 0, g_ar = 0;
static int          g_audio_thid = -1;
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

/* sceAudioOutputBlocking wants ACHANS stereo frames per call and blocks until
 * the prior buffer drains, which paces this thread to the output rate. */
static int audio_thread(SceSize args, void* argp)
{
    (void)args; (void)argp;
    static int16_t buf[ACHANS * 2];
    while (g_audio_run) {
        for (int f = 0; f < ACHANS; ++f) {
            if (g_ar != g_aw) {
                buf[f*2+0] = g_aring[g_ar*2+0];
                buf[f*2+1] = g_aring[g_ar*2+1];
                g_ar = (g_ar + 1) & (ARING - 1);
            } else {
                buf[f*2+0] = buf[f*2+1] = 0;
            }
        }
        sceAudioOutputBlocking(g_audio_chan, PSP_AUDIO_VOLUME_MAX, buf);
    }
    sceKernelExitThread(0);
    return 0;
}

static void audio_init(void)
{
    g_audio_chan = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, ACHANS,
                                     PSP_AUDIO_FORMAT_STEREO);
    if (g_audio_chan < 0) return;
    g_audio_thid = sceKernelCreateThread("la_audio", audio_thread,
                                         0x12, 0x4000, 0, NULL);
    if (g_audio_thid >= 0)
        sceKernelStartThread(g_audio_thid, 0, NULL);
}

/* --- HOME / exit callback ---------------------------------------------- */

static int exit_callback(int arg1, int arg2, void* common)
{
    (void)arg1; (void)arg2; (void)common;
    g_quit = 1;
    return 0;
}

static int callback_thread(SceSize args, void* argp)
{
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("la_exit", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int thid = sceKernelCreateThread("la_cb", callback_thread, 0x11, 0xFA0, 0, NULL);
    if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
}

/* --- Battery RAM — ms0:/linksawakening/<rom>.sav ----------------------- */

#define SAVE_DIR "ms0:/linksawakening"

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
    mkdir(SAVE_DIR, 0777);
    char path[256];
    save_path(path, sizeof(path), rom, ".sav");
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t put = fwrite(data, 1, size, f);
    fclose(f);
    return put == size;
}

/* --- Video — ARGB8888 (GB) -> ABGR8888 (PSP), 1:1 centered ------------- */

static inline uint32_t argb_to_psp(uint32_t p)
{
    /* GB: 0xAARRGGBB  ->  PSP 8888 word: 0xAABBGGRR (R and B swapped) */
    uint32_t r = (p >> 16) & 0xFF;
    uint32_t g = (p >>  8) & 0xFF;
    uint32_t b =  p        & 0xFF;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

void gb_platform_render_frame(const uint32_t* framebuffer)
{
    if (!framebuffer) return;
    for (int gy = 0; gy < GB_H; ++gy) {
        uint32_t* dst = g_fb + (g_off_y + gy) * PSP_STRIDE + g_off_x;
        const uint32_t* src = framebuffer + gy * GB_W;
        for (int gx = 0; gx < GB_W; ++gx)
            dst[gx] = argb_to_psp(src[gx]);
    }
}

/* --- Lifecycle --------------------------------------------------------- */

bool gb_platform_init(int scale)
{
    (void)scale;
    setup_callbacks();

    sceDisplaySetMode(0, PSP_W, PSP_H);
    /* Clear VRAM and show the (single) framebuffer. */
    for (int i = 0; i < PSP_STRIDE * PSP_H; ++i) g_fb[i] = 0xFF000000u;
    sceDisplaySetFrameBuf((void*)VRAM_BASE_CACHED, PSP_STRIDE,
                          PSP_DISPLAY_PIXELFORMAT_8888,
                          PSP_DISPLAY_SETBUF_NEXTFRAME);

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    audio_init();
    return true;
}

void gb_platform_shutdown(void)
{
    g_audio_run = 0;
    if (g_audio_thid >= 0) {
        sceKernelWaitThreadEnd(g_audio_thid, NULL);
        sceKernelDeleteThread(g_audio_thid);
    }
    if (g_audio_chan >= 0) sceAudioChRelease(g_audio_chan);
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
    sceCtrlReadBufferPositive(&pad, 1);

    uint8_t btns = 0xFF, dpad = 0xFF;

    if (pad.Buttons & PSP_CTRL_UP)     dpad &= ~0x04;
    if (pad.Buttons & PSP_CTRL_DOWN)   dpad &= ~0x08;
    if (pad.Buttons & PSP_CTRL_LEFT)   dpad &= ~0x02;
    if (pad.Buttons & PSP_CTRL_RIGHT)  dpad &= ~0x01;
    /* analog nub (center 128) */
    if (pad.Ly < 96)  dpad &= ~0x04;
    if (pad.Ly > 160) dpad &= ~0x08;
    if (pad.Lx < 96)  dpad &= ~0x02;
    if (pad.Lx > 160) dpad &= ~0x01;

    if (pad.Buttons & PSP_CTRL_CROSS)  btns &= ~0x01;   /* A */
    if (pad.Buttons & PSP_CTRL_CIRCLE) btns &= ~0x02;   /* B */
    if (pad.Buttons & PSP_CTRL_SELECT) btns &= ~0x04;
    if (pad.Buttons & PSP_CTRL_START)  btns &= ~0x08;

    g_joypad_buttons = btns;
    g_joypad_dpad    = dpad;
    return !g_quit;
}

uint8_t gb_platform_get_joypad(void) { return g_joypad_buttons & g_joypad_dpad; }

void gb_platform_vsync(void)
{
    sceDisplayWaitVblankStart();
    sceDisplaySetFrameBuf((void*)VRAM_BASE_CACHED, PSP_STRIDE,
                          PSP_DISPLAY_PIXELFORMAT_8888,
                          PSP_DISPLAY_SETBUF_NEXTFRAME);
}

void gb_platform_set_title(const char* title) { (void)title; }

/* --- Save state — ms0:/linksawakening/state.bin ------------------------ */

#define STATE_PATH    SAVE_DIR "/state.bin"
#define STATE_MAGIC   0x4C415350u   /* "LASP" */
#define STATE_VERSION 1u
#define SS_WRAM 0x8000
#define SS_VRAM 0x4000
#define SS_OAM  0xA0
#define SS_HRAM 0x7F
#define SS_IO   0x80

void gb_platform_save_state(GBContext* ctx)
{
    if (!ctx) return;
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
