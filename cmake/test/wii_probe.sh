#!/bin/bash
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=/opt/devkitpro/devkitPPC
echo "=== powerpc-eabi-gcc ==="
"$DEVKITPPC/bin/powerpc-eabi-gcc" --version 2>&1 | head -1
echo "=== libogc lib (wii) ==="
ls "$DEVKITPRO/libogc/lib/wii/" 2>/dev/null
echo "=== libogc key headers ==="
ls "$DEVKITPRO/libogc/include/ogcsys.h" "$DEVKITPRO/libogc/include/gccore.h" \
   "$DEVKITPRO/libogc/include/wiiuse/wpad.h" "$DEVKITPRO/libogc/include/asndlib.h" \
   "$DEVKITPRO/libogc/include/fat.h" 2>&1
echo "=== wii_rules ==="
cat "$DEVKITPPC/wii_rules" 2>&1
echo "=== elf2dol ==="
ls "$DEVKITPRO/tools/bin/" | grep -iE 'dol|elf2'
