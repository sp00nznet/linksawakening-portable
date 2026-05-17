/**
 * @file mp_sprites.h
 * @brief Remote player sprite compositing
 *
 * Extracts Link's OAM sprite data from one player's GBContext and renders
 * it onto another player's framebuffer. This makes other players visible
 * as actual Link sprites (not just indicators).
 *
 * All compositing happens host-side before framebuffer compression/sending,
 * so no protocol changes are needed.
 */

#ifndef MP_SPRITES_H
#define MP_SPRITES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBContext GBContext;

/**
 * Composite a remote player's Link sprite onto a local player's framebuffer.
 *
 * Reads OAM + VRAM + palette data from remote_ctx to find Link's sprites,
 * then renders them (with color shifting) onto the framebuffer.
 *
 * @param framebuffer     Local player's 160x144 ARGB8888 buffer (modified in place)
 * @param local_ctx       Local player's GBContext (for room/scroll info)
 * @param remote_ctx      Remote player's GBContext (source of sprite data)
 * @param remote_color_h  Remote player's chosen hue (0-360)
 * @param remote_color_s  Remote player's chosen saturation (0-1)
 * @param remote_color_v  Remote player's chosen value (0-1)
 */
void mp_sprites_composite_player(uint32_t* framebuffer,
                                  GBContext* local_ctx,
                                  GBContext* remote_ctx,
                                  float remote_color_h,
                                  float remote_color_s,
                                  float remote_color_v);

/**
 * Check if two contexts are in the same room and both in normal gameplay
 * (safe to composite sprites).
 */
bool mp_sprites_can_composite(GBContext* local_ctx, GBContext* remote_ctx);

#ifdef __cplusplus
}
#endif

#endif /* MP_SPRITES_H */
