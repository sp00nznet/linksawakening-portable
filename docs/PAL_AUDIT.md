# Phase 2 — PAL contract audit

Read of every `.c` and `.cpp` in `runtime/src/` (excluding `platform_sdl.cpp`
itself) for direct calls to SDL2, ImGui, stdio, Win32, or threading APIs.
Goal: catalogue every place the runtime escapes the platform abstraction so we
know exactly what each new backend (libxenon, NXDK, WASM, etc.) has to provide.

## TL;DR

**The PAL is in better shape than the original plan assumed.** It has two
surfaces, not one:

1. **`platform_sdl.h`** — 13 `gb_platform_*` functions the runtime calls into
   the platform.
2. **`GBPlatformCallbacks`** struct (defined in `gbrt.h:65-74`) — function
   pointers the platform registers via `gb_set_platform_callbacks(ctx, &cbs)`.
   Covers audio submission, SRAM save/load, serial link, and an alternate
   frame/joypad path.

So **audio submission and SRAM persistence already have proper PAL hooks** —
they were just invisible from the `platform_sdl.h` header alone.

The remaining escapes are concentrated in three subsystems that are inherently
host-coupled: the **debug UI** (menu_gui + asset_viewer), the **multiplayer
overlay** (mp_*.cpp), and **settings file I/O** in those same files. The
**core game runtime** (`gbrt.c`, `ppu.c`, `audio.c`, `interpreter.c`,
`hwtrace.c`) has zero direct SDL or ImGui calls.

## The full PAL contract

### Surface 1 — `runtime/include/platform_sdl.h` (runtime → platform)

```c
// State
extern uint8_t g_joypad_buttons;
extern uint8_t g_joypad_dpad;

// Lifecycle
bool gb_platform_init(int scale);
void gb_platform_register_context(GBContext* ctx);
void gb_platform_shutdown(void);

// Frame loop
bool gb_platform_poll_events(GBContext* ctx);
void gb_platform_render_frame(const uint32_t* framebuffer);  // 160x144 ARGB8888
void gb_platform_vsync(void);

// Misc
uint8_t gb_platform_get_joypad(void);
void    gb_platform_set_title(const char* title);
void    gb_platform_save_state(GBContext* ctx);
void    gb_platform_load_state(GBContext* ctx);

// Optional debug
void gb_platform_set_input_script(const char* script);
void gb_platform_set_dump_frames(const char* frames);
void gb_platform_set_screenshot_prefix(const char* prefix);
```

### Surface 2 — `runtime/include/gbrt.h:65-74` (platform → runtime via fn ptrs)

```c
typedef struct {
    void    (*on_vblank)(GBContext* ctx, const uint8_t* framebuffer);
    void    (*on_audio_sample)(GBContext* ctx, int16_t left, int16_t right);
    uint8_t (*get_joypad)(GBContext* ctx);
    void    (*on_serial_byte)(GBContext* ctx, uint8_t byte);

    bool    (*load_battery_ram)(GBContext* ctx, const char* rom_name, void* data, size_t size);
    bool    (*save_battery_ram)(GBContext* ctx, const char* rom_name, const void* data, size_t size);
} GBPlatformCallbacks;

void gb_set_platform_callbacks(GBContext* ctx, const GBPlatformCallbacks* callbacks);
```

Registered during `gb_platform_register_context()` (which the existing SDL2
backend does at `platform_sdl.cpp:1017`). The runtime calls each pointer
from inside `gb_audio_callback`, `gb_save_battery_ram_to_disk`, etc.

## Inventory of escapes (excluding `platform_sdl.cpp`)

### SDL2 direct calls (103 total across 6 files)

| File                          | Count | What for                                          |
| ----------------------------- | ----- | ------------------------------------------------- |
| `menu_gui.cpp`                |   54  | ImGui SDL renderer hookup, key/text input         |
| `asset_viewer.cpp`            |   25  | Capture window/renderer pointer for ImGui textures |
| `multiplayer/mp_voice.cpp`    |   15  | `SDL_OpenAudioDevice` for voice **capture**       |
| `menu_gui.h`                  |    6  | Forward decls                                     |
| `multiplayer/mp_integration.h`|    2  | Forward decls                                     |
| `multiplayer/mp_menu.cpp`     |    1  | Hint string                                       |

### ImGui direct calls (505 total across 4 files)

| File                          | Count | What for                                          |
| ----------------------------- | ----- | ------------------------------------------------- |
| `multiplayer/mp_menu.cpp`     |  220  | Host / Join / Settings / Trade / Voice / PvP UI   |
| `menu_gui.cpp`                |  179  | Main menu bar + all settings windows              |
| `asset_viewer.cpp`            |  105  | Tile / palette / OAM / waveform inspector         |
| `menu_gui.h`                  |    1  | Forward decl                                      |

### File I/O — fopen / fwrite / fclose / fseek

