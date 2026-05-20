/**
 * @file platform_sdl.cpp
 * @brief SDL2 platform implementation for GameBoy runtime with ImGui
 */

#include "platform_sdl.h"
#include "gbrt.h"   /* For GBPlatformCallbacks */
#include "ppu.h"
#include "gbrt_debug.h"
#include "menu_gui.h"
#include "asset_viewer.h"
#ifdef LA_HAS_MULTIPLAYER
#include "multiplayer/mp_session.h"
#include "multiplayer/mp_indicators.h"
#endif

#ifdef GB_HAS_SDL2
#include <SDL.h>
#ifdef LA_HAS_IMGUI
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#endif

/* ============================================================================
 * SDL State
 * ========================================================================== */

static SDL_Window* g_window = NULL;
static SDL_Renderer* g_renderer = NULL;
static SDL_Texture* g_texture = NULL;
static int g_scale = 3;
static uint32_t g_last_frame_time = 0;
static SDL_AudioDeviceID g_audio_device = 0;
static bool g_vsync = true;
static int g_filter_mode = 0;
static SDL_Texture* g_texture_2x = NULL; /* For Scale2x (320x288) */

/* Palette data for DMG-mode color remapping */
static const uint32_t g_palettes[][4] = {
    { 0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820 }, // CGB Original
    { 0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820 }, // Classic Green (same as CGB)
    { 0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000 }, // B&W
    { 0xFFFFB000, 0xFFCB4F0E, 0xFF800000, 0xFF330000 }  // Amber
};

/* Joypad state - exported for gbrt.c to access */
uint8_t g_joypad_buttons = 0xFF;  /* Active low: Start, Select, B, A */
uint8_t g_joypad_dpad = 0xFF;     /* Active low: Down, Up, Left, Right */

/* Stored context for menu access */
static GBContext* g_ctx = NULL;

/* Gamepad */
static SDL_GameController* g_gamepad = NULL;
/* Raw-joystick fallback: used when a device has no SDL_GameController
 * mapping — notably the DualShock on the OpenOrbis PS4 SDL2 port. */
static SDL_Joystick* g_joystick = NULL;

/* ============================================================================
 * Automation State
 * ========================================================================== */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_SCRIPT_ENTRIES 100
typedef struct {
    uint32_t start_frame;
    uint32_t duration;
    uint8_t dpad;    /* Active LOW mask to apply (0 = Pressed) */
    uint8_t buttons; /* Active LOW mask to apply (0 = Pressed) */
} ScriptEntry;

static ScriptEntry g_input_script[MAX_SCRIPT_ENTRIES];
static int g_script_count = 0;

#define MAX_DUMP_FRAMES 100
static uint32_t g_dump_frames[MAX_DUMP_FRAMES];
static int g_dump_count = 0;
static char g_screenshot_prefix[64] = "screenshot";

/* Helper to parse button string "U,D,L,R,A,B,S,T" */
static void parse_buttons(const char* btn_str, uint8_t* dpad, uint8_t* buttons) {
    *dpad = 0xFF;
    *buttons = 0xFF;
    // Simple parser: check for existence of characters
    if (strchr(btn_str, 'U')) *dpad &= ~0x04;
    if (strchr(btn_str, 'D')) *dpad &= ~0x08;
    if (strchr(btn_str, 'L')) *dpad &= ~0x02;
    if (strchr(btn_str, 'R')) *dpad &= ~0x01;
    if (strchr(btn_str, 'A')) *buttons &= ~0x01;
    if (strchr(btn_str, 'B')) *buttons &= ~0x02;
    if (strchr(btn_str, 'S')) *buttons &= ~0x08; /* Start */
    if (strchr(btn_str, 'T')) *buttons &= ~0x04; /* Select (T for selecT) */
}

/* Portable strdup — some console libc headers (e.g. OpenOrbis/PS4) don't
 * declare strdup without feature macros. */
static char* gb_strdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

void gb_platform_set_input_script(const char* script) {
    // Format: frame:buttons:duration,...
    if (!script) return;

    char* copy = gb_strdup(script);
    char* token = strtok(copy, ",");
    g_script_count = 0;
    
    while (token && g_script_count < MAX_SCRIPT_ENTRIES) {
        uint32_t frame = 0, duration = 0;
        char btn_buf[16] = {0};
        
        if (sscanf(token, "%u:%15[^:]:%u", &frame, btn_buf, &duration) == 3) {
            ScriptEntry* e = &g_input_script[g_script_count++];
            e->start_frame = frame;
            e->duration = duration;
            parse_buttons(btn_buf, &e->dpad, &e->buttons);
            printf("[AUTO] Added input: Frame %u, Btns '%s', Dur %u\n", frame, btn_buf, duration);
        }
        token = strtok(NULL, ",");
    }
    free(copy);
}

void gb_platform_set_dump_frames(const char* frames) {
    if (!frames) return;
    char* copy = gb_strdup(frames);
    char* token = strtok(copy, ",");
    g_dump_count = 0;
    while (token && g_dump_count < MAX_DUMP_FRAMES) {
        g_dump_frames[g_dump_count++] = (uint32_t)strtoul(token, NULL, 10);
        token = strtok(NULL, ",");
    }
    free(copy);
}

void gb_platform_set_screenshot_prefix(const char* prefix) {
    if (prefix) snprintf(g_screenshot_prefix, sizeof(g_screenshot_prefix), "%s", prefix);
}

