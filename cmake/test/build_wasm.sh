#!/bin/bash
# Build Link's Awakening for WebAssembly via Emscripten.
#
# Reuses platform_sdl.cpp through Emscripten's SDL2 port. wasm32 is
# little-endian so rom.c compiles unchanged. ImGui + multiplayer off
# (same as the PS4 build). The browser owns the event loop — rom_main.c
# uses emscripten_set_main_loop when __EMSCRIPTEN__ is defined.
#
# Pipeline: emcc -> la360.html + la360.js + la360.wasm
#
# Usage:  bash build_wasm.sh [output-dir]

set -e

source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1

LA=/mnt/d/ports/la360
RT=$LA/runtime
OUT="${1:-$HOME/la360-wasm}"

# -sUSE_SDL=2 selects Emscripten's bundled SDL2 port (headers + lib).
CFLAGS="-O2 -w -sUSE_SDL=2 -DGB_HAS_SDL2"
CFLAGS="$CFLAGS -I$LA -I$RT/include -I$RT/src"

mkdir -p "$OUT"
cd "$OUT"

OBJS=()
cc1()  { echo "  cc  $(basename $1)"; emcc $CFLAGS -c "$1" -o "$2"; OBJS+=("$2"); }
cxx1() { echo "  cxx $(basename $1)"; em++ $CFLAGS -std=c++17 -c "$1" -o "$2"; OBJS+=("$2"); }

echo '[1/4] Runtime (fast — fail here before rom.c)'
for s in gbrt ppu audio interpreter hwtrace; do
    cc1 "$RT/src/$s.c" "$s.o"
done
cc1 "$RT/src/menu_gui_stubs.c"     "menu_gui_stubs.o"
cc1 "$RT/src/asset_viewer_stubs.c" "asset_viewer_stubs.o"
cxx1 "$RT/src/platform_sdl.cpp"    "platform_sdl.o"

echo '[2/4] Game entry + ROM data'
cc1 "$LA/rom_main.c" "rom_main.o"
cc1 "$LA/rom_rom.c"  "rom_rom.o"

echo '[3/4] Recompiled game — rom.c (~115 MB, slow)'
cc1 "$LA/rom.c" "rom.o"

echo '[4/4] Link -> la360.html'
# ALLOW_MEMORY_GROWTH: the recompiled game + ROM array needs a lot of
# linear memory; let it grow rather than guessing a fixed size.
em++ -O2 -sUSE_SDL=2 \
    -sALLOW_MEMORY_GROWTH=1 \
    -sINITIAL_MEMORY=134217728 \
    -sEXIT_RUNTIME=0 \
    -sSTACK_SIZE=1048576 \
    "${OBJS[@]}" \
    -o la360.html

echo
echo '=== output ==='
ls -lh la360.html la360.js la360.wasm 2>/dev/null
if [ -d "$LA/build-wasm" ]; then
    cp la360.html la360.js la360.wasm "$LA/build-wasm/" 2>/dev/null || true
    echo "Copied to D:\\ports\\la360\\build-wasm\\"
fi
