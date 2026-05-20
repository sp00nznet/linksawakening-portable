/* Main entry point */
#include "rom.h"
#include "gbrt.h"
#include "hwtrace.h"
#ifdef GB_HAS_SDL2
#include "platform_sdl.h"
#ifdef LA_HAS_MULTIPLAYER
#include "multiplayer/mp_session.h"
#endif
#endif
#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

    int max_frames = 0;  /* 0 = unlimited */

#ifdef GB_HAS_SDL2
/* One game-loop iteration. Returns 0 when the platform asks to quit.
 * Shared by the native blocking loop and the Emscripten rAF callback —
 * the browser can't run a blocking while(1), so on WASM this is driven
 * by emscripten_set_main_loop instead. */
static int g_frame_count = 0;
static int la_loop_iter(GBContext* ctx) {
#ifdef LA_HAS_MULTIPLAYER
    if (mp_session_is_client_connected()) {
        /* Client mode: service network, display received framebuffers. */
        if (!gb_platform_poll_events(ctx)) return 0;
        mp_session_update();
        const uint32_t* fb = mp_session_get_framebuffer();
        if (fb) gb_platform_render_frame(fb);
        gb_platform_vsync();
        return 1;
    }
#endif
    /* Normal mode (solo or host): run local game */
    gb_run_frame(ctx);
    if (!gb_platform_poll_events(ctx)) return 0;
    if (ctx->frame_done) {
        const uint32_t* fb = gb_get_framebuffer(ctx);
        if (fb) gb_platform_render_frame(fb);
        gb_reset_frame(ctx);
        ctx->stopped = 0;
        gb_platform_vsync();
        g_frame_count++;
        if (max_frames > 0 && g_frame_count >= max_frames) return 0;
    }
    return 1;
}

#ifdef __EMSCRIPTEN__
static GBContext* g_em_ctx = NULL;
static void la_em_frame(void) { la_loop_iter(g_em_ctx); }
#endif
#endif /* GB_HAS_SDL2 */

int main(int argc, char* argv[]) {
    // Parse args
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0) {
            gbrt_trace_enabled = true;
            printf("Trace enabled\n");
        } else if (strcmp(argv[i], "--trace-entries") == 0 && i + 1 < argc) {
            gbrt_set_trace_file(argv[++i]);
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            gbrt_instruction_limit = strtoull(argv[++i], NULL, 10);
            printf("Instruction limit: %llu\n", (unsigned long long)gbrt_instruction_limit);
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            gb_platform_set_input_script(argv[++i]);
        } else if (strcmp(argv[i], "--dump-frames") == 0 && i + 1 < argc) {
            gb_platform_set_dump_frames(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot-prefix") == 0 && i + 1 < argc) {
            gb_platform_set_screenshot_prefix(argv[++i]);
        } else if (strcmp(argv[i], "--hw-trace") == 0 && i + 1 < argc) {
            hwtrace_init(argv[++i]);
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        }
    }

    GBContext* ctx = gb_context_create(NULL);
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
#ifdef GB_HAS_SDL2
    // Initialize SDL2 platform with 3x scaling
    if (!gb_platform_init(3)) {
        fprintf(stderr, "Failed to initialize platform\n");
        gb_context_destroy(ctx);
        return 1;
    }
    // Register callbacks BEFORE rom_init so save file loads during gb_context_load_rom
    gb_platform_register_context(ctx);
#endif

    rom_init(ctx);

#ifdef GB_HAS_SDL2

#ifdef __EMSCRIPTEN__
    /* The browser owns the event loop — hand the per-frame callback to
     * emscripten. simulate_infinite_loop=1 means this call never returns,
     * so shutdown/destroy below are intentionally unreachable on WASM. */
    g_em_ctx = ctx;
    emscripten_set_main_loop(la_em_frame, 0, 1);
#else
    // Run the game loop
    while (la_loop_iter(ctx)) { }
    gb_platform_shutdown();
#endif
#else
    // No SDL2 - just run for testing
    rom_run(ctx);
    printf("Recompiled code executed successfully!\n");
    printf("Registers: A=%02X B=%02X C=%02X\n", ctx->a, ctx->b, ctx->c);
#endif

    gb_context_destroy(ctx);
    return 0;
}
