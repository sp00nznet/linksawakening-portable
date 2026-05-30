/*
 * platform_ps2.c — Sony PlayStation 2 backend for the gb-recompiled runtime.
 *
 * Implements the platform_sdl.h contract + GBPlatformCallbacks (gbrt.h),
 * against the PS2SDK (ee-gcc / mips64r5900el toolchain) + gsKit. Plain C.
 *
 * PS2 (Emotion Engine) is MIPS little-endian — rom.c compiles unchanged.
 *
 * This is the heaviest backend because the PS2 splits work across the EE and
 * the IOP: peripherals live behind IOP modules (IRX) that must be loaded at
 * boot. We load the controller modules from rom0: (always present) and embed
 * the audio + USB-storage modules in the ELF (the build script runs bin2c on
 * the PS2SDK .irx files — see build_ps2.sh).
 *
 * Subsystems:
 *   gsKit    — GB image uploaded to a CT32 texture, drawn 3x centered (640x448)
 *   audsrv   — 44.1 kHz stereo 16-bit, fed from a ring by an EE thread
 *   libpad   — digital pad + left analog (SIO2MAN + PADMAN from rom0)
 *   mass:/   — saves on USB storage (usbd.irx + usbhdfsd.irx, embedded)
 *
 * IOP module loading is the part most likely to need tweaking per environment
 * (PCSX2 vs real hardware, module name variants); it is isolated in load_iop().
 *
 * Output: an ELF — runs in PCSX2 and on hardware via uLaunchELF.
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

#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <sbv_patches.h>
#include <libpad.h>
#include <audsrv.h>

#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>

/* IRX images embedded by the build (bin2c). */
extern unsigned char freesd_irx[];    extern unsigned int size_freesd_irx;
extern unsigned char audsrv_irx[];    extern unsigned int size_audsrv_irx;
extern unsigned char usbd_irx[];      extern unsigned int size_usbd_irx;
extern unsigned char usbhdfsd_irx[];  extern unsigned int size_usbhdfsd_irx;

/* --- Joypad globals (contract) ----------------------------------------- */
uint8_t g_joypad_buttons = 0xFF;
uint8_t g_joypad_dpad    = 0xFF;

#define GB_W 160
#define GB_H 144

static GBContext* g_ctx     = NULL;
static int        g_quit    = 0;
static GSGLOBAL*  g_gs      = NULL;
static GSTEXTURE  g_tex;

/* --- Controller -------------------------------------------------------- */
static char g_pad_buf[256] __attribute__((aligned(64)));

/* --- Audio (audsrv — 44.1 kHz stereo 16-bit, EE worker thread) --------- */

#define AGRAIN  1024                       /* stereo frames per push        */
#define ARING   16384
#define ABYTES  (AGRAIN * 2 * 2)           /* 2 ch * 16-bit                 */

static int16_t      g_aring[ARING * 2];
static volatile int g_aw = 0, g_ar = 0;
static int          g_audio_ok = 0;
static int          g_audio_tid = -1;
static volatile int g_audio_run = 1;
static unsigned char g_audio_stack[16 * 1024] __attribute__((aligned(16)));

static void xb_on_audio_sample(GBContext* ctx, int16_t left, int16_t right)
{
    (void)ctx;
    int nw = (g_aw + 1) & (ARING - 1);
    if (nw == g_ar) return;               /* ring full — drop */
    g_aring[g_aw * 2 + 0] = left;
    g_aring[g_aw * 2 + 1] = right;
    g_aw = nw;
}

static void audio_thread(void* arg)
{
    (void)arg;
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
        audsrv_wait_audio(ABYTES);        /* block until the IOP buffer drains */
        audsrv_play_audio((char*)buf, ABYTES);
    }
    ExitThread();
}

static void audio_init(void)
{
    if (audsrv_init() != 0) return;
    struct audsrv_fmt_t fmt;
    fmt.freq     = 44100;
    fmt.bits     = 16;
    fmt.channels = 2;
    if (audsrv_set_format(&fmt) != 0) return;
    audsrv_set_volume(MAX_VOLUME);

    ee_thread_t th;
    memset(&th, 0, sizeof(th));
    th.func             = audio_thread;
    th.stack            = g_audio_stack;
    th.stack_size       = sizeof(g_audio_stack);
    th.initial_priority = 0x40;
    /* gp_reg must point at this module's $gp. */
    void* gp; __asm__ volatile ("move %0, $gp" : "=r"(gp));
    th.gp_reg = gp;
    g_audio_tid = CreateThread(&th);
    if (g_audio_tid >= 0) { StartThread(g_audio_tid, NULL); g_audio_ok = 1; }
}

/* --- IOP modules ------------------------------------------------------- */

static void load_iop(void)
{
    int id;
    /* Reset the IOP so embedded modules load cleanly, then re-init RPC. */
    SifInitRpc(0);
    while (!SifIopReset("", 0)) {}
    while (!SifIopSync()) {}
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    /* Controller — always available from the boot ROM. */
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);

    /* Audio (embedded). */
    SifExecModuleBuffer(freesd_irx,   size_freesd_irx,   0, NULL, &id);
    SifExecModuleBuffer(audsrv_irx,   size_audsrv_irx,   0, NULL, &id);

    /* USB mass storage for saves (embedded). */
    SifExecModuleBuffer(usbd_irx,     size_usbd_irx,     0, NULL, &id);
    SifExecModuleBuffer(usbhdfsd_irx, size_usbhdfsd_irx, 0, NULL, &id);
}