static void save_ppm(const char* filename, const uint32_t* fb, int width, int height, int frame_count) {
    // Calculate simple hash
    uint32_t hash = 0;
    for (int k = 0; k < width * height; k++) {
        hash = (hash * 33) ^ fb[k];
    }
    printf("[AUTO] Frame %d hash: %08X\n", frame_count, hash);

    FILE* f = fopen(filename, "wb");
    if (!f) return;
    
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    
    uint8_t* row = (uint8_t*)malloc(width * 3);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t p = fb[y * width + x];
            row[x*3+0] = (p >> 16) & 0xFF; // R
            row[x*3+1] = (p >> 8) & 0xFF;  // G
            row[x*3+2] = (p >> 0) & 0xFF;  // B
        }
        fwrite(row, 1, width * 3, f);
    }
    
    free(row);
    fclose(f);
    printf("[AUTO] Saved screenshot: %s\n", filename);
}


static int g_frame_count = 0;

/* ============================================================================
 * Platform Functions
 * ========================================================================== */

void gb_platform_shutdown(void) {
#ifdef LA_HAS_MULTIPLAYER
    mp_session_shutdown();
#endif

    if (g_gamepad) {
        SDL_GameControllerClose(g_gamepad);
        g_gamepad = NULL;
    }
    if (g_joystick) {
        SDL_JoystickClose(g_joystick);
        g_joystick = NULL;
    }
    if (g_audio_device) {
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
    }
    
    asset_viewer_shutdown();

#ifdef LA_HAS_IMGUI
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
#endif

    if (g_texture_2x) {
        SDL_DestroyTexture(g_texture_2x);
        g_texture_2x = NULL;
    }
    if (g_texture) {
        SDL_DestroyTexture(g_texture);
        g_texture = NULL;
    }
    if (g_renderer) {
        SDL_DestroyRenderer(g_renderer);
        g_renderer = NULL;
    }
    if (g_window) {
        SDL_DestroyWindow(g_window);
        g_window = NULL;
    }
    SDL_Quit();
}

/* ============================================================================
 * Audio
 * ========================================================================== */

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_BUFFER_SIZE 4096 /* Samples (stereo frames) */


static int16_t g_audio_buffer[AUDIO_BUFFER_SIZE * 2]; /* *2 for stereo */
static int g_audio_write_pos = 0;
static int g_audio_read_pos = 0;

static void sdl_audio_callback(void* userdata, Uint8* stream, int len) {
    (void)userdata;
    int16_t* output = (int16_t*)stream;
    int samples_needed = len / sizeof(int16_t) / 2; /* Stereo frames */
    
    for (int i = 0; i < samples_needed; i++) {
        if (g_audio_read_pos != g_audio_write_pos) {
            output[i*2] = g_audio_buffer[g_audio_read_pos*2];
            output[i*2+1] = g_audio_buffer[g_audio_read_pos*2+1];
            g_audio_read_pos = (g_audio_read_pos + 1) % AUDIO_BUFFER_SIZE;
        } else {
            /* Buffer underrun - silence */
            output[i*2] = 0;
            output[i*2+1] = 0;
        }
    }
}

static void on_audio_sample(GBContext* ctx, int16_t left, int16_t right) {
    (void)ctx;
    /* Apply master volume from menu */
    float vol = menu_gui_get_master_volume();
    left = (int16_t)(left * vol);
    right = (int16_t)(right * vol);

    int next_pos = (g_audio_write_pos + 1) % AUDIO_BUFFER_SIZE;
    if (next_pos != g_audio_read_pos) {
        g_audio_buffer[g_audio_write_pos*2] = left;
        g_audio_buffer[g_audio_write_pos*2+1] = right;
        g_audio_write_pos = next_pos;
    }
    /* Feed asset viewer waveform display / WAV recording */
    asset_viewer_push_audio(left, right);
}

