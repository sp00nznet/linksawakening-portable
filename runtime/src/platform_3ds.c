/*
 * platform_3ds.c — Nintendo 3DS backend for the gb-recompiled runtime.
 *
 * Implements the platform_sdl.h contract + the GBPlatformCallbacks struct
 * from gbrt.h, against libctru (devkitARM toolchain). Plain C.
 *
 * libctru subsystems used:
 *   <3ds.h> umbrella — gfx (top-screen framebuffer), hid (buttons),
 *   ndsp (audio), apt (app lifecycle), sdmc (file I/O via newlib stdio).
 *
 * The 3DS top screen is 400x240. Its framebuffer is stored rotated 90°
 * (column-major, 240 tall in memory) — see fb_plot() below. The GB image
 * (160x144) is blitted 1:1, centered, leaving a border. Integer/▒fractional
 * upscaling is a later optimization.
 */

#include "platform_sdl.h"   /* the contract */
#include "gbrt.h"           /* GBContext + GBPlatformCallbacks */
#include "ppu.h"            /* GBPPU for save-state */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>   /* mkdir */

#include <3ds.h>

/* --- Joypad globals required by the contract --------------------------- */
uint8_t g_joypad_buttons = 0xFF;  /* active low: Start, Select, B, A */
uint8_t g_joypad_dpad    = 0xFF;  /* active low: Down, Up, Left, Right */

#define GB_W 160
#define GB_H 144
#define TOP_W 400
#define TOP_H 240

static GBContext* g_ctx   = NULL;
static int        g_off_x = (TOP_W - GB_W) / 2;   /* 120 */
static int        g_off_y = (TOP_H - GB_H) / 2;   /* 48  */
static int        g_quit  = 0;

/* --- Audio (ndsp) -------------------------------------------------------
 * The runtime calls on_audio_sample() per stereo frame at 44100 Hz. We
 * stage samples in a ring, then each frame copy ready chunks into one of
 * two ndsp wave buffers in linear memory. */

#define SR          44100
#define ARING       8192                  /* stereo frames */
#define AWBUF_FR    1024                  /* frames per wave buffer */
#define AWBUF_N     2

static int          g_audio_ok = 0;
static int16_t      g_aring[ARING * 2];
static volatile int g_aw = 0, g_ar = 0;
static ndspWaveBuf  g_wbuf[AWBUF_N];
static int16_t*     g_wbuf_mem[AWBUF_N];

static void audio_init(void)
{
    if (ndspInit() != 0) { g_audio_ok = 0; return; }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, SR);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    for (int i = 0; i < AWBUF_N; ++i) {
        g_wbuf_mem[i] = (int16_t*)linearAlloc(AWBUF_FR * 2 * sizeof(int16_t));
        memset(&g_wbuf[i], 0, sizeof(ndspWaveBuf));
        if (g_wbuf_mem[i]) {
            memset(g_wbuf_mem[i], 0, AWBUF_FR * 2 * sizeof(int16_t));
            g_wbuf[i].data_vaddr = g_wbuf_mem[i];
            g_wbuf[i].nsamples   = AWBUF_FR;
            g_wbuf[i].status     = NDSP_WBUF_DONE;
        }
    }
    g_audio_ok = 1;
}

/* Move ready samples from the ring into any free wave buffer. */
static void audio_update(void)
{
    if (!g_audio_ok) return;
    for (int i = 0; i < AWBUF_N; ++i) {
        if (g_wbuf[i].status != NDSP_WBUF_DONE && g_wbuf[i].status != NDSP_WBUF_FREE)
            continue;
        int avail = (g_aw - g_ar) & (ARING - 1);
        if (avail < AWBUF_FR) return;     /* wait for a full buffer's worth */

        for (int f = 0; f < AWBUF_FR; ++f) {
            int rp = (g_ar + f) & (ARING - 1);
            g_wbuf_mem[i][f * 2 + 0] = g_aring[rp * 2 + 0];
            g_wbuf_mem[i][f * 2 + 1] = g_aring[rp * 2 + 1];
        }
        g_ar = (g_ar + AWBUF_FR) & (ARING - 1);

        DSP_FlushDataCache(g_wbuf_mem[i], AWBUF_FR * 2 * sizeof(int16_t));
        g_wbuf[i].nsamples = AWBUF_FR;
        ndspChnWaveBufAdd(0, &g_wbuf[i]);
    }
}

static void xb_on_audio_sample(GBContext* ctx, int16_t left, int16_t right)
{
    (void)ctx;
    int nw = (g_aw + 1) & (ARING - 1);
    if (nw == g_ar) return;               /* ring full — drop */
    g_aring[g_aw * 2 + 0] = left;
    g_aring[g_aw * 2 + 1] = right;
    g_aw = nw;
}

/* --- Battery RAM (SRAM) — sdmc:/3ds/linksawakening/<rom>.sav ----------- */

#define SAVE_DIR "sdmc:/3ds/linksawakening"

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
    mkdir("sdmc:/3ds", 0777);
    mkdir(SAVE_DIR, 0777);
    char path[256];
    save_path(path, sizeof(path), rom, ".sav");
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t put = fwrite(data, 1, size, f);
    fclose(f);
    return put == size;
}

/* --- Video -------------------------------------------------------------- */

