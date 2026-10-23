#!/usr/bin/env python3
"""Writes packaging/flatpak/python-wheels.json: the wheels the AI service needs in the Flatpak,
pinned by ai/uv.lock (each with its URL and SHA-256), for CPython 3.12 on x86_64 Linux.

Usage: tools/flatpak-python-sources.py   (after changing ai/pyproject.toml or ai/uv.lock)
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TAGS = re.compile(r"-(py3|py2.py3|cp312|cp3\d+-abi3|cp3\d+)-(none|abi3|cp312)-(any|manylinux[^/]*x86_64[^/]*)\.whl$")


def needed() -> dict[str, str]:
    """name -> version, as `uv export` resolves them for Linux (the service and its "listening" and "notes" extras)."""
    text = subprocess.run(
        ["uv", "export", "--frozen", "--no-dev", "--extra", "listening", "--extra", "notes", "--no-emit-project", "--no-hashes",
         "--format", "requirements-txt"],
        cwd=ROOT / "ai", check=True, capture_output=True, text=True).stdout
    found = {}
    for line in text.splitlines():
        match = re.match(r"^([A-Za-z0-9_.-]+)==([^ ;]+)(?: ; (.*))?$", line.strip())
        if match and "win32" not in (match.group(3) or "") and "PyPy" != (match.group(3) or ""):
            found[match.group(1).lower().replace("_", "-")] = match.group(2)
    return found


def main() -> int:
    lock = tomllib.loads((ROOT / "ai/uv.lock").read_text())
    sources = []
    for name, version in sorted(needed().items()):
        package = next((p for p in lock["package"] if p["name"] == name and p["version"] == version), None)
        if package is None:
            print(f"{name} {version} is not in ai/uv.lock", file=sys.stderr)
            return 1
        wheels = [w for w in package.get("wheels", []) if TAGS.search(w["url"].rsplit("/", 1)[-1])]
        # Pure Python first, then the widest-compatible manylinux build.
        wheels.sort(key=lambda w: ("none-any" not in w["url"], "manylinux_2_17" not in w["url"]))
        if not wheels:
            print(f"no wheel for {name} {version} on CPython 3.12 x86_64 Linux", file=sys.stderr)
            return 1
        wheel = wheels[0]
        sources.append({"type": "file", "url": wheel["url"], "sha256": wheel["hash"].removeprefix("sha256:"),
                        "dest": "wheels"})
    out = ROOT / "packaging/flatpak/python-wheels.json"
    out.write_text(json.dumps(sources, indent=2) + "\n")
    print(f"{len(sources)} wheels -> {out.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
