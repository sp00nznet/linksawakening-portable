#!/bin/bash
# Variant of build_hello.sh that links at 0x82000000 (Xbox 360 retail load
# address) instead of 0x80000000 (libxenon's RGH/JTAG convention) so the
# output can be loaded directly by Xenia as ELF32.
#
# Use: bash build_hello_xenia.sh [output-dir]

set -e

DEVKITXENON="${DEVKITXENON:-/usr/local/xenon}"
OUT="${1:-/tmp/la360-hello-xenia}"
SRC="$(cd "$(dirname "$0")" && pwd)/hello_xenon.c"

# Patch app.lds: replace the 0x80000000 base with 0x82000000, shift heap+stack.
mkdir -p "$OUT"
sed -e 's|0x80000000|0x82000000|' \
    -e 's|0x9E000000|0xA0000000|' \
    -e 's|0xa0000000|0xA2000000|' \
    "$DEVKITXENON/app.lds" > "$OUT/xenia.lds"

cd "$OUT"
echo '--- patched linker script load addrs:'
grep -E '\. = 0x' xenia.lds

echo '--- compile + link (target: 0x82000000)'
"$DEVKITXENON/bin/xenon-gcc" \
    -DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float -O2 -Wall \
    -I"$DEVKITXENON/usr/include" \
    -L"$DEVKITXENON/xenon/lib/32" \
    -L"$DEVKITXENON/usr/lib" \
    -T"$OUT/xenia.lds" \
    "$SRC" \
    -lxenon -lm -lc -lgcc \
    -o hello-xenia.elf

echo '--- strip + relocate to ELF32 (no VMA shift needed — already at 0x82000000)'
"$DEVKITXENON/bin/xenon-objcopy" -O elf32-powerpc hello-xenia.elf hello-xenia.elf32
"$DEVKITXENON/bin/xenon-strip" hello-xenia.elf32

echo '--- LOAD segments (should all be in 0x82000000 range):'
"$DEVKITXENON/bin/xenon-readelf" -l hello-xenia.elf32 | grep -E 'LOAD|VirtAddr' | head -10

echo '--- wrap in XEX via elf2xex'
ELF2XEX="/mnt/c/xbox360nfs/tools/elf2xex"
if [ -x "$ELF2XEX" ]; then
    "$ELF2XEX" hello-xenia.elf32 hello-xenia.xex
fi

ls -lh hello-xenia.elf32 hello-xenia.xex 2>/dev/null
