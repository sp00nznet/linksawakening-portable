/**
 * GameBoy Recompiled — asset_viewer stub implementations.
 *
 * Compiled into the runtime when LA_HAS_IMGUI is NOT defined. The asset
 * viewer is an ImGui-only debug tool; without ImGui all entry points
 * become no-ops.
 *
 * Plain C so it can be compiled by toolchains that don't have a C++
 * compiler installed.
 */

#include "asset_viewer.h"

void asset_viewer_init(void* sdl_renderer)         { (void)sdl_renderer; }
void asset_viewer_draw(GBContext* ctx)             { (void)ctx; }
void asset_viewer_shutdown(void)                   {}
void asset_viewer_set_visible(bool v)              { (void)v; }
bool asset_viewer_is_visible(void)                 { return 0; }
void asset_viewer_push_audio(int16_t l, int16_t r) { (void)l; (void)r; }
