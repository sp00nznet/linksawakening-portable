# GB Recompilation Debug Tracing Tools

Generic tools for comparing hardware state between a reference Game Boy emulator (SameBoy) and a statically recompiled game. Works with any GB/GBC game.

## Quick Start

### 1. Build the SameBoy Tracer

```bash
cd tools
PATH="/c/msys64/mingw64/bin:$PATH" cmake -G Ninja -B build
PATH="/c/msys64/mingw64/bin:$PATH" ninja -C build
```

Requires: SameBoy source at `D:/SameBoy` (clone from https://github.com/LIJI32/SameBoy)

### 2. Capture Reference Trace

```bash
./build/sb_tracer.exe --rom path/to/game.gbc --trace ref_trace.log --frames 300
```

This runs the game headlessly in SameBoy for 300 frames, dumping:
- Per-scanline PPU register state (LCDC, STAT, SCX, SCY, BGP, OBP0/1, WY, WX)
- CGB palette RAM (64 bytes BG + 64 bytes OBJ) at frame start
- Per-VBlank checksums (OAM, tile maps, tile data, VRAM bank 1, framebuffer)
- CPU register state at each VBlank

### 3. Capture Recompiled Trace

Add `--hw-trace` flag to your recompiled game:

```bash
./build/rom.exe --hw-trace recomp_trace.log --limit 10000000
```

The `--limit` flag stops after N instructions (10M ~= 350 frames).

### 4. Compare Traces

```bash
python3 tools/compare_hw_trace.py ref_trace.log recomp_trace.log
```

Comparison modes:
- `--mode vblank` - Compare per-frame VBLANK checksums
- `--mode scanline` - Compare per-scanline PPU register state
- `--mode palette` - Compare CGB palette RAM
- `--mode all` - Run all comparisons (default)

## Integrating Into a New Project

### Add hw-trace to your runtime

1. Copy `hwtrace.c` and `hwtrace.h` from `gb-recompiled/runtime/`
2. Add to CMakeLists.txt
3. Call from your PPU:
   - `hwtrace_scanline(ctx, line)` at each scanline render
   - `hwtrace_vblank(ctx)` at VBlank entry
4. Add `--hw-trace <file>` CLI argument: call `hwtrace_init(filename)`

### Trace Output Format

```
# Lines starting with # are comments
SCANLINE:<line> FRAME:<n> LCDC=XX STAT=XX SCX=XX SCY=XX BGP=XX OBP0=XX OBP1=XX WY=XX WX=XX [BCPS=XX OCPS=XX]
BGPAL FRAME:<n> CRC=XXXXXXXX DATA=<128 hex chars>
OBJPAL FRAME:<n> CRC=XXXXXXXX DATA=<128 hex chars>
VBLANK FRAME:<n> OAM_CRC=X TMAP0_CRC=X TMAP1_CRC=X TDATA_CRC=X VRAM1_CRC=X FB_CRC=X
REGS FRAME:<n> AF=XXXX BC=XXXX DE=XXXX HL=XXXX SP=XXXX PC=XXXX BANK=N
```

### Common Divergences and Fixes

| Symptom | Likely Cause |
|---------|-------------|
| OBP0/OBP1 different | Wrong post-bootrom init values (CGB uses 0x00) |
| Palette RAM all FF | CGB palette register routing missing (FF68-FF6B) |
| VRAM1_CRC wrong | VRAM bank switching not working (FF4F) |
| TMAP CRC diverges early | Timing difference in game init |
| FB_CRC different but VRAM matches | PPU rendering bug (palette application, attribute handling) |
| STAT register wrong | PPU mode timing issue |

## Files

- `sb_tracer.c` - SameBoy headless tracer
- `compare_hw_trace.py` - Trace comparison tool
- `CMakeLists.txt` - Build for sb_tracer
- `hwtrace.c/h` are in `gb-recompiled/runtime/` (shared across projects)
