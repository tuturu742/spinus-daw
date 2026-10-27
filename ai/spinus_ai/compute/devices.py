"""Which GPUs this computer has, read without PyTorch.

AMD GPUs come from the kernel's compute topology (/sys/class/kfd), which gives each GPU's target
(for example gfx1151); Nvidia GPUs from nvidia-smi (or the driver's version file);
Intel GPUs from the display cards in /sys/class/drm. Names come from the PCI ID database.
On Windows the display adapters come from Windows itself (Win32_VideoController), Nvidia's details from
nvidia-smi as on Linux; AMD's GPU targets cannot be read there, so AMD cards are listed but not used.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

PCI_IDS = (Path("/usr/share/hwdata/pci.ids"), Path("/usr/share/misc/pci.ids"))

# Intel GPUs PyTorch's XPU build supports, by the start of their PCI device ID: Arc A (Alchemist, 56xx),
# Arc B (Battlemage, e2xx), Core Ultra Meteor Lake and Arrow Lake (7dxx), Lunar Lake (64xx) and
# Panther Lake (b0xx). Older integrated graphics are not supported.
INTEL_XPU_PREFIXES = ("56", "e2", "7d", "64", "b0")


@dataclass
class Gpu:
    vendor: str  # "amd", "nvidia" or "intel"
    name: str
    target: str = ""  # AMD: gfx target (gfx1151); Nvidia: compute capability (8.6); Intel: PCI device ID
    driver: str = ""  # Nvidia: driver version
    units: int = 0  # AMD: SIMD units, to prefer the bigger of two GPUs
    notes: list[str] = field(default_factory=list)

    def to_json(self) -> dict[str, Any]:
        return asdict(self)


def gfx_name(version: int) -> str:
    """The kernel's gfx_target_version (110501) as a target name (gfx1151): major, minor, stepping."""
    major, minor, stepping = version // 10000, version // 100 % 100, version % 100
    return f"gfx{major}{minor:x}{stepping:x}"


def pci_name(vendor: str, device: str, databases: tuple[Path, ...] = PCI_IDS) -> str:
    vendor, device = vendor.lower().removeprefix("0x"), device.lower().removeprefix("0x")
    for database in databases:
        if not database.exists():
            continue
        in_vendor = False
        with database.open(encoding="utf-8", errors="replace") as lines:
            for line in lines:
                if line.startswith("#") or not line.strip():
                    continue
                if not line.startswith("\t"):
                    if in_vendor and line[:1].isalnum():
                        break
                    in_vendor = line.startswith(vendor + " ")
                elif in_vendor and line.startswith("\t" + device + " "):
                    return line.strip()[len(device) :].strip()
    return ""


def read(file: Path) -> str:
    try:
        return file.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return ""


def amd_gpus(root: Path = Path("/")) -> list[Gpu]:
    found: list[Gpu] = []
    nodes = root / "sys/class/kfd/kfd/topology/nodes"
    for node in sorted(nodes.glob("*")) if nodes.is_dir() else []:
        properties = dict(
            line.split(maxsplit=1) for line in read(node / "properties").splitlines() if len(line.split()) == 2
        )
        version = int(properties.get("gfx_target_version", "0") or 0)
        if version == 0:
            continue  # a CPU node
        device = f"{int(properties.get('device_id', '0')):04x}"
        name = pci_name("1002", device, tuple(root / p.relative_to("/") for p in PCI_IDS)) or "AMD GPU"
        found.append(Gpu("amd", name, gfx_name(version), units=int(properties.get("simd_count", "0") or 0)))
    if found and not os.access(root / "dev/kfd", os.R_OK | os.W_OK):
        for gpu in found:
            gpu.notes.append("Your account may not be allowed to use it: add yourself to the render and video groups.")
    return found


def nvidia_gpus(root: Path = Path("/"), smi: str | None = None) -> list[Gpu]:
    found: list[Gpu] = []
    smi = smi if smi is not None else shutil.which("nvidia-smi")
    if smi:
        try:
            out = subprocess.run(
                [smi, "--query-gpu=name,compute_cap,driver_version", "--format=csv,noheader"],
                capture_output=True, text=True, timeout=10, check=True,
            ).stdout  # fmt: skip
            for line in out.splitlines():
                parts = [p.strip() for p in line.split(",")]
                if len(parts) == 3:
                    found.append(Gpu("nvidia", parts[0], parts[1], parts[2]))
        except (OSError, subprocess.SubprocessError):
            pass
    if not found:
        version = re.search(r"Kernel Module\s+(?:for \S+\s+)?([\d.]+)", read(root / "proc/driver/nvidia/version"))
        gpus = root / "proc/driver/nvidia/gpus"
        for gpu in sorted(gpus.glob("*")) if gpus.is_dir() else []:
            model = re.search(r"Model:\s*(.+)", read(gpu / "information"))
            found.append(Gpu("nvidia", model[1].strip() if model else "Nvidia GPU", "", version[1] if version else ""))
    return found


