# Contributing

Issues and pull requests are welcome.

Where a change belongs:

- **A console backend** (`platform_<target>.c`) or anything else in the
  runtime: [sp00nznet/gb-recompiled](https://github.com/sp00nznet/gb-recompiled).
  Link the issue here that found it.
- **The entry point or the Windows build**: [sp00nznet/LinksAwakening](https://github.com/sp00nznet/LinksAwakening).
- **Build scripts, packaging and per-target docs**: here.

## Before you open a PR

- Build the target you changed with its `scripts/build_<target>.sh`, and say
  where you ran it (emulator or hardware, and which).
- Never commit the ROM, `LinksAwakening/gen/`, build output, save files, or
  anything else derived from the game.

## Where your code comes from

Contributions must be your own work or under an MIT-compatible licence. Don't
port code from GPL or non-commercial emulators. Running an emulator such as
Dolphin, RPCS3, PCSX2 or Flycast to test a build is fine; copying from their
source is not. If something you contribute came from another project, say
which one in the PR.

AI-assisted contributions are welcome, provided a human understood and checked
the change.