| File                          | Lines           | What it writes                                |
| ----------------------------- | --------------- | --------------------------------------------- |
| `asset_viewer.cpp`            | 54, 80–95       | BMP screenshot of current frame               |
| `asset_viewer.cpp`            | 112–126, 526–703| WAV recording of APU output                   |
| `menu_gui.cpp`                | 165–227         | Settings (bindings, master volume) — `bindings.cfg` |
| `multiplayer/mp_menu.cpp`     | 887–939         | MP settings (player name, color, ports) — separate config |
| `hwtrace.c`                   | 54, 55, 66      | Hardware trace log (debug-only, `--hw-trace FILE`) |
| `gbrt.c`                      | 61, 79          | Instruction trace dump (debug-only, `--trace-entries FILE`) |

### Network — Winsock / sockets

| File                       | Line | What for                              |
| -------------------------- | ---- | ------------------------------------- |
| `multiplayer/mp_menu.cpp`  |  34  | `WSAStartup(MAKEWORD(2,2), &wsa)`     |

ENet's own sockets are abstracted inside `third_party/enet/`; the WSAStartup
above is a one-shot Winsock init the MP menu does directly. On non-Windows
targets, ENet provides equivalent init via its own `enet_initialize()`.

### printf / fprintf (127 hits across 13 files)

Used liberally for debug logging. Acceptable on every target — `printf` is
in the C standard library on libxenon (devkitPPC), Android NDK, Emscripten,
NXDK, PSL1GHT, and KallistiOS. No PAL extension needed; just note that
`stderr` may not be visible on consoles without a serial cable.

### Time / clock / threading

**Zero direct calls.** All timing goes through `gb_platform_vsync()` in
`platform_sdl.cpp`, which uses SDL's own pacing. Runtime is single-threaded.

### Direct `#include <SDL.h>` outside `platform_sdl.cpp`

| File                          | Line |
| ----------------------------- | ---- |
| `asset_viewer.cpp`            |  6   |
| `menu_gui.cpp`                | 10   |
| `multiplayer/mp_menu.cpp`     | 15   |
| `multiplayer/mp_voice.cpp`    |  7   |

All in the "debug UI + multiplayer" cluster — none in the core engine.

## Findings — what each escape means for new backends

### Core engine is clean

`gbrt.c`, `ppu.c`, `audio.c`, `interpreter.c` have **no SDL or ImGui calls**
of any kind. The CPU, PPU, APU, and interpreter compile and run on any
target that has a C compiler. This is the load-bearing finding.

### Audio submission and SRAM are already abstracted

- **Audio**: `audio.c:648` calls `gb_audio_callback(ctx, left, right)`,
  which `gbrt.c:1287` dispatches to `ctx->callbacks.on_audio_sample`. SDL
  backend registers `on_audio_sample` at `platform_sdl.cpp:234` to write
  into a ring buffer that `sdl_audio_callback` drains. For libxenon, we
  register our own `on_audio_sample` that writes to a libxenon audio
  output buffer. No PAL extension required.
- **SRAM**: `GBPlatformCallbacks.load_battery_ram` / `save_battery_ram`.
  Path resolution is per-platform (the callback signature takes
  `rom_name` and lets the backend decide where to put it). SDL backend
  resolves to CWD via stdio. For libxenon, write to `uda:/linksawakening/
  <rom_name>.sav`. No PAL extension required.

### Settings file I/O needs a small contract addition

`menu_gui.cpp` and `mp_menu.cpp` directly `fopen("bindings.cfg", ...)` in
CWD. Two options:

1. **Add `gb_platform_fs_read/write(logical_name, buf, len)` to
   `platform_sdl.h`** and route both files through it. Each backend
   resolves `"bindings.cfg"` to its own paths
   (`uda:/linksawakening/bindings.cfg` on libxenon, IDBFS path on WASM,
   `getFilesDir()` on Android). 5 call sites total.
2. **Stub menu_gui + mp_menu out on non-SDL targets** — no settings file
   means joypad uses default bindings. Acceptable for a v1 360 build.

Recommended: **defer the PAL extension to Phase 6**, ship 360 with the
default bindings to keep Phase 5 scope tight.

### menu_gui.cpp + asset_viewer.cpp = ImGui-or-stub

The menu and asset viewer are pure ImGui + SDL renderer state. Options:

1. **Stub** them on libxenon (`#ifdef GB_HAS_SDL2` guards or empty
   replacement files). No in-game menu, no asset viewer. The game runs
   fine without them — controls go through `g_joypad_buttons`/`g_joypad_dpad`
   regardless.
2. **Port ImGui to Xenos** — write a Xenos renderer backend for ImGui.
   Substantial work; defer indefinitely.

Recommended: **stub on libxenon for Phase 5.** Add `LA_HAS_IMGUI` CMake
option, default ON on SDL, default OFF on libxenon. menu_gui.cpp and
asset_viewer.cpp compile only when ON.

### Multiplayer overlay = stub-or-patch

Three coupling points:
- `mp_menu.cpp` uses ImGui (220 calls) + Winsock init + settings file I/O
- `mp_voice.cpp` uses SDL audio capture (15 calls)
- The rest of the `mp_*.cpp` files use ENet but go through ENet's
  cross-platform layer — actually portable to libxenon if ENet builds

