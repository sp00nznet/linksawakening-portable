/**
 * Hardware Trace output for gb-recompiled runtime
 *
 * Produces trace output in the same format as the SameBoy sb_tracer tool,
 * allowing direct comparison of hardware state between reference emulator
 * and recompiled build.
 */

#include "gbrt.h"
#include "ppu.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ============================================================================
 * State
 * ========================================================================== */

static FILE *hwtrace_file = NULL;
static uint32_t hwtrace_frame = 0;

/* ============================================================================
 * CRC32
 * ========================================================================== */

static uint32_t crc32_table[256];
static int crc32_init_done = 0;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_init_done = 1;
}

static uint32_t crc32(const uint8_t *data, size_t len) {
    if (!crc32_init_done) crc32_init();
    uint32_t c = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFF;
}

/* ============================================================================
 * Public API
 * ========================================================================== */

void hwtrace_init(const char *filename) {
    if (hwtrace_file) fclose(hwtrace_file);
    hwtrace_file = fopen(filename, "w");
    if (hwtrace_file) {
        fprintf(hwtrace_file, "# gb-recompiled HW Trace\n");
        fprintf(hwtrace_file, "# Mode: CGB\n");
        fprintf(stderr, "[HWTRACE] Tracing to %s\n", filename);
    }
    hwtrace_frame = 0;
}

void hwtrace_close(void) {
    if (hwtrace_file) {
        fclose(hwtrace_file);
        hwtrace_file = NULL;
    }
}

void hwtrace_scanline(GBContext *ctx, uint8_t line) {
    if (!hwtrace_file) return;
    GBPPU *ppu = (GBPPU *)ctx->ppu;
    if (!ppu) return;

    fprintf(hwtrace_file, "SCANLINE:%d FRAME:%u LCDC=%02X STAT=%02X SCX=%02X SCY=%02X BGP=%02X OBP0=%02X OBP1=%02X WY=%02X WX=%02X",
            line, hwtrace_frame,
            ppu->lcdc, ppu->stat & 0x7F, ppu->scx, ppu->scy,
            ppu->bgp, ppu->obp0, ppu->obp1, ppu->wy, ppu->wx);

    if (ppu->cgb_mode) {
        fprintf(hwtrace_file, " BCPS=%02X OCPS=%02X", ppu->bcps, ppu->ocps);
    }
    fprintf(hwtrace_file, "\n");

    /* Dump CGB palette RAM at scanline 0 */
    if (line == 0 && ppu->cgb_mode) {
        fprintf(hwtrace_file, "BGPAL FRAME:%u CRC=%08X DATA=", hwtrace_frame,
                crc32(ppu->bg_palette_ram, 64));
        for (int i = 0; i < 64; i++) {
            fprintf(hwtrace_file, "%02X", ppu->bg_palette_ram[i]);
        }
        fprintf(hwtrace_file, "\n");

        fprintf(hwtrace_file, "OBJPAL FRAME:%u CRC=%08X DATA=", hwtrace_frame,
                crc32(ppu->obj_palette_ram, 64));
        for (int i = 0; i < 64; i++) {
            fprintf(hwtrace_file, "%02X", ppu->obj_palette_ram[i]);
        }
        fprintf(hwtrace_file, "\n");
    }
}

void hwtrace_vblank(GBContext *ctx) {
    if (!hwtrace_file) return;
    GBPPU *ppu = (GBPPU *)ctx->ppu;

    /* OAM CRC */
    uint32_t oam_crc = ctx->oam ? crc32(ctx->oam, 0xA0) : 0;

    /* VRAM bank 0: tilemap and tile data CRCs */
    uint32_t tmap0_crc = 0, tmap1_crc = 0, tdata_crc = 0, vram1_crc = 0;
    if (ctx->vram) {
        tmap0_crc = crc32(ctx->vram + 0x1800, 0x400);   /* 9800-9BFF */
        tmap1_crc = crc32(ctx->vram + 0x1C00, 0x400);   /* 9C00-9FFF */
        tdata_crc = crc32(ctx->vram, 0x1800);            /* 8000-97FF */
        vram1_crc = crc32(ctx->vram + 0x2000, 0x2000);   /* VRAM bank 1 */
    }

    /* Framebuffer CRC */
    uint32_t fb_crc = ppu ? crc32((uint8_t *)ppu->rgb_framebuffer, sizeof(ppu->rgb_framebuffer)) : 0;

    fprintf(hwtrace_file, "VBLANK FRAME:%u OAM_CRC=%08X TMAP0_CRC=%08X TMAP1_CRC=%08X TDATA_CRC=%08X VRAM1_CRC=%08X FB_CRC=%08X\n",
            hwtrace_frame, oam_crc, tmap0_crc, tmap1_crc, tdata_crc, vram1_crc, fb_crc);

    /* WRAM CRC for game state comparison */
    uint32_t wram_crc = ctx->wram ? crc32(ctx->wram, 0x8000) : 0;

    /* CPU registers + DIV + WRAM CRC */
    gb_pack_flags(ctx);
    fprintf(hwtrace_file, "REGS FRAME:%u AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X PC=%04X BANK=%d DIV=%04X WRAM=%08X\n",
            hwtrace_frame, ctx->af, ctx->bc, ctx->de, ctx->hl, ctx->sp, ctx->pc, ctx->rom_bank,
            ctx->div_counter, wram_crc);

    hwtrace_frame++;
}

bool hwtrace_active(void) {
    return hwtrace_file != NULL;
}

uint32_t hwtrace_frame_count(void) {
    return hwtrace_frame;
}