/* Plot one pixel at visual top-screen coord (x,y). The 3DS framebuffer is
 * rotated: in memory it is 240 tall, column-major, with screen-row y at
 * memory offset (239 - y). Format is GSP_BGR8_OES (3 bytes, B,G,R). */
static inline void fb_plot(uint8_t* fb, int x, int y,
                           uint8_t r, uint8_t g, uint8_t b)
{
    int idx = (x * TOP_H + (TOP_H - 1 - y)) * 3;
    fb[idx + 0] = b;
    fb[idx + 1] = g;
    fb[idx + 2] = r;
}

void gb_platform_render_frame(const uint32_t* framebuffer)
{
    if (!framebuffer) return;
    audio_update();

    uint8_t* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (!fb) return;

    for (int gy = 0; gy < GB_H; ++gy) {
        for (int gx = 0; gx < GB_W; ++gx) {
            uint32_t p = framebuffer[gy * GB_W + gx];
            uint8_t r = (p >> 16) & 0xFF;
            uint8_t g = (p >>  8) & 0xFF;
            uint8_t b = (p      ) & 0xFF;
            fb_plot(fb, g_off_x + gx, g_off_y + gy, r, g, b);
        }
    }

    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_TOP, true);
}

/* --- Lifecycle ---------------------------------------------------------- */

bool gb_platform_init(int scale)
{
    (void)scale;                 /* 3DS: fixed 1:1 centered blit for now */
    gfxInitDefault();
    /* GSP_BGR8_OES is the gfx default; set explicitly for clarity. */
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    consoleInit(GFX_BOTTOM, NULL);   /* bottom screen = debug text console */
    printf("la360: platform_3ds init\n");
    audio_init();
    printf("la360: audio %s\n", g_audio_ok ? "ndsp OK" : "disabled");
    return true;
}

void gb_platform_shutdown(void)
{
    if (g_audio_ok) {
        ndspExit();
        for (int i = 0; i < AWBUF_N; ++i)
            if (g_wbuf_mem[i]) linearFree(g_wbuf_mem[i]);
    }
    gfxExit();
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

/* --- Input -------------------------------------------------------------- */

bool gb_platform_poll_events(GBContext* ctx)
{
    (void)ctx;
    if (g_quit || !aptMainLoop()) return false;

    hidScanInput();
    uint32_t k = hidKeysHeld();

    uint8_t btns = 0xFF, dpad = 0xFF;

    /* D-pad — accept both the +Control Pad and the Circle Pad */
    if (k & (KEY_DUP    | KEY_CPAD_UP))    dpad &= ~0x04;
    if (k & (KEY_DDOWN  | KEY_CPAD_DOWN))  dpad &= ~0x08;
    if (k & (KEY_DLEFT  | KEY_CPAD_LEFT))  dpad &= ~0x02;
    if (k & (KEY_DRIGHT | KEY_CPAD_RIGHT)) dpad &= ~0x01;

    /* Buttons: GB A/B on 3DS A/B; Select/Start direct */
    if (k & KEY_A)      btns &= ~0x01;
    if (k & KEY_B)      btns &= ~0x02;
    if (k & KEY_SELECT) btns &= ~0x04;
    if (k & KEY_START)  btns &= ~0x08;

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
    audio_update();
    gspWaitForVBlank();          /* 3DS runs at ~59.83 Hz */
}

void gb_platform_set_title(const char* title) { (void)title; }

/* --- Save state — sdmc:/3ds/linksawakening/state.bin ------------------- */

#define STATE_PATH    SAVE_DIR "/state.bin"
#define STATE_MAGIC   0x4C413344u   /* "LA3D" */
#define STATE_VERSION 1u

#define SS_WRAM 0x8000
#define SS_VRAM 0x4000
#define SS_OAM  0xA0
#define SS_HRAM 0x7F
#define SS_IO   0x80

void gb_platform_save_state(GBContext* ctx)
{
    if (!ctx) return;
    mkdir("sdmc:/3ds", 0777);
    mkdir(SAVE_DIR, 0777);
    FILE* f = fopen(STATE_PATH, "wb");
    if (!f) return;

    uint32_t magic = STATE_MAGIC, ver = STATE_VERSION;
    fwrite(&magic, 4, 1, f);
    fwrite(&ver,   4, 1, f);
    fwrite(&ctx->af, 2, 1, f);  fwrite(&ctx->bc, 2, 1, f);
    fwrite(&ctx->de, 2, 1, f);  fwrite(&ctx->hl, 2, 1, f);
    fwrite(&ctx->sp, 2, 1, f);  fwrite(&ctx->pc, 2, 1, f);
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
    fread(&magic, 4, 1, f);
    fread(&ver,   4, 1, f);
    if (magic != STATE_MAGIC || ver != STATE_VERSION) { fclose(f); return; }
    fread(&ctx->af, 2, 1, f);  fread(&ctx->bc, 2, 1, f);
    fread(&ctx->de, 2, 1, f);  fread(&ctx->hl, 2, 1, f);
    fread(&ctx->sp, 2, 1, f);  fread(&ctx->pc, 2, 1, f);
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

/* --- Debug / dev no-ops ------------------------------------------------- */
void gb_platform_set_input_script(const char* path)        { (void)path; }
void gb_platform_set_dump_frames(const char* dir)          { (void)dir; }
void gb_platform_set_screenshot_prefix(const char* prefix) { (void)prefix; }
