"""Check that launcher/launcher.json's sources match the submodule commits.

The recomp launcher downloads zips, which carry no submodules, so
launcher.json names each submodule's repo and commit itself. This fails when
those drift from the commits git actually pins (after a submodule bump), so
the installer never builds against a different toolkit than a clone does.
Needs the submodules checked out. Run from the repo root.
"""
import json
import subprocess
import sys
from pathlib import Path


def gitlink(repo, path):
    """The commit a submodule at `path` (inside `repo`) is pinned to, or None."""
    out = subprocess.run(["git", "-C", str(repo), "ls-tree", "HEAD", "--", path],
                         capture_output=True, text=True).stdout.split()
    return out[2] if out and out[0] == "160000" else None


def pin(repo, rel):
    parts = rel.split("/")
    for i in range(len(parts), 0, -1):  # deepest submodule that contains rel
        sha = gitlink(repo, "/".join(parts[:i]))
        if sha:
            rest = "/".join(parts[i:])
            return pin(repo / "/".join(parts[:i]), rest) if rest else sha
    return None


m = json.loads(Path("launcher/launcher.json").read_text(encoding="utf-8"))
name = m["repo"].split("/")[1]
bad = 0
for s in m.get("sources", []):
    rel = s["dir"].removeprefix(name + "/")
    want = pin(Path("."), rel)
    ok = want is not None and want.startswith(s["ref"])
    bad += not ok
    print(f"{'ok  ' if ok else 'BAD '} {s['dir']}: launcher.json {s['ref'][:7]}, git {want[:7] if want else 'not a submodule'}")
sys.exit(1 if bad else 0)
