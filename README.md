# linksawakening-portable

Multi-platform port of the static recompilation of *The Legend of Zelda:
Link's Awakening DX* (Game Boy Color). Starting target: **Xbox 360** via
[libxenon](https://github.com/Free60Project/libxenon), testable in
[Xenia](https://xenia.jp/). Designed so additional backends — WebAssembly,
Android, Original Xbox (NXDK), PS3 (PSL1GHT), Dreamcast (KallistiOS) — can
drop in as siblings of the existing SDL2 backend.

Built on:

- **[`sp00nznet/la-dx-recompiled`](https://github.com/sp00nznet/la-dx-recompiled)** (private)
  — the game project (`rom.c` ~115 MB recompiler output, `rom_main.c` entry,
  `rom_rom.c` ROM bytes). Vendored into this tree at the root.
- **[`sp00nznet/gb-recompiled`](https://github.com/sp00nznet/gb-recompiled)**
  (fork of [`arcanite24/gb-recompiled`](https://github.com/arcanite24/gb-recompiled))
  — the runtime engine (`gbrt`, `ppu`, `audio`, `interpreter`, `menu_gui`, plus
  the SDL2 platform layer in `platform_sdl.cpp` and `platform_sdl.h`). Vendored
  at `runtime/`.

> **Status:** Phase 1 — Windows reference build from this tree. Xbox 360 backend
> not yet started.

---

## The big architectural insight

The upstream `gb-recompiled` runtime **already has a Platform Abstraction Layer**:
`runtime/include/platform_sdl.h` defines a 13-function `gb_platform_*` contract
that the rest of the runtime calls into (init, poll events, render frame, vsync,
get joypad, save/load state, set title). `platform_sdl.cpp` is the SDL2
implementation of that contract.

That means **per-target porting is mostly: write a sibling `platform_<target>.cpp`
that satisfies the same header.** No runtime refactor required.

```
runtime/include/platform_sdl.h        ← the contract (do not rename)
runtime/src/platform_sdl.cpp          ← SDL2 backend       (Windows / macOS / Linux / WASM-via-SDL)
runtime/src/platform_libxenon.cpp     ← Xbox 360 backend   (Phase 4)
runtime/src/platform_nxdk.cpp         ← Original Xbox      (later)
runtime/src/platform_psl1ght.cpp      ← PS3                (later)
runtime/src/platform_kos.cpp          ← Dreamcast          (later)
```

The CMakeLists.txt picks which `platform_*.cpp` to build based on the target
toolchain file (added in Phase 4).

---

## Phase plan

| # | Phase                                  | Deliverable                                                          | Status      |
| - | -------------------------------------- | -------------------------------------------------------------------- | ----------- |
| 0 | Repo scaffold                          | Private GH repo + working tree, README, .gitignore, CMakeLists       | **done**    |
| 1 | Windows reference build from new tree  | `rom.exe` builds & matches upstream behavior                         | in progress |
| 2 | Audit PAL contract; document gaps      | Inventory the 13 `gb_platform_*` calls + any escapes from contract   | next        |
| 3 | Endianness + 32-bit audit              | Big-endian-safe; ROM-derived arrays use byte accessors only          | pending     |
| 4 | Xbox 360 toolchain                     | devkitPPC + libxenon installed; CMake toolchain file; hello-world XEX boots Xenia | pending |
| 5 | `platform_libxenon.cpp`                | Xenos framebuffer, USB gamepad, audio out, fs; full gb_platform_* contract | pending |
| 6 | rom_main.c multiplayer guard           | Wrap `mp_session_is_client_connected()` etc. in `#ifdef LA_HAS_MULTIPLAYER` so `LA_MULTIPLAYER=OFF` builds work | pending |
| 7 | Iterate to playable in Xenia           | Boot + title screen + intro rendering correctly                      | pending     |
| 8 | Additional backends                    | WASM (Emscripten), Android (NDK + SDL2), NXDK, PSL1GHT, KOS          | pending     |

Phase 2 in the original plan was "extract a PAL." That work was already done
by upstream — `platform_sdl.h` is the PAL. The current Phase 2 is just an audit
to make sure no runtime code escapes the contract by calling SDL2 directly.

---

## Repository layout

```
linksawakening-portable/
├── README.md                       # this file — keep the phase table current
├── .gitignore                      # ROM, recompiler output, build artifacts excluded
├── CMakeLists.txt                  # Windows/SDL2 build (LA_MULTIPLAYER=ON by default)
├── rom_main.c                      # Entry point (from la-dx-recompiled)
├── rom.h                           # Generated declarations
├── rom.c                           # gitignored — 115 MB recompiler output
├── rom_rom.c                       # gitignored — 6.3 MB ROM-as-C-array
├── runtime/                        # Vendored from gb-recompiled
│   ├── include/
│   │   ├── platform_sdl.h          # ★ THE PAL — implement this per target
│   │   ├── gbrt.h, ppu.h, audio.h, hwtrace.h, gbrt_debug.h
│   ├── src/
│   │   ├── gbrt.c                  # 52 KB — CPU + memory + bank switching
│   │   ├── ppu.c                   # 23 KB — scanline PPU with CGB support
│   │   ├── audio.c                 # 23 KB — APU mixer
│   │   ├── interpreter.c           # 39 KB — fallback for unresolved indirect jumps
│   │   ├── hwtrace.c               # 5 KB  — debug trace
│   │   ├── platform_sdl.cpp        # 38 KB — SDL2 PAL implementation
│   │   ├── menu_gui.cpp            # 41 KB — ImGui menu
│   │   ├── asset_viewer.cpp        # 27 KB — debug asset viewer
│   │   └── multiplayer/            # mp_*.cpp (only built when LA_MULTIPLAYER=ON)
│   └── third_party/
│       ├── imgui/                  # base menu uses this — always built
│       └── enet/                   # only built when LA_MULTIPLAYER=ON
├── tools/                          # Debug tracing tools (SameBoy headless ref tracer)
└── docs/
    └── PORTING.md                  # PAL interface, endianness checklist, backend matrix
```

---

## ROM handling

The ROM is **never committed**. The recompiler output `rom.c` and `rom_rom.c`
are also gitignored — both are derived from your ROM. You supply the ROM and
regenerate both files locally via [gb-recompiled](https://github.com/sp00nznet/gb-recompiled).

To pull updates from the upstream LA project (when `rom.c` itself gets
regenerated with a new recompiler bug fix, like the `(HL)` ALU fix or the
STORE8 union aliasing fix in earlier commits), copy
`D:\la-dx-recompiled\rom.c` and `rom_rom.c` back into this tree.

---

## Building (Windows reference)

Prereqs: MSYS2 with MinGW64 + SDL2 + CMake 3.16+ + Ninja.

```bash
# from msys2 mingw64 shell
PATH="/c/msys64/mingw64/bin:$PATH"
cmake -S . -B build -G Ninja
cmake --build build
./build/rom.exe
```

Multiplayer is on by default (matches upstream). To build single-player only
(useful as a smaller surface for the 360 port):

```bash
cmake -S . -B build -G Ninja -DLA_MULTIPLAYER=OFF
# Note: rom_main.c still calls mp_session_is_client_connected() unconditionally.
# Phase 6 fixes that gating; until then, LA_MULTIPLAYER=OFF will fail to link.
```

The Xbox 360 build will live alongside this one once Phase 4 lands. It will use
a separate CMake toolchain file (`cmake/toolchain-libxenon.cmake`) and a
sibling `runtime/src/platform_libxenon.cpp`.

---

## What gets reused vs replaced per platform

**Reused as-is on every target:**

- `rom.c`, `rom.h`, `rom_main.c` (well — `rom_main.c` after the Phase 6 MP guard)
- `rom_rom.c` (ROM data as byte array — already endian-clean)
- All of `runtime/src/*.c` (CPU, PPU, audio, interpreter, hwtrace)
- `runtime/src/menu_gui.cpp` and `asset_viewer.cpp` (depend only on ImGui and the PAL contract)

**Per-platform:**

- `runtime/src/platform_<target>.cpp` — the PAL implementation
- ImGui backend — for SDL2 it's `imgui_impl_sdl2.cpp` + `imgui_impl_sdlrenderer2.cpp`;
  for libxenon it'd be a custom Xenos backend (or stub the menu entirely)
- CMake toolchain file (`cmake/toolchain-<target>.cmake`)
- Packaging glue (`.xex`, `.apk`, `.wasm`)

**Audit-then-keep:**

- Anything in the runtime that does `*(uint16_t*)ptr` / reinterpret loads
  — needs byte-by-byte access on big-endian (PPC) targets
- 64-bit assumptions in pointer math (most targets are 32-bit user space)

---

## Credits

- Game: Nintendo / Grezzo (Link's Awakening DX, 1998)
- Recompiler: [`arcanite24/gb-recompiled`](https://github.com/arcanite24/gb-recompiled) + your fork
- Disassembly: [LADX-Disassembly](https://github.com/zladx/LADX-Disassembly) contributors
- Multiplayer overlay (when enabled): [`sp00nznet/la-mp`](https://github.com/sp00nznet/la-mp)
- Reference emulator: [SameBoy](https://github.com/LIJI32/SameBoy)
- 360 toolchain: [libxenon](https://github.com/Free60Project/libxenon) / Free60 project

Educational / preservation. You must supply your own legally obtained ROM.
