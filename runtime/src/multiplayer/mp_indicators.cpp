/**
 * @file mp_indicators.cpp
 * @brief Player position indicator rendering
 *
 * Draws colored markers on the framebuffer to show where other players are.
 * - Same room: colored triangle/dot at the player's position
 * - Different room: colored arrow at screen edge pointing toward them
 * - Off-map: shows room coordinates in corner
 */

#include "mp_indicators.h"
#include "mp_protocol.h"
#include <string.h>
#include <math.h>

static bool g_enabled = true;
static MPIndicatorStyle g_style = MP_INDICATOR_ARROW;

/* ============================================================================
 * Color Helpers
 * ========================================================================== */

static uint32_t hsv_to_argb(float h, float s, float v) {
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r, g, b;

    if      (h < 60)  { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else              { r = c; g = 0; b = x; }

    uint8_t rb = (uint8_t)((r + m) * 255);
    uint8_t gb = (uint8_t)((g + m) * 255);
    uint8_t bb = (uint8_t)((b + m) * 255);

    return 0xFF000000 | ((uint32_t)rb << 16) | ((uint32_t)gb << 8) | bb;
}

/* ============================================================================
 * Pixel Drawing (bounds-checked)
 * ========================================================================== */

static inline void put_pixel(uint32_t* fb, int x, int y, uint32_t color) {
    if (x >= 0 && x < MP_SCREEN_W && y >= 0 && y < MP_SCREEN_H)
        fb[y * MP_SCREEN_W + x] = color;
}

static void draw_filled_circle(uint32_t* fb, int cx, int cy, int r, uint32_t color) {
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx*dx + dy*dy <= r*r)
                put_pixel(fb, cx + dx, cy + dy, color);
        }
    }
}

/* Draw a small downward-pointing triangle (indicator above Link) */
static void draw_arrow_down(uint32_t* fb, int cx, int cy, uint32_t color) {
    /* 7px wide, 5px tall triangle pointing down */
    for (int row = 0; row < 5; row++) {
        int half_w = 3 - row;
        for (int dx = -half_w; dx <= half_w; dx++) {
            put_pixel(fb, cx + dx, cy + row, color);
        }
    }
    /* Black outline */
    uint32_t outline = 0xFF000000;
    for (int row = 0; row < 5; row++) {
        int half_w = 3 - row;
        put_pixel(fb, cx - half_w - 1, cy + row, outline);
        put_pixel(fb, cx + half_w + 1, cy + row, outline);
    }
    /* Top edge */
    for (int dx = -3; dx <= 3; dx++)
        put_pixel(fb, cx + dx, cy - 1, outline);
    /* Bottom point */
    put_pixel(fb, cx, cy + 5, outline);
}

/* Draw an edge arrow pointing in a direction */
static void draw_edge_arrow(uint32_t* fb, int x, int y, int dir, uint32_t color) {
    /* dir: 0=right, 1=down, 2=left, 3=up */
    int dx_arr[] = { 1, 0, -1, 0 };
    int dy_arr[] = { 0, 1, 0, -1 };
    int px_arr[] = { 0, -1, 0, 1 }; /* perpendicular */
    int py_arr[] = { 1, 0, -1, 0 };

    /* Draw 5-pixel arrow */
    for (int i = 0; i < 5; i++) {
        put_pixel(fb, x + dx_arr[dir] * i, y + dy_arr[dir] * i, color);
    }
    /* Arrow head */
    for (int i = 1; i <= 2; i++) {
        put_pixel(fb, x + dx_arr[dir] * (4-i) + px_arr[dir] * i,
                  y + dy_arr[dir] * (4-i) + py_arr[dir] * i, color);
        put_pixel(fb, x + dx_arr[dir] * (4-i) - px_arr[dir] * i,
                  y + dy_arr[dir] * (4-i) - py_arr[dir] * i, color);
    }
}

/* Render a small colored health bar */
static void draw_mini_health(uint32_t* fb, int x, int y,
                              uint8_t health, uint8_t max_health, uint32_t color)
{
    if (max_health == 0) return;
    int bar_w = 12;
    int fill = (health * bar_w) / max_health;

    /* Background */
    for (int dx = 0; dx < bar_w; dx++)
        put_pixel(fb, x + dx, y, 0xFF000000);

    /* Fill */
    for (int dx = 0; dx < fill; dx++)
        put_pixel(fb, x + dx, y, color);
}

/* ============================================================================
 * Overworld Map Layout
 *
 * LADX overworld is 16x16 rooms. Room ID = (row * 16) + col.
 * ========================================================================== */

static void room_to_grid(uint8_t room, int* col, int* row) {
    *col = room & 0x0F;
    *row = (room >> 4) & 0x0F;
}

/* ============================================================================
 * Main Render
 * ========================================================================== */