bool gb_platform_init(int scale) {
    g_scale = scale;
    if (g_scale < 1) g_scale = 1;
    if (g_scale > 8) g_scale = 8;
    
    fprintf(stderr, "[SDL] Initializing SDL...\n");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "[SDL] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    fprintf(stderr, "[SDL] SDL initialized.\n");
    
    /* Initialize Audio */
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = sdl_audio_callback;
    want.userdata = NULL;
    
    g_audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio_device == 0) {
        fprintf(stderr, "[SDL] Failed to open audio: %s\n", SDL_GetError());
    } else {
        fprintf(stderr, "[SDL] Audio initialized: %d Hz, %d channels\n", have.freq, have.channels);
        SDL_PauseAudioDevice(g_audio_device, 0); /* Start playing */
    }
    
    fprintf(stderr, "[SDL] Creating window...\n");
    g_window = SDL_CreateWindow(
        "Link's Awakening Recompiled",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        GB_SCREEN_WIDTH * g_scale,
        GB_SCREEN_HEIGHT * g_scale,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    
    if (!g_window) {
        fprintf(stderr, "[SDL] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    fprintf(stderr, "[SDL] Window created.\n");
    
    fprintf(stderr, "[SDL] Creating renderer...\n");
    g_renderer = SDL_CreateRenderer(g_window, -1, 
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        
    if (!g_renderer) {
        fprintf(stderr, "[SDL] Hardware renderer failed (flags=0x%x), trying software fallback...\n", 
                SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
        
    if (!g_renderer) {
        fprintf(stderr, "[SDL] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return false;
    }
    
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

#ifdef LA_HAS_IMGUI
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Platform/Renderer backends
    ImGui_ImplSDL2_InitForSDLRenderer(g_window, g_renderer);
    ImGui_ImplSDLRenderer2_Init(g_renderer);
#endif

    // Initialize menu system (applies theme, loads saved settings)
    menu_gui_init();
    /* Use saved scale if available, otherwise keep the default */
    g_scale = menu_gui_get_scale();

    // Initialize asset viewer (needs renderer for texture creation)
    asset_viewer_init(g_renderer);

    /* Apply saved window scale */
    SDL_SetWindowSize(g_window, GB_SCREEN_WIDTH * g_scale, GB_SCREEN_HEIGHT * g_scale);
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    g_texture = SDL_CreateTexture(
        g_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        GB_SCREEN_WIDTH,
        GB_SCREEN_HEIGHT
    );
    
    if (!g_texture) {
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return false;
    }
    
    g_last_frame_time = SDL_GetTicks();
    
    return true;
}

/* Bit masks for each GB button in joypad registers.
 * D-pad buttons live in g_joypad_dpad, face buttons in g_joypad_buttons.
 * Active low: 0 = pressed, 1 = released. */
static const uint8_t gb_btn_dpad_mask[GB_BTN_COUNT] = {
    0x04, 0x08, 0x02, 0x01,  /* Up, Down, Left, Right */
    0, 0, 0, 0               /* A, B, Select, Start → buttons register */
};
static const uint8_t gb_btn_buttons_mask[GB_BTN_COUNT] = {
    0, 0, 0, 0,              /* Up, Down, Left, Right → dpad register */
    0x01, 0x02, 0x04, 0x08   /* A, B, Select, Start */
};

bool gb_platform_poll_events(GBContext* ctx) {
    g_ctx = ctx;

    if (menu_gui_quit_requested()) return false;

    /* ---- Open gamepad if needed ---- */
    if (!g_gamepad) {
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_IsGameController(i)) {
                g_gamepad = SDL_GameControllerOpen(i);
                if (g_gamepad) {
                    fprintf(stderr, "[SDL] Gamepad opened: %s\n", SDL_GameControllerName(g_gamepad));
                    break;
                }
            }
        }
    }
    /* If no device matched a GameController mapping, fall back to a raw
     * joystick (the OpenOrbis PS4 SDL2 port exposes the DualShock this
     * way — as an unmapped SDL_Joystick). */
    if (!g_gamepad && !g_joystick && SDL_NumJoysticks() > 0) {
        g_joystick = SDL_JoystickOpen(0);
        if (g_joystick)
            fprintf(stderr, "[SDL] Joystick opened (raw): %s\n",
                    SDL_JoystickName(g_joystick));
    }

    /* ---- Process SDL events (for ImGui, window, hotplug) ---- */
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
#ifdef LA_HAS_IMGUI
        ImGui_ImplSDL2_ProcessEvent(&event);
#endif
        if (event.type == SDL_QUIT) return false;
        if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE
            && event.window.windowID == SDL_GetWindowID(g_window))
            return false;

        /* Special keys — handled as events so they trigger once */
        if (event.type == SDL_KEYDOWN) {
            if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                menu_gui_toggle_settings();
            } else if (event.key.keysym.scancode == SDL_SCANCODE_F2) {
                menu_gui_toggle_debug();
            } else if (event.key.keysym.scancode == SDL_SCANCODE_F5) {
                gb_platform_save_state(ctx);
            } else if (event.key.keysym.scancode == SDL_SCANCODE_F7) {
                gb_platform_load_state(ctx);
            }
        }

        /* Gamepad hotplug */
        if (event.type == SDL_CONTROLLERDEVICEADDED && !g_gamepad) {
            int idx = event.cdevice.which;
            if (SDL_IsGameController(idx)) {
                g_gamepad = SDL_GameControllerOpen(idx);
                if (g_gamepad)
                    fprintf(stderr, "[SDL] Gamepad connected: %s\n", SDL_GameControllerName(g_gamepad));
            }
        }
        if (event.type == SDL_CONTROLLERDEVICEREMOVED && g_gamepad
            && event.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_gamepad))) {
            fprintf(stderr, "[SDL] Gamepad disconnected\n");
            SDL_GameControllerClose(g_gamepad);
            g_gamepad = NULL;
        }
    }

    /* ================================================================
     * POLL-BASED INPUT: rebuild joypad state from scratch every frame.
     * This eliminates stiffness from event ordering conflicts between
     * d-pad buttons, analog stick, and keyboard.
     * ================================================================ */

    uint8_t joyp = ctx ? ctx->io[0x00] : 0xFF;
    bool dpad_selected = !(joyp & 0x10);
    bool buttons_selected = !(joyp & 0x20);

    /* Save old state to detect new presses for interrupt */
    uint8_t old_dpad = g_joypad_dpad;
    uint8_t old_buttons = g_joypad_buttons;

    /* Start with all released */
    g_joypad_dpad = 0xFF;
    g_joypad_buttons = 0xFF;

    /* ---- Keyboard (poll) ---- */
    const Uint8* kbstate = SDL_GetKeyboardState(NULL);
    const KeyBind* kb = menu_gui_get_key_binds();

    for (int i = 0; i < GB_BTN_COUNT; i++) {
        bool held = false;
        if (kb[i].key0 >= 0 && kbstate[kb[i].key0]) held = true;
        if (kb[i].key1 >= 0 && kbstate[kb[i].key1]) held = true;

        if (held) {
            g_joypad_dpad    &= ~gb_btn_dpad_mask[i];
            g_joypad_buttons &= ~gb_btn_buttons_mask[i];
        }
    }

    /* ---- Gamepad (poll buttons + axes) ---- */
    if (g_gamepad) {
        const PadBind* pb = menu_gui_get_pad_binds();
        float dz = menu_gui_get_deadzone();
        int16_t dz_val = (int16_t)(dz * 32767);

        for (int i = 0; i < GB_BTN_COUNT; i++) {
            bool held = false;

            /* Primary button */
            if (pb[i].button >= 0 &&
                SDL_GameControllerGetButton(g_gamepad, (SDL_GameControllerButton)pb[i].button))
                held = true;

            /* Alt button */
            if (pb[i].button1 >= 0 &&
                SDL_GameControllerGetButton(g_gamepad, (SDL_GameControllerButton)pb[i].button1))
                held = true;

            /* Axis */
            if (pb[i].axis >= 0) {
                int16_t val = SDL_GameControllerGetAxis(g_gamepad, (SDL_GameControllerAxis)pb[i].axis);
                if (pb[i].axis_dir > 0 ? (val > dz_val) : (val < -dz_val))
                    held = true;
            }

            if (held) {
                g_joypad_dpad    &= ~gb_btn_dpad_mask[i];
                g_joypad_buttons &= ~gb_btn_buttons_mask[i];
            }
        }
    }
    /* ---- Raw joystick fallback (device has no GameController mapping) ---- */
    else if (g_joystick) {
        /* OpenOrbis PS4 SDL2 DualShock raw-button layout. Index order by
         * GB_BTN_*: UP, DOWN, LEFT, RIGHT, A, B, SELECT, START. */
        static const int js_btn[GB_BTN_COUNT] = {
            13,  /* UP                */
            14,  /* DOWN              */
            15,  /* LEFT              */
            16,  /* RIGHT             */
            0,   /* A      <- Cross   */
            1,   /* B      <- Circle  */
            8,   /* SELECT <- Share   */
            9,   /* START  <- Options */
        };
        int nb = SDL_JoystickNumButtons(g_joystick);
        int na = SDL_JoystickNumAxes(g_joystick);
        for (int i = 0; i < GB_BTN_COUNT; i++) {
            bool held = false;
            if (js_btn[i] < nb && SDL_JoystickGetButton(g_joystick, js_btn[i]))
                held = true;
            /* left analog stick (axes 0/1) also drives the d-pad */
            if (na >= 2) {
                if (i == GB_BTN_LEFT  && SDL_JoystickGetAxis(g_joystick, 0) < -16000) held = true;
                if (i == GB_BTN_RIGHT && SDL_JoystickGetAxis(g_joystick, 0) >  16000) held = true;
                if (i == GB_BTN_UP    && SDL_JoystickGetAxis(g_joystick, 1) < -16000) held = true;
                if (i == GB_BTN_DOWN  && SDL_JoystickGetAxis(g_joystick, 1) >  16000) held = true;
            }
            if (held) {
                g_joypad_dpad    &= ~gb_btn_dpad_mask[i];
                g_joypad_buttons &= ~gb_btn_buttons_mask[i];
            }
        }
    }

    /* ---- Automation script inputs ---- */
    for (int i = 0; i < g_script_count; i++) {
        ScriptEntry* e = &g_input_script[i];
        if (g_frame_count >= (int)e->start_frame && g_frame_count < (int)(e->start_frame + e->duration)) {
            g_joypad_dpad &= e->dpad;
            g_joypad_buttons &= e->buttons;
        }
    }

    /* ---- Fire joypad interrupt on new presses ---- */
    if (ctx) {
        /* Detect newly pressed bits (went from 1→0, active low) */
        uint8_t new_dpad_presses = old_dpad & ~g_joypad_dpad;
        uint8_t new_btn_presses  = old_buttons & ~g_joypad_buttons;

        bool trigger = (new_dpad_presses && dpad_selected) ||
                       (new_btn_presses && buttons_selected);
        if (trigger) {
            ctx->io[0x0F] |= 0x10;
            if (ctx->halted) ctx->halted = 0;
        }
    }

    return true;
}



