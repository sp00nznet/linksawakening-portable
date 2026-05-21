#!/bin/bash
# Build the full Link's Awakening game into an Xbox 360 XEX via libxenon.
#
# Native libxenon backend (platform_libxenon.c). Plain C. The 360 is
# PowerPC *big-endian* — the AF/BC/DE/HL register-pair fix in gbrt.h
# (shared with PS3/Wii) makes the recompiled rom.c correct here.
#
# Pipeline:  xenon-gcc -> link libxenon -> xenon-objcopy -> elf2xex -> .xex
#
# NOTE: Xenia (the 360 emulator) crashes on libxenon homebrew that drives
# the video/audio hardware directly — the XEX builds and is hardware-ready
# (RGH/JTAG) but cannot currently be validated in an emulator.
#
# Usage:  bash build_libxenon.sh [output-dir]

set -e

DEVKITXENON="${DEVKITXENON:-/usr/local/xenon}"
LA=/mnt/d/ports/la360
RT=$LA/runtime
OUT="${1:-$HOME/la360-xenon}"

XCC="$DEVKITXENON/bin/xenon-gcc"
ELF2XEX="/mnt/c/xbox360nfs/tools/elf2xex"

# -DGB_HAS_SDL2: rom_main.c's game-loop selector. The 360 has no SDL2 —
#  platform_libxenon.c supplies every gb_platform_* symbol — but that macro
#  name is what rom_main.c keys the real game loop off.
CFLAGS="-DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float -O2 -w -DGB_HAS_SDL2"
CFLAGS="$CFLAGS -I$DEVKITXENON/usr/include -I$LA -I$RT/include -I$RT/src"

mkdir -p "$OUT"
cd "$OUT"

OBJS=()
cc1() { echo "  cc  $(basename $1)"; "$XCC" $CFLAGS -c "$1" -o "$2"; OBJS+=("$2"); }

echo '[1/4] Runtime + platform_libxenon (fast — fail here before rom.c)'
for s in gbrt ppu audio interpreter hwtrace; do
    cc1 "$RT/src/$s.c" "$s.o"
done
cc1 "$RT/src/menu_gui_stubs.c"     "menu_gui_stubs.o"
cc1 "$RT/src/asset_viewer_stubs.c" "asset_viewer_stubs.o"
cc1 "$RT/src/platform_libxenon.c"  "platform_libxenon.o"

echo '[2/4] Game entry + ROM data'
cc1 "$LA/rom_main.c" "rom_main.o"
cc1 "$LA/rom_rom.c"  "rom_rom.o"

echo '[3/4] Recompiled game — rom.c (~105 MB, slow)'
cc1 "$LA/rom.c" "rom.o"

echo '[4/4] Link + elf2xex'
# Bump the load addresses (Xenia/homebrew-friendlier) — same as the
# libxenon test scripts.
sed -e 's|0x80000000|0x82000000|' \
    -e 's|0x9E000000|0xA0000000|' \
    -e 's|0xa0000000|0xA2000000|' \
    "$DEVKITXENON/app.lds" > xenia.lds

"$XCC" $CFLAGS \
    -L$DEVKITXENON/xenon/lib/32 -L$DEVKITXENON/usr/lib \
    -T xenia.lds \
    "${OBJS[@]}" \
    -lxenon -lm -lc -lgcc \
    -o la360.elf

"$DEVKITXENON/bin/xenon-objcopy" -O elf32-powerpc la360.elf la360.elf32
"$DEVKITXENON/bin/xenon-strip" la360.elf32
"$ELF2XEX" la360.elf32 la360.xex

echo
echo '=== output ==='
ls -lh la360.elf32 la360.xex 2>/dev/null
if [ -d "$LA/build-xenon" ]; then
    cp la360.elf32 la360.xex "$LA/build-xenon/" 2>/dev/null || true
    cp la360.xex "$LA/build-xenon/linksawakening.xex" 2>/dev/null || true
    echo "Copied to D:\\ports\\la360\\build-xenon\\linksawakening.xex"
fi
