#!/bin/sh
# Build Link's Awakening for the Original Xbox.
#
# Runs INSIDE the NXDK Docker image (ghcr.io/xboxdev/nxdk), which ships a
# fully-built NXDK (libnxdk, libSDL2, libpbkit, ...). la360 is mounted at
# /src. Reuses the SDL2 backend (platform_sdl.cpp) via NXDK's SDL2 port —
# the same approach as PS4 / Android / WebAssembly. x86 little-endian: the
# recompiled rom.c compiles unchanged.
#
# Run from the host (PowerShell — git-bash mangles the /src mount path):
#   docker pull ghcr.io/xboxdev/nxdk
#   docker run --rm -v D:/ports/la360:/src ghcr.io/xboxdev/nxdk \
#       sh /src/cmake/test/build_xbox.sh
#
# Output: build-xbox/linksawakening.xbe
set -e

cd /src/xbox
make -j"$(nproc)"

echo
echo '=== output ==='
ls -lh /src/xbox/bin/default.xbe /src/xbox/*.iso 2>/dev/null
mkdir -p /src/build-xbox
if [ -f /src/xbox/bin/default.xbe ]; then
    cp /src/xbox/bin/default.xbe /src/build-xbox/linksawakening.xbe
fi
for f in /src/xbox/*.iso; do
    [ -f "$f" ] && cp "$f" /src/build-xbox/linksawakening.iso
done
echo 'Copied to build-xbox/'
