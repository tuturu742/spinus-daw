"""The PyTorch builds the analysis engine can install, and which one suits this computer.

builds.json and locks/ are written by tools/compute-locks.py: every build pins torch and its other
packages by hash, for Python 3.12 on x86-64 Linux; builds-windows.json and locks/windows/ the same for
Windows (without AMD's builds).
"""

from __future__ import annotations

import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .devices import AMD_ON_WINDOWS, INTEL_XPU_PREFIXES, Gpu

HERE = Path(__file__).resolve().parent
CATALOG = HERE / ("builds-windows.json" if sys.platform == "win32" else "builds.json")

# Nvidia: CUDA 13 needs driver 580 and a Turing or newer card (compute capability 7.5); CUDA 12.6 runs
# on driver 525 and cards from Maxwell (5.0) to Hopper, but not on Blackwell (12.0).
CUDA_13_DRIVER, CUDA_12_DRIVER = 580, 525


@dataclass(frozen=True)
class Build:
    id: str
    vendor: str  # "cpu", "amd", "nvidia" or "intel"
    label: str
    index: str
    torch: str
    download: int  # bytes
    disk: int  # bytes once installed
    tested: bool
    target: str = ""  # AMD: the GPU target the build is for
    locks: str = "locks"  # the lock's folder in compute/

    @property
    def lock(self) -> Path:
        return HERE / self.locks / f"{self.id}.txt"

    def to_json(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "vendor": self.vendor,
            "label": self.label,
            "download": self.download,
            "disk": self.disk,
            "tested": self.tested,
        }


@dataclass(frozen=True)
class Catalog:
    python: str
    pypi: str
    builds: tuple[Build, ...]

    @staticmethod
    def load(file: Path = CATALOG) -> Catalog:
        data = json.loads(file.read_text(encoding="utf-8"))
        fields = Build.__dataclass_fields__
        return Catalog(
            str(data["python"]),
            str(data["pypi"]),
            tuple(Build(**{k: v for k, v in b.items() if k in fields}) for b in data["builds"]),
        )

    def find(self, build_id: str) -> Build | None:
        return next((b for b in self.builds if b.id == build_id), None)


@dataclass
class Choice:
    """The build suggested for this computer, and which GPU it is for."""

    build: str = "cpu"
    gpu: Gpu | None = None
    gpu_index: int = 0  # among the GPUs of its vendor, for HIP_VISIBLE_DEVICES / CUDA_VISIBLE_DEVICES
    reason: str = ""  # why there is no GPU build, when there is a GPU


def major(version: str) -> int:
    match = re.match(r"(\d+)", version)
    return int(match[1]) if match else 0


def capability(text: str) -> float:
    try:
        return float(text)
    except ValueError:
        return 0.0


def nvidia_build(gpu: Gpu) -> tuple[str, str]:
    driver, cc = major(gpu.driver), capability(gpu.target)
    if driver == 0:
        return "", "The Nvidia driver's version could not be read."
    if driver >= CUDA_13_DRIVER and (cc == 0 or cc >= 7.5):
        return "cuda-13", ""
    if cc >= 12.0:
        return "", f"This card needs Nvidia driver {CUDA_13_DRIVER} or newer (you have {gpu.driver})."
    if driver >= CUDA_12_DRIVER and (cc == 0 or cc >= 5.0):
        return "cuda-12", ""
    if driver < CUDA_12_DRIVER:
        return "", f"PyTorch needs Nvidia driver {CUDA_12_DRIVER} or newer (you have {gpu.driver})."
    return "", "PyTorch no longer supports this Nvidia card."


def gpu_build(gpu: Gpu, catalog: Catalog) -> tuple[str, str]:
    """The build for one GPU, or why there is none."""
    if gpu.vendor == "nvidia":
        build, reason = nvidia_build(gpu)
    elif gpu.vendor == "amd":
        build = f"rocm-{gpu.target}" if gpu.target else ""
        reason = f"AMD's PyTorch has no build for this GPU ({gpu.target})." if gpu.target else AMD_ON_WINDOWS
    else:
        supported = gpu.target.startswith(INTEL_XPU_PREFIXES)
        build, reason = ("xpu", "") if supported else ("", "PyTorch does not support this Intel GPU.")
    if build and catalog.find(build):
        return build, ""
    return "", reason or f"There is no PyTorch build for {gpu.name}."


def choose(gpus: list[Gpu], catalog: Catalog) -> Choice:
    """Nvidia first (on a laptop it is the strong one), then AMD (the GPU with the most compute units),
    then Intel; the CPU build when none of them can run PyTorch."""
    reasons: list[str] = []
    for vendor in ("nvidia", "amd", "intel"):
        of_vendor = [g for g in gpus if g.vendor == vendor]
        for gpu in sorted(of_vendor, key=lambda g: -g.units):
            build, reason = gpu_build(gpu, catalog)
            if build:
                return Choice(build, gpu, of_vendor.index(gpu))
            reasons.append(reason)
    return Choice("cpu", None, 0, reasons[0] if reasons else "")