void gb_platform_render_frame(const uint32_t* framebuffer) {
    if (!g_texture || !g_renderer || !framebuffer) {
        DBG_FRAME("Platform render_frame: SKIPPED (null: texture=%d, renderer=%d, fb=%d)",
                  g_texture == NULL, g_renderer == NULL, framebuffer == NULL);
        return;
    }
    
    g_frame_count++;
    
    /* Handle Screenshot Dumping */
    for (int i = 0; i < g_dump_count; i++) {
        if (g_dump_frames[i] == (uint32_t)g_frame_count) {
             char filename[128];
             snprintf(filename, sizeof(filename), "%s_%05d.ppm", g_screenshot_prefix, g_frame_count);
             save_ppm(filename, framebuffer, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT, g_frame_count);
        }
    }
    
    /* Debug: check framebuffer content on first few frames */
    if (g_frame_count <= 3) {
        /* Check if framebuffer has any non-white pixels */
        bool has_content = false;
        uint32_t white = 0xFFE0F8D0;  /* DMG palette color 0 */
        for (int i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; i++) {
            if (framebuffer[i] != white) {
                has_content = true;
                break;
            }
        }
        DBG_FRAME("Platform frame %d - has_content=%d, first_pixel=0x%08X",
                  g_frame_count, has_content, framebuffer[0]);
    }
    
    /* Auto-save ERAM every ~5 seconds (300 frames at 60fps) */
    if (g_ctx && g_frame_count % 300 == 0) {
        gb_context_save_ram(g_ctx);
    }

    /* Auto-save state every 5 minutes (18000 frames at 60fps) */
    if (g_ctx && menu_gui_get_auto_save() && g_frame_count > 0 && g_frame_count % 18000 == 0) {
        gb_platform_save_state(g_ctx);
        fprintf(stderr, "[AUTO] Auto-saved state at frame %d\n", g_frame_count);
    }
    
    /* Apply palette remap to a working buffer */
    static uint32_t fb_work[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT];
    int palette_idx = menu_gui_get_palette_idx();
    if (palette_idx == 0) {
        memcpy(fb_work, framebuffer, sizeof(fb_work));
    } else {
        for (int i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; i++) {
            uint32_t c = framebuffer[i];
            uint8_t r = (c >> 16) & 0xFF;
            uint8_t g = (c >>  8) & 0xFF;
            uint8_t b = (c >>  0) & 0xFF;
            int lum = (r * 77 + g * 150 + b * 29) >> 8;
            int shade;
            if      (lum < 64)  shade = 3;
            else if (lum < 128) shade = 2;
            else if (lum < 192) shade = 1;
            else                shade = 0;
            fb_work[i] = g_palettes[palette_idx][shade];
        }
    }

    /* Apply scanline effect (darken every other row by 40%) */
    bool do_scanlines = menu_gui_get_scanlines() != 0;
    if (do_scanlines) {
        for (int y = 1; y < GB_SCREEN_HEIGHT; y += 2) {
            for (int x = 0; x < GB_SCREEN_WIDTH; x++) {
                uint32_t c = fb_work[y * GB_SCREEN_WIDTH + x];
                uint8_t r = ((c >> 16) & 0xFF) * 3 / 5;
                uint8_t g = ((c >>  8) & 0xFF) * 3 / 5;
                uint8_t b = ((c >>  0) & 0xFF) * 3 / 5;
                fb_work[y * GB_SCREEN_WIDTH + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
        }
    }

#ifdef LA_HAS_MULTIPLAYER
    /* Render multiplayer player indicators (colored arrows/dots) */
    if (mp_session_is_active()) {
        int local_slot = mp_session_get_local_slot();
        const MPPlayer* local_p = mp_session_get_player(local_slot);
        if (local_p) {
            /* Build MPPlayerState array from session data */
            MPPlayerState ps[MP_MAX_PLAYERS];
            memset(ps, 0, sizeof(ps));
            for (int i = 0; i < MP_MAX_PLAYERS; i++) {
                const MPPlayer* p = mp_session_get_player(i);
                if (p) {
                    ps[i].connected = 1;
                    memcpy(ps[i].name, p->name, MP_MAX_NAME_LEN);
                    ps[i].color_h = p->color_h;
                    ps[i].color_s = p->color_s;
                    ps[i].color_v = p->color_v;
                    ps[i].map_room = p->map_room;
                    ps[i].is_indoor = p->is_indoor;
                    ps[i].dungeon_idx = p->dungeon_idx;
                    ps[i].health = p->health;
                    ps[i].max_health = p->max_health;
                    ps[i].link_x = p->link_x;
                    ps[i].link_y = p->link_y;
                    ps[i].ping_ms = p->ping_ms;
                }
            }
            mp_indicators_render(fb_work, local_slot,
                                 local_p->map_room, local_p->is_indoor,
                                 ps, MP_MAX_PLAYERS);
        }
    }
#endif

    /* Scale2x filter: produce 320x288 output with smoothed pixel edges */
    if (g_filter_mode == 2) {
        if (!g_texture_2x) {
            g_texture_2x = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                              SDL_TEXTUREACCESS_STREAMING,
                                              GB_SCREEN_WIDTH * 2, GB_SCREEN_HEIGHT * 2);
        }
        void* pixels;
        int pitch;
        SDL_LockTexture(g_texture_2x, NULL, &pixels, &pitch);
        uint32_t* dst = (uint32_t*)pixels;
        int dst_w = pitch / sizeof(uint32_t);

        for (int y = 0; y < GB_SCREEN_HEIGHT; y++) {
            for (int x = 0; x < GB_SCREEN_WIDTH; x++) {
                uint32_t P = fb_work[y * GB_SCREEN_WIDTH + x];
                uint32_t A = (y > 0) ? fb_work[(y-1) * GB_SCREEN_WIDTH + x] : P;
                uint32_t C = (y < GB_SCREEN_HEIGHT-1) ? fb_work[(y+1) * GB_SCREEN_WIDTH + x] : P;
                uint32_t B = (x > 0) ? fb_work[y * GB_SCREEN_WIDTH + x - 1] : P;
                uint32_t D = (x < GB_SCREEN_WIDTH-1) ? fb_work[y * GB_SCREEN_WIDTH + x + 1] : P;

                /* EPX/Scale2x algorithm */
                uint32_t E0 = P, E1 = P, E2 = P, E3 = P;
                if (A != C && B != D) {
                    if (B == A) E0 = B;
                    if (A == D) E1 = D;
                    if (B == C) E2 = B;
                    if (C == D) E3 = D;
                }

                dst[(y*2)   * dst_w + x*2]     = E0;
                dst[(y*2)   * dst_w + x*2 + 1] = E1;
                dst[(y*2+1) * dst_w + x*2]     = E2;
                dst[(y*2+1) * dst_w + x*2 + 1] = E3;
            }
        }
        SDL_UnlockTexture(g_texture_2x);

        SDL_RenderClear(g_renderer);
        SDL_RenderCopy(g_renderer, g_texture_2x, NULL, NULL);
    } else {
        /* Nearest or Bilinear: use 1x texture */
        void* pixels;
        int pitch;
        SDL_LockTexture(g_texture, NULL, &pixels, &pitch);
        memcpy(pixels, fb_work, GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT * sizeof(uint32_t));
        SDL_UnlockTexture(g_texture);

        SDL_RenderClear(g_renderer);
        SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    }

    // ImGui Frame
#ifdef LA_HAS_IMGUI
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
#endif

#ifdef LA_HAS_MULTIPLAYER
    /* Update multiplayer session */
    mp_session_update();
#endif

    /* Draw menu system */
    menu_gui_draw(g_ctx);

    /* Apply menu settings that affect SDL */
    int new_scale = menu_gui_get_scale();
    if (new_scale != g_scale) {
        g_scale = new_scale;
        SDL_SetWindowSize(g_window, GB_SCREEN_WIDTH * g_scale, GB_SCREEN_HEIGHT * g_scale);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }

    bool new_vsync = menu_gui_get_vsync() != 0;
    if (new_vsync != g_vsync) {
        g_vsync = new_vsync;
#if SDL_VERSION_ATLEAST(2, 0, 18)
        SDL_RenderSetVSync(g_renderer, g_vsync ? 1 : 0);
#else
        /* Older SDL2 (e.g. OpenOrbis's PS4 port) has no runtime vsync
         * toggle — vsync is fixed at renderer-creation time. */
#endif
    }

    int new_filter = menu_gui_get_filter_mode();
    if (new_filter != g_filter_mode) {
        g_filter_mode = new_filter;
        /* Recreate 1x texture with appropriate scale mode */
        if (g_texture) SDL_DestroyTexture(g_texture);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
                    (g_filter_mode == 1) ? "linear" : "nearest");
        g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT);
        /* Destroy/recreate 2x texture if switching to/from Scale2x */
        if (g_texture_2x) { SDL_DestroyTexture(g_texture_2x); g_texture_2x = NULL; }
    }

    /* Handle save state requests from menu */
    if (menu_gui_save_state_requested()) {
        gb_platform_save_state(g_ctx);
        menu_gui_clear_save_state_request();
    }
    if (menu_gui_load_state_requested()) {
        gb_platform_load_state(g_ctx);
        menu_gui_clear_load_state_request();
    }

