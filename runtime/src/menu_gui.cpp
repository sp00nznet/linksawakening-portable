/**
 * GameBoy Recompiled - ImGui Menu System
 *
 * Windows-style menu bar always visible at top of window.
 * File, Config, Graphics, Sound, Controller menus with dropdown items.
 * Debug window (F2) with game state, cheats, memory inspector.
 */

#include "imgui.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern "C" {
#include "menu_gui.h"
#include "asset_viewer.h"
#include "gbrt.h"
}

#include "multiplayer/mp_menu.h"
#include "multiplayer/mp_session.h"

/* ================================================================
 * Menu state
 * ================================================================ */

static struct {
    bool quit_requested;

    /* Window visibility */
    bool show_debug;
    bool show_about;
    bool show_controller;

    /* Graphics settings */
    int   scale;              /* 1-8 */
    bool  vsync;
    int   filter_mode;       /* 0=Nearest, 1=Linear, 2=Scale2x */
    bool  scanlines;
    int   palette_idx;       /* 0=Original, 1=Green, 2=B&W, 3=Amber */

    /* Sound settings */
    int   master_volume;      /* 0-100 percent */
    bool  mute;
    bool  ch1_enabled;
    bool  ch2_enabled;
    bool  ch3_enabled;
    bool  ch4_enabled;

    /* Speed */
    int   speed_percent;
    bool  show_fps;
    bool  auto_save;

    /* Debug cheats */
    bool  invincible;
    bool  unlimited_rupees;
    bool  unlimited_bombs;
    bool  unlimited_arrows;
    bool  unlimited_powder;

    /* Save state requests */
    bool  save_state_requested;
    bool  load_state_requested;

} g_menu = {
    false,
    /* Windows */
    false, false, false,
    /* Graphics */
    3, true, 0, false, 0,
    /* Sound */
    100, false, true, true, true, true,
    /* Speed */
    100, false, false,
    /* Cheats */
    false, false, false, false, false,
    /* Save state */
    false, false,
};

/* ================================================================
 * Input binding state
 * ================================================================ */

static const char* gb_btn_names[GB_BTN_COUNT] = {
    "Up", "Down", "Left", "Right", "A", "B", "Select", "Start"
};

/* Default keyboard bindings */
static KeyBind g_key_binds[GB_BTN_COUNT] = {
    { SDL_SCANCODE_UP,     SDL_SCANCODE_W },         /* Up */
    { SDL_SCANCODE_DOWN,   SDL_SCANCODE_S },         /* Down */
    { SDL_SCANCODE_LEFT,   SDL_SCANCODE_A },         /* Left */
    { SDL_SCANCODE_RIGHT,  SDL_SCANCODE_D },         /* Right */
    { SDL_SCANCODE_Z,      SDL_SCANCODE_J },         /* A */
    { SDL_SCANCODE_X,      SDL_SCANCODE_K },         /* B */
    { SDL_SCANCODE_RSHIFT, SDL_SCANCODE_BACKSPACE }, /* Select */
    { SDL_SCANCODE_RETURN, -1 },                     /* Start */
};

/* Default gamepad bindings */
static PadBind g_pad_binds[GB_BTN_COUNT] = {
    { SDL_CONTROLLER_BUTTON_DPAD_UP,    -1, SDL_CONTROLLER_AXIS_LEFTY, -1 }, /* Up */
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN,  -1, SDL_CONTROLLER_AXIS_LEFTY, +1 }, /* Down */
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT,  -1, SDL_CONTROLLER_AXIS_LEFTX, -1 }, /* Left */
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, -1, SDL_CONTROLLER_AXIS_LEFTX, +1 }, /* Right */
    { SDL_CONTROLLER_BUTTON_A,          -1, -1, 0 },                         /* A */
    { SDL_CONTROLLER_BUTTON_B,          -1, -1, 0 },                         /* B */
    { SDL_CONTROLLER_BUTTON_BACK,       SDL_CONTROLLER_BUTTON_X, -1, 0 },    /* Select (+ X) */
    { SDL_CONTROLLER_BUTTON_START,      SDL_CONTROLLER_BUTTON_Y, -1, 0 },    /* Start  (+ Y) */
};

static float g_deadzone = 0.2f;

/* Rebinding state */
static int g_rebind_btn = -1;       /* Which GB button we're rebinding, -1 = none */
static int g_rebind_slot = 0;       /* 0 = key0, 1 = key1, 2 = pad button */
static bool g_rebind_is_pad = false;

static const char* scancode_name(int sc) {
    if (sc < 0) return "---";
    const char* name = SDL_GetScancodeName((SDL_Scancode)sc);
    return (name && name[0]) ? name : "???";
}

static const char* pad_button_name(int btn) {
    if (btn < 0) return "---";
    static const char* names[] = {
        "A/Cross", "B/Circle", "X/Square", "Y/Triangle",
        "Back/Share", "Guide", "Start/Options",
        "L-Stick", "R-Stick", "LB/L1", "RB/R1",
        "D-Up", "D-Down", "D-Left", "D-Right",
        "Misc1", "Paddle1", "Paddle2", "Paddle3", "Paddle4", "Touchpad"
    };
    if (btn >= 0 && btn < (int)(sizeof(names)/sizeof(names[0])))
        return names[btn];
    return "???";
}

