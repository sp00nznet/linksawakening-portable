#!/bin/bash
# Build Link's Awakening for Sony PSP via the PSPSDK (pspdev / psp-gcc).
#
# Native PSPSDK backend (platform_psp.c). Pure C build — no SDL2, no ImGui,
# no C++. PSP is MIPS little-endian so rom.c compiles unchanged.
#
# Pipeline:  psp-gcc -> psp-fixup-imports -> psp-strip -> mksfoex -> pack-pbp
#
# Requires pspdev on PATH (psp-gcc, psp-config, mksfoex, pack-pbp). Install via
# https://github.com/pspdev/pspdev  (sets $PSPDEV; adds $PSPDEV/bin to PATH).
#
# Usage:  bash build_psp.sh [output-dir]

set -e

PSPSDK="$(psp-config --pspsdk-path)"
LA="/mnt/d/ports/la360"
RT="$LA/runtime"
OUT="${1:-$HOME/la360-psp}"

CC="psp-gcc"

# -DGB_HAS_SDL2 selects the real game loop in rom_main.c (macro name is the
#  loop selector — there is no SDL2 on PSP; platform_psp.c supplies the symbols).
CFLAGS="-O2 -G0 -ffunction-sections -D_PSP_FW_VERSION=600 -DGB_HAS_SDL2"
CFLAGS="$CFLAGS -I$PSPSDK/include -I$LA -I$RT/include -I$RT/src -w"
LDFLAGS="-L$PSPSDK/lib -Wl,--gc-sections"
LIBS="-lpspaudio -lpspdisplay -lpspge -lpspctrl -lpsputility -lpspuser \
      -lpspkernel -lpsprtc -lc -lpspnet -lpspnet_inet -lm"

mkdir -p "$OUT"
cd "$OUT"

OBJS=()
cc1() { echo "  cc  $(basename $1)"; "$CC" $CFLAGS -c "$1" -o "$2"; OBJS+=("$2"); }

echo '[1/5] Runtime + platform_psp (fast — fail here before rom.c)'
for s in gbrt ppu audio interpreter hwtrace; do
    cc1 "$RT/src/$s.c" "$s.o"
done
cc1 "$RT/src/menu_gui_stubs.c"     "menu_gui_stubs.o"
cc1 "$RT/src/asset_viewer_stubs.c" "asset_viewer_stubs.o"
cc1 "$RT/src/platform_psp.c"       "platform_psp.o"

echo '[2/5] Game entry + ROM data'
cc1 "$LA/rom_main.c" "rom_main.o"
cc1 "$LA/rom_rom.c"  "rom_rom.o"

echo '[3/5] Recompiled game — rom.c (~115 MB, slow)'
cc1 "$LA/rom.c" "rom.o"

echo '[4/5] Link'
"$CC" $LDFLAGS "${OBJS[@]}" $LIBS -o la360.elf
psp-fixup-imports la360.elf

echo '[5/5] EBOOT.PBP'
psp-strip la360.elf -o la360_strip.elf
mksfoex -d MEMSIZE=1 "Links Awakening DX" PARAM.SFO
pack-pbp EBOOT.PBP PARAM.SFO NULL NULL NULL NULL NULL la360_strip.elf NULL

echo
echo '=== output ==='
ls -lh la360.elf EBOOT.PBP 2>/dev/null
if [ -d "$LA/build-psp" ]; then
    cp EBOOT.PBP "$LA/build-psp/EBOOT.PBP" 2>/dev/null || true
    echo "Copied to D:\\ports\\la360\\build-psp\\EBOOT.PBP"
fi
