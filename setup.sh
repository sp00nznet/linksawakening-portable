#!/bin/bash
# Build Link's Awakening DX for every console in scripts/targets.tsv, from
# your ROM, using Docker (Linux and macOS; Windows runs tools/setup.ps1, which
# does the same). Everything happens in containers: nothing but Docker is
# installed on your machine, and your ROM never leaves it.
#
#   ./setup.sh [--yes] [--game-dir <folder with your ROM>] [--targets wii,psp]
#
# Each step prints "[n/N] ..." and a failure ends with "Setup stopped: ...",
# which the recomp launcher shows (launcher/launcher.json). A console that
# fails to build doesn't stop the rest; the summary says which, and its log
# is in build-logs/. Finished work is skipped on a rerun.
# No "set -u": macOS bash 3.2 treats an empty array as unset under it.
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
YES=0 GAME="" ONLY=""
while [ $# -gt 0 ]; do
    case "$1" in
        --yes|-y) YES=1 ;;
        --game-dir|--game) GAME="$2"; shift ;;
        --targets) ONLY=",$2,"; shift ;;
        *) echo "unknown argument: $1"; exit 2 ;;
    esac
    shift
done
mkdir -p build-logs
exec > >(tee setup.log) 2>&1

TARGETS=()
while IFS= read -r line; do
    case "$line" in \#*|"") continue ;; esac
    t="${line%%$'\t'*}"
    [ -z "$ONLY" ] || [[ "$ONLY" == *",$t,"* ]] && TARGETS+=("$line")
done < scripts/targets.tsv
N=$((3 + ${#TARGETS[@]}))

step() { echo; echo "[$1/$N] $2"; }
fail() { echo; echo "Setup stopped: $1"; echo "Details are in $ROOT/setup.log"; exit 1; }
# Containers run as root; give what they wrote back to you (Linux).
mine() { [ "$(uname)" = Linux ] && docker run --rm -v "$ROOT:/src" alpine chown -R "$(id -u):$(id -g)" "${@/#//src/}" >/dev/null 2>&1; true; }

step 1 "Checking Docker"
command -v docker >/dev/null || fail "Docker isn't installed. Install it (https://docs.docker.com/get-docker/), start it, and run this again."
docker info >/dev/null 2>&1 || fail "Docker isn't running. Start it and run this again."
echo "  Docker $(docker version --format '{{.Server.Version}}')"

step 2 "Checking the source"
if [ ! -f LinksAwakening/gbrecomp/runtime/src/gbrt.c ]; then
    [ -d .git ] || fail "LinksAwakening/ is empty and this isn't a git clone. Clone with --recurse-submodules."
    git -c core.longpaths=true submodule update --init --recursive || fail "fetching the LinksAwakening submodule failed."
fi
echo "  LinksAwakening and gb-recompiled are in place."

step 3 "Turning your ROM into C"
ROMDIR=()
if [ ! -f LinksAwakening/rom.gbc ]; then
    if [ -z "$GAME" ]; then
        [ $YES = 1 ] && fail "no ROM given. Pass the folder it's in with --game-dir."
        read -r -p "Folder (or file) with your Link's Awakening DX ROM (.gbc or .zip): " GAME
    fi
    [ -e "$GAME" ] || fail "there's nothing at '$GAME'."
    [ -f "$GAME" ] && GAME="$(dirname "$GAME")"
    ROMDIR=(-v "$(cd "$GAME" && pwd):/rom:ro")
fi
out="$(docker run --rm -u 0 -v "$ROOT:/src" "${ROMDIR[@]}" devkitpro/devkitppc bash /src/scripts/recomp.sh 2>&1)"
code=$?
# Indented: a tool's own "[3/12] ..." line must not look like one of our steps.
echo "$out" | tail -n 8 | sed 's/^/  /'
mine LinksAwakening/gen LinksAwakening/rom.gbc build-tools
if [ $code -ne 0 ]; then
    why="$(echo "$out" | sed -n 's/.*Setup stopped: //p' | tail -n 1)"
    fail "${why:-recompiling the ROM failed (the lines above say why).}"
fi

n=3; ok=(); bad=()
for line in "${TARGETS[@]}"; do
    IFS=$'\t' read -r t image cmd files state <<<"$line"
    n=$((n + 1))
    step $n "$t: building (5 to 15 minutes; the first run also downloads its toolchain)"
    if [ "$image" = linksawakening-android ]; then
        docker build -q -t "$image" -f docker/android.Dockerfile docker >/dev/null ||
            { echo "  Building the Android toolchain image failed."; bad+=("$t"); continue; }
    fi
    if docker run --rm -u 0 -v "$ROOT:/src" -w /src "$image" sh -c "$cmd" > "build-logs/$t.log" 2>&1; then
        mkdir -p "builds/$t"
        for f in $files; do
            for p in build-$t/$f; do
                # rm first: a copy a container left there is root's (Linux); the folder is ours
                d="builds/$t/$(basename "$p" | sed 's/^la360/linksawakening/')"
                [ -f "$p" ] && rm -f "$d" && cp "$p" "$d"
            done
        done
        ok+=("$t"); echo "  Built. $state."
    else
        bad+=("$t"); echo "  Failed. The last lines of build-logs/$t.log:"; tail -n 5 "build-logs/$t.log" | sed 's/^/    /'
    fi
    mine "build-$t" builds build-logs
done

{
    echo "Link's Awakening DX console builds ($(date +%Y-%m-%d)). How to install each: docs/BUILDING.md."
    echo
    for line in "${TARGETS[@]}"; do
        IFS=$'\t' read -r t image cmd files state <<<"$line"
        if [[ " ${ok[*]} " == *" $t "* ]]; then echo "  $t: built. $state."; else echo "  $t: FAILED (build-logs/$t.log)"; fi
    done
} > builds/README.txt 2>/dev/null || true

echo; cat builds/README.txt 2>/dev/null
[ ${#ok[@]} -gt 0 ] || fail "no console built. The logs in build-logs/ say why."

launcher="Console builds (recomp).sh"
[ "$(uname)" = Darwin ] && launcher="Console builds (recomp).command"
printf '#!/bin/sh\n# Opens the folder with the console builds.\ncd "$(dirname "$0")/builds" && { open . 2>/dev/null || xdg-open .; }\n' > "$launcher"
chmod +x "$launcher"
echo
echo "Done: ${#ok[@]} of ${#TARGETS[@]} consoles built${bad:+ (failed: ${bad[*]})}. They're in builds/."
