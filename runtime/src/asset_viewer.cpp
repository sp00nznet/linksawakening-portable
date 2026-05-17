/**
 * Asset Viewer - ImGui tile/sprite/tilemap/audio/palette viewer & exporter
 */

#include "imgui.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern "C" {
#include "asset_viewer.h"
#include "gbrt.h"
#include "ppu.h"
}

/* ================================================================
 * State
 * ================================================================ */

static bool g_visible = false;
static SDL_Renderer* g_renderer = NULL;

/* Tile viewer */
static SDL_Texture* g_tile_texture = NULL;   /* 128x192 (16x24 tiles) */
static int g_tile_bank = 0;
static int g_tile_palette = 0;
static int g_tile_zoom = 2;
static int g_selected_tile = -1;

/* Sprite viewer */
static SDL_Texture* g_sprite_texture = NULL; /* 320x32 (40 sprites * 8px wide) */

/* Tilemap viewer */
static SDL_Texture* g_bgmap_texture = NULL;  /* 256x256 */
static SDL_Texture* g_winmap_texture = NULL; /* 256x256 */
static int g_tilemap_zoom = 1;
static bool g_show_viewport = true;

/* Audio recorder */
#define WAVEFORM_SAMPLES 2048
static int16_t g_wave_left[WAVEFORM_SAMPLES];
static int16_t g_wave_right[WAVEFORM_SAMPLES];
static int g_wave_pos = 0;
static bool g_recording = false;
static FILE* g_wav_file = NULL;
static uint32_t g_wav_samples = 0;

/* ================================================================
 * BMP export helper (32-bit ARGB, no external deps)
 * ================================================================ */

static bool write_bmp(const char* path, const uint32_t* pixels, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;

    uint32_t row_bytes = w * 4;
    uint32_t img_size = row_bytes * h;
    uint32_t file_size = 54 + img_size;

    /* BMP file header (14 bytes) */
    uint8_t hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &file_size, 4);
    uint32_t offset = 54;
    memcpy(hdr + 10, &offset, 4);

    /* DIB header (40 bytes) */
    uint32_t dib_size = 40;
    memcpy(hdr + 14, &dib_size, 4);
    int32_t bmp_w = w, bmp_h = -(int32_t)h; /* top-down */
    memcpy(hdr + 18, &bmp_w, 4);
    memcpy(hdr + 22, &bmp_h, 4);
    uint16_t planes = 1;
    memcpy(hdr + 26, &planes, 2);
    uint16_t bpp = 32;
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &img_size, 4);

    fwrite(hdr, 1, 54, f);

    /* Pixel data: ARGB -> BGRA for BMP */
    uint8_t* row = (uint8_t*)malloc(row_bytes);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = pixels[y * w + x];
            row[x*4+0] = (p >>  0) & 0xFF; /* B */
            row[x*4+1] = (p >>  8) & 0xFF; /* G */
            row[x*4+2] = (p >> 16) & 0xFF; /* R */
            row[x*4+3] = (p >> 24) & 0xFF; /* A */
        }
        fwrite(row, 1, row_bytes, f);
    }
    free(row);
    fclose(f);
    return true;
}

/* ================================================================
 * WAV export helpers
 * ================================================================ */

