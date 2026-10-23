"""Installs and removes the analysis engine's Python environments.

Each build gets its own environment in ~/.local/share/spinus/python/<build>/, made by uv with its own
Python 3.12 (in python/interpreters/, so the system's Python version does not matter) and the build's
lock file, every package checked against its pinned hash. An environment counts as installed only once
its marker file is written, after everything else; a half-finished one is wiped on the next install.
"""

from __future__ import annotations

import asyncio
import contextlib
import hashlib
import json
import os
import shutil
import signal
import time
from collections.abc import Awaitable, Callable, Coroutine
from pathlib import Path
from typing import Any

from .builds import Build, Catalog

MARKER = "spinus-build.json"
Progress = Callable[[float, str], Awaitable[None]]

# A little more than the build needs, for uv's own files and Python itself.
SPARE_BYTES = 500_000_000


class InstallError(Exception):
    pass


def lock_hash(build: Build) -> str:
    return hashlib.sha256(build.lock.read_bytes()).hexdigest()


def find_uv() -> str | None:
    found = shutil.which("uv")
    if found:
        return found
    # The one a package brings along (AppImage, Flatpak: <ai>/bin/uv), then the usual user installs.
    bundled = Path(__file__).resolve().parents[2] / "bin" / "uv"
    for candidate in (bundled, Path.home() / ".local/bin/uv", Path.home() / ".cargo/bin/uv"):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return None


def folder_size(folders: list[Path]) -> int:
    """Bytes in some folders, counting hard-linked files once (uv links its cache into environments)."""
    seen: set[tuple[int, int]] = set()
    total = 0
    for folder in folders:
        for top, _dirs, files in os.walk(folder):
            for name in files:
                try:
                    info = os.lstat(os.path.join(top, name))
                except OSError:
                    continue
                if (info.st_dev, info.st_ino) not in seen:
                    seen.add((info.st_dev, info.st_ino))
                    total += info.st_blocks * 512
    return total


class Environments:
    def __init__(self, root: Path, catalog: Catalog) -> None:
        self.root = root
        self.catalog = catalog
        self.installing: str = ""

    def folder(self, build: Build) -> Path:
        return self.root / build.id

    def python(self, build: Build) -> Path:
        return self.folder(build) / "bin" / "python"

    def marker(self, build: Build) -> dict[str, Any] | None:
        file = self.folder(build) / MARKER
        try:
            data: dict[str, Any] = json.loads(file.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None
        return data if self.python(build).exists() else None

    def is_installed(self, build: Build) -> bool:
        return self.marker(build) is not None

    def is_outdated(self, build: Build) -> bool:
        """Installed from an older lock (Spinus was updated since): it still works; Update installs anew."""
        marker = self.marker(build)
        return marker is not None and marker.get("lock") != lock_hash(build)

    def installed(self) -> list[Build]:
        return [b for b in self.catalog.builds if self.is_installed(b)]

    def describe(self, build: Build) -> dict[str, Any]:
        marker = self.marker(build) or {}
        return {
            **build.to_json(),
            "installed": bool(marker),
            "outdated": self.is_outdated(build),
            "installing": self.installing == build.id,
            "folder": str(self.folder(build)),
            "size": int(marker.get("size", 0)),
        }

    async def install(self, build: Build, progress: Progress, uv: str | None = None) -> None:
        uv = uv or find_uv()
        if uv is None:
            raise InstallError("Installing the analysis engine needs uv. Install it with: sudo pacman -S uv")
        if self.installing:
            raise InstallError("Another build is being installed.")
        self.root.mkdir(parents=True, exist_ok=True)
        free = shutil.disk_usage(self.root).free
        if free < build.disk + SPARE_BYTES:
            raise InstallError(
                f"Not enough free disk space: this needs about {gigabytes(build.disk + SPARE_BYTES)}, and "
                f"{self.root} has {gigabytes(free)} free."
            )
        folder, cache = self.folder(build), self.root / ".uv-cache"
        log = self.root / f"{build.id}.log"
        env = {
            **os.environ,
            "UV_CACHE_DIR": str(cache),
            "UV_PYTHON_INSTALL_DIR": str(self.root / "interpreters"),
            "UV_PYTHON_PREFERENCE": "only-managed",
            "UV_NO_CONFIG": "1",
            "UV_NO_PROGRESS": "1",
        }
        self.installing = build.id
        try:
            await asyncio.to_thread(shutil.rmtree, folder, True)
            log.write_text("", encoding="utf-8")
            await progress(0.0, "Getting Python")
            await self.run([uv, "venv", "--python", self.catalog.python, str(folder)], env, log, None)
            await progress(0.01, "Downloading PyTorch")

            async def watch() -> None:
                while True:
                    await asyncio.sleep(2.0)
                    size = await asyncio.to_thread(folder_size, [folder, cache])
                    await progress(min(0.98, 0.01 + 0.97 * size / max(1, build.disk)), "Downloading PyTorch")

            await self.run(
                [uv, "pip", "sync", "--python", str(self.python(build)), "--require-hashes",
                 "--index-url", self.catalog.pypi, "--extra-index-url", build.index, str(build.lock)],
                env, log, watch,
            )  # fmt: skip
            await progress(0.99, "Finishing")
            size = await asyncio.to_thread(folder_size, [folder])
            marker = {"build": build.id, "lock": lock_hash(build), "torch": build.torch, "size": size,
                      "installed": time.strftime("%Y-%m-%d %H:%M:%S")}  # fmt: skip
            (folder / MARKER).write_text(json.dumps(marker, indent=1) + "\n", encoding="utf-8")
        except BaseException:
            await asyncio.to_thread(shutil.rmtree, folder, True)
            raise
        finally:
            self.installing = ""
            await asyncio.to_thread(shutil.rmtree, cache, True)

    @staticmethod
    async def run(
        command: list[str], env: dict[str, str], log: Path, watch: Callable[[], Coroutine[Any, Any, None]] | None
    ) -> None:
        with log.open("a", encoding="utf-8") as out:
            out.write("$ " + " ".join(command) + "\n")
            out.flush()
            process = await asyncio.create_subprocess_exec(
                *command, env=env, stdout=out, stderr=out, start_new_session=True
            )
        watcher = asyncio.create_task(watch()) if watch else None
        try:
            code = await process.wait()
        except asyncio.CancelledError:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGTERM)
            with contextlib.suppress(Exception):
                await asyncio.wait_for(process.wait(), 10)
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            raise
        finally:
            if watcher:
                watcher.cancel()
        if code != 0:
            tail = log.read_text(encoding="utf-8", errors="replace").strip().splitlines()[-6:]
            raise InstallError("The installation failed:\n" + "\n".join(tail) + f"\n(The whole log is in {log}.)")

    async def remove(self, build: Build) -> None:
        if self.installing == build.id:
            raise InstallError("It is being installed: stop that first.")
        await asyncio.to_thread(shutil.rmtree, self.folder(build), True)
        with contextlib.suppress(OSError):
            (self.root / f"{build.id}.log").unlink()
        if not self.installed():
            await asyncio.to_thread(shutil.rmtree, self.root / "interpreters", True)


def gigabytes(size: int) -> str:
    return f"{size / 1e9:.1f} GB"
