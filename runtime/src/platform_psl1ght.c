/*
 * platform_psl1ght.c — PlayStation 3 backend for the gb-recompiled runtime.
 *
 * Implements the platform_sdl.h contract + GBPlatformCallbacks (gbrt.h),
 * against PSL1GHT (ps3dev toolchain, powerpc64-ps3-elf-gcc). Plain C.
 *
 * PS3 is PowerPC big-endian — the AF/BC/DE/HL register-pair union fix in
 * gbrt.h (Phase 5a) is what makes the recompiled rom.c correct here.
 *
 * Subsystems:
 *   rsx/rsx.h + sysutil/video.h — RSX framebuffer (canonical rsxutil setup)
 *   io/pad.h                    — DualShock 3 input
 *   audio/audio.h               — 48 kHz float stereo output
 *   sysutil/sysutil.h           — XMB exit callback
 *   stdio (newlib)              — save files on /dev_hdd0
 *
 * Video is a CPU software blit: the RSX display buffers from rsxMemalign
 * are CPU-writable u32 XRGB8888, row-major (no rotation, unlike the 3DS).
 */

#include "platform_sdl.h"
#include "gbrt.h"
#include "ppu.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/stat.h>

#include <ppu-types.h>
#include <sys/process.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>
#include <sysutil/sysutil.h>
#include <io/pad.h>
#include <audio/audio.h>

SYS_PROCESS_PARAM(1001, 0x100000)

/* --- Joypad globals (contract) ----------------------------------------- */
uint8_t g_joypad_buttons = 0xFF;
uint8_t g_joypad_dpad    = 0xFF;

#define GB_W 160
#define GB_H 144
#define CB_SIZE   0x100000
#define HOST_SIZE (32 * 1024 * 1024)

/* --- RSX framebuffer state (canonical rsxutil pattern) ----------------- */
static gcmContextData* context = NULL;
static videoResolution res;
static u32  display_width, display_height;
static u32  color_pitch;
static u32  color_offset[2];
static u32* color_buffer[2];
static u32  depth_pitch, depth_offset;
static u32* depth_buffer;
static u32  curr_fb = 0, first_fb = 1;
static u32  s_label = 1;
#define GCM_LABEL_INDEX 255

static GBContext* g_ctx  = NULL;
static int        g_quit = 0;
static int        g_scale = 1, g_off_x = 0, g_off_y = 0;

static void rsx_wait_finish(void)
{
    rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, s_label);
    rsxFlushBuffer(context);
    while (*(vu32*)gcmGetLabelAddress(GCM_LABEL_INDEX) != s_label) usleep(30);
    ++s_label;
}
static void rsx_wait_idle(void)
{
    rsxSetWriteBackendLabel(context, GCM_LABEL_INDEX, s_label);
    rsxSetWaitLabel(context, GCM_LABEL_INDEX, s_label);
    ++s_label;
    rsx_wait_finish();
}

static void set_render_target(u32 idx)
{
    gcmSurface sf;
    memset(&sf, 0, sizeof(sf));
    sf.colorFormat      = GCM_TF_COLOR_X8R8G8B8;
    sf.colorTarget      = GCM_TF_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX;
    sf.colorOffset[0]   = color_offset[idx];
    sf.colorPitch[0]    = color_pitch;
    sf.colorLocation[1] = sf.colorLocation[2] = sf.colorLocation[3] = GCM_LOCATION_RSX;
    sf.colorPitch[1] = sf.colorPitch[2] = sf.colorPitch[3] = 64;
    sf.depthFormat   = GCM_TF_ZETA_Z16;
    sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset   = depth_offset;
    sf.depthPitch    = depth_pitch;
    sf.type          = GCM_TF_TYPE_LINEAR;
    sf.antiAlias     = GCM_TF_CENTER_1;
    sf.width  = display_width;
    sf.height = display_height;
    rsxSetSurface(context, &sf);
}