#ifdef LA_HAS_IMGUI
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_renderer);
#endif

    SDL_RenderPresent(g_renderer);
}

uint8_t gb_platform_get_joypad(void) {
    /* Return combined state based on P1 register selection */
    /* Caller should AND with the appropriate selection bits */
    return g_joypad_buttons & g_joypad_dpad;
}

void gb_platform_vsync(void) {
    /* Target 59.7 FPS * Speed Multiplier */
    const uint32_t base_frame_time_ms = 16;
    int speed_pct = menu_gui_get_speed_percent();
    uint32_t scaled_frame_time = (base_frame_time_ms * 100) / (speed_pct > 0 ? speed_pct : 1);
    
    uint32_t current_time = SDL_GetTicks();
    uint32_t elapsed = current_time - g_last_frame_time;
    
    if (elapsed < scaled_frame_time) {
        SDL_Delay(scaled_frame_time - elapsed);
    }
    
    g_last_frame_time = SDL_GetTicks();
}

void gb_platform_set_title(const char* title) {
    if (g_window) {
        SDL_SetWindowTitle(g_window, title);
    }
}

/* ============================================================================
 * Save States (full program state snapshot)
 * ========================================================================== */

#define SAVE_STATE_MAGIC 0x4C415353  /* "LASS" - LA Save State */
#define SAVE_STATE_VERSION 1

