/**
 * @file mp_sprites.cpp
 * @brief Remote player sprite compositing implementation
 *
 * Extracts Link's sprite tiles from a remote player's GBContext (OAM + VRAM
 * + CGB palettes) and renders them onto the local player's framebuffer.
 *
 * How it works:
 * 1. Read Link's screen position from WRAM (entity table at D100/D110)
 * 2. Scan OAM for sprite entries near Link's position
 * 3. Decode 2bpp tile data from VRAM (respecting CGB bank select)
 * 4. Look up CGB OBJ palette colors, apply player color shift
 * 5. Draw decoded pixels onto the framebuffer with transparency
 *
 * The CGB has 40 OAM entries (0xFE00-0xFE9F), each 4 bytes:
 *   Byte 0: Y position (screen Y + 16)
 *   Byte 1: X position (screen X + 8)
 *   Byte 2: Tile index
 *   Byte 3: Attributes
 *     Bit 7: BG priority (1 = behind BG colors 1-3)
 *     Bit 6: Y flip
 *     Bit 5: X flip
 *     Bit 3: CGB VRAM bank (0 or 1)
 *     Bits 0-2: CGB OBJ palette number (0-7)
 */

#include "mp_sprites.h"
#include "mp_protocol.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

extern "C" {
#include "gbrt.h"
#include "ppu.h"
}

/* ============================================================================
 * Constants
 * ========================================================================== */

/* LADX WRAM addresses */
#define LADX_LINK_X         0xD100
#define LADX_LINK_Y         0xD110
#define LADX_MAP_ROOM       0xD401
#define LADX_IS_INDOOR      0xD402
#define LADX_GAMEPLAY_TYPE  0xDB95

/* Normal gameplay type value in LADX */
#define GAMEPLAY_NORMAL     0x07

/* Memory sizes */
#define SS_OAM_SIZE   0xA0
#define SS_VRAM_SIZE  (0x2000 * 2)

/* Max OAM entries on Game Boy */
#define OAM_COUNT     40

/* Max sprites we'll extract for one Link (body + sword + effects) */
#define MAX_LINK_SPRITES 12

/* Proximity threshold: OAM entries within this many pixels of Link's
 * entity position are considered part of Link's sprite cluster */
#define LINK_PROXIMITY   24

/* ============================================================================
 * WRAM Helper
 * ========================================================================== */

static uint8_t read_wram(GBContext* ctx, uint16_t addr) {
    if (!ctx || !ctx->wram) return 0;
    if (addr >= 0xC000 && addr < 0xE000)
        return ctx->wram[addr - 0xC000];
    return 0;
}

/* ============================================================================
 * CGB Palette Access
 *
 * The PPU stores CGB OBJ palette data as 64 bytes:
 *   8 palettes * 4 colors * 2 bytes per color (15-bit RGB, little-endian)
 *   Format per color: 0bbb bbgg gggr rrrr (low byte first)
 *
 * We access this via the PPU's obj_palette_ram[64] field.
 * ========================================================================== */

