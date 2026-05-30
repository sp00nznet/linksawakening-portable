/*
 * platform_dreamcast.c — Sega Dreamcast backend for the gb-recompiled runtime.
 *
 * Implements the platform_sdl.h contract + GBPlatformCallbacks (gbrt.h),
 * against KallistiOS (KOS; sh-elf toolchain). Plain C.
 *
 * Dreamcast is SH-4 little-endian — rom.c compiles unchanged. The catch is
 * RAM: 16 MB total. The recompiled binary is large, so build with care and
 * watch headroom — this is the tightest of the console targets (see
 * docs/PORTING.md). VRAM (8 MB) is separate, so the framebuffer is free of the
 * main-RAM budget.
 *
 * Subsystems:
 *   dc/video        — 640x480 RGB565 framebuffer; GB image blitted 3x centered
 *   dc/sound/stream — 44.1 kHz stereo, callback pulls from a ring; polled/frame
 *   dc/maple        — standard controller (d-pad + face buttons + analog)
 *   /vmu/a1         — battery-RAM saves (small; fits a VMU)
 *
 * Output: an .elf — runs in lxdream/flycast and, scrambled into 1ST_READ.BIN
 *         on a CD image, on real hardware (see build_dreamcast.sh).
 */

#include "platform_sdl.h"
#include "gbrt.h"
#include "ppu.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <kos.h>

/* KOS init flags — must be present in one linked TU; main() is in rom_main.c. */
KOS_INIT_FLAGS(INIT_DEFAULT);

/* --- Joypad globals (contract) ----------------------------------------- */
uint8_t g_joypad_buttons = 0xFF;
uint8_t g_joypad_dpad    = 0xFF;

#define GB_W   160
#define GB_H   144
#define SCR_W  640
#define SCR_H  480
#define GB_SCALE 3
#define GB_OX  ((SCR_W - GB_W * GB_SCALE) / 2)   /* 80 */
#define GB_OY  ((SCR_H - GB_H * GB_SCALE) / 2)   /* 24 */

static GBContext* g_ctx  = NULL;
static int        g_quit = 0;

/* --- Audio (snd_stream — 44.1 kHz stereo 16-bit) -----------------------
 * Define LA_DC_NO_AUDIO at build time for a silent build (diagnostic: isolates
 * whether audio is what's costing speed / causing the choppiness). */

#ifndef LA_DC_NO_AUDIO

#define ARING 16384

static int16_t      g_aring[ARING * 2];
static volatile int g_aw = 0, g_ar = 0;
static snd_stream_hnd_t g_snd = SND_STREAM_INVALID;
/* the stream callback hands back a pointer to this staging buffer */
static int16_t g_sndbuf[SND_STREAM_BUFFER_MAX / 2] __attribute__((aligned(32)));

static void xb_on_audio_sample(GBContext* ctx, int16_t left, int16_t right)
{
    (void)ctx;
    int nw = (g_aw + 1) & (ARING - 1);
    if (nw == g_ar) return;               /* ring full — drop */
    g_aring[g_aw * 2 + 0] = left;
    g_aring[g_aw * 2 + 1] = right;
    g_aw = nw;
}

/* Called by the stream driver; smp_req is bytes wanted (stereo 16-bit). */
static void* audio_cb(snd_stream_hnd_t hnd, int smp_req, int* smp_recv)
{
    (void)hnd;
    int frames = smp_req / 4;             /* 2 ch * 16-bit = 4 bytes/frame */
    if (frames > SND_STREAM_BUFFER_MAX / 4) frames = SND_STREAM_BUFFER_MAX / 4;
    for (int f = 0; f < frames; ++f) {
        if (g_ar != g_aw) {
            g_sndbuf[f*2+0] = g_aring[g_ar*2+0];
            g_sndbuf[f*2+1] = g_aring[g_ar*2+1];
            g_ar = (g_ar + 1) & (ARING - 1);
        } else {
            g_sndbuf[f*2+0] = g_sndbuf[f*2+1] = 0;
        }
    }
    *smp_recv = frames * 4;
    return g_sndbuf;
}

static void audio_init(void)
{
    if (snd_stream_init() != 0) return;
    g_snd = snd_stream_alloc(audio_cb, SND_STREAM_BUFFER_MAX);
    if (g_snd == SND_STREAM_INVALID) return;
    snd_stream_start(g_snd, 44100, 1);    /* 1 = stereo */
}

#else  /* LA_DC_NO_AUDIO — silent build */

static void xb_on_audio_sample(GBContext* ctx, int16_t left, int16_t right)
{ (void)ctx; (void)left; (void)right; }
static void audio_init(void) {}

#endif /* LA_DC_NO_AUDIO */

/* --- Battery RAM — /vmu/a1/<name>.sav ---------------------------------- */
/* VMU filenames are short; we use a fixed 8.3-ish name rather than the ROM
 * basename. A VMU holds ~100 KB usable, enough for LA's battery RAM. */

#define SAVE_DIR  "/vmu/a1"
#define SAVE_FILE SAVE_DIR "/LADX_SAV"

