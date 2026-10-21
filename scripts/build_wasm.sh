#!/bin/bash
# Build Link's Awakening for WebAssembly via Emscripten.
#
# Reuses platform_sdl.cpp through Emscripten's SDL2 port. wasm32 is
# little-endian so rom.c compiles unchanged. ImGui + multiplayer off
# (same as the PS4 build). The browser owns the event loop — rom_main.c
# uses emscripten_set_main_loop when __EMSCRIPTEN__ is defined.
#
# Pipeline: emcc -> linksawakening.html + linksawakening.js + linksawakening.wasm
#
# Usage:  bash build_wasm.sh [output-dir]

set -e

# emcc is on PATH in the emscripten/emsdk image; a WSL install sources emsdk_env.sh.
command -v emcc >/dev/null || source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1

. "$(dirname "$0")/env.sh"
OUT="${1:-$HOME/la360-wasm}"

# -sUSE_SDL=2 selects Emscripten's bundled SDL2 port (headers + lib).
CFLAGS="-O2 -w -sUSE_SDL=2 -DGB_HAS_SDL2"
CFLAGS="$CFLAGS $LA_DEFS -I$GEN -I$RT/include -I$RT/src"

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
cc1 "$GEN/rom_rom.c"  "rom_rom.o"

echo '[3/4] Recompiled game — rom.c (~115 MB, slow)'
# -fno-inline: rom.c is one giant translation unit; at -O2 the compiler
# inlines thousands of small recompiled functions together, producing a
# single function over WebAssembly's 7.65 MB per-function limit. Disabling
# inlining keeps every recompiled function a separate (bounded) wasm function.
echo '  cc  rom.c  (-fno-inline)'
if fresh rom.o; then echo "  rom.o is up to date (skipping)"; else
emcc $CFLAGS -fno-inline -c "$GEN/rom.c" -o "rom.o"
fi
OBJS+=("rom.o")

echo '[4/4] Link -> linksawakening.html'
# Link at -O0: at -O1+ emscripten runs wasm-opt, whose whole-program
# inliner merges the ~9000 small recompiled functions back together until
# one exceeds WebAssembly's 7.65 MB per-function cap. -O0 skips wasm-opt,
# so every recompiled function stays a separate, bounded wasm function.
# The per-function code is still clang -O2 optimized (from the .o files).
# ALLOW_MEMORY_GROWTH: the recompiled game + ROM array needs a lot of
# linear memory; let it grow rather than guessing a fixed size.
em++ -O0 -sUSE_SDL=2 \
    -sALLOW_MEMORY_GROWTH=1 \
    -sINITIAL_MEMORY=134217728 \
    -sEXIT_RUNTIME=0 \
    -sSTACK_SIZE=1048576 \
    --shell-file "$ROOT/scripts/wasm_shell.html" \
    "${OBJS[@]}" \
    -o linksawakening.html

echo
echo '=== output ==='
ls -lh linksawakening.html linksawakening.js linksawakening.wasm 2>/dev/null
if [ -d "$ROOT/build-wasm" ]; then
    cp linksawakening.html linksawakening.js linksawakening.wasm "$ROOT/build-wasm/" 2>/dev/null || true
    echo "Copied to $ROOT/build-wasm/"
fi
