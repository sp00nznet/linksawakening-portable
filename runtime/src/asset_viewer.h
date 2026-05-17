/**
 * Asset Viewer - ImGui-based tile/sprite/tilemap/audio/palette viewer
 *
 * Accessible from the Tools menu. Provides tabs for viewing and exporting
 * game graphics and audio assets.
 */

#ifndef GBRT_ASSET_VIEWER_H
#define GBRT_ASSET_VIEWER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBContext GBContext;

/* Initialize asset viewer (call after ImGui + SDL renderer are ready) */
void asset_viewer_init(void* sdl_renderer);

/* Draw the asset viewer window (call between NewFrame/Render) */
void asset_viewer_draw(GBContext* ctx);

/* Cleanup textures */
void asset_viewer_shutdown(void);

/* Visibility toggle */
void asset_viewer_set_visible(bool visible);
bool asset_viewer_is_visible(void);

/* Feed audio samples for waveform display / WAV recording */
void asset_viewer_push_audio(int16_t left, int16_t right);

#ifdef __cplusplus
}
#endif

#endif /* GBRT_ASSET_VIEWER_H */
