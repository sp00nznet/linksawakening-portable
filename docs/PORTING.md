# Porting design

The runtime already has a Platform Abstraction Layer. This doc describes the
contract (`runtime/include/platform_sdl.h`), what's known about each call's
semantics, and what each backend has to deliver.

## The PAL contract

13 functions, declared in `runtime/include/platform_sdl.h`. The header is
named `platform_sdl.h` for historical reasons, but the prototypes are
platform-agnostic (`gb_platform_*`). Do not rename it — too many includes.

```c
// State the platform owns and the runtime reads:
extern uint8_t g_joypad_buttons;     // A, B, Select, Start (bits set = pressed)
extern uint8_t g_joypad_dpad;        // Right, Left, Up, Down

// Lifecycle:
bool gb_platform_init(int scale);                       // create window, audio, input
void gb_platform_register_context(GBContext* ctx);      // wire callbacks (save file load fires here)
void gb_platform_shutdown(void);

// Frame loop (called per frame in this order):
bool gb_platform_poll_events(GBContext* ctx);           // false = quit requested
void gb_platform_render_frame(const uint32_t* fb);      // 160x144 ARGB8888 framebuffer
void gb_platform_vsync(void);                           // pace to 60 Hz

// Input pull (called by the GB joypad register read):
uint8_t gb_platform_get_joypad(void);                   // active-low GB joypad byte

// Window cosmetics:
void gb_platform_set_title(const char* title);

// Save state:
void gb_platform_save_state(GBContext* ctx);
void gb_platform_load_state(GBContext* ctx);
```

Plus three optional debug/test entry points referenced by `rom_main.c` but
implemented in `platform_sdl.cpp` only:

```c
void gb_platform_set_input_script(const char* path);      // replay deterministic inputs
void gb_platform_set_dump_frames(const char* dir);        // dump every frame as PNG
void gb_platform_set_screenshot_prefix(const char* prefix);
```

For non-SDL backends, these can be no-op stubs.

## What's NOT in the contract (and probably should be)

These would be Phase 2 audit findings. The audit hasn't run yet — list will
grow as we discover them.

- **Audio submit.** `audio.c` in the runtime currently uses SDL audio directly
  (we should grep to confirm — if so, it's an escape from the PAL contract
  that the libxenon backend will have to work around or that we'll need to
  add to the contract).
- **File system.** Save files and config paths are resolved by the SDL backend
  using stdio in CWD. On 360/Android/WASM the storage path is platform-specific
  and the runtime needs a hook to resolve "save file for cartridge XYZ" → path.
- **Time.** Audio mixing needs monotonic time; the SDL backend reads
  `SDL_GetTicks` directly inside the runtime. Same audit issue.
- **Log.** The runtime calls `printf` / `fprintf(stderr, ...)`. On 360 stderr
  goes to USB serial or to libxenon's logging API. Probably fine to leave as
  `printf` and let the platform redirect.

If the audit finds direct SDL2 calls in `runtime/src/*.c`, we have two options:

1. **Extend the contract.** Add e.g. `gb_platform_audio_submit()` to
   `platform_sdl.h` and move the SDL audio code from `audio.c` into
   `platform_sdl.cpp`. Cleanest.
2. **Per-target shim.** Let each backend provide its own `SDL_*` stubs. Faster
   but bleeds SDL semantics into every backend forever.

Default to option 1 when the audit finds violations.

## Backend rollout

| Backend         | File                      | Status     | Notes |
| --------------- | ------------------------- | ---------- | ----- |
| SDL2 (current)  | `platform_sdl.cpp`        | ✓ upstream | Windows, macOS, Linux, also valid on WASM via Emscripten's SDL2 port |
| libxenon (360)  | `platform_libxenon.cpp`   | Phase 5    | Xenos GPU framebuffer, USB gamepad, audio out, FATFS for saves |
| NXDK (OG Xbox)  | `platform_nxdk.cpp`       | later      | x86 native, D3D8 directly |
| PSL1GHT (PS3)   | `platform_psl1ght.cpp`    | later      | PPC big-endian, inherits all 360 endian fixes |
| KOS (Dreamcast) | `platform_kos.cpp`        | later      | SH-4, 16 MB RAM — squeeze; PVR framebuffer; AICA audio |
| Emscripten/WASM | `platform_wasm.cpp` *or* reuse `platform_sdl.cpp` | later | Emscripten has SDL2 — may not need a new backend at all |

