"""Gathers a Windows build of Spinus into the folder the installer is made from (packaging/windows/spinus.nsi).

    python packaging/windows/stage.py --build build/windows --out build/windows-stage [--cache DIR]

Runs on Linux (after the clang-cl cross-build) and on Windows (GitHub Actions) alike. <out>/app gets what is
installed:

    spinus.exe, spinus-plugin-scanner.exe, spinus-plugin-host.exe
    ai/spinus_ai, ai/run_spinus_ai.py, ai/docs/user, ai/commands.schema.json   (as cmake/SpinusInstall.cmake)
    ai/python/       the Windows embeddable Python (PSF licence), pinned, with the service's packages from
                     ai/uv.lock (Windows wheels, hashes checked) in ai/python/Lib/site-packages
    ai/bin/uv.exe    uv (MIT or Apache-2.0), pinned: installs the analysis engine when the user asks
    demo/<Name>/            the demo songs (as cmake/SpinusInstall.cmake's share/spinus/demo)
    samples/Spinus Starter Pack/   the starter sample pack (as share/spinus/samples)
    LICENSE.txt, THIRD_PARTY.md, docs/   (the manual)

Spinus finds ai/python/python.exe by itself (AiService::findInstallation). Beside app/, the installer's
own files: spinus.ico and version.nsh (the version, from the build).
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import urllib.request
import zipfile
from pathlib import Path

PYTHON_VERSION = "3.12.10"  # the last 3.12 release with Windows binaries
PYTHON_URL = f"https://www.python.org/ftp/python/{PYTHON_VERSION}/python-{PYTHON_VERSION}-embed-amd64.zip"
PYTHON_SHA256 = "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3"
# The same uv as the Flatpak's.
UV_URL = "https://github.com/astral-sh/uv/releases/download/0.12.24/uv-x86_64-pc-windows-msvc.zip"
UV_SHA256 = "7c38608c8a18ee137d748a1773053b07ec8f3a30fab49aebaa6f4e4efeceb019"
PROGRAMS = ["spinus.exe", "spinus-plugin-scanner.exe", "spinus-plugin-host.exe"]

source = Path(__file__).resolve().parents[2]


def fetch(url: str, sha256: str, cache: Path) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / url.rsplit("/", 1)[1]
    if not target.exists() or hashlib.sha256(target.read_bytes()).hexdigest() != sha256:
        print(f"Downloading {url}")
        partial = target.with_suffix(".partial")
        with urllib.request.urlopen(url) as response, open(partial, "wb") as out:
            shutil.copyfileobj(response, out)
        partial.replace(target)
    if hashlib.sha256(target.read_bytes()).hexdigest() != sha256:
        raise SystemExit(f"{target.name} is not the expected file (its checksum differs)")
    return target


def stage_uv(ai: Path, cache: Path) -> None:
    with zipfile.ZipFile(fetch(UV_URL, UV_SHA256, cache)) as archive:
        name = next(n for n in archive.namelist() if n.rsplit("/", 1)[-1] == "uv.exe")
        (ai / "bin").mkdir(parents=True, exist_ok=True)
        (ai / "bin" / "uv.exe").write_bytes(archive.read(name))


def stage_python(ai: Path, cache: Path) -> None:
    python = ai / "python"
    with zipfile.ZipFile(fetch(PYTHON_URL, PYTHON_SHA256, cache)) as archive:
        archive.extractall(python)
    # The embeddable Python reads its search path only from this file: its own library, the packages
    # beside it, and the ai folder (spinus_ai and run_spinus_ai.py). "import site" stays off.
    pth = next(python.glob("python3*._pth"))
    pth.write_text(f"{pth.stem}.zip\n.\nLib\\site-packages\n..\n")

    requirements = cache / "requirements.txt"
    exported = subprocess.run(
        ["uv", "export", "--frozen", "--no-dev", "--extra", "listening", "--extra", "notes", "--no-emit-project",
         "--format", "requirements-txt"],
        cwd=source / "ai", check=True, capture_output=True, text=True,
    ).stdout  # fmt: skip
    requirements.write_text(exported)
    subprocess.run(
        ["uv", "pip", "install", "--target", str(python / "Lib" / "site-packages"),
         "--python-platform", "x86_64-pc-windows-msvc", "--python-version", PYTHON_VERSION,
         "--only-binary", ":all:", "--require-hashes", "-r", str(requirements)],
        check=True,
    )  # fmt: skip


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True, help="the build folder with spinus.exe")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cache", type=Path, help="downloads (default: <out>.cache)")
    args = parser.parse_args()
    out: Path = args.out
    cache: Path = args.cache or out.with_name(out.name + ".cache")

    if out.exists():
        shutil.rmtree(out)
    app = out / "app"
    app.mkdir(parents=True)
    for program in PROGRAMS:
        found = next(args.build.rglob(program), None)
        if found is None:
            raise SystemExit(f"{program} is not in {args.build}: build Spinus first")
        shutil.copy2(found, app / program)

    # The installer's icon and version, as the build made them.
    shutil.copy2(next(args.build.rglob("JuceLibraryCode/icon.ico")), out / "spinus.ico")
    header = next(args.build.rglob("generated/SpinusVersion.h")).read_text()
    version = re.search(r'versionString = "([^"]+)"', header)
    if version is None:
        raise SystemExit("No version in SpinusVersion.h")
    # Windows wants four numbers in a file's version ("0.1.0-alpha.1" -> 0.1.0.0).
    numbers = (re.findall(r"\d+", version[1].split("-")[0]) + ["0"] * 4)[:4]
    (out / "version.nsh").write_text(f'!define VERSION "{version[1]}"\n!define NUMERIC_VERSION "{".".join(numbers)}"\n')

    ai = app / "ai"
    shutil.copytree(source / "ai" / "spinus_ai", ai / "spinus_ai", ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copy2(source / "ai" / "run_spinus_ai.py", ai)
    shutil.copytree(source / "docs" / "user", ai / "docs" / "user")
    shutil.copy2(source / "docs" / "dev" / "commands.schema.json", ai)
    stage_python(ai, cache)
    stage_uv(ai, cache)

    # The demo songs: every project folder in demo/ (src/session/DemoSong.h).
    for song in sorted((source / "demo").iterdir()):
        if song.is_dir() and any(song.glob("*.spn")):
            shutil.copytree(song, app / "demo" / song.name, ignore=shutil.ignore_patterns(".autosave", ".session.lock"))
    # The starter sample pack (src/dsp/drums/StarterPack.h).
    shutil.copytree(source / "samples", app / "samples")
    shutil.copy2(source / "LICENSE", app / "LICENSE.txt")
    shutil.copy2(source / "THIRD_PARTY.md", app)
    shutil.copytree(source / "docs" / "user", app / "docs")
    print(f"Staged Spinus {version[1]} in {out}")


if __name__ == "__main__":
    main()
