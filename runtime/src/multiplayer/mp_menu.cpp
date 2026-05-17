/**
 * @file mp_menu.cpp
 * @brief Multiplayer ImGui menu implementation
 */

#include "mp_menu.h"
#include "mp_session.h"
#include "mp_protocol.h"
#include "mp_trade.h"
#include "mp_voice.h"
#include "mp_pvp.h"
#include "mp_indicators.h"

#include "imgui.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#endif

extern "C" {
#include "gbrt.h"
}

/* Get the local machine's IP address(es) for display */
static void get_local_ip(char* buf, size_t size) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        snprintf(buf, size, "(unknown)");
        return;
    }

    char hostname[256] = {0};
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        struct addrinfo hints = {0}, *result = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(hostname, NULL, &hints, &result) == 0 && result) {
            struct sockaddr_in* addr = (struct sockaddr_in*)result->ai_addr;
            inet_ntop(AF_INET, &addr->sin_addr, buf, (DWORD)size);
            freeaddrinfo(result);
        } else {
            snprintf(buf, size, "(unknown)");
        }
    } else {
        snprintf(buf, size, "(unknown)");
    }

    WSACleanup();
#else
    snprintf(buf, size, "(check ifconfig)");
#endif
}

/* ============================================================================
 * Menu State
 * ========================================================================== */

static struct {
    /* Window visibility */
    bool show_host_dialog;
    bool show_join_dialog;
    bool show_settings;
    bool show_trade;
    bool show_voice;
    bool show_pvp;

    /* Host dialog state */
    int  host_port;

    /* Join dialog state */
    char join_address[128];
    int  join_port;

    /* Settings state (mirrors mp_session, editable) */
    char settings_name[MP_MAX_NAME_LEN];
    float settings_color_h;
    float settings_color_s;
    float settings_color_v;
    bool  settings_debug_overlay;

    /* Status message */
    char status_msg[128];
    float status_timer;

} g_mp_menu = {
    .host_port = MP_DEFAULT_PORT,
    .join_address = "127.0.0.1",
    .join_port = MP_DEFAULT_PORT,
    .settings_color_h = 120.0f,
    .settings_color_s = 0.8f,
    .settings_color_v = 0.8f,
};

/* ============================================================================
 * Helpers
 * ========================================================================== */

static void set_status(const char* msg) {
    strncpy(g_mp_menu.status_msg, msg, sizeof(g_mp_menu.status_msg) - 1);
    g_mp_menu.status_timer = 5.0f; /* show for 5 seconds */
}

/** Convert HSV to ImVec4 (RGB) for color preview */
static ImVec4 hsv_to_rgb(float h, float s, float v) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h / 360.0f, s, v, r, g, b);
    return ImVec4(r, g, b, 1.0f);
}

/* ============================================================================
 * Host Dialog
 * ========================================================================== */

