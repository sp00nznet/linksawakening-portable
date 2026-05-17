/**
 * @file ppu.c
 * @brief GameBoy PPU (Pixel Processing Unit) implementation
 */

#include "ppu.h"
#include "gbrt.h"
#include "gbrt_debug.h"
#include "hwtrace.h"
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Default Color Palette (DMG green shades)
 * ========================================================================== */

static const uint32_t dmg_palette[4] = {
    0xFFE0F8D0,  /* Lightest (white) - RGBA */
    0xFF88C070,  /* Light */
    0xFF346856,  /* Dark */
    0xFF081820,  /* Darkest (black) */
};

/**
 * @brief Convert CGB RGB555 to ARGB8888
 */
static inline uint32_t rgb555_to_argb(uint16_t rgb555) {
    uint8_t r5 = rgb555 & 0x1F;
    uint8_t g5 = (rgb555 >> 5) & 0x1F;
    uint8_t b5 = (rgb555 >> 10) & 0x1F;
    /* Scale 5-bit to 8-bit: (x << 3) | (x >> 2) */
    uint8_t r = (r5 << 3) | (r5 >> 2);
    uint8_t g = (g5 << 3) | (g5 >> 2);
    uint8_t b = (b5 << 3) | (b5 >> 2);
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

/**
 * @brief Get a color from CGB palette RAM
 */
static inline uint32_t cgb_palette_color(const uint8_t* palette_ram, uint8_t palette_num, uint8_t color_idx) {
    int offset = palette_num * 8 + color_idx * 2;
    uint16_t rgb555 = palette_ram[offset] | (palette_ram[offset + 1] << 8);
    return rgb555_to_argb(rgb555);
}

/* ============================================================================
 * PPU Initialization
 * ========================================================================== */

void ppu_init(GBPPU* ppu) {
    memset(ppu, 0, sizeof(GBPPU));
    ppu_reset(ppu);
    DBG_PPU("PPU initialized");
}

void ppu_reset(GBPPU* ppu) {
    /* LCD Registers - post-bootrom state */
    ppu->lcdc = 0x91;  /* LCD on, BG on, tiles at 0x8000 */
    ppu->stat = 0x00;
    ppu->scy = 0;
    ppu->scx = 0;
    ppu->ly = 0;
    ppu->lyc = 0;
    ppu->dma = 0;
    ppu->bgp = 0xFC;   /* 11 11 11 00 */
    ppu->obp0 = 0x00;  /* CGB post-bootrom value */
    ppu->obp1 = 0x00;  /* CGB post-bootrom value */
    ppu->wy = 0;
    ppu->wx = 0;

    /* CGB palette registers */
    ppu->bcps = 0;
    ppu->ocps = 0;
    memset(ppu->bg_palette_ram, 0xFF, sizeof(ppu->bg_palette_ram));
    memset(ppu->obj_palette_ram, 0xFF, sizeof(ppu->obj_palette_ram));

    /* HDMA state */
    ppu->hdma_src = 0;
    ppu->hdma_dst = 0;
    ppu->hdma_len = 0xFF;
    ppu->hdma_active = false;

    /* CGB mode - will be set by context init */
    ppu->cgb_mode = true;  /* Default to CGB since we set A=0x11 */

    /* Internal state - start in VBlank mode at LY=144 to match SameBoy's
     * post-bootrom state (STAT=0x01 = VBlank). The first gb_run_frame will
     * complete the VBlank period, render a full frame (LY 0-143), then
     * trigger frame_done at the next VBlank. This ensures frame 0 is a
     * complete visible frame, matching SameBoy's tracing. */
    ppu->mode = PPU_MODE_VBLANK;
    ppu->mode_cycles = 0;
    ppu->ly = 144;
    ppu->window_line = 0;
    ppu->window_triggered = false;
    ppu->frame_ready = false;

    /* Clear framebuffers */
    memset(ppu->framebuffer, 0, sizeof(ppu->framebuffer));
    for (int i = 0; i < GB_FRAMEBUFFER_SIZE; i++) {
        ppu->rgb_framebuffer[i] = dmg_palette[0];
    }

    DBG_PPU("PPU reset - LCDC=0x%02X, BGP=0x%02X, mode=%s, CGB=%d",
            ppu->lcdc, ppu->bgp, ppu_mode_name(ppu->mode), ppu->cgb_mode);
}

/* ============================================================================
 * Tile Fetching
 * ========================================================================== */

/**
 * @brief Get tile data address for a tile index
 */
static uint16_t get_tile_data_addr(GBPPU* ppu, uint8_t tile_idx, bool is_obj) {
    if (is_obj || (ppu->lcdc & LCDC_TILE_DATA)) {
        /* 8000 addressing mode - unsigned indexing */
        return 0x8000 + (tile_idx * 16);
    } else {
        /* 8800 addressing mode - signed indexing from 0x9000 */
        return 0x9000 + ((int8_t)tile_idx * 16);
    }
}

/**
 * @brief Get tile map address
 */
static uint16_t get_bg_tilemap_addr(GBPPU* ppu) {
    return (ppu->lcdc & LCDC_BG_TILEMAP) ? 0x9C00 : 0x9800;
}

static uint16_t get_window_tilemap_addr(GBPPU* ppu) {
    return (ppu->lcdc & LCDC_WINDOW_TILEMAP) ? 0x9C00 : 0x9800;
}

/**
 * @brief Read a byte from VRAM bank 0
 */
static uint8_t vram_read(GBContext* ctx, uint16_t addr) {
    if (addr >= 0x8000 && addr <= 0x9FFF) {
        return ctx->vram[addr - 0x8000];
    }
    return 0xFF;
}

/**
 * @brief Read a byte from a specific VRAM bank (CGB)
 */
static uint8_t vram_read_bank(GBContext* ctx, uint16_t addr, uint8_t bank) {
    if (addr >= 0x8000 && addr <= 0x9FFF) {
        return ctx->vram[(bank * 0x2000) + (addr - 0x8000)];
    }
    return 0xFF;
}

/* ============================================================================
 * Scanline Rendering
 * ========================================================================== */

/**
 * @brief Apply palette to a 2-bit color value
 */
static uint8_t apply_palette(uint8_t color, uint8_t palette) {
    return (palette >> (color * 2)) & 0x03;
}

/**
 * @brief Render background/window for current scanline (DMG + CGB)
 */
static void render_bg_scanline(GBPPU* ppu, GBContext* ctx, uint8_t* bg_prio) {
    uint8_t scanline = ppu->ly;

    if (!(ppu->lcdc & LCDC_LCD_ENABLE)) {
        memset(&ppu->framebuffer[scanline * GB_SCREEN_WIDTH], 0, GB_SCREEN_WIDTH);
        if (bg_prio) memset(bg_prio, 0, GB_SCREEN_WIDTH);
        return;
    }

    bool bg_enable = ppu->cgb_mode ? true : (ppu->lcdc & LCDC_BG_ENABLE);
    bool window_enable = (ppu->lcdc & LCDC_WINDOW_ENABLE) && (ppu->wx <= 166) && (ppu->wy <= scanline);
    if (!ppu->cgb_mode) window_enable = window_enable && (ppu->lcdc & LCDC_BG_ENABLE);

    if (window_enable && !ppu->window_triggered) {
        ppu->window_triggered = true;
    }

    int fb_base = scanline * GB_SCREEN_WIDTH;

    for (int x = 0; x < GB_SCREEN_WIDTH; x++) {
        uint8_t color = 0;
        uint8_t cgb_palette = 0;
        bool bg_priority = false;

        bool in_window = window_enable && (x >= (ppu->wx - 7));

        int map_x, map_y;
        uint16_t tilemap_addr;

        if (in_window) {
            map_x = x - (ppu->wx - 7);
            map_y = ppu->window_line;
            tilemap_addr = get_window_tilemap_addr(ppu);
        } else if (bg_enable) {
            map_x = (x + ppu->scx) & 0xFF;
            map_y = (scanline + ppu->scy) & 0xFF;
            tilemap_addr = get_bg_tilemap_addr(ppu);
        } else {
            /* BG disabled (DMG only) */
            ppu->framebuffer[fb_base + x] = 0;
            if (ppu->cgb_mode) {
                ppu->rgb_framebuffer[fb_base + x] = cgb_palette_color(ppu->bg_palette_ram, 0, 0);
            }
            if (bg_prio) bg_prio[x] = 0;
            continue;
        }

        uint8_t tile_col = map_x / 8;
        uint8_t tile_row = map_y / 8;
        uint16_t map_offset = tilemap_addr + tile_row * 32 + tile_col;

        /* Read tile index from VRAM bank 0 */
        uint8_t tile_idx = vram_read_bank(ctx, map_offset, 0);

        /* CGB: read attributes from VRAM bank 1 */
        uint8_t attrs = 0;
        uint8_t tile_vram_bank = 0;
        bool flip_x = false, flip_y = false;

        if (ppu->cgb_mode) {
            attrs = vram_read_bank(ctx, map_offset, 1);
            cgb_palette = attrs & BG_ATTR_PALETTE;
            tile_vram_bank = (attrs & BG_ATTR_VRAM_BANK) ? 1 : 0;
            flip_x = (attrs & BG_ATTR_FLIP_X) != 0;
            flip_y = (attrs & BG_ATTR_FLIP_Y) != 0;
            bg_priority = (attrs & BG_ATTR_PRIORITY) != 0;
        }

        /* Get pixel from tile */
        uint16_t tile_addr = get_tile_data_addr(ppu, tile_idx, false);
        uint8_t pixel_y = map_y % 8;
        uint8_t pixel_x = map_x % 8;

        if (flip_y) pixel_y = 7 - pixel_y;
        if (flip_x) pixel_x = 7 - pixel_x;

        uint8_t lo = vram_read_bank(ctx, tile_addr + pixel_y * 2, tile_vram_bank);
        uint8_t hi = vram_read_bank(ctx, tile_addr + pixel_y * 2 + 1, tile_vram_bank);

        uint8_t bit = 7 - pixel_x;
        color = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);

        if (ppu->cgb_mode) {
            /* CGB: store raw color for priority, write RGB directly */
            ppu->framebuffer[fb_base + x] = color;
            ppu->rgb_framebuffer[fb_base + x] = cgb_palette_color(ppu->bg_palette_ram, cgb_palette, color);
        } else {
            /* DMG: apply palette */
            ppu->framebuffer[fb_base + x] = apply_palette(color, ppu->bgp);
        }
        if (bg_prio) bg_prio[x] = bg_priority ? (color | 0x80) : color;
    }

    if (ppu->window_triggered && window_enable) {
        ppu->window_line++;
    }
}