static void wav_write_header(FILE* f, uint32_t num_samples) {
    uint32_t data_size = num_samples * 2 * sizeof(int16_t); /* stereo */
    uint32_t file_size = 36 + data_size;
    uint16_t channels = 2;
    uint32_t sample_rate = 44100;
    uint32_t byte_rate = sample_rate * channels * sizeof(int16_t);
    uint16_t block_align = channels * sizeof(int16_t);
    uint16_t bits = 16;

    fwrite("RIFF", 1, 4, f);
    fwrite(&file_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    uint16_t fmt_tag = 1; /* PCM */
    fwrite(&fmt_tag, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
}

/* ================================================================
 * Color helpers
 * ================================================================ */

static inline uint32_t rgb555_to_argb(uint16_t rgb555) {
    uint8_t r5 = rgb555 & 0x1F;
    uint8_t g5 = (rgb555 >> 5) & 0x1F;
    uint8_t b5 = (rgb555 >> 10) & 0x1F;
    uint8_t r = (r5 << 3) | (r5 >> 2);
    uint8_t g = (g5 << 3) | (g5 >> 2);
    uint8_t b = (b5 << 3) | (b5 >> 2);
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

static uint32_t get_palette_color(GBPPU* ppu, bool is_obj, int pal, int color_idx) {
    const uint8_t* ram = is_obj ? ppu->obj_palette_ram : ppu->bg_palette_ram;
    int offset = pal * 8 + color_idx * 2;
    uint16_t rgb555 = ram[offset] | (ram[offset + 1] << 8);
    return rgb555_to_argb(rgb555);
}

/* ================================================================
 * Tile rendering
 * ================================================================ */

static void decode_tile(GBContext* ctx, GBPPU* ppu, int tile_idx, int bank, int palette,
                        bool is_obj, uint32_t* out_8x8) {
    int base = bank * 0x2000 + tile_idx * 16;
    for (int y = 0; y < 8; y++) {
        uint8_t lo = ctx->vram[base + y * 2];
        uint8_t hi = ctx->vram[base + y * 2 + 1];
        for (int x = 0; x < 8; x++) {
            int bit = 7 - x;
            int color = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
            out_8x8[y * 8 + x] = get_palette_color(ppu, is_obj, palette, color);
        }
    }
}

/* ================================================================
 * Tab: Tiles
 * ================================================================ */

static void draw_tile_tab(GBContext* ctx) {
    GBPPU* ppu = (GBPPU*)ctx->ppu;
    if (!ppu) return;

    ImGui::SliderInt("VRAM Bank", &g_tile_bank, 0, 1);
    ImGui::SliderInt("Palette", &g_tile_palette, 0, 7);
    ImGui::SliderInt("Zoom", &g_tile_zoom, 1, 4);
    ImGui::SameLine();
    bool as_obj = false;
    ImGui::Checkbox("OBJ Palette", &as_obj);

    /* Render 384 tiles (16 cols x 24 rows) into texture */
    uint32_t pixels[128 * 192];
    memset(pixels, 0, sizeof(pixels));

    for (int t = 0; t < 384; t++) {
        int col = t % 16;
        int row = t / 16;
        uint32_t tile[64];
        decode_tile(ctx, ppu, t, g_tile_bank, g_tile_palette, as_obj, tile);
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                pixels[(row * 8 + y) * 128 + (col * 8 + x)] = tile[y * 8 + x];
            }
        }
    }

    if (!g_tile_texture) {
        g_tile_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                           SDL_TEXTUREACCESS_STREAMING, 128, 192);
    }
    SDL_UpdateTexture(g_tile_texture, NULL, pixels, 128 * sizeof(uint32_t));

    ImVec2 size(128.0f * g_tile_zoom, 192.0f * g_tile_zoom);
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(intptr_t)g_tile_texture, size);

    /* Detect tile click */
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
        ImVec2 mouse = ImGui::GetMousePos();
        int mx = (int)((mouse.x - cursor.x) / g_tile_zoom);
        int my = (int)((mouse.y - cursor.y) / g_tile_zoom);
        int col = mx / 8;
        int row = my / 8;
        if (col >= 0 && col < 16 && row >= 0 && row < 24) {
            g_selected_tile = row * 16 + col;
        }
    }

    if (g_selected_tile >= 0 && g_selected_tile < 384) {
        ImGui::Text("Selected: Tile %d (0x%03X)", g_selected_tile, g_selected_tile);
    }

    /* Export buttons */
    if (ImGui::Button("Export All Tiles (BMP)")) {
        char* base = SDL_GetBasePath();
        char path[512];
        snprintf(path, sizeof(path), "%stiles_bank%d_pal%d.bmp", base ? base : "", g_tile_bank, g_tile_palette);
        if (base) SDL_free(base);
        if (write_bmp(path, pixels, 128, 192)) {
            fprintf(stderr, "[ASSET] Saved tiles to %s\n", path);
        }
    }
    if (g_selected_tile >= 0) {
        ImGui::SameLine();
        if (ImGui::Button("Export Selected Tile")) {
            uint32_t tile[64];
            decode_tile(ctx, ppu, g_selected_tile, g_tile_bank, g_tile_palette, as_obj, tile);
            char* base = SDL_GetBasePath();
            char path[512];
            snprintf(path, sizeof(path), "%stile_%d.bmp", base ? base : "", g_selected_tile);
            if (base) SDL_free(base);
            write_bmp(path, tile, 8, 8);
            fprintf(stderr, "[ASSET] Saved tile %d to %s\n", g_selected_tile, path);
        }
    }
}

