#!/bin/bash
# Build Link's Awakening for PlayStation 4 via the OpenOrbis toolchain.
#
# Reuses platform_sdl.cpp against OpenOrbis's SDL2 port. PS4 is x86-64 LE so
# rom.c compiles unchanged. Multiplayer OFF. ImGui OFF — OpenOrbis's bundled
# SDL2 predates 2.0.17 (no SDL_RenderGeometry), so the ImGui SDL_Renderer
# backend won't build; platform_sdl.cpp's ImGui calls are #ifdef LA_HAS_IMGUI
# gated and we link the menu_gui/asset_viewer stubs instead.
#
# Pipeline:  clang -> ld.lld -> create-fself -> eboot.bin
# (.pkg packaging is a separate step — needs libssl1.1 for PkgTool.Core)
#
# Usage:  bash build_ps4.sh [output-dir]

set -e

export OO_PS4_TOOLCHAIN="$HOME/PS4Toolchain"
TC="$OO_PS4_TOOLCHAIN"
LA="/mnt/d/ports/la360"
RT="$LA/runtime"
OUT="${1:-/tmp/la360-ps4}"

TARGET="x86_64-pc-freebsd12-elf"
CC="clang"
CXX="clang++"
LD="ld.lld"

# Compile flags (from the OpenOrbis SDL2 sample Makefile)
CFLAGS="--target=$TARGET -fPIC -funwind-tables -O2"
CFLAGS="$CFLAGS -isysroot $TC -isystem $TC/include -isystem $TC/include/SDL2"
CFLAGS="$CFLAGS -DGB_HAS_SDL2"
CFLAGS="$CFLAGS -I$LA -I$RT/include -I$RT/src"
CFLAGS="$CFLAGS -Wno-everything"
CXXFLAGS="$CFLAGS -isystem $TC/include/c++/v1 -std=c++17"

mkdir -p "$OUT"
cd "$OUT"

compile_c()   { echo "  cc  $(basename $1)"; $CC  $CFLAGS   -c "$1" -o "$2"; }
compile_cxx() { echo "  cxx $(basename $1)"; $CXX $CXXFLAGS -c "$1" -o "$2"; }

OBJS=()

echo '[1/4] Runtime (fast — fail here before the big rom.c compile)'
# Core engine (C)
for s in gbrt ppu audio interpreter hwtrace; do
    compile_c "$RT/src/$s.c" "$s.o";  OBJS+=("$s.o")
done
# Menu / asset viewer stubs (LA_HAS_IMGUI is OFF — plain C)
for s in menu_gui_stubs asset_viewer_stubs; do
    compile_c "$RT/src/$s.c" "$s.o";  OBJS+=("$s.o")
done
# SDL2 platform backend (C++ — ImGui calls inside are #ifdef-gated off)
compile_cxx "$RT/src/platform_sdl.cpp" "platform_sdl.o";  OBJS+=("platform_sdl.o")

echo '[2/4] Game entry + ROM data (C)'
compile_c "$LA/rom_main.c" "rom_main.o";  OBJS+=("rom_main.o")
compile_c "$LA/rom_rom.c"  "rom_rom.o";   OBJS+=("rom_rom.o")

echo '[3/4] Recompiled game — rom.c (~115 MB, this is the slow one)'
compile_c "$LA/rom.c" "rom.o";  OBJS+=("rom.o")

echo '[4/4] Link + create-fself'
$LD "${OBJS[@]}" -o la360.elf \
    -m elf_x86_64 -pie --script "$TC/link.x" --eh-frame-hdr \
    -L"$TC/lib" \
    -lc -lkernel -lc++ \
    -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad -lSceSysmodule \
    -lSDL2 \
    "$TC/lib/crt1.o"

"$TC/bin/linux/create-fself" \
    -in=la360.elf -out=la360.oelf \
    --eboot eboot.bin --paid 0x3800000000000011

echo
echo "=== output ==="
ls -lh la360.elf eboot.bin 2>/dev/null
echo
echo "eboot.bin built. .pkg packaging is a separate step (needs libssl1.1)."