/* Memory sizes (must match gbrt.c allocations) */
#define SS_WRAM_SIZE   (0x1000 * 8)   /* 8 WRAM banks */
#define SS_VRAM_SIZE   (0x2000 * 2)   /* 2 VRAM banks */
#define SS_OAM_SIZE    0xA0
#define SS_HRAM_SIZE   0x7F
#define SS_IO_SIZE     0x81

static void get_state_path(char* buf, size_t size) {
    char* base = SDL_GetBasePath();
    if (base) {
        snprintf(buf, size, "%ssavestate.bin", base);
        SDL_free(base);
    } else {
        snprintf(buf, size, "savestate.bin");
    }
}

void gb_platform_save_state(GBContext* ctx) {
    if (!ctx) return;

    char path[512];
    get_state_path(path, sizeof(path));

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[STATE] Failed to open %s for writing\n", path);
        return;
    }

    /* Header */
    uint32_t magic = SAVE_STATE_MAGIC;
    uint32_t version = SAVE_STATE_VERSION;
    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);

    /* CPU registers and state (fixed-size portion of GBContext) */
    /* Write everything from af through to last_joypad — the scalar fields */
    fwrite(&ctx->af, sizeof(uint16_t), 1, f);
    fwrite(&ctx->bc, sizeof(uint16_t), 1, f);
    fwrite(&ctx->de, sizeof(uint16_t), 1, f);
    fwrite(&ctx->hl, sizeof(uint16_t), 1, f);
    fwrite(&ctx->sp, sizeof(uint16_t), 1, f);
    fwrite(&ctx->pc, sizeof(uint16_t), 1, f);
    fwrite(&ctx->f_z, 1, 1, f);
    fwrite(&ctx->f_n, 1, 1, f);
    fwrite(&ctx->f_h, 1, 1, f);
    fwrite(&ctx->f_c, 1, 1, f);
    fwrite(&ctx->ime, 1, 1, f);
    fwrite(&ctx->ime_pending, 1, 1, f);
    fwrite(&ctx->halted, 1, 1, f);
    fwrite(&ctx->stopped, 1, 1, f);
    fwrite(&ctx->halt_bug, 1, 1, f);
    fwrite(&ctx->speed_switch_halt, sizeof(int32_t), 1, f);
    fwrite(&ctx->dma, sizeof(ctx->dma), 1, f);
    fwrite(&ctx->rom_bank, sizeof(uint16_t), 1, f);
    fwrite(&ctx->ram_bank, 1, 1, f);
    fwrite(&ctx->wram_bank, 1, 1, f);
    fwrite(&ctx->vram_bank, 1, 1, f);
    fwrite(&ctx->mbc_type, 1, 1, f);
    fwrite(&ctx->ram_enabled, 1, 1, f);
    fwrite(&ctx->mbc_mode, 1, 1, f);
    fwrite(&ctx->rom_bank_upper, 1, 1, f);
    fwrite(&ctx->rtc_mode, 1, 1, f);
    fwrite(&ctx->rtc_reg, 1, 1, f);
    fwrite(&ctx->cycles, sizeof(uint32_t), 1, f);
    fwrite(&ctx->frame_cycles, sizeof(uint32_t), 1, f);
    fwrite(&ctx->last_sync_cycles, sizeof(uint32_t), 1, f);
    fwrite(&ctx->frame_done, 1, 1, f);
    fwrite(&ctx->div_counter, sizeof(uint16_t), 1, f);
    fwrite(&ctx->last_joypad, 1, 1, f);

    /* Memory regions */
    if (ctx->wram)  fwrite(ctx->wram, 1, SS_WRAM_SIZE, f);
    if (ctx->vram)  fwrite(ctx->vram, 1, SS_VRAM_SIZE, f);
    if (ctx->oam)   fwrite(ctx->oam,  1, SS_OAM_SIZE, f);
    if (ctx->hram)  fwrite(ctx->hram, 1, SS_HRAM_SIZE, f);
    if (ctx->io)    fwrite(ctx->io,   1, SS_IO_SIZE, f);

    /* External RAM */
    uint32_t eram_sz = (uint32_t)ctx->eram_size;
    fwrite(&eram_sz, 4, 1, f);
    if (ctx->eram && eram_sz > 0)
        fwrite(ctx->eram, 1, eram_sz, f);

    /* PPU state */
    if (ctx->ppu) {
        uint8_t has_ppu = 1;
        fwrite(&has_ppu, 1, 1, f);
        fwrite(ctx->ppu, sizeof(GBPPU), 1, f);
    } else {
        uint8_t has_ppu = 0;
        fwrite(&has_ppu, 1, 1, f);
    }

    fclose(f);
    fprintf(stderr, "[STATE] Save state written to %s\n", path);
}

