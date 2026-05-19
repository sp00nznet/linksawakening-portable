/**
 * GameBoy Recompiled — menu_gui stub implementations.
 *
 * Compiled into the runtime when LA_HAS_IMGUI is NOT defined (i.e. on
 * platforms without SDL2/ImGui — libxenon, KOS, etc.). Provides the
 * `menu_gui_*` accessors that `platform_*.c[pp]` calls into so the
 * runtime links without the ImGui menu system.
 *
 * Returns sensible default values; mutating entry points are no-ops.
 *
 * Plain C so it can be compiled by toolchains that don't have a C++
 * compiler installed (e.g. the typical libxenon devkitxenon install
 * which only ships xenon-gcc, not xenon-g++).
 */

#include "menu_gui.h"

void menu_gui_init(void)              {}
void menu_gui_draw(GBContext* ctx)    { (void)ctx; }

void menu_gui_toggle_settings(void)   {}
void menu_gui_toggle_debug(void)      {}
int  menu_gui_is_active(void)         { return 0; }

int   menu_gui_get_scale(void)          { return 3; }
void  menu_gui_set_scale(int s)         { (void)s; }
int   menu_gui_get_speed_percent(void)  { return 100; }
int   menu_gui_get_palette_idx(void)    { return 0; }
int   menu_gui_get_vsync(void)          { return 1; }
float menu_gui_get_master_volume(void)  { return 1.0f; }
int   menu_gui_get_show_fps(void)       { return 0; }
int   menu_gui_get_filter_mode(void)    { return 0; }
int   menu_gui_get_volume(void)         { return 100; }
int   menu_gui_get_scanlines(void)      { return 0; }
int   menu_gui_get_auto_save(void)      { return 1; }
int   menu_gui_quit_requested(void)     { return 0; }

/* Default key bindings: empty (no keyboard on console targets — joypad only) */
static const KeyBind g_stub_kb[GB_BTN_COUNT] = {{0}};
static const PadBind g_stub_pb[GB_BTN_COUNT] = {
    /* UP    */ { 11, -1, 1,  -1 },  /* DPAD_UP    + LeftY- */
    /* DOWN  */ { 12, -1, 1,   1 },  /* DPAD_DOWN  + LeftY+ */
    /* LEFT  */ { 13, -1, 0,  -1 },  /* DPAD_LEFT  + LeftX- */
    /* RIGHT */ { 14, -1, 0,   1 },  /* DPAD_RIGHT + LeftX+ */
    /* A     */ {  0, -1, -1, 0 },   /* A button */
    /* B     */ {  1, -1, -1, 0 },   /* B button */
    /* SELECT*/ {  4,  2, -1, 0 },   /* Back / X */
    /* START */ {  6,  3, -1, 0 },   /* Start / Y */
};

const KeyBind* menu_gui_get_key_binds(void) { return g_stub_kb; }
const PadBind* menu_gui_get_pad_binds(void) { return g_stub_pb; }

void  menu_gui_save_bindings(void)          {}
void  menu_gui_load_bindings(void)          {}
float menu_gui_get_deadzone(void)           { return 0.20f; }

int   menu_gui_save_state_requested(void)        { return 0; }
int   menu_gui_load_state_requested(void)        { return 0; }
void  menu_gui_clear_save_state_request(void)    {}
void  menu_gui_clear_load_state_request(void)    {}
