#!/bin/bash
# ROM -> C, in the tool container (devkitpro/devkitppc: cmake, ninja, g++,
# python3). Picks your ROM from /rom (a folder mounted read-only: the .gbc, or
# a .zip holding one), copies it to LinksAwakening/rom.gbc, builds the
# recompiler and writes LinksAwakening/gen/. Each part is skipped when done.
#
#   docker run --rm -v <this repo>:/src -v <rom folder>:/rom:ro devkitpro/devkitppc bash /src/scripts/recomp.sh
set -e
LA=/src/LinksAwakening
TOOL=/src/build-tools/gbrecomp

if [ ! -f "$LA/rom.gbc" ]; then
    python3 - "$LA/rom.gbc" <<'PY'
import hashlib, pathlib, sys, zipfile
TESTED = "1c091225688d966928cc74336dbef2e07d12a47c"  # LADX (USA, Europe) (Rev 2)
found = []                                            # (name, bytes)
for p in sorted(pathlib.Path("/rom").iterdir()):
    if p.suffix.lower() == ".gbc":
        found.append((p.name, p.read_bytes()))
    elif p.suffix.lower() == ".zip" and zipfile.is_zipfile(p):
        with zipfile.ZipFile(p) as z:
            found += [(n, z.read(n)) for n in z.namelist() if n.lower().endswith(".gbc")]
if not found:
    sys.exit("Setup stopped: there's no .gbc file (or .zip holding one) in the folder you chose.")
found.sort(key=lambda f: (hashlib.sha1(f[1]).hexdigest() != TESTED, not any(w in f[0].lower() for w in ("awakening", "zelda"))))
name, data = found[0]
print(f"  Using {name}")
if hashlib.sha1(data).hexdigest() != TESTED:
    print("  Warning: not the tested revision, Link's Awakening DX (USA, Europe) (Rev 2). It may not work.")
pathlib.Path(sys.argv[1]).write_bytes(data)
PY
fi

if [ ! -x "$TOOL/bin/gbrecomp" ]; then
    echo '  Building the recompiler'
    cmake -S "$LA/gbrecomp" -B "$TOOL" -G Ninja -DLA_HAS_MULTIPLAYER=OFF >/dev/null
    cmake --build "$TOOL" --target gbrecomp
fi

cd "$LA"
if [ ! -f gen/rom.c ] || [ gen/rom.c -ot rom.gbc ]; then
    "$TOOL/bin/gbrecomp" rom.gbc -o gen | tail -n 3
else
    echo '  gen/ is up to date (skipping)'
fi