void gb_platform_load_state(GBContext* ctx) {
    if (!ctx) return;

    char path[512];
    get_state_path(path, sizeof(path));

    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[STATE] No save state found at %s\n", path);
        return;
    }

    /* Verify header */
    uint32_t magic = 0, version = 0;
    fread(&magic, 4, 1, f);
    fread(&version, 4, 1, f);
    if (magic != SAVE_STATE_MAGIC || version != SAVE_STATE_VERSION) {
        fprintf(stderr, "[STATE] Invalid save state (magic=%08X ver=%u)\n", magic, version);
        fclose(f);
        return;
    }

    /* CPU registers and state */
    fread(&ctx->af, sizeof(uint16_t), 1, f);
    fread(&ctx->bc, sizeof(uint16_t), 1, f);
    fread(&ctx->de, sizeof(uint16_t), 1, f);
    fread(&ctx->hl, sizeof(uint16_t), 1, f);
    fread(&ctx->sp, sizeof(uint16_t), 1, f);
    fread(&ctx->pc, sizeof(uint16_t), 1, f);
    fread(&ctx->f_z, 1, 1, f);
    fread(&ctx->f_n, 1, 1, f);
    fread(&ctx->f_h, 1, 1, f);
    fread(&ctx->f_c, 1, 1, f);
    fread(&ctx->ime, 1, 1, f);
    fread(&ctx->ime_pending, 1, 1, f);
    fread(&ctx->halted, 1, 1, f);
    fread(&ctx->stopped, 1, 1, f);
    fread(&ctx->halt_bug, 1, 1, f);
    fread(&ctx->speed_switch_halt, sizeof(int32_t), 1, f);
    fread(&ctx->dma, sizeof(ctx->dma), 1, f);
    fread(&ctx->rom_bank, sizeof(uint16_t), 1, f);
    fread(&ctx->ram_bank, 1, 1, f);
    fread(&ctx->wram_bank, 1, 1, f);
    fread(&ctx->vram_bank, 1, 1, f);
    fread(&ctx->mbc_type, 1, 1, f);
    fread(&ctx->ram_enabled, 1, 1, f);
    fread(&ctx->mbc_mode, 1, 1, f);
    fread(&ctx->rom_bank_upper, 1, 1, f);
    fread(&ctx->rtc_mode, 1, 1, f);
    fread(&ctx->rtc_reg, 1, 1, f);
    fread(&ctx->cycles, sizeof(uint32_t), 1, f);
    fread(&ctx->frame_cycles, sizeof(uint32_t), 1, f);
    fread(&ctx->last_sync_cycles, sizeof(uint32_t), 1, f);
    fread(&ctx->frame_done, 1, 1, f);
    fread(&ctx->div_counter, sizeof(uint16_t), 1, f);
    fread(&ctx->last_joypad, 1, 1, f);

    /* Memory regions */
    if (ctx->wram)  fread(ctx->wram, 1, SS_WRAM_SIZE, f);
    if (ctx->vram)  fread(ctx->vram, 1, SS_VRAM_SIZE, f);
    if (ctx->oam)   fread(ctx->oam,  1, SS_OAM_SIZE, f);
    if (ctx->hram)  fread(ctx->hram, 1, SS_HRAM_SIZE, f);
    if (ctx->io)    fread(ctx->io,   1, SS_IO_SIZE, f);

    /* External RAM */
    uint32_t eram_sz = 0;
    fread(&eram_sz, 4, 1, f);
    if (ctx->eram && eram_sz > 0 && eram_sz <= ctx->eram_size)
        fread(ctx->eram, 1, eram_sz, f);

    /* PPU state */
    uint8_t has_ppu = 0;
    fread(&has_ppu, 1, 1, f);
    if (has_ppu && ctx->ppu)
        fread(ctx->ppu, sizeof(GBPPU), 1, f);

    fclose(f);

    /* Clear audio ring buffer to prevent crackling from stale samples */
    SDL_LockAudioDevice(g_audio_device);
    memset(g_audio_buffer, 0, sizeof(g_audio_buffer));
    g_audio_write_pos = 0;
    g_audio_read_pos = 0;
    SDL_UnlockAudioDevice(g_audio_device);

    fprintf(stderr, "[STATE] Save state loaded from %s\n", path);
}