static void draw_host_dialog(GBContext* ctx) {
    ImGui::SetNextWindowSize(ImVec2(380, 580), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Host Multiplayer Game", &g_mp_menu.show_host_dialog)) {
        MPSessionState state = mp_session_get_state();

        if (state == MP_STATE_IDLE) {
            /* Show local IP */
            static char local_ip[64] = {0};
            static bool ip_fetched = false;
            if (!ip_fetched) {
                get_local_ip(local_ip, sizeof(local_ip));
                ip_fetched = true;
            }
            ImGui::Text("Your IP:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "%s", local_ip);
            ImGui::Text("Share this IP with players who want to join.");
            ImGui::Spacing();

            ImGui::InputInt("Port", &g_mp_menu.host_port);
            if (g_mp_menu.host_port < 1024) g_mp_menu.host_port = 1024;
            if (g_mp_menu.host_port > 65535) g_mp_menu.host_port = 65535;

            /* ---- Starting Equipment for joining players ---- */
            ImGui::Separator();
            ImGui::Text("Joining Player Equipment:");

            static int min_hearts = 3;
            static int start_sword = 1;
            static int start_shield = 1;
            static int start_bracelet = 0;
            static bool start_boots = false;
            static bool start_flippers = false;
            static bool start_feather = true;
            static bool start_bow = false;
            static bool start_hookshot = false;
            static bool start_rod = false;
            static bool start_ocarina = false;
            static bool start_shovel = false;
            static int start_rupees = 100;
            static int start_bombs = 10;
            static int start_arrows = 10;
            static int start_powder = 10;

            ImGui::SliderInt("Hearts", &min_hearts, 1, 20);

            const char* sword_names[] = { "None", "Sword L1", "Sword L2" };
            ImGui::Combo("Sword", &start_sword, sword_names, 3);

            const char* shield_names[] = { "None", "Shield", "Mirror Shield" };
            ImGui::Combo("Shield", &start_shield, shield_names, 3);

            const char* bracelet_names[] = { "None", "Bracelet L1", "Bracelet L2" };
            ImGui::Combo("Power Bracelet", &start_bracelet, bracelet_names, 3);

            ImGui::Checkbox("Roc's Feather", &start_feather);
            ImGui::SameLine();
            ImGui::Checkbox("Pegasus Boots", &start_boots);

            ImGui::Checkbox("Bow", &start_bow);
            ImGui::SameLine();
            ImGui::Checkbox("Hookshot", &start_hookshot);

            ImGui::Checkbox("Magic Rod", &start_rod);
            ImGui::SameLine();
            ImGui::Checkbox("Ocarina", &start_ocarina);

            ImGui::Checkbox("Shovel", &start_shovel);
            ImGui::SameLine();
            ImGui::Checkbox("Flippers", &start_flippers);

            ImGui::SliderInt("Rupees", &start_rupees, 0, 999);
            ImGui::SliderInt("Bombs", &start_bombs, 0, 99);
            ImGui::SliderInt("Arrows", &start_arrows, 0, 99);
            ImGui::SliderInt("Magic Powder", &start_powder, 0, 99);

            ImGui::Spacing();

            if (ImGui::Button("Start Hosting", ImVec2(-1, 30))) {
                if (ctx) {
                    mp_session_set_min_hearts(min_hearts);
                    mp_session_set_start_sword(start_sword);
                    mp_session_set_start_shield(start_shield);
                    mp_session_set_start_bracelet(start_bracelet);
                    mp_session_set_start_boots(start_boots);
                    mp_session_set_start_flippers(start_flippers);
                    mp_session_set_start_feather(start_feather);
                    mp_session_set_start_bow(start_bow);
                    mp_session_set_start_hookshot(start_hookshot);
                    mp_session_set_start_rod(start_rod);
                    mp_session_set_start_ocarina(start_ocarina);
                    mp_session_set_start_shovel(start_shovel);
                    mp_session_set_start_rupees(start_rupees);
                    mp_session_set_start_bombs(start_bombs);
                    mp_session_set_start_arrows(start_arrows);
                    mp_session_set_start_powder(start_powder);
                    if (mp_session_host(ctx, (uint16_t)g_mp_menu.host_port)) {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "Hosting on %s:%d", local_ip, g_mp_menu.host_port);
                        set_status(msg);
                    } else {
                        set_status("Failed to start host.");
                    }
                } else {
                    set_status("Load a game first before hosting.");
                }
            }

        } else if (state == MP_STATE_HOSTING) {
            int count = mp_session_get_player_count();

            /* Show IP prominently */
            static char local_ip[64] = {0};
            static bool ip_fetched = false;
            if (!ip_fetched) {
                get_local_ip(local_ip, sizeof(local_ip));
                ip_fetched = true;
            }
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f),
                               "HOSTING on %s:%d", local_ip, g_mp_menu.host_port);
            ImGui::Text("Players: %d / %d", count, MP_MAX_PLAYERS);
            ImGui::Spacing();

            /* List connected players */
            for (int i = 0; i < MP_MAX_PLAYERS; i++) {
                const MPPlayer* p = mp_session_get_player(i);
                if (p) {
                    ImVec4 col = hsv_to_rgb(p->color_h, p->color_s, p->color_v);
                    ImGui::TextColored(col, "P%d: %s", i + 1, p->name);
                    if (i > 0) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%u ms)", p->ping_ms);
                    } else {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(host)");
                    }
                }
            }

            ImGui::Spacing();
            if (ImGui::Button("Stop Hosting", ImVec2(-1, 25))) {
                mp_session_leave();
                set_status("Stopped hosting.");
            }

        } else {
            ImGui::Text("Cannot host while in another session.");
            if (ImGui::Button("Leave Current Session")) {
                mp_session_leave();
            }
        }
    }
    ImGui::End();
}

