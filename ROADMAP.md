# Roadmap

## Next

1. **Run the first-time builds.** GameCube, PS2, PSP and Vita build but have
   never been run. Boot each in an emulator (Dolphin, PCSX2, PPSSPP, Vita3K)
   through offstage, and record a screenshot or "doesn't boot, here's where".
2. **Follow the gb-recompiled upstream sync.** When the fork is rebuilt on
   upstream (see LinksAwakening's ROADMAP), rebuild every target here: the
   PPU and context fields change, so each console backend needs checking.
3. **One-command builds.** A `Build.cmd <target>` that runs the target's Docker
   image, so a console build is one step after `LinksAwakening\Setup.cmd`.
   Docker images exist for PS3, PSP, Vita, PS2, Dreamcast and the original
   Xbox. Wii, GameCube, 3DS, 360, PS4 and WASM still use a WSL install.
4. **netlab recipes**, so these builds run on the farm instead of the workstation.

## Open problems

- **Dreamcast** isn't full speed. It's CPU/frame-pacing bound, not audio: a
  silent `-DLA_DC_NO_AUDIO` build runs no faster. It must be built `-Os` and
  stripped to fit 16 MB.
- **Original Xbox** hangs at the Xbox logo in xemu before the first frame,
  probably in `gb_platform_init` against NXDK's SDL2.
- **Xbox 360** can't be tested in Xenia (Canary host-crashes, master
  guest-crashes on libxenon homebrew). Needs RGH/JTAG hardware.
- **WebAssembly** links at `-O0` to avoid `wasm-opt`'s function merging, and
  the canvas isn't centred on the page.
- **Settings persistence** on targets without plain stdio: a
  `gb_platform_fs_read/write` extension so control bindings survive.

## Deferred

- Nintendo Switch: feasible through SDL2 like Android, but needs a modded
  console or a working homebrew emulator to develop against.

## Out of scope

- Multiplayer. It lives in [sp00nznet/la-mp](https://github.com/sp00nznet/la-mp).
- Distributing the ROM, the generated C, or any build that contains game code.
