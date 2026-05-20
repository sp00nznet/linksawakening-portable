#!/bin/bash
# Package the built eboot.bin into an installable PS4 .pkg.
#
# Run AFTER build_ps4.sh has produced eboot.bin. Expects libssl1.1 to be
# installed (PkgTool.Core's .NET Core 3.0 runtime needs it for pkg_build).
#
# Usage:  bash package_ps4.sh [build-dir]

set -e

export OO_PS4_TOOLCHAIN="$HOME/PS4Toolchain"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1   # PkgTool .NET has no ICU
TC="$OO_PS4_TOOLCHAIN"
PKGTOOL="$TC/bin/linux/PkgTool.Core"
CREATEGP4="$TC/bin/linux/create-gp4"
OUT="${1:-$HOME/la360-ps4}"   # match build_ps4.sh — persistent path

# Package metadata
TITLE="Link's Awakening DX"
TITLE_ID="LADX00001"
CONTENT_ID="IV0000-${TITLE_ID}_00-LINKSAWAKENINGDX"
VERSION="1.00"

cd "$OUT"
if [ ! -f eboot.bin ]; then
    echo "ERROR: eboot.bin not found in $OUT — run build_ps4.sh first."
    exit 1
fi

echo '[1/4] sce_sys metadata (param.sfo)'
mkdir -p sce_sys
"$PKGTOOL" sfo_new sce_sys/param.sfo
"$PKGTOOL" sfo_setentry sce_sys/param.sfo APP_TYPE   --type Integer --maxsize 4   --value 1
"$PKGTOOL" sfo_setentry sce_sys/param.sfo APP_VER    --type Utf8    --maxsize 8   --value "$VERSION"
"$PKGTOOL" sfo_setentry sce_sys/param.sfo ATTRIBUTE  --type Integer --maxsize 4   --value 0
"$PKGTOOL" sfo_setentry sce_sys/param.sfo CATEGORY   --type Utf8    --maxsize 4   --value "gd"
"$PKGTOOL" sfo_setentry sce_sys/param.sfo CONTENT_ID --type Utf8    --maxsize 48  --value "$CONTENT_ID"
"$PKGTOOL" sfo_setentry sce_sys/param.sfo DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
"$PKGTOOL" sfo_setentry sce_sys/param.sfo SYSTEM_VER --type Integer --maxsize 4   --value 0
"$PKGTOOL" sfo_setentry sce_sys/param.sfo TITLE      --type Utf8    --maxsize 128 --value "$TITLE"
"$PKGTOOL" sfo_setentry sce_sys/param.sfo TITLE_ID   --type Utf8    --maxsize 12  --value "$TITLE_ID"
"$PKGTOOL" sfo_setentry sce_sys/param.sfo VERSION    --type Utf8    --maxsize 8   --value "$VERSION"

echo '[2/4] required sce_sys assets'
# An icon is required. Reuse the OpenOrbis sample icon as a placeholder
# (swap in real LA artwork later).
if [ ! -f sce_sys/icon0.png ]; then
    SAMPLE_ICON="$TC/samples/hello_world/sce_sys/icon0.png"
    [ -f "$SAMPLE_ICON" ] && cp "$SAMPLE_ICON" sce_sys/icon0.png
fi
# right.sprx (license placeholder) — copy from a sample if present
mkdir -p sce_sys/about
SAMPLE_SPRX="$TC/samples/hello_world/sce_sys/about/right.sprx"
[ -f "$SAMPLE_SPRX" ] && cp "$SAMPLE_SPRX" sce_sys/about/right.sprx
# sce_module: libc.prx + libSceFios2.prx
mkdir -p sce_module
for m in libc.prx libSceFios2.prx; do
    SM="$TC/samples/hello_world/sce_module/$m"
    [ -f "$SM" ] && cp "$SM" "sce_module/$m"
done

echo '[3/4] create-gp4 project'
GP4FILES="eboot.bin sce_sys/param.sfo sce_sys/icon0.png"
[ -f sce_sys/about/right.sprx ]  && GP4FILES="$GP4FILES sce_sys/about/right.sprx"
[ -f sce_module/libc.prx ]       && GP4FILES="$GP4FILES sce_module/libc.prx"
[ -f sce_module/libSceFios2.prx ] && GP4FILES="$GP4FILES sce_module/libSceFios2.prx"
"$CREATEGP4" -out pkg.gp4 --content-id="$CONTENT_ID" --files "$GP4FILES"

echo '[4/4] build .pkg'
"$PKGTOOL" pkg_build pkg.gp4 .

echo
echo '=== output ==='
ls -lh *.pkg 2>/dev/null
PKG=$(ls *.pkg 2>/dev/null | head -1)
if [ -n "$PKG" ] && [ -d /mnt/d/ports/la360/build-ps4 ]; then
    cp "$PKG" /mnt/d/ports/la360/build-ps4/linksawakening.pkg
    echo "Copied to D:\\ports\\la360\\build-ps4\\linksawakening.pkg"
fi