/* ============================================================================
 * Join Dialog
 * ========================================================================== */

static void draw_join_dialog(GBContext* ctx) {
    (void)ctx;
    ImGui::SetNextWindowSize(ImVec2(340, 200), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Join Multiplayer Game", &g_mp_menu.show_join_dialog)) {
        MPSessionState state = mp_session_get_state();

        if (state == MP_STATE_IDLE) {
            ImGui::Text("Connect to another player's hosted game.");
            ImGui::Spacing();

            ImGui::InputText("Host Address", g_mp_menu.join_address,
                             sizeof(g_mp_menu.join_address));
            ImGui::InputInt("Port", &g_mp_menu.join_port);

            ImGui::Spacing();

            /* Show current name/color */
            ImVec4 col = hsv_to_rgb(g_mp_menu.settings_color_h,
                                     g_mp_menu.settings_color_s,
                                     g_mp_menu.settings_color_v);
            ImGui::TextColored(col, "Joining as: %s", g_mp_menu.settings_name);

            ImGui::Spacing();

            if (ImGui::Button("Connect", ImVec2(-1, 30))) {
                /* Apply settings before connecting */
                mp_session_set_name(g_mp_menu.settings_name);
                mp_session_set_color(g_mp_menu.settings_color_h,
                                      g_mp_menu.settings_color_s,
                                      g_mp_menu.settings_color_v);

                if (mp_session_join(g_mp_menu.join_address,
                                     (uint16_t)g_mp_menu.join_port,
                                     g_mp_menu.settings_name,
                                     g_mp_menu.settings_color_h,
                                     g_mp_menu.settings_color_s,
                                     g_mp_menu.settings_color_v)) {
                    set_status("Connecting...");
                } else {
                    set_status("Failed to connect.");
                }
            }

        } else if (state == MP_STATE_CONNECTING) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Connecting...");
            ImGui::Spacing();
            if (ImGui::Button("Cancel")) {
                mp_session_leave();
                set_status("Connection cancelled.");
            }

        } else if (state == MP_STATE_CONNECTED) {
            int slot = mp_session_get_local_slot();
            int count = mp_session_get_player_count();
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f),
                               "CONNECTED as Player %d", slot + 1);
            ImGui::Text("Players: %d / %d", count, MP_MAX_PLAYERS);

            uint32_t rtt = 0;
            /* Show ping from player info */
            const MPPlayer* me = mp_session_get_player(slot);
            if (me) rtt = me->ping_ms;
            ImGui::Text("Ping: %u ms", rtt);

            ImGui::Spacing();
            if (ImGui::Button("Disconnect", ImVec2(-1, 25))) {
                mp_session_leave();
                set_status("Disconnected.");
            }

        } else if (state == MP_STATE_DISCONNECTED) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Disconnected");
            if (ImGui::Button("Return to Menu")) {
                mp_session_leave();
            }
        }
    }
    ImGui::End();
}

/* ============================================================================
 * Settings Window
 * ========================================================================== */

