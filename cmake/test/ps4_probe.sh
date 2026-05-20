#!/bin/bash
# Probe the OpenOrbis PS4 toolchain layout + build the hello_world sample
# to validate clang -> ld.lld -> create-fself -> create-gp4 -> PkgTool.
export OO_PS4_TOOLCHAIN="$HOME/PS4Toolchain"

# PkgTool.Core is a .NET app; without an ICU package it aborts. Run it in
# invariant-globalization mode so no system ICU is required.
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

echo "=== OO_PS4_TOOLCHAIN = $OO_PS4_TOOLCHAIN ==="
echo "=== SDL2 header ==="
find "$OO_PS4_TOOLCHAIN/include" -iname 'SDL.h' 2>/dev/null
echo "=== SDL2 include dir listing ==="
ls "$OO_PS4_TOOLCHAIN/include" | head -30
echo "=== samples ==="
ls "$OO_PS4_TOOLCHAIN/samples"
echo
echo "=== build hello_world ==="
cd "$OO_PS4_TOOLCHAIN/samples/hello_world" || { echo "no hello_world dir"; exit 1; }
make clean >/dev/null 2>&1
make 2>&1 | tail -15
echo "=== output ==="
ls -la *.pkg eboot.bin 2>/dev/null
