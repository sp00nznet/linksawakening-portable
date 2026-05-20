# Building

Step-by-step build instructions for every platform. Each port is driven
by a script in `cmake/test/`; this document explains the prerequisites
and how to run each one.

## Contents

- [Prerequisites common to all targets](#prerequisites-common-to-all-targets)
- [Windows (reference build)](#windows-reference-build)
- [PlayStation 4](#playstation-4)
- [PlayStation 3](#playstation-3)
- [Nintendo 3DS](#nintendo-3ds)
- [Nintendo Wii](#nintendo-wii)
- [Android](#android)
- [Parked / blocked targets](#parked--blocked-targets)

---

## Prerequisites common to all targets

### The recompiled game files

Three files are **not in the repo** — they are recompiler output derived
from the ROM, and are `.gitignore`d:

| File | What it is |
| --- | --- |
| `rom.c` | ~115 MB — the entire game recompiled to C (17,819 functions) |
| `rom.h` | generated function declarations |
| `rom_rom.c` | ~6 MB — the ROM image as a C byte array |

Generate them by running [gb-recompiled](https://github.com/sp00nznet/gb-recompiled)
against a **legally obtained Link's Awakening DX ROM**, then drop all three
at the repository root. Every build below expects them there.

### The runtime

`runtime/` is vendored from [sp00nznet/gb-recompiled](https://github.com/sp00nznet/gb-recompiled).
It is already in the repo — no action needed unless you want to re-sync it
to a newer gb-recompiled commit.

### WSL

The console cross-compilers run on Linux. On Windows, install **WSL** with a
**Debian** distro (`wsl --install -d Debian`). The PS4, PS3, and 3DS
toolchains all install inside WSL. The Windows reference build uses MSYS2
instead and does not need WSL.

---

## Windows (reference build)

The reference build — native Windows, SDL2, full ImGui menu + multiplayer.

**Prerequisites** — [MSYS2](https://www.msys2.org/), then from a MinGW64 shell:

```bash
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
          mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2
```

**Build:**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**Output:** `build/rom.exe` (~25 MB). Run it directly.

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
   bash cmake/test/fetch_libssl11.sh           # downloads the .deb to ~/
   sudo dpkg -i ~/libssl11.deb
   ```

### Build

```powershell
# from Windows PowerShell — the scripts default their output dir to ~/la360-ps4
wsl -d Debian -- bash /mnt/d/ports/la360/cmake/test/build_ps4.sh
wsl -d Debian -- bash /mnt/d/ports/la360/cmake/test/package_ps4.sh
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
docker run --rm -v D:/ports/la360:/src ps3dev/ps3dev:submodules `
    bash /src/cmake/test/build_ps3.sh
```

This compiles inside the container (la360 mounted at `/src`):
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
wsl -d Debian -- bash /mnt/d/ports/la360/cmake/test/build_3ds.sh
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
wsl -d Debian -- bash /mnt/d/ports/la360/cmake/test/build_wii.sh
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
repo — `build_android.ps1` stages an SDL2 source tree into it at build time.
This build runs natively on Windows (not WSL).

### One-time toolchain setup (Windows)

1. **JDK 17+** — e.g. [Eclipse Temurin](https://adoptium.net/).
2. **Android SDK command-line tools** — download from the
   [Android Studio page](https://developer.android.com/studio#command-line-tools-only),
   unzip so that `sdkmanager.bat` ends up at
   `D:\Android\Sdk\cmdline-tools\latest\bin\`, then:

   ```powershell
   $sdk = "D:\Android\Sdk"
   $sm  = "$sdk\cmdline-tools\latest\bin\sdkmanager.bat"
   & $sm --sdk_root=$sdk --licenses
   & $sm --sdk_root=$sdk platform-tools "platforms;android-34" `
       "build-tools;34.0.0" "ndk;27.2.12479018" "cmake;3.22.1"
   ```

3. **SDL2 source** — download the SDL2 2.32.x source release from
   [libsdl.org](https://github.com/libsdl-org/SDL/releases) and unzip it
   (e.g. to `D:\ports\android-build\SDL2-2.32.10`).

### Build

```powershell
powershell -File cmake\test\build_android.ps1 `
    -SdlSrc D:\ports\android-build\SDL2-2.32.10 -SdkDir D:\Android\Sdk
```

The script stages SDL2 into `android/`, then runs `gradlew assembleDebug` —
the NDK CMake build compiles `rom.c` + the runtime + `platform_sdl.cpp` into
`libmain.so`. The default ABI is `x86_64` (the emulator); add `arm64-v8a` to
`abiFilters` in `android/app/build.gradle` for physical phones.

**Output:** `build-android/linksawakening.apk`.

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

## Parked / blocked targets

### Xbox 360 — parked

`platform_libxenon.c` is written and a hello-world XEX builds and runs in
Xenia, but the full-game XEX hits a memory error in Xenia (the 115 MB
`rom.c` versus Xenia's homebrew memory model). The libxenon toolchain
(`/usr/local/xenon`, `xenon-gcc`) and `cmake/test/build_ps4.sh`-style
scripts are in place if you want to pick it up — likely needs real
RGH/JTAG hardware to validate.

### WebAssembly — blocked

Emscripten builds the whole game, but WebAssembly caps individual
functions at ~7.65 MB and the recompiler emits several functions larger
than that (`gb_dispatch` and a few giant recompiled routines — one is
10.8 MB). The fix is upstream in gb-recompiled: the recompiler must emit
size-bounded functions. `cmake/test/build_wasm.sh` is ready for when that
lands.
