/*
 * platform_libxenon.c — Xbox 360 backend for the gb-recompiled runtime.
 *
 * Implements the platform_sdl.h contract (despite the name, that header is
 * platform-agnostic) plus the GBPlatformCallbacks struct from gbrt.h.
 *
 * Designed to be compiled with xenon-gcc (libxenon devkitxenon toolchain).
 * Plain C — no C++ features used.
 *
 * APIs used:
 *   xenos/xenos.h          — xenos_init (video bring-up)
 *   console/console.h      — console_init, console_pset (simple framebuffer blit)
 *   input/input.h          — get_controller_data (USB gamepad)
 *   xenon_sound/sound.h    — xenon_sound_init, xenon_sound_submit, get_free
 *   time/time.h            — udelay, mdelay
 *   xenon_uart/xenon_uart.h— uart_puts (debug log to USB serial)
 *   <stdio.h>              — fopen/fwrite/fread/fclose for save files
 */

#include "platform_sdl.h"   /* the contract */
#include "gbrt.h"           /* for GBPlatformCallbacks + GBContext */
#include "ppu.h"            /* for GBPPU (save_state writes it whole) */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <xetypes.h>
#include <xenos/xenos.h>
#include <console/console.h>
#include <input/input.h>
#include <xenon_sound/sound.h>
#include <xenon_uart/xenon_uart.h>
#include <time/time.h>

/* --- Joypad state (exported globals the contract requires) ------------- */
uint8_t g_joypad_buttons = 0xFF;  /* active low: Start, Select, B, A */
uint8_t g_joypad_dpad    = 0xFF;  /* active low: Down, Up, Left, Right */

static GBContext* g_ctx     = NULL;
static int        g_scale   = 4;        /* 4× → 640×576 inside a 1280×720 console */
static int        g_off_x   = 0;        /* centering offsets */
static int        g_off_y   = 0;
static int        g_quit    = 0;
static int        g_fbw     = 1280;     /* set from console_get_dimensions */
static int        g_fbh     = 720;

#define GB_W 160
#define GB_H 144

/* --- Audio ring buffer --------------------------------------------------
 * The runtime calls on_audio_sample() once per stereo frame (44100 Hz).
 * libxenon's audio hardware wants interleaved 16-bit stereo samples.
 * We pack the sample pair as a single uint32 (low 16 = left, high 16 = right
 * — the order will be confirmed empirically; if audio is byteswapped on
 * playback, fix here). */

#define AUDIO_RING  4096                 /* stereo frames */
static int16_t g_audio_ring[AUDIO_RING * 2];
static volatile int g_audio_w = 0;       /* writer (producer)  */
static volatile int g_audio_r = 0;       /* reader (drain)     */

/* Drain helper: push as many full stereo frames as the hardware will take. */
static void audio_drain(void)
{
    int avail = (g_audio_w - g_audio_r) & (AUDIO_RING - 1);
    if (avail == 0) return;

    int hw_free_bytes = xenon_sound_get_free();
    int hw_free_frames = hw_free_bytes / 4;   /* 2 ch × 2 bytes */
    if (hw_free_frames == 0) return;

    int submit = avail < hw_free_frames ? avail : hw_free_frames;
    /* Submit in two pieces if it wraps. */
    int first = AUDIO_RING - g_audio_r;
    if (first > submit) first = submit;
    xenon_sound_submit(&g_audio_ring[g_audio_r * 2], first * 4);
    if (submit > first) {
        xenon_sound_submit(&g_audio_ring[0], (submit - first) * 4);
    }
    g_audio_r = (g_audio_r + submit) & (AUDIO_RING - 1);
}

/* The platform's audio callback that the runtime invokes through the
 * GBPlatformCallbacks struct. */
static void xb_on_audio_sample(GBContext* ctx, int16_t left, int16_t right)
{
    (void)ctx;
    int next_w = (g_audio_w + 1) & (AUDIO_RING - 1);
    if (next_w == g_audio_r) {
        /* Ring full — drop this sample. Underrun protection: better to
         * drop than block the game thread. */
        return;
    }
    g_audio_ring[g_audio_w * 2 + 0] = left;
    g_audio_ring[g_audio_w * 2 + 1] = right;
    g_audio_w = next_w;
}