static void draw_settings_window(void) {
    ImGui::SetNextWindowSize(ImVec2(380, 350), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Multiplayer Settings", &g_mp_menu.show_settings)) {
        /* ---- Screen Name ---- */
        ImGui::Text("Screen Name");
        ImGui::InputText("##name", g_mp_menu.settings_name,
                         sizeof(g_mp_menu.settings_name));
        ImGui::Spacing();

        /* ---- Link Color ---- */
        ImGui::Text("Link Color");
        ImGui::Separator();

        /* Color preview - show a colored rectangle */
        ImVec4 preview_col = hsv_to_rgb(g_mp_menu.settings_color_h,
                                         g_mp_menu.settings_color_s,
                                         g_mp_menu.settings_color_v);
        ImGui::ColorButton("##preview", preview_col, 0, ImVec2(60, 60));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::SliderFloat("Hue", &g_mp_menu.settings_color_h, 0.0f, 360.0f, "%.0f");
        ImGui::SliderFloat("Saturation", &g_mp_menu.settings_color_s, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Brightness", &g_mp_menu.settings_color_v, 0.0f, 1.0f, "%.2f");
        ImGui::EndGroup();

        /* Quick color presets */
        ImGui::Spacing();
        ImGui::Text("Presets:");
        struct { const char* name; float h, s, v; } presets[] = {
            { "Green (Classic)",  120.0f, 0.8f, 0.8f },
            { "Red",              0.0f,   0.8f, 0.8f },
            { "Blue",             220.0f, 0.8f, 0.8f },
            { "Purple",           280.0f, 0.7f, 0.8f },
            { "Orange",           30.0f,  0.9f, 0.9f },
            { "Cyan",             180.0f, 0.7f, 0.9f },
            { "Pink",             330.0f, 0.6f, 0.9f },
            { "Gold",             45.0f,  0.8f, 0.9f },
        };

        for (int i = 0; i < 8; i++) {
            if (i > 0) ImGui::SameLine();
            ImVec4 c = hsv_to_rgb(presets[i].h, presets[i].s, presets[i].v);
            ImGui::PushStyleColor(ImGuiCol_Button, c);
            char label[8];
            snprintf(label, sizeof(label), "##c%d", i);
            if (ImGui::Button(label, ImVec2(30, 30))) {
                g_mp_menu.settings_color_h = presets[i].h;
                g_mp_menu.settings_color_s = presets[i].s;
                g_mp_menu.settings_color_v = presets[i].v;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", presets[i].name);
            }
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::Separator();

        /* ---- Debug Overlay ---- */
        ImGui::Checkbox("Debug Overlay (show connected players)",
                        &g_mp_menu.settings_debug_overlay);
        mp_session_set_debug_overlay(g_mp_menu.settings_debug_overlay);

        ImGui::Spacing();
        ImGui::Separator();

        if (ImGui::Button("Apply & Save", ImVec2(-1, 28))) {
            mp_session_set_name(g_mp_menu.settings_name);
            mp_session_set_color(g_mp_menu.settings_color_h,
                                  g_mp_menu.settings_color_s,
                                  g_mp_menu.settings_color_v);
            mp_menu_save_settings();
            set_status("Settings saved.");
        }
    }
    ImGui::End();
}

/* ============================================================================
 * Debug Overlay
 * ========================================================================== */

static void draw_debug_overlay(void) {
    if (!mp_session_get_debug_overlay()) return;
    if (!mp_session_is_active()) return;

    ImGui::SetNextWindowPos(ImVec2(10, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(260, 200), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.6f);

    if (ImGui::Begin("##mp_overlay", NULL,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings)) {

        MPSessionState state = mp_session_get_state();
        const char* state_names[] = {
            "Idle", "Hosting", "Connecting", "Connected", "Disconnected"
        };

        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "MULTIPLAYER");
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", state_names[state]);
        ImGui::Separator();

        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            const MPPlayer* p = mp_session_get_player(i);
            if (!p) {
                ImGui::TextDisabled("P%d: ---", i + 1);
                continue;
            }

            ImVec4 col = hsv_to_rgb(p->color_h, p->color_s, p->color_v);
            ImGui::TextColored(col, "P%d: %s", i + 1, p->name);

            ImGui::SameLine(160);
            if (i == mp_session_get_local_slot()) {
                ImGui::TextDisabled("(you)");
            } else {
                ImGui::TextDisabled("%ums", p->ping_ms);
            }

            /* Room info */
            ImGui::TextDisabled("  Room:%02X %s HP:%d/%d",
                                p->map_room,
                                p->is_indoor ? "In" : "OW",
                                p->health, p->max_health);
        }
    }
    ImGui::End();
}

/* ============================================================================
 * Status Toast
 * ========================================================================== */

static void draw_status_toast(void) {
    if (g_mp_menu.status_timer <= 0) return;

    g_mp_menu.status_timer -= ImGui::GetIO().DeltaTime;

    float alpha = (g_mp_menu.status_timer < 1.0f) ? g_mp_menu.status_timer : 1.0f;
    ImGui::SetNextWindowBgAlpha(0.7f * alpha);

    ImVec2 viewport = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImVec2(viewport.x * 0.5f, viewport.y - 40),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if (ImGui::Begin("##mp_toast", NULL,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, alpha), "%s",
                           g_mp_menu.status_msg);
    }
    ImGui::End();
}