/* ================================================================
 * Tab: Sprites
 * ================================================================ */

static void draw_sprite_tab(GBContext* ctx) {
    GBPPU* ppu = (GBPPU*)ctx->ppu;
    if (!ppu || !ctx->oam) return;

    uint8_t sprite_height = (ppu->lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
    ImGui::Text("Sprite Size: 8x%d  (LCDC bit 2 = %d)", sprite_height, (ppu->lcdc & LCDC_OBJ_SIZE) ? 1 : 0);
    ImGui::Separator();

    if (ImGui::BeginTable("oam_table", 7, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 400))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 25);
        ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Pos", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Tile", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Pal", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int i = 0; i < 40; i++) {
            OAMEntry* spr = (OAMEntry*)(ctx->oam + i * 4);
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Text("%2d", i);

            /* Preview thumbnail */
            ImGui::TableNextColumn();
            uint32_t preview[8 * 16];
            memset(preview, 0, sizeof(preview));
            uint8_t tile_idx = spr->tile;
            if (sprite_height == 16) tile_idx &= 0xFE;
            uint8_t tile_bank = (ppu->cgb_mode && (spr->flags & OAM_CGB_BANK)) ? 1 : 0;
            int obj_pal = ppu->cgb_mode ? (spr->flags & OAM_CGB_PALETTE) : 0;

            for (int row = 0; row < sprite_height; row++) {
                int line = row;
                if (spr->flags & OAM_FLIP_Y) line = sprite_height - 1 - line;
                int base_addr = tile_bank * 0x2000 + tile_idx * 16 + line * 2;
                uint8_t lo = ctx->vram[base_addr];
                uint8_t hi = ctx->vram[base_addr + 1];
                for (int x = 0; x < 8; x++) {
                    int bit_pos = (spr->flags & OAM_FLIP_X) ? x : (7 - x);
                    int color = ((lo >> bit_pos) & 1) | (((hi >> bit_pos) & 1) << 1);
                    if (color != 0) {
                        preview[row * 8 + x] = get_palette_color(ppu, true, obj_pal, color);
                    }
                }
            }
            /* Draw inline preview using colored text (small) */
            ImGui::Text(" ");  /* placeholder; real texture preview below */

            ImGui::TableNextColumn();
            ImGui::Text("(%d,%d)", spr->x - 8, spr->y - 16);

            ImGui::TableNextColumn();
            ImGui::Text("0x%02X", spr->tile);

            ImGui::TableNextColumn();
            ImGui::Text("0x%02X", spr->flags);

            ImGui::TableNextColumn();
            ImGui::Text("%d", obj_pal);

            ImGui::TableNextColumn();
            const char* flip_x = (spr->flags & OAM_FLIP_X) ? "X" : "";
            const char* flip_y = (spr->flags & OAM_FLIP_Y) ? "Y" : "";
            const char* prio = (spr->flags & OAM_PRIORITY) ? "BG" : "FG";
            ImGui::Text("%s%s %s", flip_x, flip_y, prio);
        }
        ImGui::EndTable();
    }

    /* Export sprite sheet */
    if (ImGui::Button("Export Sprite Sheet (BMP)")) {
        int sheet_w = 8 * 10; /* 10 sprites per row */
        int sheet_h = sprite_height * 4; /* 4 rows */
        uint32_t* sheet = (uint32_t*)calloc(sheet_w * sheet_h, sizeof(uint32_t));

        for (int i = 0; i < 40; i++) {
            OAMEntry* spr = (OAMEntry*)(ctx->oam + i * 4);
            uint8_t tile_idx = spr->tile;
            if (sprite_height == 16) tile_idx &= 0xFE;
            uint8_t tile_bank = (ppu->cgb_mode && (spr->flags & OAM_CGB_BANK)) ? 1 : 0;
            int obj_pal = ppu->cgb_mode ? (spr->flags & OAM_CGB_PALETTE) : 0;
            int sx = (i % 10) * 8;
            int sy = (i / 10) * sprite_height;

            for (int row = 0; row < sprite_height; row++) {
                int line = (spr->flags & OAM_FLIP_Y) ? (sprite_height - 1 - row) : row;
                int base_addr = tile_bank * 0x2000 + tile_idx * 16 + line * 2;
                uint8_t lo = ctx->vram[base_addr];
                uint8_t hi = ctx->vram[base_addr + 1];
                for (int x = 0; x < 8; x++) {
                    int bit_pos = (spr->flags & OAM_FLIP_X) ? x : (7 - x);
                    int color = ((lo >> bit_pos) & 1) | (((hi >> bit_pos) & 1) << 1);
                    if (color != 0) {
                        sheet[(sy + row) * sheet_w + sx + x] = get_palette_color(ppu, true, obj_pal, color);
                    }
                }
            }
        }

        char* base = SDL_GetBasePath();
        char path[512];
        snprintf(path, sizeof(path), "%ssprite_sheet.bmp", base ? base : "");
        if (base) SDL_free(base);
        write_bmp(path, sheet, sheet_w, sheet_h);
        fprintf(stderr, "[ASSET] Saved sprite sheet to %s\n", path);
        free(sheet);
    }
}

