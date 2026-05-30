/*
 * platform_gamecube.c — Nintendo GameCube backend for the gb-recompiled runtime.
 *
 * Implements the platform_sdl.h contract + GBPlatformCallbacks (gbrt.h),
 * against libogc (devkitPPC toolchain). Plain C.
 *
 * The GameCube shares libogc with the Wii, so this is structurally the Wii
 * backend (platform_wii.c) minus the Wii Remote — input comes only from the
 * GameCube controller (ogc/pad). GameCube is PowerPC big-endian, so the
 * AF/BC/DE/HL register-pair fix in gbrt.h (shared with Wii/PS3/360) makes the
 * recompiled rom.c correct here.
 *
 * Subsystems:
 *   gccore / VIDEO — the external framebuffer (XFB), YUY2 (2 px / 32-bit word)
 *   ogc/pad        — GameCube controller (port 0)
 *   asndlib        — audio (44.1 kHz stereo, matches the GB runtime)
 *   fat            — saves on an SD card via an SD Gecko / SD2SP2 adapter
 *
 * Output: a .dol — runs in Dolphin and on real hardware via Swiss.
 */

#include "platform_sdl.h"
#include "gbrt.h"
#include "ppu.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/stat.h>

#include <gccore.h>
#include <ogcsys.h>
#include <ogc/pad.h>
#include <asndlib.h>
#include <fat.h>

/* --- Joypad globals (contract) ----------------------------------------- */
uint8_t g_joypad_buttons = 0xFF;
uint8_t g_joypad_dpad    = 0xFF;

#define GB_W 160
#define GB_H 144

static GBContext*    g_ctx   = NULL;
static int           g_quit  = 0;
static GXRModeObj*   g_rmode = NULL;
static uint32_t*     g_xfb   = NULL;     /* YUY2 external framebuffer */
static int           g_fbw, g_fbh;       /* XFB dimensions in pixels  */
static int           g_scale, g_off_x, g_off_y;

/* --- RGB -> YUY2 (the GC XFB stores two pixels per 32-bit word) -------- */
static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static inline uint32_t rgb2yuy2(uint32_t p1, uint32_t p2)
{
    int r1 = (p1 >> 16) & 0xFF, g1 = (p1 >> 8) & 0xFF, b1 = p1 & 0xFF;
    int r2 = (p2 >> 16) & 0xFF, g2 = (p2 >> 8) & 0xFF, b2 = p2 & 0xFF;
    int y1 = clamp255(( 299*r1 + 587*g1 + 114*b1) / 1000);
    int y2 = clamp255(( 299*r2 + 587*g2 + 114*b2) / 1000);
    int cb = clamp255((-169*r1 - 331*g1 + 500*b1) / 1000 + 128);
    int cr = clamp255(( 500*r1 - 419*g1 -  81*b1) / 1000 + 128);
    return ((uint32_t)y1 << 24) | ((uint32_t)cb << 16) |
           ((uint32_t)y2 <<  8) |  (uint32_t)cr;
}

/* --- Audio (ASND — 44.1 kHz stereo 16-bit, double-buffered) ------------ */

#define ABUF_FRAMES 2048                       /* stereo frames per buffer */
#define ABUF_BYTES  (ABUF_FRAMES * 2 * 2)      /* 2 ch * 16-bit            */
#define ARING       16384

static int          g_audio_ok = 0;
static int16_t      g_aring[ARING * 2];
static volatile int g_aw = 0, g_ar = 0;
static uint8_t*     g_abuf[2] = { NULL, NULL };
static volatile int g_acur = 0;

static void audio_fill(uint8_t* dst)
{
    int16_t* out = (int16_t*)dst;
    for (int f = 0; f < ABUF_FRAMES; ++f) {
        if (g_ar != g_aw) {
            out[f*2+0] = g_aring[g_ar*2+0];
            out[f*2+1] = g_aring[g_ar*2+1];
            g_ar = (g_ar + 1) & (ARING - 1);
        } else {
            out[f*2+0] = out[f*2+1] = 0;
        }
    }
    DCFlushRange(dst, ABUF_BYTES);
}

static void audio_cb(s32 voice)
{
    if (ASND_StatusVoice(voice) != SND_WORKING) return;
    audio_fill(g_abuf[g_acur]);
    ASND_AddVoice(voice, g_abuf[g_acur], ABUF_BYTES);
    g_acur ^= 1;
}

static void audio_init(void)
{
    ASND_Init();
    ASND_Pause(0);
    g_abuf[0] = (uint8_t*)memalign(32, ABUF_BYTES);
    g_abuf[1] = (uint8_t*)memalign(32, ABUF_BYTES);
    if (!g_abuf[0] || !g_abuf[1]) return;
    memset(g_abuf[0], 0, ABUF_BYTES);
    memset(g_abuf[1], 0, ABUF_BYTES);
    DCFlushRange(g_abuf[0], ABUF_BYTES);
    DCFlushRange(g_abuf[1], ABUF_BYTES);
    /* GB runtime emits 44100 Hz stereo 16-bit — no resample needed. */
    ASND_SetVoice(0, VOICE_STEREO_16BIT, 44100, 0,
                  g_abuf[0], ABUF_BYTES, 255, 255, audio_cb);
    ASND_AddVoice(0, g_abuf[1], ABUF_BYTES);
    g_acur = 0;
    g_audio_ok = 1;
}