static const char* pad_axis_name(int axis, int dir) {
    if (axis < 0) return "---";
    static char buf[32];
    const char* axis_names[] = { "L-X", "L-Y", "R-X", "R-Y", "L-Trig", "R-Trig" };
    const char* an = (axis >= 0 && axis < 6) ? axis_names[axis] : "?";
    snprintf(buf, sizeof(buf), "%s %s", an, dir > 0 ? "+" : "-");
    return buf;
}

/* Config file path */
static void get_config_path(char* buf, size_t size) {
    char* base = SDL_GetBasePath();
    if (base) {
        snprintf(buf, size, "%sbindings.cfg", base);
        SDL_free(base);
    } else {
        snprintf(buf, size, "bindings.cfg");
    }
}

extern "C" void menu_gui_save_bindings(void) {
    char path[512];
    get_config_path(path, sizeof(path));
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < GB_BTN_COUNT; i++) {
        fprintf(f, "key %d %d %d\n", i, g_key_binds[i].key0, g_key_binds[i].key1);
        fprintf(f, "pad %d %d %d %d %d\n", i, g_pad_binds[i].button, g_pad_binds[i].button1, g_pad_binds[i].axis, g_pad_binds[i].axis_dir);
    }
    fprintf(f, "deadzone %f\n", g_deadzone);
    /* Graphics/Sound settings */
    fprintf(f, "scale %d\n", g_menu.scale);
    fprintf(f, "vsync %d\n", g_menu.vsync ? 1 : 0);
    fprintf(f, "filter %d\n", g_menu.filter_mode);
    fprintf(f, "palette %d\n", g_menu.palette_idx);
    fprintf(f, "volume %d\n", g_menu.master_volume);
    fprintf(f, "speed %d\n", g_menu.speed_percent);
    fprintf(f, "show_fps %d\n", g_menu.show_fps ? 1 : 0);
    fprintf(f, "scanlines %d\n", g_menu.scanlines ? 1 : 0);
    fprintf(f, "auto_save %d\n", g_menu.auto_save ? 1 : 0);
    fclose(f);
    fprintf(stderr, "[MENU] Settings saved to %s\n", path);
}

extern "C" void menu_gui_load_bindings(void) {
    char path[512];
    get_config_path(path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int idx, v0, v1, v2, v3;
        float dz;
        if (sscanf(line, "key %d %d %d", &idx, &v0, &v1) == 3 && idx >= 0 && idx < GB_BTN_COUNT) {
            g_key_binds[idx].key0 = v0;
            g_key_binds[idx].key1 = v1;
        } else if (sscanf(line, "pad %d %d %d %d %d", &idx, &v0, &v1, &v2, &v3) == 5 && idx >= 0 && idx < GB_BTN_COUNT) {
            g_pad_binds[idx].button = v0;
            g_pad_binds[idx].button1 = v1;
            g_pad_binds[idx].axis = v2;
            g_pad_binds[idx].axis_dir = v3;
        } else if (sscanf(line, "deadzone %f", &dz) == 1) {
            g_deadzone = dz;
        }
        /* Graphics/Sound settings */
        int ival;
        if (sscanf(line, "scale %d", &ival) == 1 && ival >= 1 && ival <= 8)
            g_menu.scale = ival;
        else if (sscanf(line, "vsync %d", &ival) == 1)
            g_menu.vsync = ival != 0;
        else if (sscanf(line, "filter %d", &ival) == 1)
            g_menu.filter_mode = ival;
        else if (sscanf(line, "palette %d", &ival) == 1 && ival >= 0 && ival < 4)
            g_menu.palette_idx = ival;
        else if (sscanf(line, "volume %d", &ival) == 1 && ival >= 0 && ival <= 100)
            g_menu.master_volume = ival;
        else if (sscanf(line, "speed %d", &ival) == 1 && ival >= 10 && ival <= 500)
            g_menu.speed_percent = ival;
        else if (sscanf(line, "show_fps %d", &ival) == 1)
            g_menu.show_fps = ival != 0;
        else if (sscanf(line, "scanlines %d", &ival) == 1)
            g_menu.scanlines = ival != 0;
        else if (sscanf(line, "auto_save %d", &ival) == 1)
            g_menu.auto_save = ival != 0;
    }
    fclose(f);
    fprintf(stderr, "[MENU] Settings loaded from %s\n", path);
}

extern "C" const KeyBind* menu_gui_get_key_binds(void) { return g_key_binds; }
extern "C" const PadBind* menu_gui_get_pad_binds(void) { return g_pad_binds; }
extern "C" float menu_gui_get_deadzone(void) { return g_deadzone; }

/* ================================================================
 * Color theme (xemu-inspired dark green)
 * ================================================================ */