/* ================================================================
 * Tab: Tilemaps
 * ================================================================ */

static void render_tilemap(GBContext* ctx, GBPPU* ppu, uint16_t map_base, uint32_t* out_256x256) {
    for (int ty = 0; ty < 32; ty++) {
        for (int tx = 0; tx < 32; tx++) {
            uint16_t map_addr = map_base + ty * 32 + tx;
            uint8_t tile_idx = ctx->vram[map_addr - 0x8000];

            /* CGB attributes from bank 1 */
            uint8_t attrs = 0;
            uint8_t tile_bank = 0;
            int pal = 0;
            bool flip_x = false, flip_y = false;

            if (ppu->cgb_mode) {
                attrs = ctx->vram[0x2000 + (map_addr - 0x8000)];
                pal = attrs & 0x07;
                tile_bank = (attrs & 0x08) ? 1 : 0;
                flip_x = (attrs & 0x20) != 0;
                flip_y = (attrs & 0x40) != 0;
            }

            /* Get tile data address */
            uint16_t tile_addr;
            if (ppu->lcdc & LCDC_TILE_DATA) {
                tile_addr = 0x8000 + tile_idx * 16;
            } else {
                tile_addr = 0x9000 + (int8_t)tile_idx * 16;
            }

            for (int py = 0; py < 8; py++) {
                int fy = flip_y ? (7 - py) : py;
                int vram_off = tile_bank * 0x2000 + (tile_addr - 0x8000) + fy * 2;
                uint8_t lo = ctx->vram[vram_off];
                uint8_t hi = ctx->vram[vram_off + 1];
                for (int px = 0; px < 8; px++) {
                    int fx = flip_x ? px : (7 - px);
                    int color = ((lo >> fx) & 1) | (((hi >> fx) & 1) << 1);
                    uint32_t argb = get_palette_color(ppu, false, pal, color);
                    out_256x256[(ty * 8 + py) * 256 + (tx * 8 + px)] = argb;
                }
            }
        }
    }
}