/* ============================================================================
 * Trade Window
 * ========================================================================== */

static void draw_trade_window(void) {
    ImGui::SetNextWindowSize(ImVec2(320, 300), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Item Trading", &g_mp_menu.show_trade)) {
        MPTradeState ts = mp_trade_get_state();

        if (ts == TRADE_STATE_PENDING) {
            /* Incoming offer */
            uint8_t from, item;
            mp_trade_get_pending_offer(&from, &item);
            const MPPlayer* sender = mp_session_get_player(from);
            const char* sender_name = sender ? sender->name : "???";

            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f), "TRADE OFFER!");
            ImGui::Text("%s wants to give you:", sender_name);
            ImGui::BulletText("%s", (item < TRADE_ITEM_COUNT) ? mp_trade_item_names[item] : "???");
            ImGui::Spacing();
            if (ImGui::Button("Accept", ImVec2(120, 30))) mp_trade_accept();
            ImGui::SameLine();
            if (ImGui::Button("Decline", ImVec2(120, 30))) mp_trade_decline();

        } else if (ts == TRADE_STATE_OFFERING) {
            ImGui::Text("Waiting for response...");
            if (ImGui::Button("Cancel Offer")) mp_trade_cancel();

        } else if (ts == TRADE_STATE_ACCEPTED) {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Trade completed!");
            if (ImGui::Button("OK")) mp_trade_reset();

        } else if (ts == TRADE_STATE_DECLINED) {
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "Trade declined.");
            if (ImGui::Button("OK")) mp_trade_reset();

        } else {
            /* No active trade - show offer UI */
            ImGui::Text("Send items to another player:");
            ImGui::Spacing();

            static int selected_player = 1;
            static int selected_item = 0;

            /* Player selector */
            if (ImGui::BeginCombo("Target Player", "Select...")) {
                for (int i = 0; i < MP_MAX_PLAYERS; i++) {
                    if (i == mp_session_get_local_slot()) continue;
                    const MPPlayer* p = mp_session_get_player(i);
                    if (!p) continue;
                    char label[64];
                    snprintf(label, sizeof(label), "P%d: %s", i + 1, p->name);
                    if (ImGui::Selectable(label, selected_player == i))
                        selected_player = i;
                }
                ImGui::EndCombo();
            }

            /* Item selector */
            if (ImGui::BeginCombo("Item", mp_trade_item_names[selected_item])) {
                for (int i = 0; i < TRADE_ITEM_COUNT; i++) {
                    if (ImGui::Selectable(mp_trade_item_names[i], selected_item == i))
                        selected_item = i;
                }
                ImGui::EndCombo();
            }

            ImGui::Spacing();
            if (ImGui::Button("Send Offer", ImVec2(-1, 30))) {
                if (mp_trade_offer(selected_player, (MPTradeItemType)selected_item)) {
                    set_status("Trade offer sent!");
                } else {
                    set_status("Cannot send trade offer right now.");
                }
            }
        }
    }
    ImGui::End();
}

/* ============================================================================
 * Voice Chat Window
 * ========================================================================== */

