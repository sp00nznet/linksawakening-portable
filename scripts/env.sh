# Sourced by every scripts/build_*.sh. Paths resolve from this file, so the
# scripts work from any checkout: WSL (/mnt/...), or Docker with the repo
# mounted at /src.
#
#   ROOT  this repo
#   LA    LinksAwakening submodule: rom_main.c, and gen/ once you've generated it
#   RT    the gb-recompiled runtime inside it
#   GEN   recompiler output from your own ROM (rom.c, rom_rom.c, rom.h)
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
LA="$ROOT/LinksAwakening"
RT="$LA/gbrecomp/runtime"
GEN="$LA/gen"

# Per-project save locations for the console backends (gb-recompiled#1).
# Values are unchanged from before, so existing saves still load.
LA_DEFS='-DGBRT_SAVE_NAME="linksawakening" -DGBRT_VMU_PREFIX="LADX" -DGBRT_PS3_TITLE_ID="LADX00001"'

if [ ! -f "$RT/src/gbrt.c" ]; then
    echo "LinksAwakening/ submodule is empty. Run: git submodule update --init --recursive" >&2
    exit 1
fi
if [ ! -f "$GEN/rom.c" ]; then
    echo "$GEN/rom.c is missing. Generate it from your ROM first (docs/BUILDING.md, step 1)." >&2
    exit 1
fi

# fresh FILE: FILE exists and is newer than gen/rom.c. The scripts use it to
# skip recompiling rom.c (5-15 minutes) on a rerun.
fresh() { [ -f "$1" ] && [ "$1" -nt "$GEN/rom.c" ]; }