static bool xb_load_battery_ram(GBContext* ctx, const char* rom, void* data, size_t size)
{
    (void)ctx; (void)rom;
    FILE* f = fopen(SAVE_FILE, "rb");
    if (!f) return false;
    size_t got = fread(data, 1, size, f);
    fclose(f);
    return got == size;
}

static bool xb_save_battery_ram(GBContext* ctx, const char* rom, const void* data, size_t size)
{
    (void)ctx; (void)rom;
    FILE* f = fopen(SAVE_FILE, "wb");
    if (!f) return false;
    size_t put = fwrite(data, 1, size, f);
    fclose(f);
    return put == size;
}

/* --- Video — ARGB8888 (GB) -> RGB565, 3x centered ---------------------- */

static inline uint16_t argb_to_565(uint32_t p)
{
    uint32_t r = (p >> 16) & 0xFF;
    uint32_t g = (p >>  8) & 0xFF;
    uint32_t b =  p        & 0xFF;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void gb_platform_render_frame(const uint32_t* framebuffer)
{
    if (!framebuffer) return;
    uint16_t* vram = vram_s;              /* KOS-mapped 16-bit framebuffer */

    for (int gy = 0; gy < GB_H; ++gy) {
        const uint32_t* src = framebuffer + gy * GB_W;
        for (int sy = 0; sy < GB_SCALE; ++sy) {
            int screen_y = GB_OY + gy * GB_SCALE + sy;
            uint16_t* row = vram + screen_y * SCR_W + GB_OX;
            for (int gx = 0; gx < GB_W; ++gx) {
                uint16_t c = argb_to_565(src[gx]);
                uint16_t* p = row + gx * GB_SCALE;
                for (int sx = 0; sx < GB_SCALE; ++sx) p[sx] = c;
            }
        }
    }
}

/* --- Lifecycle --------------------------------------------------------- */

bool gb_platform_init(int scale)
{
    (void)scale;
    vid_set_mode(DM_640x480, PM_RGB565);
    /* clear to black */
    for (int i = 0; i < SCR_W * SCR_H; ++i) vram_s[i] = 0;
    audio_init();
    return true;
}

void gb_platform_shutdown(void)
{
#ifndef LA_DC_NO_AUDIO
    if (g_snd != SND_STREAM_INVALID) {
        snd_stream_stop(g_snd);
        snd_stream_destroy(g_snd);
        snd_stream_shutdown();
    }
#endif
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

/* --- Input — standard controller, port 0 ------------------------------- */

bool gb_platform_poll_events(GBContext* ctx)
{
    (void)ctx;
    if (g_quit) return false;

    maple_device_t* dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (dev) {
        cont_state_t* st = (cont_state_t*)maple_dev_status(dev);
        if (st) {
            uint8_t btns = 0xFF, dpad = 0xFF;
            if (st->buttons & CONT_DPAD_UP)    dpad &= ~0x04;
            if (st->buttons & CONT_DPAD_DOWN)  dpad &= ~0x08;
            if (st->buttons & CONT_DPAD_LEFT)  dpad &= ~0x02;
            if (st->buttons & CONT_DPAD_RIGHT) dpad &= ~0x01;
            /* analog stick (center 0, range -128..127) */
            if (st->joyy < -64) dpad &= ~0x04;
            if (st->joyy >  64) dpad &= ~0x08;
            if (st->joyx < -64) dpad &= ~0x02;
            if (st->joyx >  64) dpad &= ~0x01;

            if (st->buttons & CONT_A)     btns &= ~0x01;
            if (st->buttons & CONT_B)     btns &= ~0x02;
            if (st->buttons & CONT_Y)     btns &= ~0x04;   /* Select */
            if (st->buttons & CONT_START) btns &= ~0x08;
            /* A+B+X+Y+Start = quit to loader */
            if ((st->buttons & CONT_A) && (st->buttons & CONT_B) &&
                (st->buttons & CONT_X) && (st->buttons & CONT_Y) &&
                (st->buttons & CONT_START)) g_quit = 1;

            g_joypad_buttons = btns;
            g_joypad_dpad    = dpad;
        }
    }
    return !g_quit;
}

uint8_t gb_platform_get_joypad(void) { return g_joypad_buttons & g_joypad_dpad; }

void gb_platform_vsync(void)
{
#ifndef LA_DC_NO_AUDIO
    if (g_snd != SND_STREAM_INVALID) snd_stream_poll(g_snd);
#endif
    vid_waitvbl();
}

void gb_platform_set_title(const char* title) { (void)title; }

/* --- Save state — /vmu/a1/LADX_STT -------------------------------------
 * NOTE: a full save state (WRAM+VRAM+ERAM+PPU) can exceed a single VMU's
 * ~100 KB of usable space; the fopen below simply fails in that case and the
 * runtime carries on. A CF/SD adapter mount (e.g. /sd) would lift this limit. */

#define STATE_PATH    SAVE_DIR "/LADX_STT"
#define STATE_MAGIC   0x4C414443u   /* "LADC" */
#define STATE_VERSION 1u
#define SS_WRAM 0x8000
#define SS_VRAM 0x4000
#define SS_OAM  0xA0
#define SS_HRAM 0x7F
#define SS_IO   0x80

void gb_platform_save_state(GBContext* ctx)
{
    if (!ctx) return;
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