/**
 * @brief Render sprites for current scanline (DMG + CGB)
 */
static void render_sprites_scanline(GBPPU* ppu, GBContext* ctx, const uint8_t* bg_prio) {
    if (!(ppu->lcdc & LCDC_OBJ_ENABLE)) {
        return;
    }

    uint8_t scanline = ppu->ly;
    uint8_t sprite_height = (ppu->lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
    int fb_base = scanline * GB_SCREEN_WIDTH;

    /* Find sprites on this scanline (max 10) */
    int sprite_count = 0;
    int sprites[10];

    for (int i = 0; i < 40 && sprite_count < 10; i++) {
        OAMEntry* sprite = (OAMEntry*)(ctx->oam + i * 4);
        int sprite_y = sprite->y - 16;

        if (scanline >= sprite_y && scanline < sprite_y + sprite_height) {
            sprites[sprite_count++] = i;
        }
    }

    /* Render sprites in reverse order (lower index = higher priority, drawn last) */
    for (int i = sprite_count - 1; i >= 0; i--) {
        OAMEntry* sprite = (OAMEntry*)(ctx->oam + sprites[i] * 4);
        int sprite_y = sprite->y - 16;
        int sprite_x = sprite->x - 8;

        uint8_t tile_idx = sprite->tile;
        if (sprite_height == 16) {
            tile_idx &= 0xFE;
        }

        int line = scanline - sprite_y;
        if (sprite->flags & OAM_FLIP_Y) {
            line = sprite_height - 1 - line;
        }

        /* CGB: sprites can use VRAM bank 0 or 1 */
        uint8_t tile_bank = 0;
        if (ppu->cgb_mode) {
            tile_bank = (sprite->flags & OAM_CGB_BANK) ? 1 : 0;
        }

        uint16_t tile_addr = 0x8000 + tile_idx * 16 + line * 2;
        uint8_t lo = vram_read_bank(ctx, tile_addr, tile_bank);
        uint8_t hi = vram_read_bank(ctx, tile_addr + 1, tile_bank);

        bool behind_bg = (sprite->flags & OAM_PRIORITY);

        for (int px = 0; px < 8; px++) {
            int screen_x = sprite_x + px;
            if (screen_x < 0 || screen_x >= GB_SCREEN_WIDTH) continue;

            int bit_pos = (sprite->flags & OAM_FLIP_X) ? px : (7 - px);
            uint8_t color = ((lo >> bit_pos) & 1) | (((hi >> bit_pos) & 1) << 1);

            if (color == 0) continue;  /* Transparent */

            /* Priority check */
            uint8_t bg_raw = bg_prio ? bg_prio[screen_x] : 0;
            if (ppu->cgb_mode) {
                /* CGB priority: BG priority attribute (bit 7 in bg_prio) or OAM priority */
                bool bg_has_priority = (bg_raw & 0x80) != 0;
                uint8_t bg_color = bg_raw & 0x03;
                /* If LCDC bit 0 is clear, BG/WIN are always behind OBJ */
                if (ppu->lcdc & LCDC_BG_ENABLE) {
                    if ((behind_bg || bg_has_priority) && bg_color != 0) continue;
                }
                uint8_t obj_palette = sprite->flags & OAM_CGB_PALETTE;
                ppu->framebuffer[fb_base + screen_x] = color;
                ppu->rgb_framebuffer[fb_base + screen_x] = cgb_palette_color(ppu->obj_palette_ram, obj_palette, color);
            } else {
                /* DMG priority */
                if (behind_bg && (bg_raw & 0x03) != 0) continue;
                uint8_t palette = (sprite->flags & OAM_PALETTE) ? ppu->obp1 : ppu->obp0;
                ppu->framebuffer[fb_base + screen_x] = apply_palette(color, palette);
            }
        }
    }
}

void ppu_render_scanline(GBPPU* ppu, GBContext* ctx) {
    uint8_t bg_prio[GB_SCREEN_WIDTH];
    memset(bg_prio, 0, sizeof(bg_prio));

    render_bg_scanline(ppu, ctx, bg_prio);
    render_sprites_scanline(ppu, ctx, bg_prio);
}

/**
 * @brief Convert framebuffer to RGB (DMG only; CGB writes RGB directly)
 */
static void convert_to_rgb(GBPPU* ppu) {
    if (ppu->cgb_mode) {
        /* CGB: rgb_framebuffer is already populated during scanline rendering */
        return;
    }

    for (int i = 0; i < GB_FRAMEBUFFER_SIZE; i++) {
        ppu->rgb_framebuffer[i] = dmg_palette[ppu->framebuffer[i] & 0x03];
    }
}

/* ============================================================================
 * PPU Mode State Machine
 * ========================================================================== */

/**
 * @brief Update STAT register mode bits
 */
static void update_stat(GBPPU* ppu, GBContext* ctx) {
    ppu->stat = (ppu->stat & ~STAT_MODE_MASK) | ppu->mode;
    
    /* Update LY=LYC flag */
    if (ppu->ly == ppu->lyc) {
        ppu->stat |= STAT_LYC_MATCH;
    } else {
        ppu->stat &= ~STAT_LYC_MATCH;
    }
    
    /* Write back to I/O */
    ctx->io[0x41] = ppu->stat;
    ctx->io[0x44] = ppu->ly;
}

/**
 * @brief Request LCD STAT interrupt if conditions met
 */
static void check_stat_interrupt(GBPPU* ppu, GBContext* ctx) {
    bool current_state = false;
    
    if ((ppu->stat & STAT_HBLANK_INT) && ppu->mode == PPU_MODE_HBLANK) {
        current_state = true;
    }
    if ((ppu->stat & STAT_VBLANK_INT) && ppu->mode == PPU_MODE_VBLANK) {
        current_state = true;
    }
    if ((ppu->stat & STAT_OAM_INT) && ppu->mode == PPU_MODE_OAM) {
        current_state = true;
    }
    if ((ppu->stat & STAT_LYC_INT) && (ppu->stat & STAT_LYC_MATCH)) {
        current_state = true;
    }
    
    /* Edge detection: only fire on rising edge */
    if (current_state && !ppu->stat_irq_state) {
        /* Request LCD STAT interrupt (IF bit 1) */
        /* DISABLE STAT INTERRUPTS FOR DEBUGGING TETRIS */
        ctx->io[0x0F] |= 0x02;
        // fprintf(stderr, "[PPU] STAT Interrupt Fired! Mode=%d STAT=0x%02X LY=%d LYC=%d\n", ppu->mode, ppu->stat, ppu->ly, ppu->lyc);
    }
    
    ppu->stat_irq_state = current_state;
}



void ppu_tick(GBPPU* ppu, GBContext* ctx, uint32_t cycles) {
    if (!(ppu->lcdc & LCDC_LCD_ENABLE)) {
        return;  /* LCD disabled */
    }
    
    ppu->mode_cycles += cycles;
    
    switch (ppu->mode) {
        case PPU_MODE_OAM:
            if (ppu->mode_cycles >= CYCLES_OAM_SCAN) {
                ppu->mode_cycles -= CYCLES_OAM_SCAN;
                ppu->mode = PPU_MODE_DRAW;
                update_stat(ppu, ctx);
            }
            break;
            
        case PPU_MODE_DRAW:
            if (ppu->mode_cycles >= CYCLES_PIXEL_DRAW) {
                ppu->mode_cycles -= CYCLES_PIXEL_DRAW;
                
                /* Render the scanline */
                ppu_render_scanline(ppu, ctx);
                
                ppu->mode = PPU_MODE_HBLANK;

                /* CGB HDMA: transfer 16 bytes per HBlank */
                if (ppu->hdma_active) {
                    uint16_t src = ppu->hdma_src;
                    uint16_t dst = 0x8000 | ppu->hdma_dst;
                    for (int i = 0; i < 16; i++) {
                        uint8_t byte = gb_read8(ctx, src + i);
                        ctx->vram[(ctx->vram_bank * 0x2000) + ((dst + i) & 0x1FFF)] = byte;
                    }
                    ppu->hdma_src += 16;
                    ppu->hdma_dst = (ppu->hdma_dst + 16) & 0x1FFF;
                    if (ppu->hdma_len == 0) {
                        ppu->hdma_active = false;
                        ppu->hdma_len = 0xFF;
                    } else {
                        ppu->hdma_len--;
                    }
                }

                update_stat(ppu, ctx);

                /* Hardware trace: dump after STAT updated (HBlank mode), matches SameBoy lcd_line_callback */
                hwtrace_scanline(ctx, ppu->ly);

                check_stat_interrupt(ppu, ctx);
            }
            break;

        case PPU_MODE_HBLANK:
            if (ppu->mode_cycles >= CYCLES_HBLANK) {
                ppu->mode_cycles -= CYCLES_HBLANK;
                ppu->ly++;
                
                if (ppu->ly >= VISIBLE_SCANLINES) {
                    /* Enter VBlank */
                    ppu->mode = PPU_MODE_VBLANK;
                    
                    /* Convert framebuffer to RGB - only if not already ready */
                    if (!ppu->frame_ready) {
                        convert_to_rgb(ppu);
                        ppu->frame_ready = true;
                        ctx->frame_done = 1;
                    }
                    
                    /* Hardware trace: VBlank state dump */
                    hwtrace_vblank(ctx);

                    /* Request VBlank interrupt (IF bit 0) */
                    ctx->io[0x0F] |= 0x01;
                } else {
                    ppu->mode = PPU_MODE_OAM;
                }
                
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx);
            }
            break;
            
        case PPU_MODE_VBLANK:
            if (ppu->mode_cycles >= CYCLES_SCANLINE) {
                ppu->mode_cycles -= CYCLES_SCANLINE;
                ppu->ly++;
                
                if (ppu->ly >= TOTAL_SCANLINES) {
                    /* Frame complete - start new frame */
                    ppu->ly = 0;
                    ppu->window_line = 0;
                    ppu->window_triggered = false;
                    ppu->mode = PPU_MODE_OAM;
                }
                
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx);
            }
            break;
    }
}

