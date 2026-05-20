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

    int max_frames = 0;  /* 0 = unlimited */

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

    // Run the game loop
    int frame_count = 0;
    while (1) {
#ifdef LA_HAS_MULTIPLAYER
        if (mp_session_is_client_connected()) {
            /* Client mode: don't run local game, just service network
             * and display framebuffers received from the host. */
            if (!gb_platform_poll_events(ctx)) break;
            mp_session_update();
            const uint32_t* fb = mp_session_get_framebuffer();
            if (fb) gb_platform_render_frame(fb);
            gb_platform_vsync();
            frame_count++;
            continue;
        }
#endif
        /* Normal mode (solo or host): run local game */
        gb_run_frame(ctx);
        if (!gb_platform_poll_events(ctx)) break;
        if (ctx->frame_done) {
            const uint32_t* fb = gb_get_framebuffer(ctx);
            if (fb) gb_platform_render_frame(fb);
            gb_reset_frame(ctx);
            ctx->stopped = 0;
            gb_platform_vsync();
            frame_count++;
            if (max_frames > 0 && frame_count >= max_frames) break;
        }
    }
    gb_platform_shutdown();
#else
    // No SDL2 - just run for testing
    rom_run(ctx);
    printf("Recompiled code executed successfully!\n");
    printf("Registers: A=%02X B=%02X C=%02X\n", ctx->a, ctx->b, ctx->c);
#endif

    gb_context_destroy(ctx);
    return 0;
}