/* Convert a CGB 15-bit color to ARGB8888 */
static uint32_t cgb_to_argb(uint16_t cgb_color) {
    uint8_t r5 = cgb_color & 0x1F;
    uint8_t g5 = (cgb_color >> 5) & 0x1F;
    uint8_t b5 = (cgb_color >> 10) & 0x1F;
    /* 5-bit to 8-bit: (val << 3) | (val >> 2) */
    uint8_t r = (r5 << 3) | (r5 >> 2);
    uint8_t g = (g5 << 3) | (g5 >> 2);
    uint8_t b = (b5 << 3) | (b5 >> 2);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* Get a color from CGB OBJ palette RAM.
 * palette: 0-7, color_idx: 0-3 */
static uint32_t get_obj_color(GBPPU* ppu, int palette, int color_idx) {
    if (!ppu) return 0;
    int offset = (palette * 4 + color_idx) * 2;
    uint16_t cgb_color = ppu->obj_palette_ram[offset] |
                         (ppu->obj_palette_ram[offset + 1] << 8);
    return cgb_to_argb(cgb_color);
}

/* ============================================================================
 * Color Shift for Player Colors
 *
 * Instead of doing framebuffer-scan recoloring like mp_color.cpp, we know
 * exactly which palette entries are Link's, so we can remap them directly.
 * ========================================================================== */

static void rgb_to_hsl(uint8_t r, uint8_t g, uint8_t b,
                        float* h, float* s, float* l) {
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float mx = rf > gf ? (rf > bf ? rf : bf) : (gf > bf ? gf : bf);
    float mn = rf < gf ? (rf < bf ? rf : bf) : (gf < bf ? gf : bf);
    float d = mx - mn;
    *l = (mx + mn) * 0.5f;
    if (d < 0.001f) { *h = 0; *s = 0; return; }
    *s = (*l > 0.5f) ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == rf)      *h = (gf - bf) / d + (gf < bf ? 6.0f : 0.0f);
    else if (mx == gf) *h = (bf - rf) / d + 2.0f;
    else               *h = (rf - gf) / d + 4.0f;
    *h *= 60.0f;
}

static void hsl_to_rgb(float h, float s, float l,
                        uint8_t* r, uint8_t* g, uint8_t* b) {
    if (s < 0.001f) {
        uint8_t v = (uint8_t)(l * 255.0f);
        *r = *g = *b = v; return;
    }
    float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    float p = 2.0f * l - q;
    auto hue2rgb = [](float p, float q, float t) -> float {
        if (t < 0) t += 1.0f; if (t > 1) t -= 1.0f;
        if (t < 1.0f/6.0f) return p + (q - p) * 6.0f * t;
        if (t < 0.5f)      return q;
        if (t < 2.0f/3.0f) return p + (q - p) * (2.0f/3.0f - t) * 6.0f;
        return p;
    };
    float hn = h / 360.0f;
    *r = (uint8_t)(hue2rgb(p, q, hn + 1.0f/3.0f) * 255.0f + 0.5f);
    *g = (uint8_t)(hue2rgb(p, q, hn) * 255.0f + 0.5f);
    *b = (uint8_t)(hue2rgb(p, q, hn - 1.0f/3.0f) * 255.0f + 0.5f);
}

/**
 * Recolor a single ARGB pixel: shift hue to target, preserve lightness.
 * Returns 0 (transparent) for color index 0.
 */
static uint32_t recolor_pixel(uint32_t argb,
                               float target_h, float target_s, float target_v) {
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >>  8) & 0xFF;
    uint8_t b = (argb >>  0) & 0xFF;

    float h, s, l;
    rgb_to_hsl(r, g, b, &h, &s, &l);

    /* Apply target hue/saturation, scale lightness by value */
    float new_l = l * target_v;
    uint8_t nr, ng, nb;
    hsl_to_rgb(target_h, target_s, new_l, &nr, &ng, &nb);

    return 0xFF000000u | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | nb;
}

/* ============================================================================
 * OAM Sprite Extraction & Rendering
 * ========================================================================== */

typedef struct {
    int16_t  screen_x;   /* Screen X (OAM X - 8) */
    int16_t  screen_y;   /* Screen Y (OAM Y - 16) */
    uint8_t  tile_idx;   /* Tile index in VRAM */
    uint8_t  palette;    /* CGB palette number (0-7) */
    uint8_t  vram_bank;  /* CGB VRAM bank (0 or 1) */
    bool     flip_x;
    bool     flip_y;
    bool     bg_priority; /* True = behind BG colors 1-3 */
} SpriteEntry;

/**
 * Scan OAM for sprites belonging to Link (near his entity position).
 * Returns the number of sprites found.
 */
