#!/bin/bash
# Build Link's Awakening for Nintendo Wii via devkitPPC + libogc.
#
# Native libogc backend (platform_wii.c). Pure C build — no SDL2, no
# ImGui, no C++. Wii is PowerPC *big-endian*: the AF/BC/DE/HL register-
# pair fix in gbrt.h (shared with the PS3/360 ports) makes rom.c correct.
#
# Pipeline:  powerpc-eabi-gcc -> link libogc -> elf2dol -> .dol
#
# Usage:  bash build_wii.sh [output-dir]

set -e

export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=/opt/devkitpro/devkitPPC
DKP="$DEVKITPPC"
LIBOGC="$DEVKITPRO/libogc"
LA="/mnt/d/ports/la360"
RT="$LA/runtime"
OUT="${1:-$HOME/la360-wii}"      # ~ persists; /tmp does not

CC="$DKP/bin/powerpc-eabi-gcc"

# MACHDEP — copied verbatim from devkitPPC/wii_rules.
MACHDEP="-DGEKKO -mrvl -mcpu=750 -meabi -mhard-float"

# -DGB_HAS_SDL2: rom_main.c uses this to select the real game loop (vs the
#  no-platform test path). The Wii has no SDL2 — platform_wii.c supplies
#  every gb_platform_* symbol — but the macro name is the loop selector.
CFLAGS="$MACHDEP -O2 -ffunction-sections -DGB_HAS_SDL2"
CFLAGS="$CFLAGS -I$LIBOGC/include -I$LA -I$RT/include -I$RT/src -w"
LDFLAGS="$MACHDEP -Wl,--gc-sections -L$LIBOGC/lib/wii"
LIBS="-lfat -lwiiuse -lbte -lasnd -logc -lm"

mkdir -p "$OUT"
cd "$OUT"

OBJS=()
cc1() { echo "  cc  $(basename $1)"; "$CC" $CFLAGS -c "$1" -o "$2"; OBJS+=("$2"); }

echo '[1/4] Runtime + platform_wii (fast — fail here before rom.c)'
for s in gbrt ppu audio interpreter hwtrace; do
    cc1 "$RT/src/$s.c" "$s.o"
done
cc1 "$RT/src/menu_gui_stubs.c"     "menu_gui_stubs.o"
cc1 "$RT/src/asset_viewer_stubs.c" "asset_viewer_stubs.o"
cc1 "$RT/src/platform_wii.c"       "platform_wii.o"

echo '[2/4] Game entry + ROM data'
cc1 "$LA/rom_main.c" "rom_main.o"
cc1 "$LA/rom_rom.c"  "rom_rom.o"

echo '[3/4] Recompiled game — rom.c (~115 MB, slow)'
cc1 "$LA/rom.c" "rom.o"

echo '[4/4] Link + elf2dol'
"$CC" "${OBJS[@]}" $LDFLAGS $LIBS -o la360.elf

"$DEVKITPRO/tools/bin/elf2dol" la360.elf la360.dol

echo
echo '=== output ==='
ls -lh la360.elf la360.dol 2>/dev/null
if [ -d "$LA/build-wii" ]; then
    cp la360.dol "$LA/build-wii/linksawakening.dol" 2>/dev/null || true
    echo "Copied to D:\\ports\\la360\\build-wii\\linksawakening.dol"
fi
