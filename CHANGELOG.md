# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[SemVer](https://semver.org/).

## [Unreleased]

### Added
- One-step build of every console from your ROM, all in Docker: `Setup.cmd` /
  `tools/setup.ps1` on Windows, `setup.sh` on Linux and macOS. It turns the
  ROM into C (`scripts/recomp.sh`), builds each line of `scripts/targets.tsv`
  in that console's toolchain image, collects the results in `builds/`, and
  says which built and what's known to work. A failed console doesn't stop the
  rest, and a rerun skips what's done.
- Installers for Windows, Linux and macOS ([recomp-launcher](https://github.com/sp00nznet/recomp-launcher)
  with `launcher/launcher.json`), built by CI on every push and attached to `v*`
  releases. They contain no game code.
- `docker/android.Dockerfile` and `scripts/build_android.sh`, replacing the
  Windows-only `build_android.ps1`.
- CI: every script parses, `targets.tsv` is well formed, and `launcher.json`'s
  pinned commits match the submodules.

### Changed
- Every build script skips recompiling `rom.c` when its object file is newer.
- Xbox 360: the output is the XeLL-bootable `.elf32`; the `.xex` is only made
  when `ELF2XEX` points at a converter (it was a private tool).
- PS4: `OO_PS4_TOOLCHAIN` comes from the environment when set (the
  `openorbisofficial/toolchain` image sets it).
- Builds against [sp00nznet/LinksAwakening](https://github.com/sp00nznet/LinksAwakening)
  as a submodule (`LinksAwakening/`), which in turn pins
  [sp00nznet/gb-recompiled](https://github.com/sp00nznet/gb-recompiled). There
  is one `rom_main.c` and one runtime for every target, the Windows build included.
- Recompiler output is read from `LinksAwakening/gen/` instead of the repo root.
- Build scripts moved from `cmake/test/` to `scripts/`. They resolve paths
  from their own location through `scripts/env.sh`, so they work from any
  checkout and inside Docker; they no longer assume `/mnt/d/ports/la360`.
- Console backends save under the same folders as before, now set through
  `GBRT_SAVE_NAME`, `GBRT_VMU_PREFIX` and `GBRT_PS3_TITLE_ID` (gb-recompiled#1).
- `build_android.ps1` takes the SDL2 source and Android SDK from
  `-SdlSrc`/`-SdkDir` or `SDL2_SRC`/`ANDROID_HOME`, with no machine-specific defaults.
- `LICENSE` is plain MIT so GitHub detects it; the trademark note moved to the README.
- `.sh` scripts and the Xbox Makefile are always checked out with LF, so they
  run under WSL and Docker from a Windows clone.

### Fixed
- Vita link: newer vita2d also needs `SceAppMgr_stub`.
- Android build failed when any `ccache` was on PATH (SDL wraps the compiler
  in it); SDL's ccache use is now off.
- PSP link: no `--gc-sections` (it dropped the import stubs) and no
  hand-listed libc/kernel libraries (they put the stubs out of order).
- PS2 build: gsKit headers and libraries come from `$GSKIT` (the ps2dev image layout).
- PSP backend used a pixel-format constant the SDK doesn't define (gb-recompiled#3).
- PS4 and other SDL2 targets with an older SDL2 build again (gb-recompiled#2).

### Removed
- The vendored `runtime/` copy. It was gb-recompiled 170b873 plus the five
  Dreamcast/GameCube/PS2/PSP/Vita backends, which are now upstream (gb-recompiled#1).
- `rom_main.c`, `rom.h`, `CMakeLists.txt` and `tools/`. The Windows build and
  the SameBoy tools are in LinksAwakening.
- Multiplayer. It lives in [sp00nznet/la-mp](https://github.com/sp00nznet/la-mp).
- Unused CMake toolchain files and the one-off probe and hello-world scripts.
- The `.gitattributes` LFS rule for `rom.c` (the file is never committed).
