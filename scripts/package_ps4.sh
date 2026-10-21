#!/bin/bash
# Package the built eboot.bin into an installable PS4 .pkg.
#
# Run AFTER build_ps4.sh has produced eboot.bin. Expects libssl1.1 to be
# installed (PkgTool.Core's .NET Core 3.0 runtime needs it for pkg_build).
#
# Usage:  bash package_ps4.sh [build-dir]

set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

export OO_PS4_TOOLCHAIN="${OO_PS4_TOOLCHAIN:-$HOME/PS4Toolchain}"
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
# Older toolchains keep these in samples/hello_world; the current one in bin/data.
asset() { for p in "$TC/samples/hello_world/$1" "$TC/bin/data/$2"; do [ -f "$p" ] && { echo "$p"; return; }; done; }
if [ ! -f sce_sys/icon0.png ]; then
    SAMPLE_ICON="$(asset sce_sys/icon0.png icon0.png)"
    [ -n "$SAMPLE_ICON" ] && cp "$SAMPLE_ICON" sce_sys/icon0.png
fi
# right.sprx (license placeholder) — copy from a sample if present
mkdir -p sce_sys/about
SAMPLE_SPRX="$(asset sce_sys/about/right.sprx right.sprx)"
[ -n "$SAMPLE_SPRX" ] && cp "$SAMPLE_SPRX" sce_sys/about/right.sprx
# sce_module: libc.prx + libSceFios2.prx
mkdir -p sce_module
for m in libc.prx libSceFios2.prx; do
    SM="$(asset "sce_module/$m" "modules/$m")"
    [ -n "$SM" ] && cp "$SM" "sce_module/$m"
done

echo '[3/4] create-gp4 project'
GP4FILES="eboot.bin sce_sys/param.sfo sce_sys/icon0.png"
[ -f sce_sys/about/right.sprx ]  && GP4FILES="$GP4FILES sce_sys/about/right.sprx"
[ -f sce_module/libc.prx ]       && GP4FILES="$GP4FILES sce_module/libc.prx"
[ -f sce_module/libSceFios2.prx ] && GP4FILES="$GP4FILES sce_module/libSceFios2.prx"
if [ -x "$CREATEGP4" ]; then
    "$CREATEGP4" -out pkg.gp4 --content-id="$CONTENT_ID" --files "$GP4FILES"
else
    # The current toolchain has no create-gp4; this is the same project file it wrote.
    {
        echo '<?xml version="1.0"?>'
        echo '<psproject fmt="gp4" version="1000">'
        echo '  <volume>'
        echo '    <volume_type>pkg_ps4_app</volume_type>'
        echo '    <volume_id>PS4VOLUME</volume_id>'
        echo "    <volume_ts>$(date -u '+%Y-%m-%d %H:%M:%S')</volume_ts>"
        echo "    <package content_id=\"$CONTENT_ID\" passcode=\"00000000000000000000000000000000\" storage_type=\"digital50\" app_type=\"full\" />"
        echo '    <chunk_info chunk_count="1" scenario_count="1">'
        echo '      <chunks><chunk id="0" layer_no="0" label="Chunk #0" /></chunks>'
        echo '      <scenarios default_id="0"><scenario id="0" type="sp" initial_chunk_count="1" label="Scenario #0">0</scenario></scenarios>'
        echo '    </chunk_info>'
        echo '  </volume>'
        echo '  <files img_no="0">'
        for f in $GP4FILES; do echo "    <file targ_path=\"$f\" orig_path=\"$f\" />"; done
        echo '  </files>'
        echo '  <rootdir><dir targ_name="sce_sys"><dir targ_name="about" /></dir><dir targ_name="sce_module" /></rootdir>'
        echo '</psproject>'
    } > pkg.gp4
fi

echo '[4/4] build .pkg'
"$PKGTOOL" pkg_build pkg.gp4 .

echo
echo '=== output ==='
ls -lh *.pkg 2>/dev/null
PKG=$(ls *.pkg 2>/dev/null | head -1)
if [ -n "$PKG" ]; then
    mkdir -p "$ROOT/build-ps4"
    cp "$PKG" "$ROOT/build-ps4/linksawakening.pkg"
    echo "Copied to $ROOT/build-ps4/linksawakening.pkg"
fi