static void draw_voice_window(void) {
    ImGui::SetNextWindowSize(ImVec2(300, 280), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Voice Chat", &g_mp_menu.show_voice)) {
        MPVoiceMode mode = mp_voice_get_mode();

        const char* mode_names[] = { "Off", "Push-to-Talk (V)", "Voice Activated", "Always On" };
        if (ImGui::BeginCombo("Mode", mode_names[mode])) {
            for (int i = 0; i < 4; i++) {
                if (ImGui::Selectable(mode_names[i], mode == i)) {
                    mp_voice_set_mode((MPVoiceMode)i);
                    if (i != VOICE_MODE_OFF && !mp_voice_is_transmitting()) {
                        mp_voice_init(); /* Init on first use */
                    }
                }
            }
            ImGui::EndCombo();
        }

        if (mode != VOICE_MODE_OFF) {
            ImGui::Spacing();

            /* Input level meter */
            float level = mp_voice_get_input_level();
            ImGui::Text("Input Level:");
            ImGui::ProgressBar(level, ImVec2(-1, 14));

            if (mp_voice_is_transmitting()) {
                ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "TRANSMITTING");
            } else {
                ImGui::TextDisabled("Not transmitting");
            }

            ImGui::Spacing();

            /* VOX threshold (only for voice activation mode) */
            if (mode == VOICE_MODE_ACTIVE) {
                static float vox_thresh = 0.05f;
                ImGui::SliderFloat("VOX Threshold", &vox_thresh, 0.01f, 0.3f, "%.3f");
                mp_voice_set_vox_threshold(vox_thresh);
            }

            /* Volume controls */
            static float tx_vol = 1.0f;
            static float rx_vol = 1.0f;
            ImGui::SliderFloat("Mic Volume", &tx_vol, 0.0f, 2.0f, "%.1f");
            ImGui::SliderFloat("Speaker Volume", &rx_vol, 0.0f, 2.0f, "%.1f");
            mp_voice_set_tx_volume(tx_vol);
            mp_voice_set_rx_volume(rx_vol);

            /* Per-player mute */
            ImGui::Spacing();
            ImGui::Text("Mute Players:");
            for (int i = 0; i < MP_MAX_PLAYERS; i++) {
                if (i == mp_session_get_local_slot()) continue;
                const MPPlayer* p = mp_session_get_player(i);
                if (!p) continue;
                bool muted = mp_voice_is_player_muted(i);
                char label[64];
                snprintf(label, sizeof(label), "Mute P%d: %s", i + 1, p->name);
                if (ImGui::Checkbox(label, &muted))
                    mp_voice_mute_player(i, muted);
            }
        }
    }
    ImGui::End();
}

/* ============================================================================
 * PvP Arena Window
 * ========================================================================== */