static void xb_on_audio_sample(GBContext* ctx, int16_t left, int16_t right)
{
    (void)ctx;
    int nw = (g_aw + 1) & (ARING - 1);
    if (nw == g_ar) return;
    g_aring[g_aw*2+0] = left;
    g_aring[g_aw*2+1] = right;
    g_aw = nw;
}

/* --- Battery RAM — sd:/linksawakening/<rom>.sav ------------------------ */

#define SAVE_DIR "sd:/linksawakening"

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

/* --- Reset button ------------------------------------------------------ */
static void on_reset(u32 irq, void* ctx) { (void)irq; (void)ctx; g_quit = 1; }

/* --- Video ------------------------------------------------------------- */

void gb_platform_render_frame(const uint32_t* framebuffer)
{
    if (!framebuffer || !g_xfb) return;

    int stride = g_fbw / 2;                 /* u32 words per XFB row */
    for (int gy = 0; gy < GB_H; ++gy) {
        for (int sy = 0; sy < g_scale; ++sy) {
            int screen_y = g_off_y + gy * g_scale + sy;
            if (screen_y < 0 || screen_y >= g_fbh) continue;
            uint32_t* row = g_xfb + screen_y * stride;
            const uint32_t* src = framebuffer + gy * GB_W;
            for (int gx = 0; gx < GB_W; gx += 2) {
                uint32_t p0 = src[gx];
                uint32_t p1 = (gx + 1 < GB_W) ? src[gx + 1] : p0;
                int base = g_off_x + gx * g_scale;
                for (int s = 0; s < g_scale; ++s) {
                    int wx0 = (base + s) / 2;
                    int wx1 = (base + g_scale + s) / 2;
                    if (wx0 >= 0 && wx0 < stride) row[wx0] = rgb2yuy2(p0, p0);
                    if (wx1 >= 0 && wx1 < stride) row[wx1] = rgb2yuy2(p1, p1);
                }
            }
        }
    }
}

/* --- Lifecycle --------------------------------------------------------- */

bool gb_platform_init(int scale)
{
    (void)scale;
    VIDEO_Init();
    g_rmode = VIDEO_GetPreferredMode(NULL);
    g_xfb = (uint32_t*)MEM_K0_TO_K1(SYS_AllocateFramebuffer(g_rmode));
    VIDEO_Configure(g_rmode);
    VIDEO_SetNextFramebuffer(g_xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (g_rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    g_fbw = g_rmode->fbWidth;
    g_fbh = g_rmode->xfbHeight;

    uint32_t black = rgb2yuy2(0, 0);
    for (int i = 0; i < (g_fbw / 2) * g_fbh; ++i) g_xfb[i] = black;

    int sx = g_fbw / GB_W, sy = g_fbh / GB_H;
    g_scale = (sx < sy ? sx : sy);
    if (g_scale < 1) g_scale = 1;
    g_off_x = ((g_fbw - GB_W * g_scale) / 2) & ~1;   /* keep even for YUY2 */
    g_off_y =  (g_fbh - GB_H * g_scale) / 2;
    if (g_off_x < 0) g_off_x = 0;
    if (g_off_y < 0) g_off_y = 0;

    fatInitDefault();
    PAD_Init();
    audio_init();

    SYS_SetResetCallback(on_reset);

    return true;
}

void gb_platform_shutdown(void)
{
    if (g_audio_ok) ASND_Pause(1);
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

/* --- Input — GameCube controller, port 0 ------------------------------- */

bool gb_platform_poll_events(GBContext* ctx)
{
    (void)ctx;
    if (g_quit) return false;

    uint8_t btns = 0xFF, dpad = 0xFF;

    PAD_ScanPads();
    uint16_t gc = PAD_ButtonsHeld(0);
    int8_t   gx = PAD_StickX(0), gy = PAD_StickY(0);
    if (gc & PAD_BUTTON_UP)    dpad &= ~0x04;
    if (gc & PAD_BUTTON_DOWN)  dpad &= ~0x08;
    if (gc & PAD_BUTTON_LEFT)  dpad &= ~0x02;
    if (gc & PAD_BUTTON_RIGHT) dpad &= ~0x01;
    if (gy >  40) dpad &= ~0x04;
    if (gy < -40) dpad &= ~0x08;
    if (gx < -40) dpad &= ~0x02;
    if (gx >  40) dpad &= ~0x01;
    if (gc & PAD_BUTTON_A)     btns &= ~0x01;
    if (gc & PAD_BUTTON_B)     btns &= ~0x02;
    if (gc & PAD_TRIGGER_Z)    btns &= ~0x04;   /* Select */
    if (gc & PAD_BUTTON_START) btns &= ~0x08;
    /* Start+Z together = quit to loader */
    if ((gc & PAD_BUTTON_START) && (gc & PAD_TRIGGER_Z) &&
        (gc & PAD_TRIGGER_L)    && (gc & PAD_TRIGGER_R)) g_quit = 1;

    g_joypad_buttons = btns;
    g_joypad_dpad    = dpad;
    return !g_quit;
}

uint8_t gb_platform_get_joypad(void) { return g_joypad_buttons & g_joypad_dpad; }

void gb_platform_vsync(void)
{
    VIDEO_SetNextFramebuffer(g_xfb);
    VIDEO_Flush();
    VIDEO_WaitVSync();
}

void gb_platform_set_title(const char* title) { (void)title; }

/* --- Save state — sd:/linksawakening/state.bin ------------------------- */

#define STATE_PATH    SAVE_DIR "/state.bin"
#define STATE_MAGIC   0x4C413347u   /* "LA3G" */
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
