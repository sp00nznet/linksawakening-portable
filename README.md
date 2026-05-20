# linksawakening-portable

Multi-platform port of the static recompilation of *The Legend of Zelda:
Link's Awakening DX* (Game Boy Color). The game is recompiled to native C;
each platform gets a backend implementing one shared interface. **Running
on PlayStation 4, PlayStation 3, and Nintendo 3DS** (plus the Windows
reference build).

**→ Build instructions for every platform: [docs/BUILDING.md](docs/BUILDING.md)**

## Screenshots

| 3DS — title (Azahar) | 3DS — name entry | 3DS — gameplay |
|----------------------|------------------|----------------|
| ![3DS title](screenshots/3ds-title.png) | ![3DS name entry](screenshots/3ds-nameentry.png) | ![3DS gameplay](screenshots/3ds-gameplay.png) |

| PS3 — title (RPCS3) |
|---------------------|
| ![PS3 title](screenshots/ps3-title.png) |

Built on:

- **[`sp00nznet/la-dx-recompiled`](https://github.com/sp00nznet/la-dx-recompiled)** (private)
  — the game project (`rom.c` ~115 MB recompiler output, `rom_main.c` entry,
  `rom_rom.c` ROM bytes). Vendored into this tree at the root.
- **[`sp00nznet/gb-recompiled`](https://github.com/sp00nznet/gb-recompiled)**
  (fork of [`arcanite24/gb-recompiled`](https://github.com/arcanite24/gb-recompiled))
  — the runtime engine (`gbrt`, `ppu`, `audio`, `interpreter`, `menu_gui`, plus
  the SDL2 platform layer in `platform_sdl.cpp` and `platform_sdl.h`). Vendored
  at `runtime/`.

> **Status** (Windows reference build works — 25 MB `rom.exe`):
>
> | Platform | Backend | State |
> | --- | --- | --- |
> | **PlayStation 4** | `platform_sdl.cpp` (OpenOrbis SDL2) | ✅ **Running on real hardware.** Installable `linksawakening.pkg` (20 MB). |
> | **PlayStation 3** | `platform_psl1ght.c` (native PSL1GHT) | ✅ **Running in RPCS3 at ~56 FPS** — title + gameplay. `EBOOT.BIN` (25 MB). |
> | **Nintendo 3DS** | `platform_3ds.c` (native libctru) | ✅ **Running in Azahar *and* on a real New 2DS XL** — reaches gameplay. `linksawakening.3dsx` (22 MB). |
> | **Xbox 360** | `platform_libxenon.c` (native libxenon) | ⏸ Parked — full-game XEX hits a memory error in Xenia. |
> | **WebAssembly** | `platform_sdl.cpp` (Emscripten SDL2) | ⏸ Blocked — recompiler emits functions over wasm's 7.65 MB per-function cap. |
>
> Big-endian targets (PS3, and the parked 360) are carried by the
> AF/BC/DE/HL register-pair fix in `gbrt.h`. See
> [docs/BUILDING.md](docs/BUILDING.md) to build any of them.

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
| 0  | Repo scaffold                              | Private GH repo + working tree, README, .gitignore, CMakeLists       | **done**   |
| 1  | Windows reference build from new tree      | `rom.exe` builds & matches upstream behavior                         | **done**   |
| 2  | Audit PAL contract; document gaps          | docs/PAL_AUDIT.md catalogues every direct SDL/ImGui/stdio call       | **done**   |
| 3  | Endianness + 32-bit audit                  | docs/ENDIAN_AUDIT.md: core engine BE-safe; one critical fix needed in `gbrt.h` register unions | **done** |
| 4  | Xbox 360 toolchain                         | libxenon at /usr/local/xenon (WSL Debian); CMake toolchain file in cmake/; hello-world XEX loads + executes in Xenia | **done** |
| 5a | Upstream patches in gb-recompiled          | `LA_HAS_MULTIPLAYER` + `LA_HAS_IMGUI` gates; plain-C stubs; AF/BC/DE/HL register-pair BE swap in `gbrt.h` | **done** |
| 5  | `platform_libxenon.c`                      | 13 `gb_platform_*` + `GBPlatformCallbacks`; console-blit video, USB gamepad, ring-buffered audio, FATFS saves. Test XEX runs in Xenia. | **done** |
| —  | Xbox 360 full-game XEX                     | **PARKED** — full-game XEX hits a memory error in Xenia (115 MB `rom.c` vs Xenia's homebrew memory model) | parked |
| P1 | PS4 toolchain (OpenOrbis)                  | `cmake/toolchain-ps4.cmake`; clang `x86_64-pc-freebsd12-elf`; libxenon-style probe + build scripts in `cmake/test/` | **done** |
| P2 | PS4 full-game build                        | `rom.c` + runtime + `platform_sdl.cpp` (OpenOrbis SDL2) → `eboot.bin` (18 MB) | **done** |
| P3 | PS4 `.pkg` packaging                       | `create-fself` → `create-gp4` → `PkgTool.Core` → **`linksawakening.pkg`** (20 MB) | **done** |
| P4 | PS4 install + hardware test                | `.pkg` installed on the jailbroken PS4 — boots + plays. Raw-`SDL_Joystick` input fix for the DualShock | **done** |
| D1 | 3DS toolchain + backend                    | devkitARM + libctru; native `platform_3ds.c` (gfx/hid/ndsp/sdmc)               | **done**   |
| D2 | 3DS full-game build                        | `rom.c` + runtime + `platform_3ds.c` → `linksawakening.3dsx` (22 MB)            | **done**   |
| D3 | 3DS test in emulator / hardware            | Runs in Azahar + on a real New 2DS XL — reaches gameplay                       | **done**   |
| S1 | PS3 toolchain (PSL1GHT)                     | ps3dev Docker image; `cmake/test/build_ps3.sh`                                 | **done**   |
| S2 | PS3 full-game build                        | `rom.c` + runtime + `platform_psl1ght.c` → fake-self `EBOOT.BIN` (25 MB)        | **done**   |
| S3 | PS3 test in RPCS3 / hardware               | Boots in RPCS3 at ~56 FPS — title screen + gameplay render correctly           | **done**   |
| W  | WebAssembly (Emscripten)                   | **BLOCKED** — recompiler emits functions over wasm's 7.65 MB per-function cap   | blocked    |
| 6  | Settings PAL extension                     | `gb_platform_fs_read/write` so settings persist on non-stdio targets           | pending    |

Phase 2 in the original plan was "extract a PAL." That work was already done
by upstream — `platform_sdl.h` is the PAL. Phase 2 was an audit confirming no
runtime code escapes the contract.

The phase plan pivoted mid-project: the Xbox 360 target was parked after the
full-game XEX hit a Xenia memory error, and **PS4 + 3DS** became the focus
(see the P-rows above).

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
