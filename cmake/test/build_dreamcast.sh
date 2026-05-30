#!/bin/bash
# Build Link's Awakening for Sega Dreamcast via KallistiOS (KOS).
#
# Native KOS backend (platform_dreamcast.c). Pure C build — no SDL2, no ImGui,
# no C++. Dreamcast is SH-4 little-endian so rom.c compiles unchanged.
#
# RAM is the constraint: 16 MB total. This is the tightest console target — if
# the link succeeds but it crashes/OOMs at runtime, that's the squeeze, not a
# logic bug. Consider -Os (below) and trimming features before blaming code.
#
# Pipeline:  source environ.sh -> kos-cc -> la360.elf
#            (for real hardware: kos's scramble + makeip -> 1ST_READ.BIN -> CDI)
#
# Requires KallistiOS. Install: https://dreamcast.wiki/Getting_Started_with_Dreamcast_development
# Set KOS_BASE or rely on the default environ.sh path below.
#
# Usage:  bash build_dreamcast.sh [output-dir]

set -e

KOS_ENVIRON="${KOS_ENVIRON:-/opt/toolchains/dc/kos/environ.sh}"
# shellcheck disable=SC1090
source "$KOS_ENVIRON"

# Project root, derived from this script's location (cmake/test/), so the same
# script works under WSL (/mnt/d/...) and inside the Docker mount (/src).
SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LA="${LA:-$(cd "$SELF/../.." && pwd)}"
RT="$LA/runtime"
OUT="${1:-$HOME/la360-dreamcast}"

# kos-cc injects the SH-4 arch flags + KOS includes/libs. We add ours.
# Optimization level is configurable via $OPT (default -O2). On the 16 MB DC the
# -O2 image is ~15.9 MB — it links but leaves no room to run; rebuild with
# OPT=-Os to shrink .text and free heap. -DGB_HAS_SDL2 selects the real game
# loop in rom_main.c (no SDL2 on DC; platform_dreamcast.c supplies the symbols).
# $EXTRADEF passes extra -D flags, e.g. EXTRADEF=-DLA_DC_NO_AUDIO for a silent
# (diagnostic) build that skips snd_stream entirely.
CFLAGS="${OPT:--O2} -ffunction-sections -DGB_HAS_SDL2 ${EXTRADEF:-} -I$LA -I$RT/include -I$RT/src -w"
LDFLAGS="-Wl,--gc-sections"

mkdir -p "$OUT"
cd "$OUT"

OBJS=()
cc1() { echo "  cc  $(basename $1)"; kos-cc $CFLAGS -c "$1" -o "$2"; OBJS+=("$2"); }

echo '[1/4] Runtime + platform_dreamcast (fast — fail here before rom.c)'
for s in gbrt ppu audio interpreter hwtrace; do
    cc1 "$RT/src/$s.c" "$s.o"
done
cc1 "$RT/src/menu_gui_stubs.c"      "menu_gui_stubs.o"
cc1 "$RT/src/asset_viewer_stubs.c"  "asset_viewer_stubs.o"
cc1 "$RT/src/platform_dreamcast.c"  "platform_dreamcast.o"

echo '[2/4] Game entry + ROM data'
cc1 "$LA/rom_main.c" "rom_main.o"
cc1 "$LA/rom_rom.c"  "rom_rom.o"

echo '[3/4] Recompiled game — rom.c (~115 MB, slow)'
cc1 "$LA/rom.c" "rom.o"

echo '[4/4] Link + strip'
kos-cc $LDFLAGS "${OBJS[@]}" -o la360_dbg.elf -lm
# IMPORTANT: the unstripped ELF (~50 MB of debug info) is too big for flycast's
# loader — it loads then exits immediately. Strip to the ~14 MB loadable image,
# which boots. The stripped ELF is the primary artifact.
sh-elf-strip la360_dbg.elf -o la360.elf

echo
echo '=== output ==='
ls -lh la360_dbg.elf la360.elf 2>/dev/null
sh-elf-size la360.elf 2>/dev/null
echo
echo 'Run la360.elf (stripped) in flycast/lxdream. For real hardware, scramble it:'
echo '  sh-elf-objcopy -O binary la360.elf la360.bin'
echo "  \$KOS_BASE/utils/scramble/scramble la360.bin 1ST_READ.BIN"
echo '  then build a CDI with makeip + mkisofs/cdi4dc (see KOS utils).'
if [ -d "$LA/build-dreamcast" ]; then
    cp la360.elf "$LA/build-dreamcast/linksawakening.elf" 2>/dev/null || true
    echo "Copied to D:\\ports\\la360\\build-dreamcast\\linksawakening.elf"
fi