static void apply_theme(void)
{
    ImGuiStyle &style = ImGui::GetStyle();
    ImVec4 *colors = style.Colors;

    colors[ImGuiCol_WindowBg]           = ImVec4(0.10f, 0.10f, 0.10f, 0.94f);
    colors[ImGuiCol_PopupBg]            = ImVec4(0.08f, 0.08f, 0.08f, 0.96f);
    colors[ImGuiCol_Border]             = ImVec4(0.30f, 0.30f, 0.30f, 0.50f);
    colors[ImGuiCol_Text]               = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]       = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);

    colors[ImGuiCol_Header]             = ImVec4(0.20f, 0.55f, 0.20f, 0.80f);
    colors[ImGuiCol_HeaderHovered]      = ImVec4(0.25f, 0.65f, 0.25f, 0.80f);
    colors[ImGuiCol_HeaderActive]       = ImVec4(0.30f, 0.75f, 0.30f, 1.00f);

    colors[ImGuiCol_Button]             = ImVec4(0.20f, 0.55f, 0.20f, 0.65f);
    colors[ImGuiCol_ButtonHovered]      = ImVec4(0.25f, 0.65f, 0.25f, 0.80f);
    colors[ImGuiCol_ButtonActive]       = ImVec4(0.30f, 0.75f, 0.30f, 1.00f);

    colors[ImGuiCol_FrameBg]            = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]     = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgActive]      = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);

    colors[ImGuiCol_SliderGrab]         = ImVec4(0.30f, 0.70f, 0.30f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]   = ImVec4(0.35f, 0.80f, 0.35f, 1.00f);
    colors[ImGuiCol_CheckMark]          = ImVec4(0.30f, 0.80f, 0.30f, 1.00f);

    colors[ImGuiCol_Tab]                = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_TabHovered]         = ImVec4(0.25f, 0.60f, 0.25f, 0.80f);
    colors[ImGuiCol_TabActive]          = ImVec4(0.20f, 0.55f, 0.20f, 1.00f);
    /* ImGuiCol_TabSelected added in 1.90.5+, use TabActive for older versions */
#if IMGUI_VERSION_NUM >= 19050
    colors[ImGuiCol_TabSelected]        = ImVec4(0.20f, 0.55f, 0.20f, 1.00f);
#endif

    colors[ImGuiCol_Separator]          = ImVec4(0.30f, 0.30f, 0.30f, 0.50f);
    colors[ImGuiCol_TitleBg]            = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive]      = ImVec4(0.15f, 0.40f, 0.15f, 1.00f);

    colors[ImGuiCol_MenuBarBg]          = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);

    colors[ImGuiCol_ScrollbarBg]        = ImVec4(0.10f, 0.10f, 0.10f, 0.50f);
    colors[ImGuiCol_ScrollbarGrab]      = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);

    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.TabRounding       = 4.0f;
    style.WindowPadding     = ImVec2(8, 8);
    style.FramePadding      = ImVec2(6, 3);
    style.ItemSpacing       = ImVec2(6, 4);
    style.WindowBorderSize  = 1.0f;
}

/* ================================================================
 * WRAM helpers
 * ================================================================ */

/* Link's Awakening DX memory map (key addresses) */
#define LADX_GAMEPLAY_TYPE    0xDB95
#define LADX_GAMEPLAY_SUBTYPE 0xDB96
#define LADX_LINK_HEALTH      0xDB5A
#define LADX_LINK_MAX_HEALTH  0xDB5B
#define LADX_RUPEES_HIGH      0xDB5D
#define LADX_RUPEES_LOW       0xDB5E
#define LADX_BOMBS            0xDB4D
#define LADX_ARROWS           0xDB45
#define LADX_POWDER           0xDB4C
#define LADX_MAP_ROOM         0xD401
#define LADX_IS_INDOOR        0xD402
#define LADX_DUNGEON_IDX      0xDB83
#define LADX_KEYS             0xDB86

static uint8_t read_wram(GBContext* ctx, uint16_t addr) {
    if (!ctx || !ctx->wram) return 0;
    if (addr >= 0xC000 && addr < 0xE000)
        return ctx->wram[addr - 0xC000];
    return 0;
}

static void write_wram(GBContext* ctx, uint16_t addr, uint8_t value) {
    if (!ctx || !ctx->wram) return;
    if (addr >= 0xC000 && addr < 0xE000)
        ctx->wram[addr - 0xC000] = value;
}

/* ================================================================
 * Main menu bar (always visible)
 * ================================================================ */

