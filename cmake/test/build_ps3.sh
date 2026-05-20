#!/bin/bash
# Build Link's Awakening for PlayStation 3 via PSL1GHT.
#
# Runs INSIDE the ps3dev/ps3dev Docker container, with the la360 repo
# mounted at /src. Invoke from the host via:
#   docker run --rm -v D:/ports/la360:/src ps3dev/ps3dev:submodules \
#       bash /src/cmake/test/build_ps3.sh
#
# Native PSL1GHT backend (platform_psl1ght.c). Pure C — no SDL2/ImGui/C++.
# PS3 is PowerPC big-endian; the gbrt.h register-pair fix (Phase 5a) makes
# the recompiled rom.c correct here.
#
# Pipeline: powerpc64-ps3-elf-gcc -> link -> strip -> sprxlinker -> fself
# Output: /src/build-ps3/EBOOT.BIN (fake self — runs in RPCS3 and on CFW)

set -e

export PS3DEV=/usr/local/ps3dev
export PSL1GHT=/usr/local/ps3dev
export PATH=$PS3DEV/bin:$PS3DEV/ppu/bin:$PATH

LA=/src
RT=$LA/runtime
OUT=$LA/build-ps3
CC=powerpc64-ps3-elf-gcc

MACHDEP="-mcpu=cell -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections"
CFLAGS="$MACHDEP -O2 -w -DGB_HAS_SDL2"
CFLAGS="$CFLAGS -I$PSL1GHT/ppu/include -I$PSL1GHT/ppu/include/simdmath"
CFLAGS="$CFLAGS -I$LA -I$RT/include -I$RT/src"
LIBS="-lrsx -lgcm_sys -lio -lsysutil -laudio -lrt -llv2 -lm"

mkdir -p "$OUT"
cd "$OUT"

OBJS=()
cc1() { echo "  cc  $(basename $1)"; "$CC" $CFLAGS -c "$1" -o "$2"; OBJS+=("$2"); }

echo '[1/4] Runtime + platform_psl1ght (fast — fail here before rom.c)'
for s in gbrt ppu audio interpreter hwtrace; do
    cc1 "$RT/src/$s.c" "$s.o"
done
cc1 "$RT/src/menu_gui_stubs.c"     "menu_gui_stubs.o"
cc1 "$RT/src/asset_viewer_stubs.c" "asset_viewer_stubs.o"
cc1 "$RT/src/platform_psl1ght.c"   "platform_psl1ght.o"

echo '[2/4] Game entry + ROM data'
cc1 "$LA/rom_main.c" "rom_main.o"
cc1 "$LA/rom_rom.c"  "rom_rom.o"

echo '[3/4] Recompiled game — rom.c (~115 MB, slow)'
cc1 "$LA/rom.c" "rom.o"

echo '[4/4] Link -> strip -> sprxlinker -> fself'
"$CC" $MACHDEP "${OBJS[@]}" -L"$PSL1GHT/ppu/lib" $LIBS -o la360.elf
powerpc64-ps3-elf-strip la360.elf -o la360.stripped.elf
sprxlinker la360.stripped.elf
fself.py la360.stripped.elf EBOOT.BIN

echo
echo '=== output ==='
ls -lh la360.elf EBOOT.BIN 2>/dev/null
echo 'EBOOT.BIN is a fake-signed self — boots in RPCS3 and on CFW PS3.'