static void draw_pvp_window(GBContext* ctx) {
    (void)ctx;
    ImGui::SetNextWindowSize(ImVec2(360, 400), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("PvP Arena", &g_mp_menu.show_pvp)) {
        MPPvPState pvp_state = mp_pvp_get_state();

        if (pvp_state == PVP_STATE_OFF) {
            ImGui::Text("Set up a PvP battle!");
            ImGui::Spacing();

            /* Arena selector */
            int arena_count = 0;
            const MPArenaPreset* arenas = mp_pvp_get_arenas(&arena_count);
            static int selected_arena = 0;

            if (ImGui::BeginCombo("Arena", arenas[selected_arena].name)) {
                for (int i = 0; i < arena_count; i++) {
                    if (ImGui::Selectable(arenas[i].name, selected_arena == i))
                        selected_arena = i;
                }
                ImGui::EndCombo();
            }

            /* Config */
            static int starting_hearts = 3;
            static int rounds_to_win = 3;
            static bool items_enabled = false;

            ImGui::SliderInt("Starting Hearts", &starting_hearts, 1, 20);
            ImGui::SliderInt("Rounds to Win", &rounds_to_win, 1, 10);
            ImGui::Checkbox("Items Enabled", &items_enabled);

            ImGui::Spacing();
            int player_count = mp_session_get_player_count();
            if (player_count < 2) {
                ImGui::TextDisabled("Need at least 2 players to start PvP");
            } else {
                if (ImGui::Button("START MATCH", ImVec2(-1, 35))) {
                    MPPvPConfig config;
                    memset(&config, 0, sizeof(config));
                    config.arena_room = arenas[selected_arena].room;
                    config.arena_indoor = arenas[selected_arena].indoor;
                    config.starting_hearts = (uint8_t)starting_hearts;
                    config.rounds_to_win = (uint8_t)rounds_to_win;
                    config.items_enabled = items_enabled;
                    config.respawn_delay = 120;
                    mp_pvp_start(&config);
                    set_status("PvP match started!");
                }
            }

        } else {
            /* Match in progress */
            const char* state_labels[] = {
                "Off", "Lobby", "Countdown...", "FIGHT!", "Round Over", "MATCH OVER"
            };
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s",
                               state_labels[pvp_state]);
            ImGui::Text("Round %d", mp_pvp_get_round());
            ImGui::Spacing();
            ImGui::Separator();

            /* Scoreboard */
            if (ImGui::BeginTable("pvp_scores", 5, ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("Player");
                ImGui::TableSetupColumn("HP");
                ImGui::TableSetupColumn("Kills");
                ImGui::TableSetupColumn("Deaths");
                ImGui::TableSetupColumn("Rounds");
                ImGui::TableHeadersRow();

                for (int i = 0; i < MP_MAX_PLAYERS; i++) {
                    const MPPlayer* p = mp_session_get_player(i);
                    const MPPvPPlayerStats* s = mp_pvp_get_stats(i);
                    if (!p || !s) continue;

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImVec4 col = hsv_to_rgb(p->color_h, p->color_s, p->color_v);
                    ImGui::TextColored(col, "P%d: %s", i + 1, p->name);

                    ImGui::TableNextColumn();
                    if (s->alive)
                        ImGui::Text("%d", s->health / 8);
                    else
                        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "DEAD");

                    ImGui::TableNextColumn();
                    ImGui::Text("%d", s->kills);

                    ImGui::TableNextColumn();
                    ImGui::Text("%d", s->deaths);

                    ImGui::TableNextColumn();
                    ImGui::Text("%d/%d", s->rounds_won,
                                mp_pvp_get_config()->rounds_to_win);
                }
                ImGui::EndTable();
            }

            ImGui::Spacing();
            if (ImGui::Button("Stop Match", ImVec2(-1, 25))) {
                mp_pvp_stop();
                set_status("PvP match stopped.");
            }
        }
    }
    ImGui::End();
}

/* ============================================================================
 * Public API
 * ========================================================================== */

void mp_menu_init(void) {
    mp_menu_load_settings();
    mp_trade_init();
    mp_pvp_init();
}

void mp_menu_draw_menu_item(GBContext* ctx) {
    /* This is called inside BeginMainMenuBar */
    if (ImGui::BeginMenu("Multiplayer")) {
        MPSessionState state = mp_session_get_state();

        if (state == MP_STATE_IDLE) {
            if (ImGui::MenuItem("Host Game..."))
                g_mp_menu.show_host_dialog = true;
            if (ImGui::MenuItem("Join Game..."))
                g_mp_menu.show_join_dialog = true;
        } else if (state == MP_STATE_HOSTING) {
            int count = mp_session_get_player_count();
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f),
                               "Hosting (%d players)", count);
            ImGui::Separator();
            if (ImGui::MenuItem("Host Window..."))
                g_mp_menu.show_host_dialog = true;
            if (ImGui::MenuItem("Stop Hosting"))
                mp_session_leave();
        } else {
            const char* label = (state == MP_STATE_CONNECTING) ? "Connecting..." :
                                (state == MP_STATE_CONNECTED)  ? "Connected" :
                                                                  "Disconnected";
            ImGui::TextDisabled("%s", label);
            ImGui::Separator();
            if (ImGui::MenuItem("Connection..."))
                g_mp_menu.show_join_dialog = true;
            if (state != MP_STATE_IDLE && ImGui::MenuItem("Disconnect"))
                mp_session_leave();
        }

        ImGui::Separator();
        ImGui::MenuItem("Settings...", NULL, &g_mp_menu.show_settings);

        if (mp_session_is_active()) {
            ImGui::Separator();
            ImGui::MenuItem("Trade Items...", NULL, &g_mp_menu.show_trade);
            ImGui::MenuItem("Voice Chat...", NULL, &g_mp_menu.show_voice);
            if (mp_session_get_state() == MP_STATE_HOSTING) {
                ImGui::MenuItem("PvP Arena...", NULL, &g_mp_menu.show_pvp);
            }
        }

        ImGui::EndMenu();
    }

    /* Draw dialogs (outside menu) */
    if (g_mp_menu.show_host_dialog) draw_host_dialog(ctx);
    if (g_mp_menu.show_join_dialog) draw_join_dialog(ctx);
    if (g_mp_menu.show_settings)    draw_settings_window();
    if (g_mp_menu.show_trade)       draw_trade_window();
    if (g_mp_menu.show_voice)       draw_voice_window();
    if (g_mp_menu.show_pvp)         draw_pvp_window(ctx);
}