static void draw_tilemap_tab(GBContext* ctx) {
    GBPPU* ppu = (GBPPU*)ctx->ppu;
    if (!ppu) return;

    ImGui::SliderInt("Map Zoom", &g_tilemap_zoom, 1, 3);
    ImGui::Checkbox("Show Viewport", &g_show_viewport);

    /* BG Map */
    uint16_t bg_base = (ppu->lcdc & LCDC_BG_TILEMAP) ? 0x9C00 : 0x9800;
    uint16_t win_base = (ppu->lcdc & LCDC_WINDOW_TILEMAP) ? 0x9C00 : 0x9800;

    ImGui::Text("BG Map (0x%04X):", bg_base);

    uint32_t bg_pixels[256 * 256];
    render_tilemap(ctx, ppu, bg_base, bg_pixels);

    if (!g_bgmap_texture) {
        g_bgmap_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, 256, 256);
    }
    SDL_UpdateTexture(g_bgmap_texture, NULL, bg_pixels, 256 * sizeof(uint32_t));

    float map_sz = 256.0f * g_tilemap_zoom;
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(intptr_t)g_bgmap_texture, ImVec2(map_sz, map_sz));

    /* Draw viewport rectangle overlay */
    if (g_show_viewport) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float sx = cursor.x + ppu->scx * g_tilemap_zoom;
        float sy = cursor.y + ppu->scy * g_tilemap_zoom;
        float vw = 160.0f * g_tilemap_zoom;
        float vh = 144.0f * g_tilemap_zoom;
        dl->AddRect(ImVec2(sx, sy), ImVec2(sx + vw, sy + vh), IM_COL32(255, 0, 0, 255), 0, 0, 2.0f);
    }

    /* Window Map */
    ImGui::Spacing();
    ImGui::Text("Window Map (0x%04X):", win_base);

    uint32_t win_pixels[256 * 256];
    render_tilemap(ctx, ppu, win_base, win_pixels);

    if (!g_winmap_texture) {
        g_winmap_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                              SDL_TEXTUREACCESS_STREAMING, 256, 256);
    }
    SDL_UpdateTexture(g_winmap_texture, NULL, win_pixels, 256 * sizeof(uint32_t));

    ImGui::Image((ImTextureID)(intptr_t)g_winmap_texture, ImVec2(map_sz, map_sz));

    /* Export */
    if (ImGui::Button("Export BG Map (BMP)")) {
        char* base = SDL_GetBasePath();
        char path[512];
        snprintf(path, sizeof(path), "%sbg_map.bmp", base ? base : "");
        if (base) SDL_free(base);
        write_bmp(path, bg_pixels, 256, 256);
        fprintf(stderr, "[ASSET] Saved BG map to %s\n", path);
    }
    ImGui::SameLine();
    if (ImGui::Button("Export Window Map (BMP)")) {
        char* base = SDL_GetBasePath();
        char path[512];
        snprintf(path, sizeof(path), "%swin_map.bmp", base ? base : "");
        if (base) SDL_free(base);
        write_bmp(path, win_pixels, 256, 256);
        fprintf(stderr, "[ASSET] Saved Window map to %s\n", path);
    }
}

/* ================================================================
 * Tab: Audio
 * ================================================================ */