/* --- Battery RAM — mass:/linksawakening/<rom>.sav ---------------------- */

#define SAVE_DIR "mass:/linksawakening"

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

/* --- Video — ARGB8888 (GB) -> CT32 (A=0x80, swap R/B) ------------------ */

static inline uint32_t argb_to_gs(uint32_t p)
{
    uint32_t r = (p >> 16) & 0xFF;
    uint32_t g = (p >>  8) & 0xFF;
    uint32_t b =  p        & 0xFF;
    return (0x80u << 24) | (b << 16) | (g << 8) | r;   /* GS alpha 0x80 = 1.0 */
}

void gb_platform_render_frame(const uint32_t* framebuffer)
{
    if (!framebuffer || !g_gs) return;

    uint32_t* dst = (uint32_t*)g_tex.Mem;
    for (int i = 0; i < GB_W * GB_H; ++i)
        dst[i] = argb_to_gs(framebuffer[i]);
    gsKit_texture_upload(g_gs, &g_tex);

    /* 3x centered: 480x432 inside 640x448 -> x0=80, y0=8 */
    gsKit_clear(g_gs, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
    gsKit_prim_sprite_texture(g_gs, &g_tex,
        80.0f, 8.0f,                       /* screen x1,y1 */
        0.0f, 0.0f,                        /* tex u1,v1    */
        560.0f, 440.0f,                    /* screen x2,y2 */
        (float)GB_W, (float)GB_H,          /* tex u2,v2    */
        1,                                 /* z            */
        GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
    gsKit_queue_exec(g_gs);
}

/* --- Lifecycle --------------------------------------------------------- */

bool gb_platform_init(int scale)
{
    (void)scale;
    load_iop();

    padInit(0);
    padPortOpen(0, 0, g_pad_buf);

    /* --- gsKit (NTSC 640x448 interlaced, 32-bit, double-buffered) --- */
    g_gs = gsKit_init_global();
    g_gs->PSM        = GS_PSM_CT32;
    g_gs->PSMZ       = GS_PSMZ_16S;
    g_gs->ZBuffering = GS_SETTING_OFF;
    g_gs->DoubleBuffering = GS_SETTING_ON;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gsKit_init_screen(g_gs);
    gsKit_mode_switch(g_gs, GS_PERSISTENT);

    /* GB texture, system + VRAM. */
    g_tex.Width  = GB_W;
    g_tex.Height = GB_H;
    g_tex.PSM    = GS_PSM_CT32;
    g_tex.Filter = GS_FILTER_NEAREST;
    g_tex.Mem    = (u32*)memalign(128, gsKit_texture_size_ee(GB_W, GB_H, GS_PSM_CT32));
    g_tex.Vram   = gsKit_vram_alloc(g_gs,
                       gsKit_texture_size(GB_W, GB_H, GS_PSM_CT32),
                       GSKIT_ALLOC_USERBUFFER);

    audio_init();
    return true;
}

void gb_platform_shutdown(void)
{
    g_audio_run = 0;
    if (g_audio_ok && g_audio_tid >= 0) {
        /* let the thread observe the flag, then reap it */
        TerminateThread(g_audio_tid);
        DeleteThread(g_audio_tid);
    }
    audsrv_quit();
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

    struct padButtonStatus pad;
    uint8_t btns = 0xFF, dpad = 0xFF;

    if (padRead(0, 0, &pad) != 0) {
        /* libpad: bits are active-low in pad.btns; invert to "pressed=1". */
        uint16_t b = 0xFFFF ^ pad.btns;
        if (b & PAD_UP)       dpad &= ~0x04;
        if (b & PAD_DOWN)     dpad &= ~0x08;
        if (b & PAD_LEFT)     dpad &= ~0x02;
        if (b & PAD_RIGHT)    dpad &= ~0x01;
        /* left analog (center 128); valid in analog/dualshock mode */
        if (pad.ljoy_v < 96)  dpad &= ~0x04;
        if (pad.ljoy_v > 160) dpad &= ~0x08;
        if (pad.ljoy_h < 96)  dpad &= ~0x02;
        if (pad.ljoy_h > 160) dpad &= ~0x01;

        if (b & PAD_CROSS)    btns &= ~0x01;   /* A */
        if (b & PAD_CIRCLE)   btns &= ~0x02;   /* B */
        if (b & PAD_SELECT)   btns &= ~0x04;
        if (b & PAD_START)    btns &= ~0x08;
        if ((b & PAD_SELECT) && (b & PAD_START)) g_quit = 1;

        g_joypad_buttons = btns;
        g_joypad_dpad    = dpad;
    }
    return !g_quit;
}

uint8_t gb_platform_get_joypad(void) { return g_joypad_buttons & g_joypad_dpad; }

void gb_platform_vsync(void)
{
    gsKit_sync_flip(g_gs);
}

void gb_platform_set_title(const char* title) { (void)title; }

/* --- Save state — mass:/linksawakening/state.bin ----------------------- */

#define STATE_PATH    SAVE_DIR "/state.bin"
#define STATE_MAGIC   0x4C415032u   /* "LAP2" */
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