void mp_menu_draw_overlays(void) {
    draw_debug_overlay();
    draw_status_toast();
}

/* ============================================================================
 * Settings Persistence
 * ========================================================================== */

static void get_mp_config_path(char* buf, size_t size) {
    char* base = SDL_GetBasePath();
    if (base) {
        snprintf(buf, size, "%smp_settings.cfg", base);
        SDL_free(base);
    } else {
        snprintf(buf, size, "mp_settings.cfg");
    }
}

void mp_menu_save_settings(void) {
    char path[512];
    get_mp_config_path(path, sizeof(path));

    FILE* f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "name %s\n", g_mp_menu.settings_name);
    fprintf(f, "color_h %f\n", g_mp_menu.settings_color_h);
    fprintf(f, "color_s %f\n", g_mp_menu.settings_color_s);
    fprintf(f, "color_v %f\n", g_mp_menu.settings_color_v);
    fprintf(f, "debug_overlay %d\n", g_mp_menu.settings_debug_overlay ? 1 : 0);
    fprintf(f, "last_host %s\n", g_mp_menu.join_address);
    fprintf(f, "last_port %d\n", g_mp_menu.join_port);

    fclose(f);
    fprintf(stderr, "[MP_MENU] Settings saved to %s\n", path);
}

void mp_menu_load_settings(void) {
    /* Set defaults */
    strncpy(g_mp_menu.settings_name, "Link", MP_MAX_NAME_LEN);
    g_mp_menu.settings_color_h = 120.0f;
    g_mp_menu.settings_color_s = 0.8f;
    g_mp_menu.settings_color_v = 0.8f;

    char path[512];
    get_mp_config_path(path, sizeof(path));

    FILE* f = fopen(path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char sval[128];
        float fval;
        int ival;

        if (sscanf(line, "name %127[^\n]", sval) == 1) {
            strncpy(g_mp_menu.settings_name, sval, MP_MAX_NAME_LEN - 1);
            g_mp_menu.settings_name[MP_MAX_NAME_LEN - 1] = '\0';
        } else if (sscanf(line, "color_h %f", &fval) == 1) {
            g_mp_menu.settings_color_h = fval;
        } else if (sscanf(line, "color_s %f", &fval) == 1) {
            g_mp_menu.settings_color_s = fval;
        } else if (sscanf(line, "color_v %f", &fval) == 1) {
            g_mp_menu.settings_color_v = fval;
        } else if (sscanf(line, "debug_overlay %d", &ival) == 1) {
            g_mp_menu.settings_debug_overlay = ival != 0;
        } else if (sscanf(line, "last_host %127[^\n]", sval) == 1) {
            strncpy(g_mp_menu.join_address, sval, sizeof(g_mp_menu.join_address) - 1);
        } else if (sscanf(line, "last_port %d", &ival) == 1) {
            g_mp_menu.join_port = ival;
        }
    }

    fclose(f);

    /* Apply loaded settings to session */
    mp_session_set_name(g_mp_menu.settings_name);
    mp_session_set_color(g_mp_menu.settings_color_h,
                          g_mp_menu.settings_color_s,
                          g_mp_menu.settings_color_v);
    mp_session_set_debug_overlay(g_mp_menu.settings_debug_overlay);

    fprintf(stderr, "[MP_MENU] Settings loaded from %s\n", path);
}
