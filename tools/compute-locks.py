#!/usr/bin/env python3
"""Writes the pinned PyTorch builds of the analysis engine.

For every build (CPU, Nvidia CUDA 13 and 12, Intel XPU, AMD TheRock per GPU target) it resolves
ai/compute/requirements.in plus that build's torch with `uv pip compile --generate-hashes` into
ai/spinus_ai/compute/locks/<build>.txt, and records in ai/spinus_ai/compute/builds.json how much each
build downloads and takes on disk (read from the wheels' zip directories, without downloading them).

Run it after changing a version below or requirements.in, then re-run the owner checks
(tools/owner-checks/gpu.sh). Needs uv and internet:

    tools/compute-locks.py            # every build
    tools/compute-locks.py cpu rocm-gfx1151
"""

from __future__ import annotations

import json
import os
import re
import signal
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
REQUIREMENTS = ROOT / "ai" / "compute" / "requirements.in"
OUT = ROOT / "ai" / "spinus_ai" / "compute"
PYTHON = "3.12"
PLATFORM = "x86_64-manylinux_2_28"
PYPI = "https://pypi.org/simple"

TORCH = "2.14.0"
PYTORCH_ORG = "https://download.pytorch.org/whl"
THEROCK = "https://stable.repo.amd.com/rocm/whl-next/"
ROCM = "rocm10.1.0"

# GPU targets TheRock's stable channel builds torch for (its amd-torch-device-<target> packages), without
# the data-centre Instinct cards (gfx908 to gfx950), whose libraries alone are 1.8 GB.
ROCM_TARGETS = [
    "gfx1010", "gfx1011", "gfx1012",
    "gfx1030", "gfx1031", "gfx1032", "gfx1033", "gfx1034", "gfx1035", "gfx1036",
    "gfx1100", "gfx1101", "gfx1102", "gfx1103",
    "gfx1150", "gfx1151", "gfx1152", "gfx1153",
    "gfx1200", "gfx1201",
]  # fmt: skip

# Builds tested on real hardware; the rest say "untested".
TESTED = {"cpu", "rocm-gfx1151"}


def builds() -> list[dict[str, str]]:
    found = [
        {"id": "cpu", "vendor": "cpu", "label": "CPU only", "torch": f"torch=={TORCH}+cpu",
         "index": f"{PYTORCH_ORG}/cpu"},
        {"id": "cuda-13", "vendor": "nvidia", "label": "Nvidia (CUDA 13)", "torch": f"torch=={TORCH}+cu130",
         "index": f"{PYTORCH_ORG}/cu130"},
        {"id": "cuda-12", "vendor": "nvidia", "label": "Nvidia (CUDA 12, older cards and drivers)",
         "torch": f"torch=={TORCH}+cu126", "index": f"{PYTORCH_ORG}/cu126"},
        {"id": "xpu", "vendor": "intel", "label": "Intel (XPU)", "torch": f"torch=={TORCH}+xpu",
         "index": f"{PYTORCH_ORG}/xpu"},
    ]  # fmt: skip
    for target in ROCM_TARGETS:
        found.append(
            {"id": f"rocm-{target}", "vendor": "amd", "label": f"AMD ({target})",
             "torch": f"torch[device-{target}]=={TORCH}+{ROCM}", "index": THEROCK, "target": target}
        )  # fmt: skip
    return found


def compile_lock(build: dict[str, str], lock: Path, cache: Path) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        wanted = Path(tmp) / "requirements.in"
        wanted.write_text(build["torch"] + "\n" + REQUIREMENTS.read_text(encoding="utf-8"), encoding="utf-8")
        subprocess.run(
            ["uv", "pip", "compile", "-q", str(wanted), "--python-version", PYTHON, "--python-platform", PLATFORM,
             "--generate-hashes", "--no-header", "--no-annotate", "--index-url", PYPI,
             "--extra-index-url", build["index"], "-o", str(lock)],
            check=True, env={**os.environ, "UV_CACHE_DIR": str(cache)},
        )  # fmt: skip


