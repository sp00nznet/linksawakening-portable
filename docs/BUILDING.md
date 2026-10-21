# Building

Step-by-step build instructions for every platform. Each port is driven
by a script in `scripts/`; this document explains the prerequisites
and how to run each one.

## Contents

- [Prerequisites common to all targets](#prerequisites-common-to-all-targets)
- [Windows (reference build)](#windows-reference-build)
- [PlayStation 4](#playstation-4)
- [PlayStation 3](#playstation-3)
- [Nintendo 3DS](#nintendo-3ds)
- [Nintendo Wii](#nintendo-wii)
- [Android](#android)
- [WebAssembly](#webassembly)
- [Xbox 360](#xbox-360)
- [Original Xbox](#original-xbox)
- [Nintendo GameCube](#nintendo-gamecube)
- [Sega Dreamcast](#sega-dreamcast)
- [PlayStation 2](#playstation-2)
- [PSP](#psp)
- [PS Vita](#ps-vita)
- [Parked / blocked targets](#parked--blocked-targets)

---

## All targets at once

`Setup.cmd` / `tools\setup.ps1` (Windows) and `setup.sh` (Linux, macOS) do
everything below in Docker: turn your ROM into C with `scripts/recomp.sh`,
then build every line of [`scripts/targets.tsv`](../scripts/targets.tsv),
which names each target's Docker image and command. The sections below are
the same builds one at a time, plus the WSL toolchain installs that were used
before the Docker images.

---

## Prerequisites common to all targets

### The source

Clone with submodules. `LinksAwakening/` is the game repo
([sp00nznet/LinksAwakening](https://github.com/sp00nznet/LinksAwakening)), and
inside it `gbrecomp/` is the runtime and recompiler
([sp00nznet/gb-recompiled](https://github.com/sp00nznet/gb-recompiled)). Every
port builds `LinksAwakening/rom_main.c` against `LinksAwakening/gbrecomp/runtime/`.

```
git clone --recurse-submodules https://github.com/sp00nznet/linksawakening-portable.git
cd linksawakening-portable
```

On Windows, `Filename too long` during the clone means the nested submodules
hit the 260-character path limit: `git config --global core.longpaths true`,
then `git submodule update --init --recursive`.

### The recompiled game files

Generated from your own ROM into `LinksAwakening/gen/`, which is gitignored and
never committed:

| File | What it is |
| --- | --- |
| `rom.c` | ~109 MB: the entire game recompiled to C |
| `rom_rom.c` | ~6.6 MB: the ROM image as a C byte array |
| `rom.h` | the two entry points |

The simplest way is to double-click `LinksAwakening\Setup.cmd`: it asks for
your ROM, generates `gen/` and also builds the Windows version. To do it by
hand, follow steps 1-7 of *Step by step* in
[LinksAwakening's README](https://github.com/sp00nznet/LinksAwakening#step-by-step),
run inside `LinksAwakening/`. Every build below fails early with a message if
`gen/rom.c` is missing.

Tested ROM: Link's Awakening DX (USA, Europe) (Rev 2), SHA-1
`1c091225688d966928cc74336dbef2e07d12a47c`.

### WSL

The console cross-compilers run on Linux. On Windows, install **WSL** with a
**Debian** distro (`wsl --install -d Debian`). The PS4, PS3, and 3DS
toolchains all install inside WSL. The Windows reference build uses MSYS2
instead and does not need WSL.

---

## Windows (reference build)

The Windows build is LinksAwakening itself: run `LinksAwakening\Setup.cmd`, or
see [its README](https://github.com/sp00nznet/LinksAwakening#getting-started).
Output: `LinksAwakening/build/linksawakening.exe`. Multiplayer is not part of
these builds; it lives in [sp00nznet/la-mp](https://github.com/sp00nznet/la-mp).

---

## PlayStation 4

x86-64 little-endian. Reuses the SDL2 backend (`platform_sdl.cpp`) via the
OpenOrbis SDL2 port. ImGui off (OpenOrbis SDL2 predates 2.0.17), multiplayer
off. Requires a **jailbroken PS4**.

### One-time toolchain setup (in WSL Debian)

1. **OpenOrbis toolchain** — download + extract:

   ```bash
   cd ~
   wget https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz
   tar xzf toolchain-llvm-18.tar.gz
   mv OpenOrbis/PS4Toolchain ~/PS4Toolchain      # flatten the nested dir
   echo 'export OO_PS4_TOOLCHAIN=$HOME/PS4Toolchain' >> ~/.bashrc
   ```

2. **clang + lld:**

   ```bash
   sudo apt-get update && sudo apt-get install -y clang lld
   ```

3. **libssl1.1** — PkgTool.Core (.NET Core 3.0) needs it for `.pkg` signing;
   Debian 13 only ships OpenSSL 3.x:

   ```bash
   bash scripts/fetch_libssl11.sh           # downloads the .deb to ~/
   sudo dpkg -i ~/libssl11.deb
   ```

### Build

```powershell
# from Windows PowerShell, in this repo folder. Output goes to ~/la360-ps4 in WSL
wsl -d Debian -- bash scripts/build_ps4.sh
wsl -d Debian -- bash scripts/package_ps4.sh
```

`build_ps4.sh` compiles `rom.c` + runtime + `platform_sdl.cpp` with clang
(`--target=x86_64-pc-freebsd12-elf`), links with `ld.lld`, runs
`create-fself` → `eboot.bin`. `package_ps4.sh` then wraps it with
`create-gp4` + `PkgTool.Core`.

**Output:** `build-ps4/linksawakening.pkg` (~20 MB).

### Install + run

Copy the `.pkg` to a USB stick or transfer it to the PS4, then install it
via **Settings → Debug Settings → Game → Package Installer** (or your usual
homebrew install route). It appears on the home screen as "Link's Awakening DX".

---

## PlayStation 3

PowerPC big-endian. Native PSL1GHT backend (`platform_psl1ght.c`). The
big-endian register-pair fix in `gbrt.h` makes the recompiled `rom.c`
correct here.

### One-time toolchain setup

The PS3 toolchain is large; the easiest path is the **ps3dev Docker image**
(skips a multi-hour from-source build). Install Docker Desktop, then:

```powershell
docker pull ps3dev/ps3dev:submodules
```

### Build

```powershell
docker run --rm -v ${PWD}:/src ps3dev/ps3dev:submodules `
    bash /src/scripts/build_ps3.sh
```

This compiles inside the container (this repo mounted at `/src`):
`powerpc64-ps3-elf-gcc` → link against PSL1GHT → `strip` → `sprxlinker`
→ `fself` → `EBOOT.BIN`.

**Output:** `build-ps3/EBOOT.BIN` (~25 MB — a fake-signed self).

### Run

- **RPCS3:** `rpcs3.exe build-ps3/EBOOT.BIN` — boots directly.
- **CFW PS3:** the fake self runs as-is; or wrap it in a `.pkg` for the
  XMB (the PSL1GHT `ppu_rules` `%.pkg` target).

---

## Nintendo 3DS

ARM little-endian. Native libctru backend (`platform_3ds.c`).

### One-time toolchain setup (in WSL Debian)

```bash
cd ~
wget -U "dkp-apt" https://apt.devkitpro.org/install-devkitpro-pacman
chmod +x install-devkitpro-pacman
sudo ./install-devkitpro-pacman
sudo dkp-pacman -S --noconfirm 3ds-dev
```

(The `-U "dkp-apt"` user-agent is required — the server 403s without it.)

### Build

```powershell
wsl -d Debian -- bash scripts/build_3ds.sh
```

Compiles `rom.c` + runtime + `platform_3ds.c` with `arm-none-eabi-gcc`,
links against libctru, runs `3dsxtool`.

**Output:** `build-3ds/linksawakening.3dsx` (~22 MB).

### Run

- **Emulator:** open the `.3dsx` in [Azahar](https://github.com/azahar-emu/azahar)
  (the maintained Citra fork).
- **Hardware:** copy `linksawakening.3dsx` to `/3ds/` on the SD card of a
  modded 3DS and launch it from the Homebrew Launcher. Confirmed working on
  a New 2DS XL.

---

## Nintendo Wii

PowerPC big-endian. Native libogc backend (`platform_wii.c`). The same
big-endian register-pair fix in `gbrt.h` that carries the PS3 build makes
the recompiled `rom.c` correct here. Video is the YUY2 external
framebuffer (XFB); input accepts a GameCube controller or a sideways Wii
Remote; audio is ASND; saves go to `sd:/linksawakening/`.

### One-time toolchain setup (in WSL Debian)

The Wii toolchain is `devkitPPC` + `libogc`, installed via the same
`dkp-pacman` set up for the 3DS:

```bash
sudo dkp-pacman -S --noconfirm wii-dev
```

(If you have not installed `dkp-pacman` yet, do the bootstrap steps from
the [Nintendo 3DS](#nintendo-3ds) section first.)

### Build

```powershell
wsl -d Debian -- bash scripts/build_wii.sh
```

Compiles `rom.c` + runtime + `platform_wii.c` with `powerpc-eabi-gcc`
(`-mrvl -mcpu=750`), links against libogc, runs `elf2dol`.

**Output:** `build-wii/linksawakening.dol` (~24 MB).

### Run

- **Emulator:** `Dolphin.exe -e build-wii/linksawakening.dol` — boots
  directly.
- **Hardware:** copy `linksawakening.dol` to `/apps/linksawakening/boot.dol`
  on the SD card of a Wii running the Homebrew Channel and launch it there.

---

## Android

ARM64 / x86-64 little-endian. Reuses the SDL2 backend (`platform_sdl.cpp`)
via SDL2's Android port — the same backend as the Windows and PS4 builds.
On-screen touch controls (d-pad + A/B/Start/Select) are drawn by
`platform_sdl.cpp` behind `#ifdef __ANDROID__`; ImGui + multiplayer are off.

The Gradle project lives at `android/`. SDL2 is **not vendored** in the
repo: `scripts/build_android.sh` stages an SDL2 source tree into it at build
time.

### Build (Docker)

`docker/android.Dockerfile` is the toolchain: SDK 34, NDK 27.2, CMake 3.22.1
and the SDL2 2.32.10 source. Build the image once, then the APK:

```powershell
docker build -t linksawakening-android -f docker/android.Dockerfile docker
docker run --rm -v ${PWD}:/src -w /src linksawakening-android bash scripts/build_android.sh /src/build-android
```

Without Docker, run `scripts/build_android.sh` anywhere with bash, with
`ANDROID_HOME` set to an SDK holding those packages and `SDL2_SRC` to the SDL2
source. Gradle's cache goes in `build-android/.gradle`. The NDK CMake build
compiles `rom.c` + the runtime + `platform_sdl.cpp` into `libmain.so`. The
default ABI is `x86_64` (the emulator); add `arm64-v8a` to `abiFilters` in
`android/app/build.gradle` for physical phones.

**Output:** `build-android/linksawakening.apk` (~10 MB).

### Run

- **Emulator** — create an AVD, boot it, install:

  ```powershell
  $sdk = "D:\Android\Sdk"
  & "$sdk\cmdline-tools\latest\bin\sdkmanager.bat" --sdk_root=$sdk `
      emulator "system-images;android-34;default;x86_64"
  & "$sdk\cmdline-tools\latest\bin\avdmanager.bat" create avd `
      -n la_test -k "system-images;android-34;default;x86_64" -d pixel_6
  & "$sdk\emulator\emulator.exe" -avd la_test
  & "$sdk\platform-tools\adb.exe" install -r build-android\linksawakening.apk
  ```

- **Device** — `adb install build-android\linksawakening.apk` over USB
  (build with the `arm64-v8a` ABI for a physical phone).

---

## WebAssembly

wasm32 little-endian. Reuses the SDL2 backend (`platform_sdl.cpp`) through
Emscripten's bundled SDL2 port. The browser drives the loop — `rom_main.c`
uses `emscripten_set_main_loop` under `__EMSCRIPTEN__`.

WebAssembly caps a single function body at ~7.65 MB. The recompiler emits
`gb_dispatch` split into chunk functions and demotes oversized routines to
the interpreter; `build_wasm.sh` additionally compiles `rom.c` with
`-fno-inline` and links at `-O0`, so neither the C compiler nor `wasm-opt`
merges the ~9000 small recompiled functions back into one oversized one.

### One-time toolchain setup (in WSL Debian)

```bash
cd ~
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install latest && ./emsdk activate latest
```

### Build

```powershell
wsl -d Debian -- bash scripts/build_wasm.sh
```

Compiles `rom.c` + runtime + `platform_sdl.cpp` with `emcc` and links with
the minimal custom shell `scripts/wasm_shell.html`.

**Output:** `build-wasm/linksawakening.{html,js,wasm}`.

### Run

WebAssembly must be served over HTTP (not `file://`):

```powershell
wsl -d Debian -- python3 -m http.server 8000 --directory build-wasm
```

Then open `http://localhost:8000/linksawakening.html`. Confirm no function exceeds
the wasm limit with
`python3 scripts/wasm_funcsizes.py build-wasm/linksawakening.wasm`.

---

## Xbox 360

PowerPC big-endian. Native libxenon backend (`platform_libxenon.c`); the
`gbrt.h` register-pair fix (shared with PS3/Wii) carries big-endianness.

It builds and is hardware-ready, but **cannot be validated in an
emulator**: Xenia crashes on libxenon homebrew that drives the video/audio
hardware directly (Canary host-crashes; master runs further then reports
"guest has crashed"). A real RGH/JTAG console is needed.

### Build (Docker)

```powershell
docker run --rm -v ${PWD}:/src -w /src free60/libxenon bash scripts/build_libxenon.sh /src/build-xenon
```

Compiles `rom.c` + runtime + `platform_libxenon.c` with `xenon-gcc`, links
against libxenon, and writes a stripped `la360.elf32`. Set `ELF2XEX` to an
ELF-to-XEX converter to also get a `.xex` (only useful for Xenia
experiments; there's none in the image).

**Output:** `build-xenon/la360.elf32` (~28 MB).

### Run

- **Hardware:** put the `.elf32` on a USB drive as `xenon.elf` and boot XeLL
  on an RGH/JTAG Xbox 360.
- **Emulator:** Xenia does not currently run it (see above).

---

## Original Xbox

x86 little-endian — the recompiled `rom.c` compiles unchanged. Reuses the
SDL2 backend (`platform_sdl.cpp`) through NXDK's bundled SDL2 port — the
same approach as PS4 / Android / WebAssembly.

The `.xbe` builds, but **does not yet boot in xemu**: the Xbox logo shows
and the game appears to hang in init before rendering its first frame.
Under debugging.

### Toolchain — the NXDK Docker image

A from-scratch NXDK clone needs many git submodules and the fetches are
flaky; use the official prebuilt image instead (it ships a fully-built
NXDK — libnxdk, libSDL2, libpbkit, ...):

```powershell
docker pull ghcr.io/xboxdev/nxdk
```

### Build

```powershell
docker run --rm -v ${PWD}:/src ghcr.io/xboxdev/nxdk `
    sh /src/scripts/build_xbox.sh
```

Compiles `rom.c` + runtime + `platform_sdl.cpp` with NXDK's clang
(`-target i386-pc-win32`, `-Os` to keep the `.xbe` small for the Xbox's
64 MB), links libnxdk + libSDL2, and wraps the result into a `.xbe` and
an XISO. `xbox/Makefile` is a standard NXDK project Makefile.

**Output:** `build-xbox/linksawakening.xbe` and `linksawakening.iso`.

### Run

- **Emulator:** [xemu](https://xemu.app/) —
  `xemu.exe -dvd_path build-xbox\linksawakening.iso`. xemu needs an Xbox
  MCPX boot ROM, a BIOS, an EEPROM and an HDD image — supply your own.
- **Hardware:** copy `default.xbe` to a softmodded Xbox and launch it
  from a homebrew dashboard.

---

## Nintendo GameCube

PowerPC big-endian, native libogc backend (`platform_gamecube.c`): the Wii
backend without the Wii Remote. Saves go to `sd:/linksawakening/`. **Builds;
not run yet.**

### Toolchain (in WSL Debian)

```bash
sudo dkp-pacman -S --noconfirm gamecube-dev
```

### Build

```powershell
wsl -d Debian -- bash scripts/build_gamecube.sh
```

**Output:** `~/la360-gamecube/la360.dol` in WSL (~22 MB), also copied to
`build-gamecube/linksawakening.dol` if that folder exists.

### Run

`Dolphin.exe -e la360.dol`, or on hardware through Swiss.

---

## Sega Dreamcast

SH-4 little-endian, native KallistiOS backend (`platform_dreamcast.c`).
Saves go to the VMU in port A1 as `LADX_SAV` / `LADX_STT`. Builds and shows
the title screen in Flycast; not full speed (see ROADMAP).

It must be built `-Os`: the `-O2` image is ~15.9 MB and won't run in 16 MB of
RAM. The script also strips the ELF, because the 50+ MB debug ELF loads and
exits immediately in Flycast.

### Build (Docker)

```powershell
docker run --rm -v ${PWD}:/src kazade/dreamcast-sdk `
    sh -c "OPT=-Os bash /src/scripts/build_dreamcast.sh /src/build-dreamcast"
```

**Output:** `build-dreamcast/linksawakening.elf` (~14 MB, stripped).

### Run

Load the ELF in Flycast. For hardware, the script prints the scramble and
CDI steps.

---

## PlayStation 2

MIPS R5900 little-endian, native PS2SDK + gsKit backend (`platform_ps2.c`).
The audio and USB IOP modules are embedded in the ELF; controller modules
load from `rom0:`. Saves go to `mass:/linksawakening/` (USB). **Builds; not
run yet.** The USB module set has changed across PS2SDK versions, so check
`load_iop()` in `platform_ps2.c` if USB saves fail.

### Build (Docker)

The ps2dev image has no bash, so install it first:

```powershell
docker run --rm -v ${PWD}:/src ps2dev/ps2dev `
    sh -c "apk add -q bash && bash /src/scripts/build_ps2.sh /src/build-ps2"
```

**Output:** `build-ps2/linksawakening.elf` (~22 MB).

### Run

PCSX2 (boot the ELF), or on hardware through wLaunchELF or OPL.

---

## PSP

MIPS Allegrex little-endian, native PSPSDK backend (`platform_psp.c`). Saves
go to `ms0:/linksawakening/`. **Builds; not run yet.**

The link deliberately has no `--gc-sections`: it strips the import-stub
sections, and `psp-fixup-imports` then fails with `Error, no nid section found`.

### Build (Docker)

```powershell
docker run --rm -v ${PWD}:/src pspdev/pspdev bash /src/scripts/build_psp.sh /src/build-psp
```

**Output:** `build-psp/EBOOT.PBP` (~20 MB).

### Run

PPSSPP: open the `EBOOT.PBP`. On hardware, copy it to
`ms0:/PSP/GAME/linksawakening/EBOOT.PBP` on a console running custom firmware.

---

## PS Vita

ARMv7 little-endian, native VitaSDK + vita2d backend (`platform_vita.c`).
Saves go to `ux0:/data/linksawakening/`. **Builds; not run yet.**

### Build (Docker)

```powershell
docker run --rm -v ${PWD}:/src vitasdk/vitasdk bash /src/scripts/build_vita.sh /src/build-vita
```

**Output:** `build-vita/linksawakening.vpk` (~9.4 MB).

### Run

Install the VPK with VitaShell on a console running HENkaku/Ensō, or in Vita3K.

---

## Parked / blocked targets

### Nintendo Switch — parked

Not started. Feasible via the SDL2 backend (the same path as Android),
but parked alongside the 360 — developing it needs a modded console or a
working homebrew emulator to test against.