static void screen_init(void)
{
    void* host_addr = memalign(1024 * 1024, HOST_SIZE);
    context = rsxInit(CB_SIZE, HOST_SIZE, host_addr);

    videoState state;
    videoGetState(0, 0, &state);
    videoGetResolution(state.displayMode.resolution, &res);

    videoConfiguration vcfg;
    memset(&vcfg, 0, sizeof(vcfg));
    vcfg.resolution = state.displayMode.resolution;
    vcfg.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vcfg.pitch      = res.width * sizeof(u32);

    rsx_wait_idle();
    videoConfigure(0, &vcfg, NULL, 0);
    videoGetState(0, 0, &state);
    gcmSetFlipMode(GCM_FLIP_VSYNC);

    display_width  = res.width;
    display_height = res.height;
    color_pitch    = display_width * sizeof(u32);

    color_buffer[0] = (u32*)rsxMemalign(64, display_height * color_pitch);
    color_buffer[1] = (u32*)rsxMemalign(64, display_height * color_pitch);
    rsxAddressToOffset(color_buffer[0], &color_offset[0]);
    rsxAddressToOffset(color_buffer[1], &color_offset[1]);
    gcmSetDisplayBuffer(0, color_offset[0], color_pitch, display_width, display_height);
    gcmSetDisplayBuffer(1, color_offset[1], color_pitch, display_width, display_height);

    depth_pitch  = display_width * sizeof(u32);
    depth_buffer = (u32*)rsxMemalign(64, display_height * depth_pitch * 2);
    rsxAddressToOffset(depth_buffer, &depth_offset);

    /* Clear both display buffers to black once — the GB image only ever
     * touches a fixed centered rect, so the borders stay black. */
    for (int i = 0; i < 2; ++i)
        memset(color_buffer[i], 0, display_height * color_pitch);

    /* Largest integer scale of 160x144 that fits the display. */
    u32 sx = display_width / GB_W, sy = display_height / GB_H;
    g_scale = (sx < sy ? sx : sy);
    if (g_scale < 1) g_scale = 1;
    g_off_x = (display_width  - GB_W * g_scale) / 2;
    g_off_y = (display_height - GB_H * g_scale) / 2;
}

static void screen_flip(void)
{
    if (!first_fb) {
        while (gcmGetFlipStatus() != 0) usleep(200);
    }
    gcmResetFlipStatus();
    gcmSetFlip(context, curr_fb);
    rsxFlushBuffer(context);
    gcmSetWaitFlip(context);
    curr_fb ^= 1;
    set_render_target(curr_fb);
    first_fb = 0;
}

/* --- Audio (48 kHz float stereo, block ring) --------------------------- */

#define ARING 16384                     /* int16 stereo frames, 44100 in */
static int        g_audio_ok = 0;
static u32        g_aport;
static audioPortConfig g_acfg;
static int16_t    g_aring[ARING * 2];
static volatile int g_aw = 0, g_ar = 0;
static u64        g_block_idx = 1;

static void audio_init(void)
{
    if (audioInit() != 0) return;
    audioPortParam p;
    p.numChannels = AUDIO_PORT_2CH;
    p.numBlocks   = AUDIO_BLOCK_8;
    p.attrib      = 0;
    p.level       = 1.0f;
    if (audioPortOpen(&p, &g_aport) != 0) return;
    if (audioGetPortConfig(g_aport, &g_acfg) != 0) return;
    audioPortStart(g_aport);
    g_audio_ok = 1;
}

/* Fill audio blocks ahead of the hardware read index. Input is 44100 Hz
 * int16; PS3 wants 48000 Hz float — nearest-neighbour resample. */
static void audio_update(void)
{
    if (!g_audio_ok) return;
    u64 hw = *(u64*)(u64)g_acfg.readIndex;
    float* base = (float*)(u64)g_acfg.audioDataStart;

    while (g_block_idx != hw) {
        int avail = (g_aw - g_ar) & (ARING - 1);
        /* need ~AUDIO_BLOCK_SAMPLES*44100/48000 input frames per block */
        if (avail < (int)(AUDIO_BLOCK_SAMPLES)) break;

        float* blk = base + 2 * AUDIO_BLOCK_SAMPLES * g_block_idx;
        for (unsigned i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            int si = (i * 44100) / 48000;
            int rp = (g_ar + si) & (ARING - 1);
            blk[i * 2 + 0] = g_aring[rp * 2 + 0] / 32768.0f;
            blk[i * 2 + 1] = g_aring[rp * 2 + 1] / 32768.0f;
        }
        int consumed = (AUDIO_BLOCK_SAMPLES * 44100) / 48000;
        g_ar = (g_ar + consumed) & (ARING - 1);
        g_block_idx = (g_block_idx + 1) % AUDIO_BLOCK_8;
    }
}

