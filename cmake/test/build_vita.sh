#!/bin/bash
# Build Link's Awakening for PlayStation Vita via VitaSDK + vita2d.
#
# Native vita2d backend (platform_vita.c). Pure C build — no SDL2, no ImGui,
# no C++. Vita is ARM little-endian so rom.c compiles unchanged.
#
# Pipeline:  arm-vita-eabi-gcc -Wl,-q -> vita-elf-create -> vita-make-fself
#            -> vita-mksfoex -> vita-pack-vpk
#
# Requires VitaSDK on PATH ($VITASDK/bin). Install: https://vitasdk.org
# vita2d: `vdpm vita2d` (or build from xerpi/libvita2d).
#
# Usage:  bash build_vita.sh [output-dir]

set -e

LA="/mnt/d/ports/la360"
RT="$LA/runtime"
OUT="${1:-$HOME/la360-vita}"
TITLEID="LADX00001"

CC="arm-vita-eabi-gcc"

# -Wl,-q keeps relocations so vita-elf-create can build the .velf.
# -DGB_HAS_SDL2 selects the real game loop in rom_main.c (no SDL2 on Vita;
#  platform_vita.c supplies the gb_platform_* symbols).
CFLAGS="-O2 -ffunction-sections -DGB_HAS_SDL2 -I$LA -I$RT/include -I$RT/src -w"
LDFLAGS="-Wl,-q -Wl,--gc-sections"
LIBS="-lvita2d -lSceGxm_stub -lSceDisplay_stub -lSceCtrl_stub \
      -lSceAudio_stub -lSceSysmodule_stub -lSceCommonDialog_stub \
      -lScePgf_stub -lfreetype -lpng -ljpeg -lz -lm -lc"

mkdir -p "$OUT"
cd "$OUT"

OBJS=()
cc1() { echo "  cc  $(basename $1)"; "$CC" $CFLAGS -c "$1" -o "$2"; OBJS+=("$2"); }

echo '[1/6] Runtime + platform_vita (fast — fail here before rom.c)'
for s in gbrt ppu audio interpreter hwtrace; do
    cc1 "$RT/src/$s.c" "$s.o"
done
cc1 "$RT/src/menu_gui_stubs.c"     "menu_gui_stubs.o"
cc1 "$RT/src/asset_viewer_stubs.c" "asset_viewer_stubs.o"
cc1 "$RT/src/platform_vita.c"      "platform_vita.o"

echo '[2/6] Game entry + ROM data'
cc1 "$LA/rom_main.c" "rom_main.o"
cc1 "$LA/rom_rom.c"  "rom_rom.o"

echo '[3/6] Recompiled game — rom.c (~115 MB, slow)'
cc1 "$LA/rom.c" "rom.o"

echo '[4/6] Link'
"$CC" $LDFLAGS "${OBJS[@]}" $LIBS -o la360.elf

echo '[5/6] velf + fself'
vita-elf-create la360.elf la360.velf
vita-make-fself -s la360.velf eboot.bin

echo '[6/6] param.sfo + vpk'
vita-mksfoex -s "TITLE_ID=$TITLEID" "Links Awakening DX" param.sfo
vita-pack-vpk -s param.sfo -b eboot.bin la360.vpk

echo
echo '=== output ==='
ls -lh la360.elf la360.vpk 2>/dev/null
if [ -d "$LA/build-vita" ]; then
    cp la360.vpk "$LA/build-vita/linksawakening.vpk" 2>/dev/null || true
    echo "Copied to D:\\ports\\la360\\build-vita\\linksawakening.vpk"
fi
