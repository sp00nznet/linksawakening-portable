#!/bin/bash
# Verify the gb-recompiled runtime library compiles + links for libxenon
# with LA_HAS_IMGUI=OFF and LA_HAS_MULTIPLAYER=OFF (Phase 5a validation).
#
# Builds the runtime as a static .a, then links the hello-world test
# against it to confirm there are no undefined symbols.

set -e

DEVKITXENON="${DEVKITXENON:-/usr/local/xenon}"
RUNTIME="${RUNTIME:-/mnt/d/ports/la360/runtime}"
OUT="${1:-/tmp/la360-rt-xenon}"

XCC="$DEVKITXENON/bin/xenon-gcc"
XCXX="$DEVKITXENON/bin/xenon-g++"
XAR="$DEVKITXENON/bin/xenon-ar"

CFLAGS="-DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float -O2 -Wall"
CFLAGS="$CFLAGS -DGB_HAS_SDL2=0"      # not really used at this layer but explicit
CFLAGS="$CFLAGS -I$DEVKITXENON/usr/include"
CFLAGS="$CFLAGS -I$RUNTIME/include"
CFLAGS="$CFLAGS -I$RUNTIME/src"
CFLAGS="$CFLAGS -I$RUNTIME/src/multiplayer"

rm -rf "$OUT"
mkdir -p "$OUT"
cd "$OUT"

echo '[1/4] Compiling runtime sources (LA_HAS_IMGUI=OFF, LA_HAS_MULTIPLAYER=OFF)'
SOURCES=(
    "$RUNTIME/src/gbrt.c"
    "$RUNTIME/src/interpreter.c"
    "$RUNTIME/src/ppu.c"
    "$RUNTIME/src/audio.c"
    "$RUNTIME/src/hwtrace.c"
    # platform_libxenon.c replaces platform_sdl.cpp for the Xbox 360 build.
    # Implements all 13 gb_platform_* + on_audio_sample / load/save_battery_ram
    # against libxenon's xenos / xenon_sound / input / xenon_uart APIs.
    "$RUNTIME/src/platform_libxenon.c"
    "$RUNTIME/src/menu_gui_stubs.c"
    "$RUNTIME/src/asset_viewer_stubs.c"
)
OBJS=()
for src in "${SOURCES[@]}"; do
    obj="$(basename "${src%.*}").o"
    OBJS+=("$obj")
    case "$src" in
        *.cpp) $XCXX $CFLAGS -c "$src" -o "$obj" ;;
        *.c)   $XCC  $CFLAGS -c "$src" -o "$obj" ;;
    esac
    echo "  ok: $(basename "$src")"
done

echo '[2/4] Archive → libgbrt-xenon.a'
$XAR rc libgbrt-xenon.a "${OBJS[@]}"

echo '[3/4] Linking the hello-world against libgbrt-xenon.a'
HELLO_SRC=/mnt/d/ports/la360/cmake/test/hello_xenon.c
sed -e 's|0x80000000|0x82000000|' \
    -e 's|0x9E000000|0xA0000000|' \
    -e 's|0xa0000000|0xA2000000|' \
    "$DEVKITXENON/app.lds" > xenia.lds

$XCC $CFLAGS \
    -I$DEVKITXENON/usr/include \
    -L$DEVKITXENON/xenon/lib/32 -L$DEVKITXENON/usr/lib \
    -T xenia.lds \
    "$HELLO_SRC" libgbrt-xenon.a \
    -lxenon -lm -lc -lgcc -o hello-rt.elf

"$DEVKITXENON/bin/xenon-objcopy" -O elf32-powerpc hello-rt.elf hello-rt.elf32
"$DEVKITXENON/bin/xenon-strip" hello-rt.elf32

echo '[4/4] Resulting files:'
ls -lh libgbrt-xenon.a hello-rt.elf32

echo
echo "If you got this far, Phase 5a is good — the BE-aware register unions"
echo "compiled, the LA_HAS_* gates dropped all MP/ImGui code, and the"
echo "stubs satisfy every undefined symbol from platform_sdl.cpp's #else branch."