static int extract_link_sprites(GBContext* ctx, SpriteEntry* out, int max_out) {
    if (!ctx || !ctx->oam || !ctx->ppu) return 0;

    uint8_t link_x = read_wram(ctx, LADX_LINK_X);
    uint8_t link_y = read_wram(ctx, LADX_LINK_Y);
    int count = 0;

    /* Check if PPU is in 8x16 sprite mode */
    GBPPU* ppu = (GBPPU*)ctx->ppu;
    bool tall_sprites = (ppu->lcdc & LCDC_OBJ_SIZE) != 0;

    for (int i = 0; i < OAM_COUNT && count < max_out; i++) {
        uint8_t oy = ctx->oam[i * 4 + 0];  /* Raw OAM Y (screen + 16) */
        uint8_t ox = ctx->oam[i * 4 + 1];  /* Raw OAM X (screen + 8) */
        uint8_t tile = ctx->oam[i * 4 + 2];
        uint8_t attr = ctx->oam[i * 4 + 3];

        /* Convert to screen coords */
        int sx = (int)ox - 8;
        int sy = (int)oy - 16;

        /* Skip offscreen/hidden sprites */
        if (ox == 0 || oy == 0) continue;
        if (oy >= 160) continue; /* Y=0 or >=160 means hidden */

        /* Check proximity to Link's entity position.
         * In LADX the entity X/Y at D100/D110 are screen-relative. */
        int dx = abs(sx - (int)link_x);
        int dy = abs(sy - (int)link_y);

        if (dx <= LINK_PROXIMITY && dy <= LINK_PROXIMITY) {
            SpriteEntry* e = &out[count++];
            e->screen_x = (int16_t)sx;
            e->screen_y = (int16_t)sy;
            e->tile_idx = tall_sprites ? (tile & 0xFE) : tile;
            e->palette = attr & OAM_CGB_PALETTE;
            e->vram_bank = (attr & OAM_CGB_BANK) ? 1 : 0;
            e->flip_x = (attr & OAM_FLIP_X) != 0;
            e->flip_y = (attr & OAM_FLIP_Y) != 0;
            e->bg_priority = (attr & OAM_PRIORITY) != 0;
        }
    }

    return count;
}

/**
 * Decode one 8-pixel row of a 2bpp tile and draw it onto the framebuffer.
 *
 * @param fb         Destination framebuffer (160x144 ARGB8888)
 * @param tile_data  Pointer to tile data in VRAM (16 bytes per 8x8 tile)
 * @param tile_row   Which row of the tile to decode (0-7)
 * @param dest_x     Screen X to start drawing at
 * @param dest_y     Screen Y of this row
 * @param flip_x     Mirror horizontally
 * @param palette    4 ARGB colors (index 0 = transparent)
 */
static void draw_tile_row(uint32_t* fb,
                           const uint8_t* tile_data,
                           int tile_row,
                           int dest_x, int dest_y,
                           bool flip_x,
                           const uint32_t* palette) {
    if (dest_y < 0 || dest_y >= MP_SCREEN_H) return;

    uint8_t lo = tile_data[tile_row * 2];
    uint8_t hi = tile_data[tile_row * 2 + 1];

    for (int col = 0; col < 8; col++) {
        int bit = flip_x ? col : (7 - col);
        uint8_t color_id = (((hi >> bit) & 1) << 1) | ((lo >> bit) & 1);

        /* Color 0 is transparent for sprites */
        if (color_id == 0) continue;

        int px = dest_x + col;
        if (px < 0 || px >= MP_SCREEN_W) continue;

        fb[dest_y * MP_SCREEN_W + px] = palette[color_id];
    }
}

/**
 * Render a single sprite entry onto the framebuffer.
 */