ENet 1.3.18 supports Windows (`winsock`), POSIX (`socket`), and is
straightforward to port to libxenon (`network.h` provides POSIX-like sockets).
But voice chat (`mp_voice.cpp`) needs an audio capture path libxenon doesn't
have, and the MP menu needs ImGui.

Recommended: **`LA_MULTIPLAYER=OFF` on libxenon, with the upstream patch we
already designed.** Add `#ifdef LA_HAS_MULTIPLAYER` guards in `platform_sdl.cpp`
(lines 168, 603–628, 692) and `menu_gui.cpp` (lines 433, 861, 862, 897) so
the build links cleanly with MP disabled. This is the patch we identified at
the end of the LinksAwakening fix-up.

### Debug-only file I/O can be left alone

`hwtrace.c`, `gbrt.c` (trace_file), `asset_viewer.cpp` (BMP/WAV dumps) all
write to files only when explicitly enabled via CLI flags (`--hw-trace`,
`--trace-entries`, asset viewer "save" button). On targets without
writable CWD, these just silently no-op when `fopen` returns NULL. No PAL
extension needed; existing code already handles `NULL` return.

### WSAStartup in mp_menu.cpp

Single line. When `LA_MULTIPLAYER=OFF` it's not compiled. When ON on
non-Windows targets, wrap in `#ifdef _WIN32` (it's only needed for Winsock).
ENet has its own per-platform init via `enet_initialize()`.

## Phase 5 (libxenon backend) shopping list

Based on this audit, here's what the libxenon platform layer needs to provide
to get a v1 360 build running:

### Required

1. **All 13 `gb_platform_*` functions** from `platform_sdl.h`. Implementations
   use libxenon APIs:
   - `gb_platform_init` — `xenos_init`, USB gamepad init, audio init
   - `gb_platform_poll_events` — USB gamepad poll, set `g_joypad_buttons` /
     `g_joypad_dpad`
   - `gb_platform_render_frame` — upload 160×144 ARGB to a Xenos surface,
     blit scaled
   - `gb_platform_vsync` — wait for next Xenos vsync
   - `gb_platform_get_joypad` — return `g_joypad_buttons | g_joypad_dpad`
   - `gb_platform_save/load_state` — write to `uda:/linksawakening/state.bin`
   - `gb_platform_set_title` — no-op (no window title on 360)
   - `gb_platform_set_input_script / dump_frames / screenshot_prefix` —
     no-ops

2. **`GBPlatformCallbacks` with `on_audio_sample` + `load/save_battery_ram`**,
   registered in `gb_platform_register_context`. Audio writes to a libxenon
   audio output ring buffer; battery RAM goes to `uda:/linksawakening/
   <rom_name>.sav`.

### Build-time gates

3. **`LA_MULTIPLAYER=OFF`** for the 360 build. Requires the small upstream
   patch to `runtime/src/platform_sdl.cpp` and `runtime/src/menu_gui.cpp`
   adding `#ifdef LA_HAS_MULTIPLAYER` guards. ~30 lines total.
4. **`LA_HAS_IMGUI=OFF`** for the 360 build (new option). Excludes
   `menu_gui.cpp` and `asset_viewer.cpp` from the gbrt library. Requires
   the `rom_main.c` loop to not call any `menu_gui_*` functions
   (currently it doesn't — the menu is driven from inside
   `platform_sdl.cpp::gb_platform_render_frame`, which we're replacing
   anyway).

### Optional / deferrable

5. **`gb_platform_fs_read/write` PAL extension** for settings persistence.
   Defer to Phase 6 — ship Phase 5 with hardcoded default bindings.
6. **`gb_platform_log` PAL extension** to redirect `printf`/`fprintf` through
   a platform-specific logger (libxenon `xenos_log`, etc.). Cosmetic — `printf`
   works on libxenon out of the box, it just goes to USB serial.

## Endianness audit (also part of Phase 3)

Quick grep results — full audit deferred to Phase 3 but flagging early:

- Multi-byte reinterpret casts in `runtime/src/*.c`: not yet measured, but
  `ppu.c` and `gbrt.c` are the likely suspects (tile decoding, palette lookup).
- All ROM-derived data in `rom_rom.c` is `uint8_t[]` — safe.
- All recompiled game code in `rom.c` goes through `gb_read8/16` helpers —
  the helpers themselves must be endian-correct on BE targets.

## What changes in the project plan

| Phase plan item                                      | Was              | Now (after audit)                                  |
| ---------------------------------------------------- | ---------------- | -------------------------------------------------- |
| Phase 2: "Extract PAL"                               | A real refactor  | Done by upstream. Confirmed via this audit.        |
| Phase 5: "Implement `platform_libxenon.cpp`"          | 13 functions     | 13 functions + `GBPlatformCallbacks` registration  |
| New: "Upstream MP+ImGui gating patches"              | not in plan      | Add as Phase 5a (small upstream patch to gb-recompiled) |
| Old Phase 6: "rom_main.c MP guard"                   | Standalone phase | Folded into Phase 5a                               |
| New: "Settings PAL extension"                        | not in plan      | Phase 6 — small, deferrable                        |

README phase table updated accordingly.