static void draw_audio_tab(GBContext* ctx) {
    (void)ctx;

    ImGui::Text("Audio Waveform (last %.1f sec)", (float)WAVEFORM_SAMPLES / 44100.0f);

    /* Convert to float for PlotLines */
    static float left_f[WAVEFORM_SAMPLES];
    static float right_f[WAVEFORM_SAMPLES];

    for (int i = 0; i < WAVEFORM_SAMPLES; i++) {
        int idx = (g_wave_pos + i) % WAVEFORM_SAMPLES;
        left_f[i] = g_wave_left[idx] / 32768.0f;
        right_f[i] = g_wave_right[idx] / 32768.0f;
    }

    ImGui::Text("Left Channel:");
    ImGui::PlotLines("##left", left_f, WAVEFORM_SAMPLES, 0, NULL, -1.0f, 1.0f, ImVec2(-1, 80));

    ImGui::Text("Right Channel:");
    ImGui::PlotLines("##right", right_f, WAVEFORM_SAMPLES, 0, NULL, -1.0f, 1.0f, ImVec2(-1, 80));

    ImGui::Text("Mixed:");
    static float mixed_f[WAVEFORM_SAMPLES];
    for (int i = 0; i < WAVEFORM_SAMPLES; i++) {
        mixed_f[i] = (left_f[i] + right_f[i]) * 0.5f;
    }
    ImGui::PlotLines("##mixed", mixed_f, WAVEFORM_SAMPLES, 0, NULL, -1.0f, 1.0f, ImVec2(-1, 80));

    /* Recording */
    ImGui::Separator();
    if (!g_recording) {
        if (ImGui::Button("Start Recording (WAV)")) {
            char* base = SDL_GetBasePath();
            char path[512];
            snprintf(path, sizeof(path), "%srecording.wav", base ? base : "");
            if (base) SDL_free(base);
            g_wav_file = fopen(path, "wb");
            if (g_wav_file) {
                wav_write_header(g_wav_file, 0); /* placeholder, rewritten on stop */
                g_wav_samples = 0;
                g_recording = true;
                fprintf(stderr, "[ASSET] Recording started: %s\n", path);
            }
        }
    } else {
        float seconds = (float)g_wav_samples / 44100.0f;
        ImGui::Text("Recording: %.1f sec (%u samples)", seconds, g_wav_samples);
        if (ImGui::Button("Stop Recording")) {
            if (g_wav_file) {
                /* Rewrite header with correct sample count */
                fseek(g_wav_file, 0, SEEK_SET);
                wav_write_header(g_wav_file, g_wav_samples);
                fclose(g_wav_file);
                g_wav_file = NULL;
                fprintf(stderr, "[ASSET] Recording stopped: %u samples (%.1f sec)\n",
                        g_wav_samples, (float)g_wav_samples / 44100.0f);
            }
            g_recording = false;
        }
    }
}

/* ================================================================
 * Tab: Palettes
 * ================================================================ */

