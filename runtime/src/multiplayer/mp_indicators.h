/**
 * @file mp_indicators.h
 * @brief Player position indicators on other players' screens
 *
 * When multiple players are in the same room, render colored arrow
 * indicators showing where each other player's Link is. When players
 * are in different rooms, show directional arrows at screen edges
 * pointing toward their general map direction.
 */

#ifndef MP_INDICATORS_H
#define MP_INDICATORS_H

#include <stdint.h>
#include <stdbool.h>
#include "mp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Render player indicators onto a framebuffer.
 *
 * @param framebuffer   160x144 ARGB8888 framebuffer (modified in place)
 * @param local_slot    The viewing player's slot
 * @param local_room    Viewing player's current room
 * @param local_indoor  Viewing player's indoor flag
 * @param players       Array of all player states
 * @param player_count  Number of entries in players array
 */
void mp_indicators_render(uint32_t* framebuffer,
                          int local_slot,
                          uint8_t local_room,
                          uint8_t local_indoor,
                          const void* players,  /* MPPlayerState* */
                          int player_count);

/** Enable/disable indicators */
void mp_indicators_set_enabled(bool enabled);
bool mp_indicators_get_enabled(void);

/** Set indicator style */
typedef enum {
    MP_INDICATOR_ARROW,      /* Colored arrow above Link */
    MP_INDICATOR_DOT,        /* Colored dot */
    MP_INDICATOR_NAME,       /* Player name text (requires font) */
    MP_INDICATOR_GLOW,       /* Colored glow around Link sprite */
} MPIndicatorStyle;

void mp_indicators_set_style(MPIndicatorStyle style);

#ifdef __cplusplus
}
#endif

#endif /* MP_INDICATORS_H */
