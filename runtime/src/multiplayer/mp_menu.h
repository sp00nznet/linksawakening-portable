/**
 * @file mp_menu.h
 * @brief Multiplayer ImGui menu integration
 *
 * Adds "Multiplayer" menu to the existing menu bar with:
 * - Host Game
 * - Join Game
 * - Settings (name, color, debug overlay)
 * - Debug overlay (connected players, ping, sync)
 */

#ifndef MP_MENU_H
#define MP_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBContext GBContext;

/** Initialize MP menu (call after ImGui context + mp_session_init) */
void mp_menu_init(void);

/** Draw the Multiplayer menu bar item + any open windows.
 *  Call from within menu_gui_draw(), between BeginMainMenuBar/End. */
void mp_menu_draw_menu_item(GBContext* ctx);

/** Draw MP overlay windows (debug overlay, connection status).
 *  Call from menu_gui_draw() after the menu bar. */
void mp_menu_draw_overlays(void);

/** Save/load MP settings to/from config file */
void mp_menu_save_settings(void);
void mp_menu_load_settings(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_MENU_H */
