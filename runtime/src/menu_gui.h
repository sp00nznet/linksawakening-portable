/**
 * GameBoy Recompiled - ImGui Menu System
 *
 * xemu-inspired settings overlay + debug menu.
 * Toggle with ESC (settings) or F2 (debug).
 */

#ifndef GBRT_MENU_GUI_H
#define GBRT_MENU_GUI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct GBContext GBContext;

/* Initialize menu (call after ImGui context is created) */
void menu_gui_init(void);

/* Draw all active menu windows. Call between ImGui::NewFrame() and ImGui::Render() */
void menu_gui_draw(GBContext* ctx);

/* Toggle visibility */
void menu_gui_toggle_settings(void);
void menu_gui_toggle_debug(void);

/* Returns 1 if any menu is currently visible (should block game input) */
int menu_gui_is_active(void);

/* Accessors for settings that platform_sdl needs */
int   menu_gui_get_scale(void);
void  menu_gui_set_scale(int scale);
int   menu_gui_get_speed_percent(void);
int   menu_gui_get_palette_idx(void);
int   menu_gui_get_vsync(void);
float menu_gui_get_master_volume(void);
int   menu_gui_get_show_fps(void);
int   menu_gui_get_filter_mode(void);
int   menu_gui_get_volume(void);
int   menu_gui_get_scanlines(void);
int   menu_gui_get_auto_save(void);

/* Signal quit requested */
int   menu_gui_quit_requested(void);

/* ---- Input binding system ---- */

/* Game Boy buttons (used as indices into binding arrays) */
enum {
    GB_BTN_UP = 0,
    GB_BTN_DOWN,
    GB_BTN_LEFT,
    GB_BTN_RIGHT,
    GB_BTN_A,
    GB_BTN_B,
    GB_BTN_SELECT,
    GB_BTN_START,
    GB_BTN_COUNT
};

/* Key binding: two keyboard scancodes per button */
typedef struct {
    int key0;  /* SDL_Scancode, -1 = unset */
    int key1;  /* SDL_Scancode, -1 = unset */
} KeyBind;

/* Gamepad binding: SDL_GameControllerButton or axis, -1 = unset */
typedef struct {
    int button;    /* SDL_GameControllerButton (primary), -1 = unset */
    int button1;   /* SDL_GameControllerButton (alt), -1 = unset */
    int axis;      /* SDL_GameControllerAxis, -1 = unset */
    int axis_dir;  /* +1 or -1 for axis direction */
} PadBind;

/* Get current bindings (array of GB_BTN_COUNT) */
const KeyBind* menu_gui_get_key_binds(void);
const PadBind* menu_gui_get_pad_binds(void);

/* Save/load bindings to/from config file */
void menu_gui_save_bindings(void);
void menu_gui_load_bindings(void);

/* Get analog stick deadzone (0.0-1.0) */
float menu_gui_get_deadzone(void);

/* Save state request flags (set by menu, consumed by platform) */
int  menu_gui_save_state_requested(void);
int  menu_gui_load_state_requested(void);
void menu_gui_clear_save_state_request(void);
void menu_gui_clear_load_state_request(void);

#ifdef __cplusplus
}
#endif

#endif /* GBRT_MENU_GUI_H */
