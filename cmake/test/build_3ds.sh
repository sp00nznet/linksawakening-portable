#!/bin/bash
# Build Link's Awakening for Nintendo 3DS via devkitARM + libctru.
#
# Native libctru backend (platform_3ds.c). Pure C build — no SDL2, no
# ImGui, no C++. 3DS is ARM little-endian so rom.c compiles unchanged.
#
# Pipeline:  arm-none-eabi-gcc -> link libctru -> 3dsxtool -> .3dsx
#
# Usage:  bash build_3ds.sh [output-dir]

set -e

export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM
DKA="$DEVKITARM"
CTRULIB="$DEVKITPRO/libctru"
LA="/mnt/d/ports/la360"
RT="$LA/runtime"
OUT="${1:-$HOME/la360-3ds}"     # ~ persists; /tmp does not

CC="$DKA/bin/arm-none-eabi-gcc"

ARCH="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"
# -DGB_HAS_SDL2: rom_main.c uses this to select the real game loop (vs the
#  no-platform test path). The 3DS has no SDL2 — platform_3ds.c supplies
#  every gb_platform_* symbol — but the macro name is the loop selector.
CFLAGS="$ARCH -O2 -mword-relocations -ffunction-sections -D__3DS__ -DGB_HAS_SDL2"
CFLAGS="$CFLAGS -I$CTRULIB/include -I$LA -I$RT/include -I$RT/src -w"
LDFLAGS="-specs=3dsx.specs $ARCH"

mkdir -p "$OUT"
cd "$OUT"

OBJS=()
cc1() { echo "  cc  $(basename $1)"; "$CC" $CFLAGS -c "$1" -o "$2"; OBJS+=("$2"); }

echo '[1/4] Runtime + platform_3ds (fast — fail here before rom.c)'
for s in gbrt ppu audio interpreter hwtrace; do
    cc1 "$RT/src/$s.c" "$s.o"
done
cc1 "$RT/src/menu_gui_stubs.c"     "menu_gui_stubs.o"
cc1 "$RT/src/asset_viewer_stubs.c" "asset_viewer_stubs.o"
cc1 "$RT/src/platform_3ds.c"       "platform_3ds.o"

echo '[2/4] Game entry + ROM data'
cc1 "$LA/rom_main.c" "rom_main.o"
cc1 "$LA/rom_rom.c"  "rom_rom.o"

echo '[3/4] Recompiled game — rom.c (~115 MB, slow)'
cc1 "$LA/rom.c" "rom.o"

echo '[4/4] Link + 3dsxtool'
"$CC" $LDFLAGS "${OBJS[@]}" -L"$CTRULIB/lib" -lctru -lm -o la360.elf

"$DEVKITPRO/tools/bin/3dsxtool" la360.elf la360.3dsx

echo
echo '=== output ==='
ls -lh la360.elf la360.3dsx 2>/dev/null
if [ -d "$LA/build-3ds" ]; then
    cp la360.3dsx "$LA/build-3ds/linksawakening.3dsx" 2>/dev/null || true
    echo "Copied to D:\\ports\\la360\\build-3ds\\linksawakening.3dsx"
fi
