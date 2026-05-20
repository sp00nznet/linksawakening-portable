#!/bin/bash
# Verify the devkitARM / libctru 3DS toolchain.
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM
echo "=== arm-none-eabi-gcc ==="
"$DEVKITARM/bin/arm-none-eabi-gcc" --version 2>&1 | head -1
echo "=== libctru ==="
ls -la "$DEVKITPRO/libctru/lib/libctru.a" 2>&1
echo "=== 3ds tools ==="
ls "$DEVKITPRO/tools/bin" | grep -iE '3dsx|smdh|bannertool|3dslink' 2>&1
echo "=== libctru headers ==="
ls "$DEVKITPRO/libctru/include/3ds.h" 2>&1
echo "=== ctru specs/rules ==="
ls "$DEVKITPRO/devkitARM/3ds_rules" 2>&1
ls "$DEVKITARM"/*.specs 2>&1 | head -3