## Endianness audit checklist (Phase 3)

Run before Phase 5 and again before PS3 work. The recompiled `rom.c` is mostly
endian-clean because GB memory access goes through `gbrt`'s read/write helpers,
but the runtime itself has direct loads we need to audit.

- [ ] Grep `runtime/src` for `*(uint16_t*)`, `*(uint32_t*)`, `*(uint64_t*)` — any
      reinterpret loads need to use byte-by-byte helpers on BE
- [ ] Grep `runtime/src` for `memcpy(.*, .*, 2)` / `memcpy(.*, .*, 4)` followed
      by direct use of the integer — need swap on BE
- [ ] `rom_rom.c` arrays are `uint8_t[]` — safe by construction, just confirm
- [ ] Save state format (`savestate.bin`) — pick one endianness (LE) and convert
- [ ] PNG screenshot writer — uses big-endian on disk but typically reads from
      ARGB host-endian in memory; check `platform_sdl.cpp` PNG writer
- [ ] ImGui rendering — uses 32-bit packed colors; ImGui itself is endian-aware

Helper macros to introduce at the top of `runtime/include/gbrt.h` if not
already present:

```c
static inline uint16_t gb_load16_le(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t gb_load32_le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
```

## 32-bit user-space audit (Phase 3, same pass)

360, PS3, NXDK, DC are all 32-bit user space. WASM has its own width quirks.
Watch for:

- [ ] `uintptr_t` math that assumes 64 bits
- [ ] `size_t` formatted with `%lu` (use `%zu`)
- [ ] `time_t` width assumptions in save state header
- [ ] Pointer-tagging tricks (`& ~7` on pointers)
- [ ] Anywhere ImGui takes `void*` user_data with pointer-fits-in-int casts

## Multiplayer gating (Phase 6)

`rom_main.c` calls `mp_session_is_client_connected()`, `mp_session_update()`,
and `mp_session_get_framebuffer()` unconditionally — there's no `#ifdef` guard.
With `LA_MULTIPLAYER=OFF`, those symbols don't exist and the link fails.

Fix: wrap the relevant blocks in `#ifdef LA_HAS_MULTIPLAYER` (the symbol our
CMakeLists defines when `LA_MULTIPLAYER=ON`). One block at the top of the main
loop is enough. This is a small upstream-friendly patch — worth proposing
back to `la-dx-recompiled` if the maintainer is open to it.

## Open questions for the libxenon backend (Phase 5)

- **Threads** — libxenon supports threads via lwp. The APU mixer can run on
  a worker thread to smooth audio (`platform_sdl.cpp` does this). Measure
  single-thread first; only add threading if audio underruns.
- **Filesystem** — pick a save path. Probably `uda:/linksawakening/` (USB) by
  default with fallback to HDD. libxenon's FATFS layer abstracts the medium.
- **Xenia vs RGH** — Xenia compatibility for libxenon XEX files has improved
  but isn't 100%. Test on real hardware periodically if you have access.
- **ImGui on Xenos** — Xenos is a custom Radeon X1900-ish chip. ImGui doesn't
  have a stock backend; we'd write a small one against `xenos` library calls.
  Alternative: stub the menu entirely on 360 (the game itself doesn't need it
  to be playable, only the multiplayer/debug overlays do).
- **rom.c size** — 115 MB of C through GCC PPC will be **slow**. Build time
  on a single core may be 10+ minutes. Consider splitting like
  `linksawakening-win` did into per-bank files — but that's a recompiler-output
  change, not a port change.