static void draw_menu_bar(GBContext* ctx)
{
    if (ImGui::BeginMainMenuBar()) {
        /* ---- File ---- */
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save State", "F5")) {
                g_menu.save_state_requested = true;
            }
            if (ImGui::MenuItem("Load State", "F7")) {
                g_menu.load_state_requested = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) {
                menu_gui_save_bindings();
                g_menu.quit_requested = true;
            }
            ImGui::EndMenu();
        }

        /* ---- Config ---- */
        if (ImGui::BeginMenu("Config")) {
            ImGui::MenuItem("Auto Save", NULL, &g_menu.auto_save);
            ImGui::MenuItem("Show FPS", NULL, &g_menu.show_fps);
            ImGui::Separator();
            ImGui::SliderInt("Speed %", &g_menu.speed_percent, 10, 500);
            if (ImGui::MenuItem("Reset Speed")) g_menu.speed_percent = 100;
            ImGui::Separator();
            ImGui::MenuItem("Debug Window", "F2", &g_menu.show_debug);
            ImGui::EndMenu();
        }

        /* ---- Graphics ---- */
        if (ImGui::BeginMenu("Graphics")) {
            if (ImGui::BeginMenu("Window Scale")) {
                const char* scale_labels[] = {
                    "1x (160x144)", "2x (320x288)", "3x (480x432)", "4x (640x576)",
                    "5x (800x720)", "6x (960x864)", "7x (1120x1008)", "8x (1280x1152)"
                };
                for (int i = 0; i < 8; i++) {
                    if (ImGui::MenuItem(scale_labels[i], NULL, g_menu.scale == (i + 1))) {
                        g_menu.scale = i + 1;
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::MenuItem("V-Sync", NULL, &g_menu.vsync);
            ImGui::Separator();

            if (ImGui::BeginMenu("Texture Filter")) {
                if (ImGui::MenuItem("Nearest (sharp pixels)", NULL, g_menu.filter_mode == 0))
                    g_menu.filter_mode = 0;
                if (ImGui::MenuItem("Bilinear (smooth)", NULL, g_menu.filter_mode == 1))
                    g_menu.filter_mode = 1;
                if (ImGui::MenuItem("Scale2x (smooth pixels)", NULL, g_menu.filter_mode == 2))
                    g_menu.filter_mode = 2;
                ImGui::EndMenu();
            }

            ImGui::MenuItem("Scanlines", NULL, &g_menu.scanlines);

            if (ImGui::BeginMenu("Color Palette")) {
                const char* pal_names[] = { "Original (CGB Color)", "Classic Green (DMG)",
                                            "Black & White (Pocket)", "Amber (Plasma)" };
                for (int i = 0; i < 4; i++) {
                    if (ImGui::MenuItem(pal_names[i], NULL, g_menu.palette_idx == i))
                        g_menu.palette_idx = i;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        /* ---- Sound ---- */
        if (ImGui::BeginMenu("Sound")) {
            ImGui::SliderInt("Volume", &g_menu.master_volume, 0, 100, "%d%%");
            ImGui::MenuItem("Mute", NULL, &g_menu.mute);
            ImGui::Separator();
            ImGui::MenuItem("CH1 - Pulse A", NULL, &g_menu.ch1_enabled);
            ImGui::MenuItem("CH2 - Pulse B", NULL, &g_menu.ch2_enabled);
            ImGui::MenuItem("CH3 - Wave",    NULL, &g_menu.ch3_enabled);
            ImGui::MenuItem("CH4 - Noise",   NULL, &g_menu.ch4_enabled);
            ImGui::EndMenu();
        }

        /* ---- Tools ---- */
        if (ImGui::BeginMenu("Tools")) {
            bool av_visible = asset_viewer_is_visible();
            if (ImGui::MenuItem("Asset Viewer", NULL, av_visible)) {
                asset_viewer_set_visible(!av_visible);
            }
            ImGui::EndMenu();
        }

        /* ---- Controller ---- */
        if (ImGui::BeginMenu("Controller")) {
            ImGui::MenuItem("Controller Settings", NULL, &g_menu.show_controller);
            ImGui::EndMenu();
        }

        /* ---- Multiplayer ---- */
        mp_menu_draw_menu_item(ctx);

        /* ---- Help ---- */
        if (ImGui::BeginMenu("Help")) {
            ImGui::MenuItem("About", NULL, &g_menu.show_about);
            ImGui::EndMenu();
        }

        /* FPS in menu bar */
        if (g_menu.show_fps) {
            float bar_w = ImGui::GetWindowWidth();
            char fps_text[32];
            snprintf(fps_text, sizeof(fps_text), "%.1f FPS", ImGui::GetIO().Framerate);
            float text_w = ImGui::CalcTextSize(fps_text).x;
            ImGui::SameLine(bar_w - text_w - 10);
            ImGui::TextDisabled("%s", fps_text);
        }

        ImGui::EndMainMenuBar();
    }
}

/* ================================================================
 * Debug window
 * ================================================================ */

static void draw_debug_window(GBContext* ctx)
{
    ImGui::SetNextWindowSize(ImVec2(350, 500), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Debug", &g_menu.show_debug)) {
        ImGuiIO& io = ImGui::GetIO();

        if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("%.1f FPS (%.3f ms/frame)",
                io.Framerate, 1000.0f / io.Framerate);

            static float fps_history[120] = {};
            static int fps_idx = 0;
            fps_history[fps_idx] = io.Framerate;
            fps_idx = (fps_idx + 1) % 120;
            ImGui::PlotLines("FPS", fps_history, 120, fps_idx, NULL, 0.0f, 120.0f, ImVec2(0, 60));
        }

        if (ctx) {
            if (ImGui::CollapsingHeader("CPU Registers", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("AF=%04X  BC=%04X  DE=%04X  HL=%04X",
                    ctx->af, ctx->bc, ctx->de, ctx->hl);
                ImGui::Text("SP=%04X  PC=%04X  Bank=%d",
                    ctx->sp, ctx->pc, ctx->rom_bank);
                ImGui::Text("Flags: Z=%d N=%d H=%d C=%d  IME=%d",
                    ctx->f_z, ctx->f_n, ctx->f_h, ctx->f_c, ctx->ime);
                ImGui::Text("Halted=%d  Cycles=%u", ctx->halted, ctx->cycles);
            }

            if (ImGui::CollapsingHeader("Game State", ImGuiTreeNodeFlags_DefaultOpen)) {
                uint8_t gp_type = read_wram(ctx, LADX_GAMEPLAY_TYPE);
                uint8_t gp_sub  = read_wram(ctx, LADX_GAMEPLAY_SUBTYPE);
                uint8_t room    = read_wram(ctx, LADX_MAP_ROOM);
                uint8_t indoor  = read_wram(ctx, LADX_IS_INDOOR);
                uint8_t dungeon = read_wram(ctx, LADX_DUNGEON_IDX);

                const char* state_names[] = {
                    "World", "Inventory", "File Select", "Name Entry",
                    "Intro", "Ending", "Photo", "Color Dungeon Prompt",
                    "Map Transition", "Dungeon", "Side-Scroll", "Game Over"
                };
                const char* state_name = (gp_type < 12) ? state_names[gp_type] : "Unknown";

                ImGui::Text("GameplayType: %02X (%s)", gp_type, state_name);
                ImGui::Text("SubType: %02X", gp_sub);
                ImGui::Text("Room: %02X  %s  Dungeon: %d", room,
                    indoor ? "Indoor" : "Overworld", dungeon);
            }

            if (ImGui::CollapsingHeader("Link Stats")) {
                uint8_t health     = read_wram(ctx, LADX_LINK_HEALTH);
                uint8_t max_health = read_wram(ctx, LADX_LINK_MAX_HEALTH);
                uint8_t rupees_h   = read_wram(ctx, LADX_RUPEES_HIGH);
                uint8_t rupees_l   = read_wram(ctx, LADX_RUPEES_LOW);
                uint8_t bombs      = read_wram(ctx, LADX_BOMBS);
                uint8_t arrows     = read_wram(ctx, LADX_ARROWS);
                uint8_t powder     = read_wram(ctx, LADX_POWDER);
                uint8_t keys       = read_wram(ctx, LADX_KEYS);
                int rupees = (rupees_h << 8) | rupees_l;

                ImGui::Text("Health: %d/%d (%.1f/%.1f hearts)",
                    health, max_health, health / 8.0f, max_health / 8.0f);
                float health_frac = max_health > 0 ? (float)health / max_health : 0;
                ImGui::ProgressBar(health_frac, ImVec2(-1, 0), "");
                ImGui::Text("Rupees: %d", rupees);
                ImGui::Text("Bombs: %d  Arrows: %d  Powder: %d", bombs, arrows, powder);
                ImGui::Text("Keys: %d", keys);
            }

            if (ImGui::CollapsingHeader("Cheats")) {
                ImGui::Checkbox("Invincible", &g_menu.invincible);
                ImGui::Checkbox("Max Rupees", &g_menu.unlimited_rupees);
                ImGui::Checkbox("Max Bombs", &g_menu.unlimited_bombs);
                ImGui::Checkbox("Max Arrows", &g_menu.unlimited_arrows);
                ImGui::Checkbox("Max Magic Powder", &g_menu.unlimited_powder);

                if (g_menu.invincible) {
                    uint8_t max_hp = read_wram(ctx, LADX_LINK_MAX_HEALTH);
                    if (max_hp > 0) write_wram(ctx, LADX_LINK_HEALTH, max_hp);
                }
                if (g_menu.unlimited_rupees) {
                    write_wram(ctx, LADX_RUPEES_HIGH, 0x03);
                    write_wram(ctx, LADX_RUPEES_LOW, 0xE7);
                }
                if (g_menu.unlimited_bombs)  write_wram(ctx, LADX_BOMBS, 99);
                if (g_menu.unlimited_arrows) write_wram(ctx, LADX_ARROWS, 99);
                if (g_menu.unlimited_powder) write_wram(ctx, LADX_POWDER, 99);
            }

            if (ImGui::CollapsingHeader("Memory Inspector")) {
                static int inspect_addr = 0xC000;
                ImGui::InputInt("Address", &inspect_addr, 16, 256, ImGuiInputTextFlags_CharsHexadecimal);

                if (inspect_addr >= 0xC000 && inspect_addr < 0xDFF0) {
                    uint16_t addr = (uint16_t)inspect_addr;
                    ImGui::Text("%04X: %02X %02X %02X %02X  %02X %02X %02X %02X",
                        addr,
                        read_wram(ctx, addr), read_wram(ctx, addr+1),
                        read_wram(ctx, addr+2), read_wram(ctx, addr+3),
                        read_wram(ctx, addr+4), read_wram(ctx, addr+5),
                        read_wram(ctx, addr+6), read_wram(ctx, addr+7));
                    ImGui::Text("%04X: %02X %02X %02X %02X  %02X %02X %02X %02X",
                        addr+8,
                        read_wram(ctx, addr+8), read_wram(ctx, addr+9),
                        read_wram(ctx, addr+10), read_wram(ctx, addr+11),
                        read_wram(ctx, addr+12), read_wram(ctx, addr+13),
                        read_wram(ctx, addr+14), read_wram(ctx, addr+15));
                } else if (inspect_addr >= 0xFF00 && inspect_addr < 0xFF80 && ctx->io) {
                    uint16_t off = inspect_addr - 0xFF00;
                    ImGui::Text("%04X: %02X %02X %02X %02X  %02X %02X %02X %02X",
                        inspect_addr,
                        ctx->io[off], ctx->io[off+1], ctx->io[off+2], ctx->io[off+3],
                        ctx->io[off+4], ctx->io[off+5], ctx->io[off+6], ctx->io[off+7]);
                } else {
                    ImGui::TextColored(ImVec4(1,0.5f,0.5f,1), "Address out of WRAM/IO range");
                }
            }

            if (ImGui::CollapsingHeader("IO Registers")) {
                if (ctx->io) {
                    ImGui::Text("LCDC=%02X STAT=%02X SCY=%02X SCX=%02X LY=%02X",
                        ctx->io[0x40], ctx->io[0x41], ctx->io[0x42], ctx->io[0x43], ctx->io[0x44]);
                    ImGui::Text("BGP=%02X  OBP0=%02X OBP1=%02X WY=%02X  WX=%02X",
                        ctx->io[0x47], ctx->io[0x48], ctx->io[0x49], ctx->io[0x4A], ctx->io[0x4B]);
                    ImGui::Text("IE=%02X   IF=%02X   JOYP=%02X",
                        ctx->io[0x7F], ctx->io[0x0F], ctx->io[0x00]);
                    ImGui::Text("VBK=%02X  SVBK=%02X KEY1=%02X",
                        ctx->io[0x4F], ctx->io[0x70], ctx->io[0x4D]);
                }
            }
        }
    }
    ImGui::End();
}

/* ================================================================
 * Controller window
 * ================================================================ */

static void draw_controller_window(void)
{
    ImGui::SetNextWindowSize(ImVec2(480, 460), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Controller Settings", &g_menu.show_controller)) {

        /* ---- Rebind listening popup ---- */
        if (g_rebind_btn >= 0) {
            ImGui::OpenPopup("Rebind");
        }
        if (ImGui::BeginPopupModal("Rebind", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::Text("Press a %s for \"%s\"...",
                g_rebind_is_pad ? "button/axis" : "key",
                gb_btn_names[g_rebind_btn]);
            ImGui::Text("(ESC to cancel, Delete to clear)");

            if (!g_rebind_is_pad) {
                /* Listen for keyboard */
                for (int sc = 0; sc < SDL_NUM_SCANCODES; sc++) {
                    if (ImGui::IsKeyPressed((ImGuiKey)SDL_SCANCODE_TO_KEYCODE(sc), false))
                        continue; /* ImGui key mapping doesn't work this way, use SDL below */
                }
                /* Poll SDL keyboard state directly */
                const Uint8* state = SDL_GetKeyboardState(NULL);
                if (state[SDL_SCANCODE_ESCAPE]) {
                    g_rebind_btn = -1;
                    ImGui::CloseCurrentPopup();
                } else if (state[SDL_SCANCODE_DELETE]) {
                    if (g_rebind_slot == 0)
                        g_key_binds[g_rebind_btn].key0 = -1;
                    else
                        g_key_binds[g_rebind_btn].key1 = -1;
                    g_rebind_btn = -1;
                    ImGui::CloseCurrentPopup();
                } else {
                    for (int sc = 0; sc < SDL_NUM_SCANCODES; sc++) {
                        if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_DELETE) continue;
                        if (state[sc]) {
                            if (g_rebind_slot == 0)
                                g_key_binds[g_rebind_btn].key0 = sc;
                            else
                                g_key_binds[g_rebind_btn].key1 = sc;
                            g_rebind_btn = -1;
                            ImGui::CloseCurrentPopup();
                            break;
                        }
                    }
                }
            } else {
                /* Listen for gamepad */
                const Uint8* kbstate = SDL_GetKeyboardState(NULL);
                if (kbstate[SDL_SCANCODE_ESCAPE]) {
                    g_rebind_btn = -1;
                    ImGui::CloseCurrentPopup();
                } else if (kbstate[SDL_SCANCODE_DELETE]) {
                    if (g_rebind_slot == 0) g_pad_binds[g_rebind_btn].button = -1;
                    else if (g_rebind_slot == 1) g_pad_binds[g_rebind_btn].button1 = -1;
                    else { g_pad_binds[g_rebind_btn].axis = -1; }
                    g_rebind_btn = -1;
                    ImGui::CloseCurrentPopup();
                } else {
                    /* Check all open controllers */
                    for (int ji = 0; ji < SDL_NumJoysticks(); ji++) {
                        if (!SDL_IsGameController(ji)) continue;
                        SDL_GameController* gc = SDL_GameControllerOpen(ji);
                        if (!gc) continue;
                        if (g_rebind_slot <= 1) {
                            /* Listening for a button */
                            for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
                                if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b)) {
                                    if (g_rebind_slot == 0)
                                        g_pad_binds[g_rebind_btn].button = b;
                                    else
                                        g_pad_binds[g_rebind_btn].button1 = b;
                                    g_rebind_btn = -1;
                                    ImGui::CloseCurrentPopup();
                                    break;
                                }
                            }
                        } else {
                            /* Listening for an axis */
                            for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) {
                                int16_t val = SDL_GameControllerGetAxis(gc, (SDL_GameControllerAxis)a);
                                if (val > 16384 || val < -16384) {
                                    g_pad_binds[g_rebind_btn].axis = a;
                                    g_pad_binds[g_rebind_btn].axis_dir = (val > 0) ? 1 : -1;
                                    g_rebind_btn = -1;
                                    ImGui::CloseCurrentPopup();
                                    break;
                                }
                            }
                        }
                        break; /* Only use first controller */
                    }
                }
            }
            ImGui::EndPopup();
        }

        /* ---- Keyboard bindings table ---- */
        ImGui::Text("Keyboard Bindings");
        ImGui::Separator();
        if (ImGui::BeginTable("kb_table", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Key 1", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Key 2", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##clear", ImGuiTableColumnFlags_WidthFixed, 20);
            ImGui::TableHeadersRow();

            for (int i = 0; i < GB_BTN_COUNT; i++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", gb_btn_names[i]);

                ImGui::TableNextColumn();
                char label[32];
                snprintf(label, sizeof(label), "%s##k0_%d", scancode_name(g_key_binds[i].key0), i);
                if (ImGui::Button(label, ImVec2(-1, 0))) {
                    g_rebind_btn = i; g_rebind_slot = 0; g_rebind_is_pad = false;
                }

                ImGui::TableNextColumn();
                snprintf(label, sizeof(label), "%s##k1_%d", scancode_name(g_key_binds[i].key1), i);
                if (ImGui::Button(label, ImVec2(-1, 0))) {
                    g_rebind_btn = i; g_rebind_slot = 1; g_rebind_is_pad = false;
                }

                ImGui::TableNextColumn();
                /* No clear button needed, rebind popup has Delete to clear */
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();

        /* ---- Gamepad bindings table ---- */
        ImGui::Text("Gamepad Bindings");
        ImGui::Separator();

        /* Detect controller */
        const char* ctrl_name = NULL;
        for (int ji = 0; ji < SDL_NumJoysticks(); ji++) {
            if (SDL_IsGameController(ji)) {
                ctrl_name = SDL_GameControllerNameForIndex(ji);
                break;
            }
        }
        if (ctrl_name) {
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.3f, 1.0f), "Connected: %s", ctrl_name);
        } else {
            ImGui::TextDisabled("No gamepad detected");
        }

        if (ImGui::BeginTable("gp_table", 5, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Pad Btn 1", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Pad Btn 2", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Axis", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##clear", ImGuiTableColumnFlags_WidthFixed, 20);
            ImGui::TableHeadersRow();

            for (int i = 0; i < GB_BTN_COUNT; i++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", gb_btn_names[i]);

                ImGui::TableNextColumn();
                char label[48];
                snprintf(label, sizeof(label), "%s##pb0_%d", pad_button_name(g_pad_binds[i].button), i);
                if (ImGui::Button(label, ImVec2(-1, 0))) {
                    g_rebind_btn = i; g_rebind_slot = 0; g_rebind_is_pad = true;
                }

                ImGui::TableNextColumn();
                snprintf(label, sizeof(label), "%s##pb1_%d", pad_button_name(g_pad_binds[i].button1), i);
                if (ImGui::Button(label, ImVec2(-1, 0))) {
                    g_rebind_btn = i; g_rebind_slot = 1; g_rebind_is_pad = true;
                }

                ImGui::TableNextColumn();
                snprintf(label, sizeof(label), "%s##pa_%d", pad_axis_name(g_pad_binds[i].axis, g_pad_binds[i].axis_dir), i);
                if (ImGui::Button(label, ImVec2(-1, 0))) {
                    g_rebind_btn = i; g_rebind_slot = 2; g_rebind_is_pad = true;
                }

                ImGui::TableNextColumn();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::SliderFloat("Stick Deadzone", &g_deadzone, 0.05f, 0.9f, "%.2f");

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Save Bindings")) {
            menu_gui_save_bindings();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Defaults")) {
            /* Reset keyboard */
            g_key_binds[GB_BTN_UP]     = { SDL_SCANCODE_UP,     SDL_SCANCODE_W };
            g_key_binds[GB_BTN_DOWN]   = { SDL_SCANCODE_DOWN,   SDL_SCANCODE_S };
            g_key_binds[GB_BTN_LEFT]   = { SDL_SCANCODE_LEFT,   SDL_SCANCODE_A };
            g_key_binds[GB_BTN_RIGHT]  = { SDL_SCANCODE_RIGHT,  SDL_SCANCODE_D };
            g_key_binds[GB_BTN_A]      = { SDL_SCANCODE_Z,      SDL_SCANCODE_J };
            g_key_binds[GB_BTN_B]      = { SDL_SCANCODE_X,      SDL_SCANCODE_K };
            g_key_binds[GB_BTN_SELECT] = { SDL_SCANCODE_RSHIFT, SDL_SCANCODE_BACKSPACE };
            g_key_binds[GB_BTN_START]  = { SDL_SCANCODE_RETURN, -1 };
            /* Reset gamepad */
            g_pad_binds[GB_BTN_UP]     = { SDL_CONTROLLER_BUTTON_DPAD_UP,    -1, SDL_CONTROLLER_AXIS_LEFTY, -1 };
            g_pad_binds[GB_BTN_DOWN]   = { SDL_CONTROLLER_BUTTON_DPAD_DOWN,  -1, SDL_CONTROLLER_AXIS_LEFTY, +1 };
            g_pad_binds[GB_BTN_LEFT]   = { SDL_CONTROLLER_BUTTON_DPAD_LEFT,  -1, SDL_CONTROLLER_AXIS_LEFTX, -1 };
            g_pad_binds[GB_BTN_RIGHT]  = { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, -1, SDL_CONTROLLER_AXIS_LEFTX, +1 };
            g_pad_binds[GB_BTN_A]      = { SDL_CONTROLLER_BUTTON_A,    -1, -1, 0 };
            g_pad_binds[GB_BTN_B]      = { SDL_CONTROLLER_BUTTON_B,    -1, -1, 0 };
            g_pad_binds[GB_BTN_SELECT] = { SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_X, -1, 0 };
            g_pad_binds[GB_BTN_START]  = { SDL_CONTROLLER_BUTTON_START,SDL_CONTROLLER_BUTTON_Y, -1, 0 };
            g_deadzone = 0.2f;
        }
    }
    ImGui::End();
}

/* ================================================================
 * About window
 * ================================================================ */

static void draw_about_window(void)
{
    ImGui::SetNextWindowSize(ImVec2(340, 280), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About", &g_menu.show_about)) {
        ImGui::Text("Link's Awakening DX");
        ImGui::Text("Static Recompilation for Windows");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::BulletText("Recompiler: gb-recompiled");
        ImGui::BulletText("Functions: 17,805 recompiled");
        ImGui::BulletText("Code: 4.2M lines of generated C");
        ImGui::BulletText("ROM: 1MB, 64 banks, MBC5");
        ImGui::Spacing();
        ImGui::BulletText("Developer: Nintendo (1998)");
        ImGui::BulletText("CPU: Sharp SM83 @ 4/8 MHz");
        ImGui::Spacing();
        ImGui::Text("Credits");
        ImGui::BulletText("gb-recompiled by arcanite24");
        ImGui::BulletText("LADX-Disassembly contributors");
        ImGui::BulletText("SameBoy by LIJI32");
    }
    ImGui::End();
}

/* ================================================================
 * Public API
 * ================================================================ */

extern "C" void menu_gui_init(void)
{
    apply_theme();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL; /* Don't save layout */
    menu_gui_load_bindings();

    /* Initialize multiplayer */
    mp_session_init();
    mp_menu_init();
}

/* Simple settings snapshot for auto-save on change */
static uint32_t settings_hash(void) {
    uint32_t h = 0;
    h = h * 31 + g_menu.scale;
    h = h * 31 + g_menu.vsync;
    h = h * 31 + g_menu.filter_mode;
    h = h * 31 + g_menu.palette_idx;
    h = h * 31 + g_menu.master_volume;
    h = h * 31 + g_menu.speed_percent;
    h = h * 31 + g_menu.show_fps;
    h = h * 31 + g_menu.scanlines;
    h = h * 31 + g_menu.auto_save;
    return h;
}

extern "C" void menu_gui_draw(GBContext* ctx)
{
    static uint32_t prev_hash = 0;
    uint32_t cur_hash = settings_hash();

    /* Always-visible menu bar */
    draw_menu_bar(ctx);

    /* Floating windows opened from menu */
    if (g_menu.show_debug)      draw_debug_window(ctx);
    if (g_menu.show_about)      draw_about_window();
    if (g_menu.show_controller) draw_controller_window();

    /* Asset viewer */
    asset_viewer_draw(ctx);

    /* Multiplayer overlays */
    mp_menu_draw_overlays();

    /* Auto-save settings on change */
    uint32_t new_hash = settings_hash();
    if (prev_hash != 0 && new_hash != prev_hash) {
        menu_gui_save_bindings();
    }
    prev_hash = new_hash;
}

extern "C" void menu_gui_toggle_settings(void)
{
    /* ESC no longer toggles a settings popup — it's always the menu bar.
       Could use ESC to close any open floating windows. */
    if (g_menu.show_debug || g_menu.show_about || g_menu.show_controller) {
        g_menu.show_debug = false;
        g_menu.show_about = false;
        g_menu.show_controller = false;
    }
}

extern "C" void menu_gui_toggle_debug(void)
{
    g_menu.show_debug = !g_menu.show_debug;
}

extern "C" int menu_gui_is_active(void)
{
    /* Menu bar is always active but doesn't block input.
       Only block when a floating window has focus. */
    return (g_menu.show_debug || g_menu.show_about || g_menu.show_controller) ? 1 : 0;
}

extern "C" int menu_gui_get_scale(void)       { return g_menu.scale; }
extern "C" void menu_gui_set_scale(int scale)  { g_menu.scale = scale; }
extern "C" int menu_gui_get_speed_percent(void) { return g_menu.speed_percent; }
extern "C" int menu_gui_get_palette_idx(void)  { return g_menu.palette_idx; }
extern "C" int menu_gui_get_vsync(void)        { return g_menu.vsync ? 1 : 0; }
extern "C" int menu_gui_get_show_fps(void)     { return g_menu.show_fps ? 1 : 0; }
extern "C" int menu_gui_get_filter_mode(void)  { return g_menu.filter_mode; }
extern "C" int menu_gui_get_volume(void)       { return g_menu.master_volume; }
extern "C" int menu_gui_get_scanlines(void)    { return g_menu.scanlines ? 1 : 0; }
extern "C" int menu_gui_get_auto_save(void)    { return g_menu.auto_save ? 1 : 0; }
extern "C" int menu_gui_quit_requested(void)   { return g_menu.quit_requested ? 1 : 0; }

extern "C" float menu_gui_get_master_volume(void)
{
    if (g_menu.mute) return 0.0f;
    return g_menu.master_volume / 100.0f;
}

extern "C" int  menu_gui_save_state_requested(void) { return g_menu.save_state_requested ? 1 : 0; }
extern "C" int  menu_gui_load_state_requested(void) { return g_menu.load_state_requested ? 1 : 0; }
extern "C" void menu_gui_clear_save_state_request(void) { g_menu.save_state_requested = false; }
extern "C" void menu_gui_clear_load_state_request(void) { g_menu.load_state_requested = false; }