def intel_gpus(root: Path = Path("/")) -> list[Gpu]:
    found: list[Gpu] = []
    seen: set[str] = set()
    for card in sorted((root / "sys/class/drm").glob("card[0-9]*")):
        if "-" in card.name:
            continue  # a connector (card1-HDMI-A-1)
        device = card / "device"
        if read(device / "vendor").lower() != "0x8086":
            continue
        pci = read(device / "device").lower().removeprefix("0x")
        slot = os.path.realpath(device)
        if slot in seen:
            continue
        seen.add(slot)
        name = pci_name("8086", pci, tuple(root / p.relative_to("/") for p in PCI_IDS)) or "Intel GPU"
        gpu = Gpu("intel", name, pci)
        if not pci.startswith(INTEL_XPU_PREFIXES):
            gpu.notes.append("PyTorch does not support this Intel GPU (it needs Arc or Core Ultra graphics).")
        found.append(gpu)
    return found


AMD_ON_WINDOWS = "On Windows, Spinus cannot use AMD graphics cards for this yet."


def nvidia_driver_from_windows(version: str) -> str:
    """Windows' driver version (32.0.15.6094) as Nvidia's own (560.94): the last five digits."""
    digits = re.sub(r"\D", "", version.split(".", 2)[-1]) if version.count(".") >= 3 else ""
    return f"{int(digits[-5:-2])}.{digits[-2:]}" if len(digits) >= 5 else ""


def windows_adapters() -> list[tuple[str, str, str]]:
    """(name, PNP device id, driver version) of each display adapter, from Windows."""
    script = (
        "Get-CimInstance Win32_VideoController | ForEach-Object "
        "{ $_.Name + '|' + $_.PNPDeviceID + '|' + $_.DriverVersion }"
    )
    try:
        out = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
            capture_output=True, text=True, timeout=20, check=True,
        ).stdout  # fmt: skip
    except (OSError, subprocess.SubprocessError):
        return []
    return [tuple(line.strip().split("|", 2)) for line in out.splitlines() if line.count("|") == 2]  # type: ignore[misc]


def windows_gpus(adapters: list[tuple[str, str, str]], smi: str | None = None) -> list[Gpu]:
    found = nvidia_gpus(Path("/nonexistent"), smi)
    for name, device_id, driver in adapters:
        ids = re.search(r"VEN_([0-9A-Fa-f]{4})&DEV_([0-9A-Fa-f]{4})", device_id)
        if ids is None:
            continue
        vendor, device = ids[1].lower(), ids[2].lower()
        if vendor == "10de" and not any(g.vendor == "nvidia" for g in found):
            found.append(Gpu("nvidia", name, "", nvidia_driver_from_windows(driver)))
        elif vendor == "1002":
            found.append(Gpu("amd", name, notes=[AMD_ON_WINDOWS]))
        elif vendor == "8086":
            gpu = Gpu("intel", name, device)
            if not device.startswith(INTEL_XPU_PREFIXES):
                gpu.notes.append("PyTorch does not support this Intel GPU (it needs Arc or Core Ultra graphics).")
            found.append(gpu)
    return found


def detect(root: Path = Path("/")) -> list[Gpu]:
    if sys.platform == "win32" and root == Path("/"):
        return windows_gpus(windows_adapters())
    return amd_gpus(root) + nvidia_gpus(root, smi="" if root != Path("/") else None) + intel_gpus(root)


def physical_cores(root: Path = Path("/")) -> int:
    """CPU cores without their second hardware threads (16 on a 16-core, 32-thread CPU)."""
    cores = {
        read(cpu / "topology/core_cpus_list") or read(cpu / "topology/thread_siblings_list")
        for cpu in (root / "sys/devices/system/cpu").glob("cpu[0-9]*")
    }
    cores.discard("")
    return max(1, len(cores) or (os.cpu_count() or 2) // 2)
