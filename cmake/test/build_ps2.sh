#!/bin/bash
# Build Link's Awakening for PlayStation 2 via PS2SDK + gsKit.
#
# Native PS2SDK backend (platform_ps2.c). Pure C build — no SDL2, no ImGui,
# no C++. PS2 (Emotion Engine) is MIPS little-endian so rom.c compiles unchanged.
#
# The PS2 needs IOP modules (IRX) for audio and USB storage. We embed them in
# the ELF: bin2c turns each .irx into a C array the backend loads with
# SifExecModuleBuffer(). Controller modules come from rom0: at runtime.
#
# NOTE (riskiest backend): the PS2SDK USB stack module set has changed over
# time (classic usbd.irx + usbhdfsd.irx vs the newer bdm stack: usbd.irx +
# usbmass_bd.irx + bdm.irx + bdmfs_fatfs.irx). If bin2c fails to find an .irx
# below, or saves don't mount, adjust the IRX list here AND the extern symbols
# + load_iop() in platform_ps2.c to match your PS2SDK. For PCSX2 testing,
# switching SAVE_DIR to "host:" in platform_ps2.c avoids USB entirely.
#
# Pipeline:  bin2c irx -> ee-gcc -> link gsKit/audsrv/pad -> ELF
#
# Requires PS2DEV/PS2SDK. Install: https://github.com/ps2dev/ps2toolchain
#
# Usage:  bash build_ps2.sh [output-dir]

set -e

: "${PS2SDK:=/usr/local/ps2dev/ps2sdk}"
LA="/mnt/d/ports/la360"
RT="$LA/runtime"
OUT="${1:-$HOME/la360-ps2}"

CC="mips64r5900el-ps2-elf-gcc"
BIN2C="$PS2SDK/bin/bin2c"

CFLAGS="-D_EE -O2 -ffunction-sections -DGB_HAS_SDL2"
CFLAGS="$CFLAGS -I$PS2SDK/ee/include -I$PS2SDK/common/include -I$PS2SDK/ports/include"
CFLAGS="$CFLAGS -I$LA -I$RT/include -I$RT/src -w"
LDFLAGS="-T$PS2SDK/ee/startup/linkfile -L$PS2SDK/ee/lib -L$PS2SDK/ports/lib -Wl,--gc-sections"
LIBS="-lgskit -ldmakit -lgskit_toolkit -laudsrv -lpad -lpatches \
      -lkernel -lc -lm"

mkdir -p "$OUT"
cd "$OUT"

OBJS=()
cc1() { echo "  cc  $(basename $1)"; "$CC" $CFLAGS -c "$1" -o "$2"; OBJS+=("$2"); }

echo '[1/6] Embed IOP modules (bin2c)'
emb() {  # emb <irx-path> <label>
    "$BIN2C" "$1" "$2.c" "$2"
    cc1 "$2.c" "$2.o"
}
emb "$PS2SDK/iop/irx/freesd.irx"   freesd_irx
emb "$PS2SDK/iop/irx/audsrv.irx"   audsrv_irx
emb "$PS2SDK/iop/irx/usbd.irx"     usbd_irx
emb "$PS2SDK/iop/irx/usbhdfsd.irx" usbhdfsd_irx

echo '[2/6] Runtime + platform_ps2 (fast — fail here before rom.c)'
for s in gbrt ppu audio interpreter hwtrace; do
    cc1 "$RT/src/$s.c" "$s.o"
done
cc1 "$RT/src/menu_gui_stubs.c"     "menu_gui_stubs.o"
cc1 "$RT/src/asset_viewer_stubs.c" "asset_viewer_stubs.o"
cc1 "$RT/src/platform_ps2.c"       "platform_ps2.o"

echo '[3/6] Game entry + ROM data'
cc1 "$LA/rom_main.c" "rom_main.o"
cc1 "$LA/rom_rom.c"  "rom_rom.o"

echo '[4/6] Recompiled game — rom.c (~115 MB, slow)'
cc1 "$LA/rom.c" "rom.o"

echo '[5/6] Link'
"$CC" "${OBJS[@]}" $LDFLAGS $LIBS -o la360.elf

echo '[6/6] done'
ls -lh la360.elf 2>/dev/null
if [ -d "$LA/build-ps2" ]; then
    cp la360.elf "$LA/build-ps2/linksawakening.elf" 2>/dev/null || true
    echo "Copied to D:\\ports\\la360\\build-ps2\\linksawakening.elf"
fi
