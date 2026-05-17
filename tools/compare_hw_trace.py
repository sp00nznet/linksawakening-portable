#!/usr/bin/env python3
"""
Compare hardware traces from SameBoy (reference) and gb-recompiled.

Usage:
    python compare_hw_trace.py ref_trace.log recomp_trace.log [--mode vblank|scanline|palette]

Modes:
    vblank   - Compare per-frame VBLANK checksums (default)
    scanline - Compare per-scanline PPU register state
    palette  - Compare CGB palette RAM dumps
    all      - Run all comparisons
"""

import sys
import re
from collections import defaultdict


def parse_trace(filename):
    """Parse a trace file into structured data."""
    frames = defaultdict(lambda: {
        'scanlines': {},
        'vblank': None,
        'regs': None,
        'bgpal': None,
        'objpal': None,
    })

    with open(filename, 'r') as f:
        for line in f:
            line = line.strip()
            if line.startswith('#') or not line:
                continue

            if line.startswith('SCANLINE:'):
                m = re.match(r'SCANLINE:(\d+) FRAME:(\d+) (.+)', line)
                if m:
                    sl, frame, rest = int(m.group(1)), int(m.group(2)), m.group(3)
                    # Parse key=value pairs
                    regs = dict(re.findall(r'(\w+)=(\w+)', rest))
                    frames[frame]['scanlines'][sl] = regs

            elif line.startswith('VBLANK '):
                m = re.match(r'VBLANK FRAME:(\d+) (.+)', line)
                if m:
                    frame, rest = int(m.group(1)), m.group(2)
                    frames[frame]['vblank'] = dict(re.findall(r'(\w+)=(\w+)', rest))

            elif line.startswith('REGS '):
                m = re.match(r'REGS FRAME:(\d+) (.+)', line)
                if m:
                    frame, rest = int(m.group(1)), m.group(2)
                    frames[frame]['regs'] = dict(re.findall(r'(\w+)=(\w+)', rest))

            elif line.startswith('BGPAL '):
                m = re.match(r'BGPAL FRAME:(\d+) CRC=(\w+) DATA=(\w+)', line)
                if m:
                    frame = int(m.group(1))
                    frames[frame]['bgpal'] = {'crc': m.group(2), 'data': m.group(3)}

            elif line.startswith('OBJPAL '):
                m = re.match(r'OBJPAL FRAME:(\d+) CRC=(\w+) DATA=(\w+)', line)
                if m:
                    frame = int(m.group(1))
                    frames[frame]['objpal'] = {'crc': m.group(2), 'data': m.group(3)}

    return dict(frames)


def compare_vblank(ref, rec):
    """Compare per-frame VBLANK checksums."""
    print("=== VBLANK Comparison ===")
    max_frame = min(max(ref.keys(), default=0), max(rec.keys(), default=0))
    divergences = 0

    for frame in range(max_frame + 1):
        rv = ref.get(frame, {}).get('vblank')
        cv = rec.get(frame, {}).get('vblank')
        if not rv or not cv:
            continue

        diffs = []
        for key in ['OAM_CRC', 'TMAP0_CRC', 'TMAP1_CRC', 'TDATA_CRC', 'VRAM1_CRC', 'FB_CRC']:
            if rv.get(key) != cv.get(key):
                diffs.append(f"  {key}: REF={rv.get(key)} REC={cv.get(key)}")

        if diffs:
            divergences += 1
            if divergences <= 20:
                print(f"\nFrame {frame} DIVERGENCE:")
                for d in diffs:
                    print(d)

    if divergences == 0:
        print(f"  All {max_frame + 1} frames match!")
    else:
        print(f"\n  {divergences} frames diverged out of {max_frame + 1}")


def compare_scanlines(ref, rec):
    """Compare per-scanline PPU register state."""
    print("\n=== Scanline Comparison ===")
    max_frame = min(max(ref.keys(), default=0), max(rec.keys(), default=0))
    divergences = 0

    for frame in range(max_frame + 1):
        rs = ref.get(frame, {}).get('scanlines', {})
        cs = rec.get(frame, {}).get('scanlines', {})

        for sl in sorted(set(rs.keys()) | set(cs.keys())):
            rr = rs.get(sl, {})
            cr = cs.get(sl, {})
            if not rr or not cr:
                continue

            diffs = []
            for key in ['LCDC', 'STAT', 'SCX', 'SCY', 'BGP', 'OBP0', 'OBP1', 'WY', 'WX', 'BCPS', 'OCPS']:
                if rr.get(key) != cr.get(key):
                    diffs.append(f"  {key}: REF={rr.get(key)} REC={cr.get(key)}")

            if diffs:
                divergences += 1
                if divergences <= 10:
                    print(f"\nFrame {frame}, Scanline {sl} DIVERGENCE:")
                    for d in diffs:
                        print(d)

    if divergences == 0:
        print(f"  All scanlines match across {max_frame + 1} frames!")
    else:
        print(f"\n  {divergences} scanline divergences (showing first 10)")


def compare_palettes(ref, rec):
    """Compare CGB palette RAM."""
    print("\n=== Palette Comparison ===")
    max_frame = min(max(ref.keys(), default=0), max(rec.keys(), default=0))
    divergences = 0

    for frame in range(max_frame + 1):
        for pal_type in ['bgpal', 'objpal']:
            rp = ref.get(frame, {}).get(pal_type)
            cp = rec.get(frame, {}).get(pal_type)
            if not rp or not cp:
                continue

            if rp['crc'] != cp['crc']:
                divergences += 1
                if divergences <= 10:
                    name = "BG" if pal_type == 'bgpal' else 'OBJ'
                    print(f"\nFrame {frame} {name} Palette DIVERGENCE:")
                    print(f"  REF CRC: {rp['crc']}")
                    print(f"  REC CRC: {cp['crc']}")
                    # Show byte-level diff
                    rd = rp['data']
                    cd = cp['data']
                    diff_bytes = []
                    for i in range(0, min(len(rd), len(cd)), 2):
                        if rd[i:i+2] != cd[i:i+2]:
                            diff_bytes.append(f"    Byte {i//2}: REF={rd[i:i+2]} REC={cd[i:i+2]}")
                    for d in diff_bytes[:8]:
                        print(d)
                    if len(diff_bytes) > 8:
                        print(f"    ... and {len(diff_bytes) - 8} more bytes differ")

    if divergences == 0:
        print(f"  All palette data matches!")
    else:
        print(f"\n  {divergences} palette divergences (showing first 10)")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    ref_file = sys.argv[1]
    rec_file = sys.argv[2]
    mode = sys.argv[3] if len(sys.argv) > 3 else 'all'
    if mode.startswith('--mode='):
        mode = mode[7:]
    elif mode == '--mode' and len(sys.argv) > 4:
        mode = sys.argv[4]

    print(f"Reference: {ref_file}")
    print(f"Recompiled: {rec_file}")
    print()

    ref = parse_trace(ref_file)
    rec = parse_trace(rec_file)

    print(f"Reference frames: {len(ref)} (max frame {max(ref.keys(), default=0)})")
    print(f"Recompiled frames: {len(rec)} (max frame {max(rec.keys(), default=0)})")
    print()

    if mode in ('vblank', 'all'):
        compare_vblank(ref, rec)
    if mode in ('scanline', 'all'):
        compare_scanlines(ref, rec)
    if mode in ('palette', 'all'):
        compare_palettes(ref, rec)


if __name__ == '__main__':
    main()