def pins(lock: Path) -> list[tuple[str, str, set[str]]]:
    """(name, version, sha256 hashes) of every package in a lock."""
    found: list[tuple[str, str, set[str]]] = []
    for block in re.split(r"\n(?=\S)", lock.read_text(encoding="utf-8")):
        match = re.match(r"([A-Za-z0-9_.\-]+)(?:\[[^\]]*\])?==(\S+)", block)
        if match:
            found.append((match[1], match[2], set(re.findall(r"sha256:([0-9a-f]{64})", block))))
    return found


def open_url(request: urllib.request.Request, timeout: int) -> Any:
    """urlopen, tried again after a dropped connection."""
    for attempt in range(5):
        try:
            return urllib.request.urlopen(request, timeout=timeout)
        except urllib.error.HTTPError:
            raise
        except OSError:
            if attempt == 4:
                raise
            time.sleep(2 + 4 * attempt)
    raise AssertionError


def index_files(index: str, name: str) -> list[dict[str, object]]:
    url = index.rstrip("/") + "/" + re.sub(r"[-_.]+", "-", name).lower() + "/"
    request = urllib.request.Request(url, headers={"Accept": "application/vnd.pypi.simple.v1+json"})
    try:
        with open_url(request, 60) as reply:
            body = reply.read().decode()
            final_url = reply.url
            kind = reply.headers.get("Content-Type", "")
    except OSError:
        return []
    if "json" in kind:
        files: list[dict[str, object]] = json.loads(body)["files"]
        return files
    # An HTML index: <a href="url#sha256=...">file</a> (TheRock's links carry no hash).
    found = []
    for href, file in re.findall(r'href="([^"]+)"[^>]*>([^<]+)<', body):
        sha = re.search(r"#sha256=([0-9a-f]{64})", href)
        found.append(
            {"filename": file, "url": urllib.parse.urljoin(final_url, href.split("#")[0]),
             "hashes": {"sha256": sha[1]} if sha else {}}
        )  # fmt: skip
    return found


def fetch_range(url: str, start: int, end: int) -> bytes:
    request = urllib.request.Request(url, headers={"Range": f"bytes={start}-{end}"})
    with open_url(request, 120) as reply:
        data: bytes = reply.read()
        return data


def file_length(url: str) -> int:
    request = urllib.request.Request(url, method="HEAD")
    with open_url(request, 60) as reply:
        return int(reply.headers["Content-Length"])


def unpacked_size(url: str, length: int) -> int:
    """The total uncompressed size of a wheel, from its zip directory (two small range requests)."""
    tail = fetch_range(url, max(0, length - 65536), length - 1)
    end = tail.rfind(b"PK\x05\x06")
    entries, directory_size, directory_offset = struct.unpack("<HII", tail[end + 10 : end + 20])
    locator = tail.rfind(b"PK\x06\x07", 0, end)
    if locator >= 0:  # zip64
        (zip64_end,) = struct.unpack("<Q", tail[locator + 8 : locator + 16])
        record = fetch_range(url, zip64_end, zip64_end + 55)
        entries, directory_size, directory_offset = struct.unpack("<QQQ", record[32:56])
    directory = fetch_range(url, directory_offset, directory_offset + directory_size - 1)
    total, at = 0, 0
    for _ in range(entries):
        _compressed, size, name_length, extra_length, comment_length = struct.unpack(
            "<IIHHH", directory[at + 20 : at + 34]
        )
        if size == 0xFFFFFFFF:  # the real size is in the zip64 extra field
            extra = directory[at + 46 + name_length : at + 46 + name_length + extra_length]
            pos = 0
            while pos + 4 <= len(extra):
                tag, length_of = struct.unpack("<HH", extra[pos : pos + 4])
                if tag == 1:
                    (size,) = struct.unpack("<Q", extra[pos + 4 : pos + 12])
                    break
                pos += 4 + length_of
        total += size
        at += 46 + name_length + extra_length + comment_length
    return total


