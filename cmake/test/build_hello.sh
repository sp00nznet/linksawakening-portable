#!/bin/bash
# Build the hello_xenon smoke test and produce a Xenia-loadable XEX.
# Run from WSL where libxenon is installed.
#
# Usage:
#   bash /mnt/d/ports/la360/cmake/test/build_hello.sh [output-dir]
#
# Output: <out>/hello.elf, hello.elf32, hello.xex

set -e

DEVKITXENON="${DEVKITXENON:-/usr/local/xenon}"
OUT="${1:-/tmp/la360-hello}"
SRC="$(cd "$(dirname "$0")" && pwd)/hello_xenon.c"

# elf2xex hunt order: $PATH, libxenon toolchain, xbox360nfs vendored copy
ELF2XEX=""
for candidate in \
    "$DEVKITXENON/bin/elf2xex" \
    "$HOME/libxenon/toolchain/bin/elf2xex" \
    "/mnt/c/xbox360nfs/tools/elf2xex" \
    "$(which elf2xex 2>/dev/null)"; do
    if [ -x "$candidate" ]; then
        ELF2XEX="$candidate"; break
    fi
done

mkdir -p "$OUT"
cd "$OUT"

echo "[1/4] Sanity-checking toolchain at $DEVKITXENON"
test -x "$DEVKITXENON/bin/xenon-gcc"            || { echo "missing xenon-gcc"; exit 1; }
test -f "$DEVKITXENON/lib/gcc/xenon/9.2.0/32/libgcc.a" \
    || test -f "$DEVKITXENON/lib/gcc/xenon/9.2.0/libgcc.a" \
    || { echo "missing libgcc.a — run: cd ~/libxenon/toolchain/build && sudo make install-target-libgcc"; exit 1; }
test -f "$DEVKITXENON/app.lds"                  || { echo "missing app.lds"; exit 1; }
test -f "$DEVKITXENON/usr/lib/libxenon.a"       || { echo "missing libxenon.a"; exit 1; }

echo "[2/4] Compile + link → hello.elf"
"$DEVKITXENON/bin/xenon-gcc" \
    -DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float \
    -O2 -Wall \
    -I"$DEVKITXENON/usr/include" \
    -L"$DEVKITXENON/xenon/lib/32" \
    -L"$DEVKITXENON/usr/lib" \
    -T"$DEVKITXENON/app.lds" \
    "$SRC" \
    -lxenon -lm -lc -lgcc \
    -o hello.elf

echo "[3/4] Strip + relocate → hello.elf32"
"$DEVKITXENON/bin/xenon-objcopy" -O elf32-powerpc --adjust-vma 0x80000000 hello.elf hello.elf32
"$DEVKITXENON/bin/xenon-strip" hello.elf32

if [ -n "$ELF2XEX" ]; then
    echo "[4/4] Convert → hello.xex via $ELF2XEX"
    "$ELF2XEX" hello.elf32 hello.xex
    ls -lh hello.elf hello.elf32 hello.xex
    echo ""
    echo "Run in Xenia:"
    echo "  D:\\emu\\xenia-canary\\xenia_canary.exe $(wslpath -w "$OUT/hello.xex" 2>/dev/null || echo "$OUT/hello.xex")"
else
    echo "[4/4] elf2xex not found — skipping XEX conversion (you have hello.elf32)"
    ls -lh hello.elf hello.elf32
fi
