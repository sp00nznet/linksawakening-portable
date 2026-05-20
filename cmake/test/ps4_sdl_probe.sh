#!/bin/bash
TC="$HOME/PS4Toolchain"
echo "=== SDL.h locations ==="
find "$TC/include" -iname 'SDL.h' 2>/dev/null
echo "=== SDL2 subdir? ==="
ls -d "$TC/include/SDL2" 2>/dev/null && ls "$TC/include/SDL2" | head -10
echo "=== c++/v1 present? ==="
ls -d "$TC/include/c++/v1" 2>/dev/null | head -1
echo "=== imgui in our vendored runtime ==="
ls /mnt/d/ports/la360/runtime/third_party/imgui/*.cpp 2>/dev/null
echo "=== orbis headers (libkernel etc) ==="
ls "$TC/include/orbis" 2>/dev/null | head -8
