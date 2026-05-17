/**
 * @file mp_integration.h
 * @brief Integration guide for adding multiplayer to the existing build
 *
 * This file documents what changes are needed in the existing codebase
 * to integrate the multiplayer module.
 */

#ifndef MP_INTEGRATION_H
#define MP_INTEGRATION_H

/*
 * ============================================================================
 * INTEGRATION STEPS
 * ============================================================================
 *
 * 1. ADD TO menu_gui.cpp - draw_menu_bar():
 *    After the "Help" menu, add:
 *
 *        #include "multiplayer/mp_menu.h"
 *        // Inside draw_menu_bar(), before EndMainMenuBar():
 *        mp_menu_draw_menu_item(ctx);
 *
 *    In menu_gui_draw(), after asset_viewer_draw():
 *        mp_menu_draw_overlays();
 *
 * 2. ADD TO menu_gui.cpp - menu_gui_init():
 *        #include "multiplayer/mp_session.h"
 *        // In menu_gui_init():
 *        mp_session_init();
 *        mp_menu_init();
 *
 * 3. ADD TO platform_sdl.cpp - gb_platform_render_frame():
 *    After running the main frame, call:
 *        #include "multiplayer/mp_session.h"
 *        #include "multiplayer/mp_color.h"
 *        #include "multiplayer/mp_indicators.h"
 *        #include "multiplayer/mp_voice.h"
 *
 *        mp_session_update();
 *
 *    For color recoloring (before rendering):
 *        mp_color_recolor_link(fb_work, color_h, color_s, color_v);
 *
 *    For player indicators (after palette/scanline processing):
 *        mp_indicators_render(fb_work, local_slot, room, indoor,
 *                             player_states, player_count);
 *
 *    For voice chat (in event loop):
 *        // Check V key for push-to-talk
 *        if (event.key.keysym.scancode == SDL_SCANCODE_V)
 *            mp_voice_set_ptt(event.type == SDL_KEYDOWN);
 *
 *        // Per-frame voice update (in render loop):
 *        uint8_t voice_pkt[512];
 *        uint32_t voice_size = 0;
 *        mp_voice_update(voice_pkt, &voice_size);
 *        if (voice_size > 0)
 *            // send voice_pkt via network
 *
 * 4. ADD TO platform_sdl.cpp - gb_platform_shutdown():
 *        mp_voice_shutdown();
 *        mp_session_shutdown();
 *
 * 5. ADD TO CMakeLists.txt:
 *    Add the multiplayer source files:
 *        multiplayer/mp_net.cpp
 *        multiplayer/mp_session.cpp
 *        multiplayer/mp_menu.cpp
 *        multiplayer/mp_color.cpp
 *        multiplayer/mp_worldsync.cpp
 *        multiplayer/mp_indicators.cpp
 *        multiplayer/mp_delta.cpp
 *        multiplayer/mp_trade.cpp
 *        multiplayer/mp_voice.cpp
 *        multiplayer/mp_pvp.cpp
 *
 *    Link against ENet and Winsock:
 *        target_link_libraries(${PROJECT_NAME} enet ws2_32 winmm)
 *
 * 6. ADD ENet dependency:
 *    Either add as submodule or copy enet/ to third_party/
 *    ENet is very small: enet.h + ~6 .c files
 *    https://github.com/lsalzman/enet
 *
 * ============================================================================
 */

#endif /* MP_INTEGRATION_H */