/* ============================================================================
 * Register Access
 * ========================================================================== */

uint8_t ppu_read_register(GBPPU* ppu, uint16_t addr) {
    switch (addr) {
        case 0xFF40: return ppu->lcdc;
        case 0xFF41: return ppu->stat | 0x80;
        case 0xFF42: return ppu->scy;
        case 0xFF43: return ppu->scx;
        case 0xFF44: return ppu->ly;
        case 0xFF45: return ppu->lyc;
        case 0xFF46: return ppu->dma;
        case 0xFF47: return ppu->bgp;
        case 0xFF48: return ppu->obp0;
        case 0xFF49: return ppu->obp1;
        case 0xFF4A: return ppu->wy;
        case 0xFF4B: return ppu->wx;
        /* CGB palette registers */
        case 0xFF68: return ppu->bcps;
        case 0xFF69: return ppu->bg_palette_ram[ppu->bcps & 0x3F];
        case 0xFF6A: return ppu->ocps;
        case 0xFF6B: return ppu->obj_palette_ram[ppu->ocps & 0x3F];
        default: return 0xFF;
    }
}

void ppu_write_register(GBPPU* ppu, GBContext* ctx, uint16_t addr, uint8_t value) {
    static int ppu_write_count = 0;
    ppu_write_count++;
    
    /* Only log first 100 and special values */
    if (ppu_write_count <= 100 || (addr == 0xFF40 && (value == 0x91 || value == 0x00))) {
        DBG_REGS("PPU write #%d: addr=0x%04X value=0x%02X (A=0x%02X)", 
                 ppu_write_count, addr, value, ctx->a);
    }
    
    switch (addr) {
        case 0xFF40:
            DBG_REGS("LCDC: 0x%02X -> 0x%02X (LCD=%s, BG=%s, OBJ=%s)", 
                     ppu->lcdc, value,
                     (value & 0x80) ? "ON" : "OFF",
                     (value & 0x01) ? "ON" : "OFF", 
                     (value & 0x02) ? "ON" : "OFF");
            /* Check if LCD is being turned off */
            if ((ppu->lcdc & LCDC_LCD_ENABLE) && !(value & LCDC_LCD_ENABLE)) {
                /* LCD turned off - reset to line 0 */
                ppu->ly = 0;
                ppu->window_line = 0;
                ppu->window_triggered = false;
                ppu->mode = PPU_MODE_HBLANK; /* Mode 0 */
                ppu->mode_cycles = 0;
                ctx->io[0x44] = 0;
                /* Clear frame ready to avoid stale frame rendering */
                ppu->frame_ready = false;
                DBG_REGS("LCD turned OFF - reset LY to 0");
            }
            ppu->lcdc = value;
            break;
        case 0xFF41:
            /* Bits 0-2 are read-only */
            ppu->stat = (ppu->stat & 0x07) | (value & 0x78);
            break;
        case 0xFF42: ppu->scy = value; break;
        case 0xFF43: ppu->scx = value; break;
        case 0xFF45: 
            ppu->lyc = value;
            /* Immediately check for LYC match */
            if (ppu->lcdc & LCDC_LCD_ENABLE) {
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx);
            }
            break;
        case 0xFF46:
            /* OAM DMA transfer */
            DBG_REGS("DMA transfer from 0x%04X", (uint16_t)(value << 8));
            ppu->dma = value;
            {
                uint16_t src = value << 8;
                for (int i = 0; i < OAM_SIZE; i++) {
                    ctx->oam[i] = gb_read8(ctx, src + i);
                }
            }
            break;
        case 0xFF47: 
            DBG_REGS("BGP palette: 0x%02X -> 0x%02X", ppu->bgp, value);
            /* BGP uniform color logging disabled */
            ppu->bgp = value; 
            break;
        case 0xFF48: ppu->obp0 = value; break;
        case 0xFF49: ppu->obp1 = value; break;
        case 0xFF4A: ppu->wy = value; break;
        case 0xFF4B: ppu->wx = value; break;
        /* CGB palette registers */
        case 0xFF68: ppu->bcps = value; break;
        case 0xFF69: {
            uint8_t idx = ppu->bcps & 0x3F;
            ppu->bg_palette_ram[idx] = value;
            if (ppu->bcps & 0x80) { /* Auto-increment, bit 6 can overflow */
                ppu->bcps = 0x80 | (((ppu->bcps & 0x7F) + 1) & 0x7F);
            }
            break;
        }
        case 0xFF6A: ppu->ocps = value; break;
        case 0xFF6B: {
            uint8_t idx = ppu->ocps & 0x3F;
            ppu->obj_palette_ram[idx] = value;
            if (ppu->ocps & 0x80) { /* Auto-increment, bit 6 can overflow */
                ppu->ocps = 0x80 | (((ppu->ocps & 0x7F) + 1) & 0x7F);
            }
            break;
        }
    }
}

/* ============================================================================
 * Frame Handling
 * ========================================================================== */

bool ppu_frame_ready(GBPPU* ppu) {
    return ppu->frame_ready;
}

static int clear_debug = 0;
void ppu_clear_frame_ready(GBPPU* ppu) {
    ppu->frame_ready = false;
}

const uint32_t* ppu_get_framebuffer(GBPPU* ppu) {
    return ppu->rgb_framebuffer;
}
