/**
 * @file mp_color.h
 * @brief Link palette recoloring for multiplayer
 *
 * Each player picks a color (HSV). This module modifies the PPU output
 * to recolor Link's sprite for each player's viewport, so every player's
 * Link appears in their chosen color.
 *
 * How it works:
 * - Link's sprite uses OBP0 palette (OBJ palette 0) on DMG
 * - On CGB, Link uses OBJ palette 0 (8 colors across 4 sub-palettes)
 * - We intercept the framebuffer after PPU render and before display
 * - Identify pixels that came from Link's sprite palette entries
 * - Shift their hue to the player's chosen color
 *
 * This is a post-processing approach: non-invasive, works on the final
 * framebuffer without modifying PPU internals.
 */

#ifndef MP_COLOR_H
#define MP_COLOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Apply Link color recoloring to a framebuffer.
 *
 * @param framebuffer  160x144 ARGB8888 framebuffer (modified in place)
 * @param target_h     Target hue (0-360)
 * @param target_s     Target saturation (0-1)
 * @param target_v     Target value/brightness multiplier (0-1)
 */
void mp_color_recolor_link(uint32_t* framebuffer,
                           float target_h, float target_s, float target_v);

/**
 * Set reference colors for Link's default palette.
 * Call once during initialization with the default green colors.
 * These are the colors we'll detect and recolor.
 */
void mp_color_set_reference_palette(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_COLOR_H */