static void xb_on_audio_sample(GBContext* ctx, int16_t left, int16_t right)
{
    (void)ctx;
    int nw = (g_aw + 1) & (ARING - 1);
    if (nw == g_ar) return;
    g_aring[g_aw * 2 + 0] = left;
    g_aring[g_aw * 2 + 1] = right;
    g_aw = nw;
}

/* --- Battery RAM — /dev_hdd0/game/LADX00001/USRDIR/ -------------------- */

#define SAVE_DIR "/dev_hdd0/game/LADX00001/USRDIR"

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
    char path[320];
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
    char path[320];
    save_path(path, sizeof(path), rom, ".sav");
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t put = fwrite(data, 1, size, f);
    fclose(f);
    return put == size;
}

/* --- XMB exit callback -------------------------------------------------- */
static void sysutil_cb(u64 status, u64 param, void* ud)
{
    (void)param; (void)ud;
    if (status == SYSUTIL_EXIT_GAME) g_quit = 1;
}

/* --- Video render ------------------------------------------------------- */

void gb_platform_render_frame(const uint32_t* framebuffer)
{
    if (!framebuffer) return;
    audio_update();

    u32* dst = color_buffer[curr_fb];
    /* integer-scaled software blit, GB ARGB -> PS3 XRGB (alpha ignored) */
    for (int gy = 0; gy < GB_H; ++gy) {
        for (int sy = 0; sy < g_scale; ++sy) {
            u32* row = dst + (g_off_y + gy * g_scale + sy) * display_width + g_off_x;
            const uint32_t* src = framebuffer + gy * GB_W;
            for (int gx = 0; gx < GB_W; ++gx) {
                u32 p = src[gx];
                for (int sx = 0; sx < g_scale; ++sx) *row++ = p;
            }
        }
    }
    screen_flip();
}

/* --- Lifecycle ---------------------------------------------------------- */

bool gb_platform_init(int scale)
{
    (void)scale;
    screen_init();
    set_render_target(curr_fb);
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
    ioPadInit(7);
    audio_init();
    printf("la360: platform_psl1ght init (%ux%u, %dx scale, audio %s)\n",
           display_width, display_height, g_scale, g_audio_ok ? "on" : "off");
    return true;
}

void gb_platform_shutdown(void)
{
    if (g_audio_ok) { audioPortStop(g_aport); audioPortClose(g_aport); audioQuit(); }
    ioPadEnd();
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
    gcmSetWaitFlip(context);
    rsxFinish(context, 1);
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

bool gb_platform_poll_events(GBContext* ctx)
{
    (void)ctx;
    sysUtilCheckCallback();
    if (g_quit) return false;

    padInfo  pi;
    padData  pd;
    uint8_t  btns = 0xFF, dpad = 0xFF;

    ioPadGetInfo(&pi);
    for (int i = 0; i < MAX_PADS; ++i) {
        if (!pi.status[i]) continue;
        if (ioPadGetData(i, &pd) != 0) continue;
        if (pd.BTN_UP)     dpad &= ~0x04;
        if (pd.BTN_DOWN)   dpad &= ~0x08;
        if (pd.BTN_LEFT)   dpad &= ~0x02;
        if (pd.BTN_RIGHT)  dpad &= ~0x01;
        if (pd.BTN_CROSS)  btns &= ~0x01;   /* A */
        if (pd.BTN_CIRCLE) btns &= ~0x02;   /* B */
        if (pd.BTN_SELECT) btns &= ~0x04;
        if (pd.BTN_START)  btns &= ~0x08;
        break;   /* player 1 only */
    }
    g_joypad_buttons = btns;
    g_joypad_dpad    = dpad;
    return true;
}

uint8_t gb_platform_get_joypad(void) { return g_joypad_buttons & g_joypad_dpad; }

void gb_platform_vsync(void)
{
    audio_update();
    /* screen_flip() already waited on flip status with GCM_FLIP_VSYNC */
}

void gb_platform_set_title(const char* title) { (void)title; }

/* --- Save state — /dev_hdd0/game/LADX00001/USRDIR/state.bin ------------ */

#define STATE_PATH    SAVE_DIR "/state.bin"
#define STATE_MAGIC   0x4C413350u   /* "LA3P" */
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

/* --- Debug / dev no-ops ------------------------------------------------- */
void gb_platform_set_input_script(const char* path)        { (void)path; }
void gb_platform_set_dump_frames(const char* dir)          { (void)dir; }
void gb_platform_set_screenshot_prefix(const char* prefix) { (void)prefix; }