static void render_sprite(uint32_t* fb,
                           GBContext* remote_ctx,
                           const SpriteEntry* spr,
                           const uint32_t* palette,
                           bool tall_sprites) {
    if (!remote_ctx->vram) return;

    int tile_addr = spr->tile_idx * 16;

    /* Get tile data from correct VRAM bank */
    const uint8_t* vram_base = remote_ctx->vram + (spr->vram_bank * 0x2000);

    /* For 8x16 mode, we have two consecutive tiles */
    int num_tiles = tall_sprites ? 2 : 1;

    for (int t = 0; t < num_tiles; t++) {
        /* When Y-flipped, tile order is reversed: bottom tile draws first */
        int src_tile = spr->flip_y ? (num_tiles - 1 - t) : t;
        int cur_tile_addr = tile_addr + src_tile * 16;
        if (cur_tile_addr + 16 > 0x2000) continue;

        const uint8_t* tile_data = vram_base + cur_tile_addr;

        for (int row = 0; row < 8; row++) {
            int src_row = spr->flip_y ? (7 - row) : row;
            int dest_y = spr->screen_y + t * 8 + row;

            draw_tile_row(fb, tile_data, src_row,
                          spr->screen_x, dest_y,
                          spr->flip_x, palette);
        }
    }
}

/* ============================================================================
 * Public API
 * ========================================================================== */

bool mp_sprites_can_composite(GBContext* local_ctx, GBContext* remote_ctx) {
    if (!local_ctx || !remote_ctx) return false;

    /* Same room check */
    uint8_t local_room   = read_wram(local_ctx, LADX_MAP_ROOM);
    uint8_t remote_room  = read_wram(remote_ctx, LADX_MAP_ROOM);
    uint8_t local_indoor = read_wram(local_ctx, LADX_IS_INDOOR);
    uint8_t remote_indoor = read_wram(remote_ctx, LADX_IS_INDOOR);

    if (local_room != remote_room || local_indoor != remote_indoor)
        return false;

    /* Both must be in normal gameplay (not transitioning, not menu, etc.) */
    uint8_t local_gp  = read_wram(local_ctx, LADX_GAMEPLAY_TYPE);
    uint8_t remote_gp = read_wram(remote_ctx, LADX_GAMEPLAY_TYPE);

    if (local_gp != GAMEPLAY_NORMAL || remote_gp != GAMEPLAY_NORMAL)
        return false;

    return true;
}

void mp_sprites_composite_player(uint32_t* framebuffer,
                                  GBContext* local_ctx,
                                  GBContext* remote_ctx,
                                  float remote_color_h,
                                  float remote_color_s,
                                  float remote_color_v) {
    if (!framebuffer || !local_ctx || !remote_ctx) return;
    if (!remote_ctx->oam || !remote_ctx->vram || !remote_ctx->ppu) return;

    GBPPU* ppu = (GBPPU*)remote_ctx->ppu;

    if (!mp_sprites_can_composite(local_ctx, remote_ctx))
        return;

    /* Extract Link's OAM sprite entries from the remote context */
    SpriteEntry sprites[MAX_LINK_SPRITES];
    int sprite_count = extract_link_sprites(remote_ctx, sprites, MAX_LINK_SPRITES);

    if (sprite_count == 0) return;

    /* Check sprite size mode from LCDC (bit 2 = 8x16 sprites) */
    bool tall_sprites = (ppu->lcdc & LCDC_OBJ_SIZE) != 0;

    /* Build recolored palettes for each CGB OBJ palette used.
     * We lazily build them as we encounter new palette numbers. */
    uint32_t palette_cache[8][4];
    bool palette_built[8] = {};

    for (int i = 0; i < sprite_count; i++) {
        int pal = sprites[i].palette;

        if (!palette_built[pal]) {
            palette_cache[pal][0] = 0; /* Transparent */
            for (int c = 1; c < 4; c++) {
                uint32_t orig = get_obj_color(ppu, pal, c);
                palette_cache[pal][c] = recolor_pixel(orig,
                    remote_color_h, remote_color_s, remote_color_v);
            }
            palette_built[pal] = true;
        }

        render_sprite(framebuffer, remote_ctx, &sprites[i],
                      palette_cache[pal], tall_sprites);
    }
}
