#!/bin/bash
# Build platform_libxenon_test.c into a Xenia-loadable XEX.
# Links the test against the full runtime (libgbrt-xenon.a) so the actual
# platform_libxenon.c backend is exercised.
#
# Usage: bash build_platform_test_xenon.sh [output-dir]

set -e

DEVKITXENON="${DEVKITXENON:-/usr/local/xenon}"
RUNTIME="${RUNTIME:-/mnt/d/ports/la360/runtime}"
OUT="${1:-/tmp/la360-plat-test}"
TESTSRC="$(cd "$(dirname "$0")" && pwd)/platform_libxenon_test.c"

XCC="$DEVKITXENON/bin/xenon-gcc"
XAR="$DEVKITXENON/bin/xenon-ar"
ELF2XEX="/mnt/c/xbox360nfs/tools/elf2xex"

CFLAGS="-DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float -O2 -Wall"
CFLAGS="$CFLAGS -I$DEVKITXENON/usr/include -I$RUNTIME/include -I$RUNTIME/src"

rm -rf "$OUT"; mkdir -p "$OUT"; cd "$OUT"

echo '[1/5] Compile runtime sources'
RT_SOURCES=(
    gbrt.c interpreter.c ppu.c audio.c hwtrace.c
    platform_libxenon.c menu_gui_stubs.c asset_viewer_stubs.c
)
OBJS=()
for s in "${RT_SOURCES[@]}"; do
    $XCC $CFLAGS -c "$RUNTIME/src/$s" -o "${s%.c}.o"
    OBJS+=("${s%.c}.o")
    echo "  ok: $s"
done

echo '[2/5] Archive libgbrt-xenon.a'
$XAR rc libgbrt-xenon.a "${OBJS[@]}"

echo '[3/5] Compile + link the test (target 0x82000000 for Xenia)'
sed -e 's|0x80000000|0x82000000|' \
    -e 's|0x9E000000|0xA0000000|' \
    -e 's|0xa0000000|0xA2000000|' \
    "$DEVKITXENON/app.lds" > xenia.lds

$XCC $CFLAGS \
    -L$DEVKITXENON/xenon/lib/32 -L$DEVKITXENON/usr/lib \
    -T xenia.lds \
    "$TESTSRC" libgbrt-xenon.a \
    -lxenon -lm -lc -lgcc \
    -o platform_test.elf

echo '[4/5] Strip → elf32'
$XCC -v >/dev/null 2>&1 || true
"$DEVKITXENON/bin/xenon-objcopy" -O elf32-powerpc platform_test.elf platform_test.elf32
"$DEVKITXENON/bin/xenon-strip" platform_test.elf32

echo '[5/5] Wrap → XEX'
if [ -x "$ELF2XEX" ]; then
    "$ELF2XEX" platform_test.elf32 platform_test.xex
fi
ls -lh platform_test.elf32 platform_test.xex 2>/dev/null

# Copy to a Windows-visible path for Xenia
if [ -d /mnt/d/ports/la360/build-xenon ]; then
    cp platform_test.elf32 platform_test.xex /mnt/d/ports/la360/build-xenon/ 2>/dev/null || true
    echo "Copied to D:\\ports\\la360\\build-xenon\\"
fi