static void draw_palette_tab(GBContext* ctx) {
    GBPPU* ppu = (GBPPU*)ctx->ppu;
    if (!ppu) return;

    ImGui::Text("BG Palettes");
    ImGui::Separator();

    for (int p = 0; p < 8; p++) {
        ImGui::Text("BGP%d:", p);
        ImGui::SameLine();
        for (int c = 0; c < 4; c++) {
            uint32_t color = get_palette_color(ppu, false, p, c);
            float r = ((color >> 16) & 0xFF) / 255.0f;
            float g = ((color >>  8) & 0xFF) / 255.0f;
            float b = ((color >>  0) & 0xFF) / 255.0f;
            ImVec4 col(r, g, b, 1.0f);

            char label[16];
            snprintf(label, sizeof(label), "##bg%d_%d", p, c);
            ImGui::ColorButton(label, col, ImGuiColorEditFlags_NoTooltip, ImVec2(24, 24));

            if (ImGui::IsItemHovered()) {
                int offset = p * 8 + c * 2;
                uint16_t raw = ppu->bg_palette_ram[offset] | (ppu->bg_palette_ram[offset + 1] << 8);
                ImGui::SetTooltip("RGB555: 0x%04X\nR=%d G=%d B=%d",
                    raw,
                    (int)(r * 255), (int)(g * 255), (int)(b * 255));
            }
            if (c < 3) ImGui::SameLine();
        }
    }

    ImGui::Spacing();
    ImGui::Text("OBJ Palettes");
    ImGui::Separator();

    for (int p = 0; p < 8; p++) {
        ImGui::Text("OBP%d:", p);
        ImGui::SameLine();
        for (int c = 0; c < 4; c++) {
            uint32_t color = get_palette_color(ppu, true, p, c);
            float r = ((color >> 16) & 0xFF) / 255.0f;
            float g = ((color >>  8) & 0xFF) / 255.0f;
            float b = ((color >>  0) & 0xFF) / 255.0f;
            ImVec4 col(r, g, b, 1.0f);

            char label[16];
            snprintf(label, sizeof(label), "##ob%d_%d", p, c);
            ImGui::ColorButton(label, col, ImGuiColorEditFlags_NoTooltip, ImVec2(24, 24));

            if (ImGui::IsItemHovered()) {
                int offset = p * 8 + c * 2;
                uint16_t raw = ppu->obj_palette_ram[offset] | (ppu->obj_palette_ram[offset + 1] << 8);
                ImGui::SetTooltip("RGB555: 0x%04X\nR=%d G=%d B=%d",
                    raw,
                    (int)(r * 255), (int)(g * 255), (int)(b * 255));
            }
            if (c < 3) ImGui::SameLine();
        }
    }

    ImGui::Spacing();
    ImGui::Text("DMG Palettes");
    ImGui::Separator();
    const char* dmg_names[] = { "BGP", "OBP0", "OBP1" };
    uint8_t dmg_regs[] = { ppu->bgp, ppu->obp0, ppu->obp1 };
    for (int p = 0; p < 3; p++) {
        ImGui::Text("%s (0x%02X):", dmg_names[p], dmg_regs[p]);
        ImGui::SameLine();
        for (int c = 0; c < 4; c++) {
            int shade = (dmg_regs[p] >> (c * 2)) & 3;
            float v = 1.0f - shade / 3.0f;
            ImVec4 col(v, v, v, 1.0f);
            char label[16];
            snprintf(label, sizeof(label), "##dmg%d_%d", p, c);
            ImGui::ColorButton(label, col, ImGuiColorEditFlags_NoTooltip, ImVec2(24, 24));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Color %d -> Shade %d", c, shade);
            }
            if (c < 3) ImGui::SameLine();
        }
    }
}

/* ================================================================
 * Public API
 * ================================================================ */

extern "C" void asset_viewer_init(void* sdl_renderer) {
    g_renderer = (SDL_Renderer*)sdl_renderer;
}

extern "C" void asset_viewer_draw(GBContext* ctx) {
    if (!g_visible || !ctx) return;

    ImGui::SetNextWindowSize(ImVec2(600, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Asset Viewer", &g_visible)) {
        if (ImGui::BeginTabBar("AssetTabs")) {
            if (ImGui::BeginTabItem("Tiles")) {
                draw_tile_tab(ctx);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Sprites")) {
                draw_sprite_tab(ctx);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Tilemaps")) {
                draw_tilemap_tab(ctx);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Audio")) {
                draw_audio_tab(ctx);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Palettes")) {
                draw_palette_tab(ctx);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

extern "C" void asset_viewer_shutdown(void) {
    if (g_tile_texture) { SDL_DestroyTexture(g_tile_texture); g_tile_texture = NULL; }
    if (g_sprite_texture) { SDL_DestroyTexture(g_sprite_texture); g_sprite_texture = NULL; }
    if (g_bgmap_texture) { SDL_DestroyTexture(g_bgmap_texture); g_bgmap_texture = NULL; }
    if (g_winmap_texture) { SDL_DestroyTexture(g_winmap_texture); g_winmap_texture = NULL; }
    if (g_wav_file) { fclose(g_wav_file); g_wav_file = NULL; }
}

extern "C" void asset_viewer_set_visible(bool visible) {
    g_visible = visible;
}

extern "C" bool asset_viewer_is_visible(void) {
    return g_visible;
}

extern "C" void asset_viewer_push_audio(int16_t left, int16_t right) {
    g_wave_left[g_wave_pos] = left;
    g_wave_right[g_wave_pos] = right;
    g_wave_pos = (g_wave_pos + 1) % WAVEFORM_SAMPLES;

    if (g_recording && g_wav_file) {
        int16_t samples[2] = { left, right };
        fwrite(samples, sizeof(int16_t), 2, g_wav_file);
        g_wav_samples++;
    }
}