/* --- Battery RAM (SRAM save) --------------------------------------------
 * Save path: uda:/linksawakening/<rom_name>.sav (USB mass storage).
 * If uda: isn't mounted, fall back to CWD which on libxenon is usually
 * dvd:/ for disc boot or hdd:/ otherwise. */

#define SAVE_DIR_USB   "uda:/linksawakening"
#define SAVE_DIR_FALLBACK "."

static void make_save_path(char* out, size_t out_sz, const char* rom_name, const char* suffix)
{
    /* "rom_name" might include a path — strip dirs */
    const char* base = rom_name;
    for (const char* p = rom_name; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    snprintf(out, out_sz, "%s/%s%s", SAVE_DIR_USB, base, suffix);
}

static bool xb_load_battery_ram(GBContext* ctx, const char* rom_name, void* data, size_t size)
{
    (void)ctx;
    char path[256];
    make_save_path(path, sizeof(path), rom_name ? rom_name : "rom", ".sav");
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    size_t got = fread(data, 1, size, f);
    fclose(f);
    return got == size;
}

static bool xb_save_battery_ram(GBContext* ctx, const char* rom_name, const void* data, size_t size)
{
    (void)ctx;
    char path[256];
    make_save_path(path, sizeof(path), rom_name ? rom_name : "rom", ".sav");
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t put = fwrite(data, 1, size, f);
    fclose(f);
    return put == size;
}

/* --- Video --------------------------------------------------------------
 * Console-based blit. console_pset() draws a single pixel. We splat each
 * GB pixel as a scale×scale block. This is slow (a 160×144 frame at 4×
 * is 92,160 pset calls per frame) but it's correct and uses zero GPU code
 * — proving the runtime/platform plumbing before optimizing.
 *
 * Next step would be a Xenos GPU surface that we upload the framebuffer
 * to once per frame and let the hardware scale, but that's Phase 7 work. */

static inline void argb_to_rgb(uint32_t argb, uint8_t* r, uint8_t* g, uint8_t* b)
{
    *r = (argb >> 16) & 0xFF;
    *g = (argb >>  8) & 0xFF;
    *b = (argb      ) & 0xFF;
}

void gb_platform_render_frame(const uint32_t* framebuffer)
{
    if (!framebuffer) return;

    /* Drain audio every frame (cheap, lazy back-pressure mitigation) */
    audio_drain();

    /* Blit GB 160×144 → console at g_scale */
    for (int gy = 0; gy < GB_H; ++gy) {
        for (int gx = 0; gx < GB_W; ++gx) {
            uint32_t p = framebuffer[gy * GB_W + gx];
            uint8_t r, g, b;
            argb_to_rgb(p, &r, &g, &b);
            int px = g_off_x + gx * g_scale;
            int py = g_off_y + gy * g_scale;
            for (int sy = 0; sy < g_scale; ++sy) {
                for (int sx = 0; sx < g_scale; ++sx) {
                    console_pset(px + sx, py + sy, r, g, b);
                }
            }
        }
    }
}

/* --- Lifecycle ----------------------------------------------------------- */

bool gb_platform_init(int scale)
{
    /* libxenon brings up Xenos at auto-detected resolution */
    xenos_init(VIDEO_MODE_AUTO);
    console_init();
    xenon_sound_init();

    unsigned int w = 1280, h = 720;
    console_get_dimensions(&w, &h);
    g_fbw = (int)w;
    g_fbh = (int)h;

    g_scale = (scale >= 1 && scale <= 8) ? scale : 4;
    /* Re-scale-down if requested scale doesn't fit (e.g. 480p) */
    while (g_scale > 1 && (GB_W * g_scale > g_fbw || GB_H * g_scale > g_fbh)) {
        g_scale--;
    }
    g_off_x = (g_fbw - GB_W * g_scale) / 2;
    g_off_y = (g_fbh - GB_H * g_scale) / 2;
    if (g_off_x < 0) g_off_x = 0;
    if (g_off_y < 0) g_off_y = 0;

    uart_puts((unsigned char*)"la360: platform_libxenon init OK\n");
    return true;
}

void gb_platform_shutdown(void)
{
    /* libxenon has no explicit teardown hooks for xenos/sound; the XEX just
     * exits and the kernel cleans up. */
    g_ctx = NULL;
}

void gb_platform_register_context(GBContext* ctx)
{
    g_ctx = ctx;
    GBPlatformCallbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_audio_sample   = xb_on_audio_sample;
    cbs.load_battery_ram  = xb_load_battery_ram;
    cbs.save_battery_ram  = xb_save_battery_ram;
    gb_set_platform_callbacks(ctx, &cbs);
}

/* --- Input poll --------------------------------------------------------- */

bool gb_platform_poll_events(GBContext* ctx)
{
    (void)ctx;
    if (g_quit) return false;

    struct controller_data_s pad;
    if (get_controller_data(&pad, 0) == 0) {
        /* No pad — keep last state (or release everything) */
        g_joypad_buttons = 0xFF;
        g_joypad_dpad    = 0xFF;
        return true;
    }

    /* Xbox Guide button → quit */
    if (pad.logo) g_quit = 1;

    /* Active low: start at 0xFF, clear bits as keys go down */
    uint8_t btns = 0xFF;
    uint8_t dpad = 0xFF;

    /* D-pad: 0x04=Up, 0x08=Down, 0x02=Left, 0x01=Right */
    if (pad.up)    dpad &= ~0x04;
    if (pad.down)  dpad &= ~0x08;
    if (pad.left)  dpad &= ~0x02;
    if (pad.right) dpad &= ~0x01;

    /* Buttons: 0x01=A, 0x02=B, 0x04=Select, 0x08=Start */
    if (pad.a)     btns &= ~0x01;
    if (pad.b)     btns &= ~0x02;
    if (pad.back || pad.x) btns &= ~0x04;  /* Back or X → Select */
    if (pad.start || pad.y) btns &= ~0x08; /* Start or Y → Start */

    g_joypad_buttons = btns;
    g_joypad_dpad    = dpad;
    return true;
}

uint8_t gb_platform_get_joypad(void)
{
    return g_joypad_buttons & g_joypad_dpad;
}

void gb_platform_vsync(void)
{
    /* libxenon doesn't expose a vsync-wait primitive directly to apps;
     * pacing at ~60 Hz via udelay is enough to keep audio fed and the
     * frame loop steady. Refine later with proper vblank if needed. */
    audio_drain();   /* one more drain in the idle slice */
    udelay(16000);   /* ~60 fps */
}

void gb_platform_set_title(const char* title)
{
    (void)title;     /* no window title on 360 */
}

/* --- Save state (full-snapshot, separate from battery RAM) -------------- */

#define STATE_PATH       SAVE_DIR_USB "/state.bin"
#define STATE_MAGIC      0x4C413336u   /* "LA36" */
#define STATE_VERSION    1u

/* Mirror the field set platform_sdl.cpp writes. Big-endian disk format
 * would be cleaner (see ENDIAN_AUDIT.md option A); for the v1 360 build
 * we accept that state files are not portable across hosts and just
 * write host-endian. */

#define SS_WRAM_SIZE  (32*1024)
#define SS_VRAM_SIZE  (16*1024)
#define SS_OAM_SIZE   (160)
#define SS_HRAM_SIZE  (127)
#define SS_IO_SIZE    (128)

void gb_platform_save_state(GBContext* ctx)
{
    if (!ctx) return;
    FILE* f = fopen(STATE_PATH, "wb");
    if (!f) return;

    uint32_t magic = STATE_MAGIC, ver = STATE_VERSION;
    fwrite(&magic, 4, 1, f);
    fwrite(&ver,   4, 1, f);

    fwrite(&ctx->af, 2, 1, f);
    fwrite(&ctx->bc, 2, 1, f);
    fwrite(&ctx->de, 2, 1, f);
    fwrite(&ctx->hl, 2, 1, f);
    fwrite(&ctx->sp, 2, 1, f);
    fwrite(&ctx->pc, 2, 1, f);
    fwrite(&ctx->f_z, 1, 1, f);
    fwrite(&ctx->f_n, 1, 1, f);
    fwrite(&ctx->f_h, 1, 1, f);
    fwrite(&ctx->f_c, 1, 1, f);
    fwrite(&ctx->ime, 1, 1, f);
    fwrite(&ctx->halted, 1, 1, f);
    fwrite(&ctx->rom_bank, 2, 1, f);
    fwrite(&ctx->ram_bank, 1, 1, f);
    fwrite(&ctx->wram_bank, 1, 1, f);
    fwrite(&ctx->vram_bank, 1, 1, f);
    fwrite(&ctx->cycles, 4, 1, f);

    if (ctx->wram) fwrite(ctx->wram, 1, SS_WRAM_SIZE, f);
    if (ctx->vram) fwrite(ctx->vram, 1, SS_VRAM_SIZE, f);
    if (ctx->oam)  fwrite(ctx->oam,  1, SS_OAM_SIZE,  f);
    if (ctx->hram) fwrite(ctx->hram, 1, SS_HRAM_SIZE, f);
    if (ctx->io)   fwrite(ctx->io,   1, SS_IO_SIZE,   f);

    if (ctx->eram) {
        uint32_t sz = (uint32_t)ctx->eram_size;
        fwrite(&sz, 4, 1, f);
        fwrite(ctx->eram, 1, sz, f);
    } else {
        uint32_t sz = 0;
        fwrite(&sz, 4, 1, f);
    }

    if (ctx->ppu) fwrite(ctx->ppu, sizeof(GBPPU), 1, f);

    fclose(f);
    uart_puts((unsigned char*)"la360: state saved\n");
}

void gb_platform_load_state(GBContext* ctx)
{
    if (!ctx) return;
    FILE* f = fopen(STATE_PATH, "rb");
    if (!f) return;

    uint32_t magic = 0, ver = 0;
    fread(&magic, 4, 1, f);
    fread(&ver,   4, 1, f);
    if (magic != STATE_MAGIC || ver != STATE_VERSION) {
        fclose(f);
        return;
    }

    fread(&ctx->af, 2, 1, f);
    fread(&ctx->bc, 2, 1, f);
    fread(&ctx->de, 2, 1, f);
    fread(&ctx->hl, 2, 1, f);
    fread(&ctx->sp, 2, 1, f);
    fread(&ctx->pc, 2, 1, f);
    fread(&ctx->f_z, 1, 1, f);
    fread(&ctx->f_n, 1, 1, f);
    fread(&ctx->f_h, 1, 1, f);
    fread(&ctx->f_c, 1, 1, f);
    fread(&ctx->ime, 1, 1, f);
    fread(&ctx->halted, 1, 1, f);
    fread(&ctx->rom_bank, 2, 1, f);
    fread(&ctx->ram_bank, 1, 1, f);
    fread(&ctx->wram_bank, 1, 1, f);
    fread(&ctx->vram_bank, 1, 1, f);
    fread(&ctx->cycles, 4, 1, f);

    if (ctx->wram) fread(ctx->wram, 1, SS_WRAM_SIZE, f);
    if (ctx->vram) fread(ctx->vram, 1, SS_VRAM_SIZE, f);
    if (ctx->oam)  fread(ctx->oam,  1, SS_OAM_SIZE,  f);
    if (ctx->hram) fread(ctx->hram, 1, SS_HRAM_SIZE, f);
    if (ctx->io)   fread(ctx->io,   1, SS_IO_SIZE,   f);

    uint32_t eram_sz = 0;
    fread(&eram_sz, 4, 1, f);
    if (ctx->eram && eram_sz <= ctx->eram_size) {
        fread(ctx->eram, 1, eram_sz, f);
    }

    if (ctx->ppu) fread(ctx->ppu, sizeof(GBPPU), 1, f);
    fclose(f);
    uart_puts((unsigned char*)"la360: state loaded\n");
}

/* --- Debug / dev helpers (no-ops on 360) -------------------------------- */
void gb_platform_set_input_script(const char* path)        { (void)path; }
void gb_platform_set_dump_frames(const char* dir)          { (void)dir; }
void gb_platform_set_screenshot_prefix(const char* prefix) { (void)prefix; }