/* ============================================================================
 * Save Data (battery-backed SRAM)
 * ========================================================================== */

static void sdl_get_save_path(char* buffer, size_t size, const char* rom_name) {
    char* base_path = SDL_GetBasePath();
    if (base_path) {
        // Extract just the filename from rom_name to avoid path traversal issues
        const char* base_name = strrchr(rom_name, '/');
#ifdef _WIN32
        const char* base_name_win = strrchr(rom_name, '\\');
        if (base_name_win > base_name) base_name = base_name_win;
#endif
        if (base_name) {
            base_name++; // Skip separator
        } else {
            base_name = rom_name;
        }

        snprintf(buffer, size, "%s%s.sav", base_path, base_name);
        SDL_free(base_path);
    } else {
        // Fallback to CWD if SDL_GetBasePath fails
        snprintf(buffer, size, "%s.sav", rom_name);
    }
}

static bool sdl_load_battery_ram(GBContext* ctx, const char* rom_name, void* data, size_t size) {
    (void)ctx;
    char filename[512];
    sdl_get_save_path(filename, sizeof(filename), rom_name);
    
    FILE* f = fopen(filename, "rb");
    if (!f) return false;
    
    size_t read = fread(data, 1, size, f);
    fclose(f);
    
    return read == size;
}

static bool sdl_save_battery_ram(GBContext* ctx, const char* rom_name, const void* data, size_t size) {
    (void)ctx;
    char filename[512];
    sdl_get_save_path(filename, sizeof(filename), rom_name);
    
    FILE* f = fopen(filename, "wb");
    if (!f) return false;
    
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    
    return written == size;
}

void gb_platform_register_context(GBContext* ctx) {
    GBPlatformCallbacks callbacks = {
        .on_audio_sample = on_audio_sample,
        .load_battery_ram = sdl_load_battery_ram,
        .save_battery_ram = sdl_save_battery_ram
    };
    gb_set_platform_callbacks(ctx, &callbacks);
}

#else  /* !GB_HAS_SDL2 */

/* Stub implementations when SDL2 is not available */

bool gb_platform_init(int scale) {
    (void)scale;
    return false;
}

void gb_platform_shutdown(void) {}

bool gb_platform_poll_events(GBContext* ctx) {
    (void)ctx;
    return true;
}

void gb_platform_render_frame(const uint32_t* framebuffer) {
    (void)framebuffer;
}

uint8_t gb_platform_get_joypad(void) {
    return 0xFF;
}

void gb_platform_vsync(void) {}

void gb_platform_set_title(const char* title) {
    (void)title;
}

void gb_platform_register_context(GBContext* ctx) { (void)ctx; }

#endif /* GB_HAS_SDL2 */