void mp_indicators_render(uint32_t* framebuffer,
                          int local_slot,
                          uint8_t local_room,
                          uint8_t local_indoor,
                          const void* players_ptr,
                          int player_count)
{
    if (!g_enabled || !framebuffer || !players_ptr) return;

    const MPPlayerState* players = (const MPPlayerState*)players_ptr;

    for (int i = 0; i < player_count && i < MP_MAX_PLAYERS; i++) {
        if (i == local_slot) continue;  /* Don't show indicator for self */
        if (!players[i].connected) continue;

        uint32_t color = hsv_to_argb(players[i].color_h,
                                      players[i].color_s,
                                      players[i].color_v);

        bool same_room = (players[i].map_room == local_room &&
                          players[i].is_indoor == local_indoor);

        if (same_room) {
            /* ---- Same room: show indicator at player position ---- */
            int px = players[i].link_x;
            int py = players[i].link_y;

            /* Clamp to screen */
            if (px < 4) px = 4;
            if (px > MP_SCREEN_W - 4) px = MP_SCREEN_W - 4;
            if (py < 10) py = 10;
            if (py > MP_SCREEN_H - 4) py = MP_SCREEN_H - 4;

            switch (g_style) {
            case MP_INDICATOR_DOT:
                draw_filled_circle(framebuffer, px, py - 10, 3, color);
                break;
            case MP_INDICATOR_GLOW:
                /* Draw semi-transparent glow around Link position */
                for (int dy = -8; dy <= 8; dy++) {
                    for (int dx = -8; dx <= 8; dx++) {
                        int dist2 = dx*dx + dy*dy;
                        if (dist2 < 64 && dist2 > 16) {
                            int sx = px + dx;
                            int sy = py + dy;
                            if (sx >= 0 && sx < MP_SCREEN_W && sy >= 0 && sy < MP_SCREEN_H) {
                                /* Blend color with existing pixel */
                                uint32_t existing = framebuffer[sy * MP_SCREEN_W + sx];
                                uint8_t er = (existing >> 16) & 0xFF;
                                uint8_t eg = (existing >>  8) & 0xFF;
                                uint8_t eb = (existing >>  0) & 0xFF;
                                uint8_t cr = (color >> 16) & 0xFF;
                                uint8_t cg = (color >>  8) & 0xFF;
                                uint8_t cb = (color >>  0) & 0xFF;
                                float alpha = 0.4f * (1.0f - (float)dist2 / 64.0f);
                                uint8_t fr = (uint8_t)(er * (1-alpha) + cr * alpha);
                                uint8_t fg = (uint8_t)(eg * (1-alpha) + cg * alpha);
                                uint8_t fb_c = (uint8_t)(eb * (1-alpha) + cb * alpha);
                                framebuffer[sy * MP_SCREEN_W + sx] =
                                    0xFF000000 | (fr << 16) | (fg << 8) | fb_c;
                            }
                        }
                    }
                }
                break;
            case MP_INDICATOR_ARROW:
            default:
                draw_arrow_down(framebuffer, px, py - 12, color);
                break;
            }

            /* Mini health bar below indicator */
            draw_mini_health(framebuffer, px - 6, py - 6,
                             players[i].health, players[i].max_health, color);

        } else if (!local_indoor && !players[i].is_indoor) {
            /* ---- Different overworld room: edge arrow ---- */
            int my_col, my_row, their_col, their_row;
            room_to_grid(local_room, &my_col, &my_row);
            room_to_grid(players[i].map_room, &their_col, &their_row);

            int dx = their_col - my_col;
            int dy = their_row - my_row;

            /* Determine which edge to show arrow on */
            int arrow_x, arrow_y, arrow_dir;

            if (abs(dx) >= abs(dy)) {
                /* Primarily horizontal */
                if (dx > 0) {
                    arrow_x = MP_SCREEN_W - 8;
                    arrow_dir = 0; /* right */
                } else {
                    arrow_x = 8;
                    arrow_dir = 2; /* left */
                }
                arrow_y = MP_SCREEN_H / 2 + (dy * 20);
                if (arrow_y < 20) arrow_y = 20;
                if (arrow_y > MP_SCREEN_H - 20) arrow_y = MP_SCREEN_H - 20;
            } else {
                /* Primarily vertical */
                if (dy > 0) {
                    arrow_y = MP_SCREEN_H - 8;
                    arrow_dir = 1; /* down */
                } else {
                    arrow_y = 20; /* below menu bar */
                    arrow_dir = 3; /* up */
                }
                arrow_x = MP_SCREEN_W / 2 + (dx * 20);
                if (arrow_x < 8) arrow_x = 8;
                if (arrow_x > MP_SCREEN_W - 8) arrow_x = MP_SCREEN_W - 8;
            }

            draw_edge_arrow(framebuffer, arrow_x, arrow_y, arrow_dir, color);

            /* Distance indicator (number of rooms away) */
            int dist = abs(dx) + abs(dy);
            if (dist > 0 && dist < 10) {
                /* Draw single digit as pixels */
                /* Just draw a dot per room distance */
                for (int d = 0; d < dist && d < 5; d++) {
                    put_pixel(framebuffer,
                              arrow_x + (arrow_dir == 0 ? -2 : (arrow_dir == 2 ? 2 : 0)),
                              arrow_y + d * 3 - (dist * 3 / 2),
                              color);
                }
            }

        } else {
            /* ---- Indoor/different area: corner indicator ---- */
            int corner_x = 4 + i * 20;
            int corner_y = MP_SCREEN_H - 8;

            draw_filled_circle(framebuffer, corner_x, corner_y, 3, color);
            draw_mini_health(framebuffer, corner_x - 6, corner_y + 5,
                             players[i].health, players[i].max_health, color);
        }
    }
}

void mp_indicators_set_enabled(bool enabled) { g_enabled = enabled; }
bool mp_indicators_get_enabled(void) { return g_enabled; }
void mp_indicators_set_style(MPIndicatorStyle style) { g_style = style; }