def compatible(filename: str) -> bool:
    if filename.endswith(".tar.gz"):  # a source package (TheRock's "rocm"), built when installed
        return True
    if not filename.endswith(".whl"):
        return False
    tags = filename[:-4].split("-")[-3:]
    return (tags[0] in ("py3", "py2.py3", "cp312") or tags[1] == "abi3") and (tags[2] == "any" or "x86_64" in tags[2])


def of_version(filename: str, name: str, version: str) -> bool:
    """Whether a file (wheel or source package) is this version of this package."""
    for ending in (".tar.gz", ".whl"):
        if filename.endswith(ending):
            head = filename[: -len(ending)]
            break
    else:
        return False
    marker = f"-{version}"
    at = head.find(marker + "-") if ending == ".whl" else (len(head) - len(marker) if head.endswith(marker) else -1)
    return at > 0 and re.sub(r"[-_.]+", "-", head[:at]).lower() == re.sub(r"[-_.]+", "-", name).lower()


def sizes(build: dict[str, str], lock: Path) -> tuple[int, int]:
    download = disk = 0
    for name, version, hashes in pins(lock):
        for index in (build["index"], PYPI):
            files = [f for f in index_files(index, name) if compatible(str(f["filename"]))]
            chosen = next((f for f in files if dict(f["hashes"]).get("sha256") in hashes), None)  # type: ignore[call-overload]
            if chosen is None:  # an index without hashes: the file of this version
                named = [f for f in files if of_version(urllib.parse.unquote(str(f["filename"])), name, version)]
                chosen = next((f for f in named if str(f["filename"]).endswith(".whl")), next(iter(named), None))
            if chosen is not None:
                break
        else:
            raise SystemExit(f"{build['id']}: no wheel found for {name}=={version}")
        url = str(chosen["url"])
        length = file_length(url)
        download += length
        disk += unpacked_size(url, length) if url.endswith(".whl") else length
        print(f"  {name}=={version}: {length / 1e6:.0f} MB", file=sys.stderr)
    return download, disk


def main() -> None:
    # Stopped (kill): leave through SystemExit, so the temporary download cache is deleted.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    chosen = set(sys.argv[1:])
    catalog_file = OUT / "builds.json"
    old = json.loads(catalog_file.read_text(encoding="utf-8"))["builds"] if catalog_file.exists() else []
    known: dict[str, Any] = {b["id"]: b for b in old}
    (OUT / "locks").mkdir(parents=True, exist_ok=True)
    # uv downloads every TheRock wheel to hash it (that index lists no hashes): keep those gigabytes out
    # of the user's uv cache.
    with tempfile.TemporaryDirectory(prefix="spinus-locks-") as cache:
        for build in builds():
            if chosen and build["id"] not in chosen:
                continue
            make(build, Path(cache), known, catalog_file)
    for stale in set(known) - {b["id"] for b in builds()}:
        del known[stale]
        (OUT / "locks" / f"{stale}.txt").unlink(missing_ok=True)
    save(catalog_file, known)


def make(build: dict[str, str], cache: Path, known: dict[str, Any], catalog_file: Path) -> None:
    print(f"{build['id']}:", file=sys.stderr)
    lock = OUT / "locks" / f"{build['id']}.txt"
    compile_lock(build, lock, cache)
    download, disk = sizes(build, lock)
    known[build["id"]] = {**build, "download": download, "disk": disk, "tested": build["id"] in TESTED}
    save(catalog_file, known)


def save(catalog_file: Path, known: dict[str, Any]) -> None:
    order = [b["id"] for b in builds()]
    catalog = {
        "python": PYTHON,
        "pypi": PYPI,
        "builds": sorted(known.values(), key=lambda b: order.index(b["id"]) if b["id"] in order else len(order)),
    }
    catalog_file.write_text(json.dumps(catalog, indent=1) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
