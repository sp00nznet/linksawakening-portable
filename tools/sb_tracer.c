/**
 * SameBoy Hardware Tracer
 *
 * Runs LA DX in SameBoy (headless) and captures per-frame hardware state:
 * - Per-scanline: PPU registers, CGB palette state
 * - Per-VBlank: OAM/VRAM checksums, framebuffer hash
 * - IO register writes (via instrumented memory.c callback)
 *
 * Output format is designed to be diffed against matching output from
 * the gb-recompiled runtime.
 */

#define GB_INTERNAL
#include <Core/gb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ============================================================================
 * Configuration
 * ========================================================================== */

static const char *rom_path = NULL;
static const char *trace_path = "ref_trace.log";
static int max_frames = 300;
static FILE *trace_file = NULL;
static unsigned int frame_count = 0;

/* Framebuffer for SameBoy pixel output */
static uint32_t screen_buffer[160 * 144];

/* ============================================================================
 * CRC32 for checksumming memory regions
 * ========================================================================== */

static uint32_t crc32_table[256];
static bool crc32_table_init = false;

static void init_crc32(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_init = true;
}

static uint32_t crc32(const uint8_t *data, size_t len) {
    if (!crc32_table_init) init_crc32();
    uint32_t c = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFF;
}

/* ============================================================================
 * RGB Encode callback (SameBoy requires this)
 * ========================================================================== */

static uint32_t rgb_encode(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b) {
    (void)gb;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

/* ============================================================================
 * Per-Scanline Callback
 * ========================================================================== */

static void lcd_line_callback(GB_gameboy_t *gb, uint8_t line) {
    if (!trace_file) return;

    /* Dump PPU register state at each visible scanline */
    fprintf(trace_file, "SCANLINE:%d FRAME:%u LCDC=%02X STAT=%02X SCX=%02X SCY=%02X BGP=%02X OBP0=%02X OBP1=%02X WY=%02X WX=%02X",
            line, frame_count,
            gb->io_registers[GB_IO_LCDC],
            gb->io_registers[GB_IO_STAT],
            gb->io_registers[GB_IO_SCX],
            gb->io_registers[GB_IO_SCY],
            gb->io_registers[GB_IO_BGP],
            gb->io_registers[GB_IO_OBP0],
            gb->io_registers[GB_IO_OBP1],
            gb->io_registers[GB_IO_WY],
            gb->io_registers[GB_IO_WX]);

    /* CGB palette index registers */
    if (GB_is_cgb(gb)) {
        fprintf(trace_file, " BCPS=%02X OCPS=%02X",
                gb->io_registers[GB_IO_BGPI],
                gb->io_registers[GB_IO_OBPI]);
    }

    fprintf(trace_file, "\n");

    /* At scanline 0, dump CGB palette RAM (start of frame) */
    if (line == 0 && GB_is_cgb(gb)) {
        size_t bg_pal_size = 0, obj_pal_size = 0;
        uint16_t bg_bank = 0, obj_bank = 0;
        uint8_t *bg_pal = GB_get_direct_access(gb, GB_DIRECT_ACCESS_BGP, &bg_pal_size, &bg_bank);
        uint8_t *obj_pal = GB_get_direct_access(gb, GB_DIRECT_ACCESS_OBP, &obj_pal_size, &obj_bank);

        if (bg_pal && bg_pal_size >= 64) {
            fprintf(trace_file, "BGPAL FRAME:%u CRC=%08X", frame_count, crc32(bg_pal, 64));
            /* Dump first 4 palettes (32 bytes) in hex for detailed comparison */
            fprintf(trace_file, " DATA=");
            for (int i = 0; i < 64; i++) {
                fprintf(trace_file, "%02X", bg_pal[i]);
            }
            fprintf(trace_file, "\n");
        }
        if (obj_pal && obj_pal_size >= 64) {
            fprintf(trace_file, "OBJPAL FRAME:%u CRC=%08X", frame_count, crc32(obj_pal, 64));
            fprintf(trace_file, " DATA=");
            for (int i = 0; i < 64; i++) {
                fprintf(trace_file, "%02X", obj_pal[i]);
            }
            fprintf(trace_file, "\n");
        }
    }
}

/* ============================================================================
 * VBlank Callback
 * ========================================================================== */

static void vblank_callback(GB_gameboy_t *gb, GB_vblank_type_t type) {
    (void)type;
    if (!trace_file) return;

    /* Get OAM */
    size_t oam_size = 0;
    uint16_t oam_bank = 0;
    uint8_t *oam = GB_get_direct_access(gb, GB_DIRECT_ACCESS_OAM, &oam_size, &oam_bank);

    /* Get VRAM */
    size_t vram_size = 0;
    uint16_t vram_bank = 0;
    uint8_t *vram = GB_get_direct_access(gb, GB_DIRECT_ACCESS_VRAM, &vram_size, &vram_bank);

    uint32_t oam_crc = oam ? crc32(oam, oam_size < 0xA0 ? oam_size : 0xA0) : 0;

    /* Tilemap CRCs (0x9800-0x9BFF and 0x9C00-0x9FFF relative to VRAM start) */
    uint32_t tmap0_crc = 0, tmap1_crc = 0, tdata_crc = 0;
    if (vram && vram_size >= 0x2000) {
        tmap0_crc = crc32(vram + 0x1800, 0x400);  /* 9800-9BFF */
        tmap1_crc = crc32(vram + 0x1C00, 0x400);  /* 9C00-9FFF */
        tdata_crc = crc32(vram, 0x1800);           /* 8000-97FF tile data */
    }

    /* VRAM bank 1 CRC (CGB only) */
    uint32_t vram1_crc = 0;
    if (vram && vram_size >= 0x4000) {
        vram1_crc = crc32(vram + 0x2000, 0x2000);
    }

    /* Framebuffer CRC */
    uint32_t fb_crc = crc32((uint8_t*)screen_buffer, sizeof(screen_buffer));

    fprintf(trace_file, "VBLANK FRAME:%u OAM_CRC=%08X TMAP0_CRC=%08X TMAP1_CRC=%08X TDATA_CRC=%08X VRAM1_CRC=%08X FB_CRC=%08X\n",
            frame_count, oam_crc, tmap0_crc, tmap1_crc, tdata_crc, vram1_crc, fb_crc);

    /* WRAM CRC */
    size_t wram_size = 0;
    uint16_t wram_bank = 0;
    uint8_t *wram = GB_get_direct_access(gb, GB_DIRECT_ACCESS_RAM, &wram_size, &wram_bank);
    uint32_t wram_crc = wram ? crc32(wram, wram_size < 0x8000 ? wram_size : 0x8000) : 0;

    /* CPU register state at VBlank + DIV + WRAM */
    fprintf(trace_file, "REGS FRAME:%u AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X PC=%04X BANK=%d DIV=%04X WRAM=%08X\n",
            frame_count,
            gb->registers[GB_REGISTER_AF],
            gb->registers[GB_REGISTER_BC],
            gb->registers[GB_REGISTER_DE],
            gb->registers[GB_REGISTER_HL],
            gb->registers[GB_REGISTER_SP],
            gb->registers[GB_REGISTER_PC],
            gb->mbc_rom_bank,
            gb->div_counter,
            wram_crc);

    frame_count++;
    if ((int)frame_count >= max_frames) {
        fprintf(stderr, "[TRACER] Reached %d frames, stopping\n", max_frames);
        fclose(trace_file);
        trace_file = NULL;
        exit(0);
    }
}

/* ============================================================================
 * Log Callback (suppress SameBoy debug output)
 * ========================================================================== */

static void log_callback(GB_gameboy_t *gb, const char *string, GB_log_attributes_t attributes) {
    (void)gb; (void)attributes;
    /* Only print warnings/errors to stderr */
    if (strstr(string, "error") || strstr(string, "warn")) {
        fprintf(stderr, "[SB] %s", string);
    }
}

/* ============================================================================
 * Main
 * ========================================================================== */

int main(int argc, char *argv[]) {
    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            rom_path = argv[++i];
        } else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: sb_tracer --rom <rom.gbc> [--trace <output.log>] [--frames <N>]\n");
            return 0;
        }
    }

    if (!rom_path) {
        fprintf(stderr, "Error: --rom <path> is required\n");
        return 1;
    }

    /* Open trace file */
    trace_file = fopen(trace_path, "w");
    if (!trace_file) {
        fprintf(stderr, "Error: Cannot open trace file '%s'\n", trace_path);
        return 1;
    }

    /* Initialize SameBoy as CGB */
    GB_gameboy_t gb;
    GB_init(&gb, GB_MODEL_CGB_E);

    /* Set callbacks */
    GB_set_vblank_callback(&gb, vblank_callback);
    GB_set_lcd_line_callback(&gb, lcd_line_callback);
    GB_set_pixels_output(&gb, screen_buffer);
    GB_set_rgb_encode_callback(&gb, rgb_encode);
    GB_set_log_callback(&gb, log_callback);

    /* Load ROM */
    if (GB_load_rom(&gb, rom_path)) {
        fprintf(stderr, "Error: Failed to load ROM '%s'\n", rom_path);
        GB_free(&gb);
        return 1;
    }

    /* Skip boot ROM - set post-bootrom state manually */
    /* Write 1 to FF50 to mark boot ROM as finished */
    gb.boot_rom_finished = true;
    /* Set CGB post-bootrom register values */
    gb.registers[GB_REGISTER_AF] = 0x1180;
    gb.registers[GB_REGISTER_BC] = 0x0000;
    gb.registers[GB_REGISTER_DE] = 0xFF56;
    gb.registers[GB_REGISTER_HL] = 0x000D;
    gb.registers[GB_REGISTER_SP] = 0xFFFE;
    gb.registers[GB_REGISTER_PC] = 0x0100;
    /* Set post-bootrom IO registers */
    gb.io_registers[GB_IO_LCDC] = 0x91;
    gb.io_registers[GB_IO_BGP] = 0xFC;
    gb.io_registers[GB_IO_STAT] = 0x01; /* Mode 1 (VBlank) */

    fprintf(stderr, "[TRACER] Loaded ROM: %s\n", rom_path);
    fprintf(stderr, "[TRACER] Tracing %d frames to %s\n", max_frames, trace_path);

    /* Write trace header */
    fprintf(trace_file, "# SameBoy HW Trace - %s\n", rom_path);
    fprintf(trace_file, "# Model: CGB-E\n");
    fprintf(trace_file, "# Frames: %d\n", max_frames);

    /* Run frames */
    for (int f = 0; f < max_frames; f++) {
        GB_run_frame(&gb);
    }

    fprintf(stderr, "[TRACER] Done - %u frames traced\n", frame_count);

    if (trace_file) fclose(trace_file);
    GB_free(&gb);
    return 0;
}
