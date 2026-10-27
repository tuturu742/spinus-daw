"""The analysis engine: finding GPUs, choosing and installing a PyTorch build, and the worker.

Everything here runs without PyTorch: a fake `uv` installs fake environments, and a fake `torch`
(NumPy underneath) stands in inside them. SPINUS_TEST_COMPUTE_PYTHON=<an environment's python> also
runs the real worker in a real PyTorch environment.
"""

from __future__ import annotations

import asyncio
import json
import os
import re
import shutil
import stat
import sys
from pathlib import Path
from typing import Any

import pytest

from spinus_ai.compute import devices
from spinus_ai.compute.builds import HERE, Catalog, choose
from spinus_ai.compute.devices import Gpu, gfx_name
from spinus_ai.compute.engine import Engine, EngineError
from spinus_ai.compute.environments import MARKER, Environments, InstallError, lock_hash
from spinus_ai.connections import MemoryKeyStore
from spinus_ai.service import Service

CATALOG = Catalog.load(HERE / "builds.json")  # Linux's builds, on any system
WINDOWS_CATALOG = Catalog.load(HERE / "builds-windows.json")
POSIX_ONLY = pytest.mark.skipif(sys.platform == "win32", reason="the stand-ins are POSIX shell scripts")
FAKES = Path(__file__).resolve().parent / "fakes"  # a stand-in uv, and a stand-in torch (NumPy underneath)
PCI_IDS = """\
1002  Advanced Micro Devices, Inc. [AMD/ATI]
\t1114  Krackan [Radeon 840M / 860M Graphics]
# A comment inside the vendor's list
\t1586  Strix Halo [Radeon 8060S Graphics]
\t744c  Navi 31 [Radeon RX 7900 XT/7900 XTX]
10de  NVIDIA Corporation
\t2684  AD102 [GeForce RTX 4090]
8086  Intel Corporation
\t7d55  Meteor Lake-P [Intel Arc Graphics]
\t9a49  TigerLake-LP GT2 [Iris Xe Graphics]
"""


def write(file: Path, text: str) -> Path:
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(text, encoding="utf-8")
    return file


def executable(file: Path, text: str) -> Path:
    write(file, text)
    file.chmod(file.stat().st_mode | stat.S_IXUSR)
    return file


# ---- finding GPUs ---------------------------------------------------------------------------------


def test_gfx_targets_are_named_from_the_kernels_numbers() -> None:
    assert gfx_name(110501) == "gfx1151"
    assert gfx_name(90010) == "gfx90a"
    assert gfx_name(90402) == "gfx942"
    assert gfx_name(100300) == "gfx1030"
    assert gfx_name(120001) == "gfx1201"


def fake_amd(root: Path, nodes: list[tuple[int, str, int]]) -> None:
    write(root / "usr/share/hwdata/pci.ids", PCI_IDS)
    write(root / "sys/class/kfd/kfd/topology/nodes/0/properties", "simd_count 0\ngfx_target_version 0\n")
    for number, (version, device, simds) in enumerate(nodes, start=1):
        write(
            root / f"sys/class/kfd/kfd/topology/nodes/{number}/properties",
            f"simd_count {simds}\ngfx_target_version {version}\nvendor_id 4098\ndevice_id {int(device, 16)}\n",
        )


def fake_card(root: Path, number: int, vendor: str, device: str) -> None:
    write(root / f"sys/class/drm/card{number}/device/vendor", f"0x{vendor}\n")
    write(root / f"sys/class/drm/card{number}/device/device", f"0x{device}\n")
    (root / f"sys/class/drm/card{number}-HDMI-A-1").mkdir(parents=True)


def test_amd_and_intel_gpus_are_found_and_named(tmp_path: Path) -> None:
    fake_amd(tmp_path, [(110501, "1586", 80)])
    fake_card(tmp_path, 1, "8086", "7d55")
    fake_card(tmp_path, 2, "8086", "9a49")
    found = devices.detect(tmp_path)
    assert [(g.vendor, g.name, g.target) for g in found] == [
        ("amd", "Strix Halo [Radeon 8060S Graphics]", "gfx1151"),
        ("intel", "Meteor Lake-P [Intel Arc Graphics]", "7d55"),
        ("intel", "TigerLake-LP GT2 [Iris Xe Graphics]", "9a49"),
    ]
    assert found[0].units == 80
    assert "render and video groups" in found[0].notes[0]  # the fake root has no /dev/kfd
    assert not found[1].notes and "does not support" in found[2].notes[0]


@POSIX_ONLY
def test_nvidia_gpus_come_from_nvidia_smi_or_the_driver(tmp_path: Path) -> None:
    smi = executable(tmp_path / "nvidia-smi", "#!/bin/sh\necho 'NVIDIA GeForce RTX 4090, 8.9, 590.44.01'\n")
    assert devices.nvidia_gpus(tmp_path, str(smi)) == [Gpu("nvidia", "NVIDIA GeForce RTX 4090", "8.9", "590.44.01")]
    write(tmp_path / "proc/driver/nvidia/version", "NVRM version: NVIDIA UNIX x86_64 Kernel Module  550.120  Sat\n")
    write(tmp_path / "proc/driver/nvidia/gpus/0000:01:00.0/information", "Model: \t\t NVIDIA GeForce GTX 1080\n")
    assert devices.nvidia_gpus(tmp_path, "") == [Gpu("nvidia", "NVIDIA GeForce GTX 1080", "", "550.120")]


def test_windows_lists_its_display_adapters() -> None:
    adapters = [
        ("NVIDIA GeForce RTX 4070 Laptop GPU", "PCI\\VEN_10DE&DEV_2820&SUBSYS_3A3C17AA&REV_A1\\4&1", "32.0.15.6094"),
        ("AMD Radeon 780M Graphics", "PCI\\VEN_1002&DEV_15BF&SUBSYS_3A3C17AA&REV_C4\\4&2", "31.0.24027.1012"),
        ("Intel(R) Arc(TM) Graphics", "PCI\\VEN_8086&DEV_7D55&SUBSYS_0B341028&REV_08\\3&1", "32.0.101.5972"),
        ("Intel(R) Iris(R) Xe Graphics", "PCI\\VEN_8086&DEV_9A49&SUBSYS_0A8A1028&REV_01\\3&2", "31.0.101.4502"),
        ("Microsoft Basic Display Adapter", "ROOT\\BASICDISPLAY\\0000", "10.0.22621.1"),
    ]
    found = devices.windows_gpus(adapters, smi="")
    assert [(g.vendor, g.name, g.target, g.driver) for g in found] == [
        ("nvidia", "NVIDIA GeForce RTX 4070 Laptop GPU", "", "560.94"),
        ("amd", "AMD Radeon 780M Graphics", "", ""),
        ("intel", "Intel(R) Arc(TM) Graphics", "7d55", ""),
        ("intel", "Intel(R) Iris(R) Xe Graphics", "9a49", ""),
    ]
    assert found[1].notes == [devices.AMD_ON_WINDOWS] and not found[2].notes and found[3].notes
    # Its driver's version is enough to pick the build; AMD's cards are not used there.
    assert choose(found, WINDOWS_CATALOG).build == "cuda-12"  # no compute capability: CUDA 13 needs it known
    assert choose(found[1:2], WINDOWS_CATALOG).reason == devices.AMD_ON_WINDOWS
    assert choose(found[2:3], WINDOWS_CATALOG).build == "xpu"


def test_windows_driver_versions_become_nvidias() -> None:
    assert devices.nvidia_driver_from_windows("32.0.15.6094") == "560.94"
    assert devices.nvidia_driver_from_windows("31.0.15.3623") == "536.23"
    assert devices.nvidia_driver_from_windows("") == ""


def test_physical_cores_count_each_core_once(tmp_path: Path) -> None:
    for cpu in range(8):
        write(tmp_path / f"sys/devices/system/cpu/cpu{cpu}/topology/core_cpus_list", f"{cpu % 4},{cpu % 4 + 4}\n")
    assert devices.physical_cores(tmp_path) == 4


# ---- choosing a build ------------------------------------------------------------------------------


def check_locks(catalog: Catalog) -> None:
    for build in catalog.builds:
        lock = build.lock.read_text(encoding="utf-8")
        version = re.search(r"==(\S+)$", build.torch)
        assert version and f"torch=={version[1]} \\" in lock, build.id
        packages = [block for block in re.split(r"\n(?=\S)", lock.strip()) if not block.startswith("#")]
        assert all("--hash=sha256:" in block for block in packages), build.id
        assert build.download > 0 and build.disk > build.download, build.id


def test_every_build_has_a_hash_pinned_lock_with_its_torch() -> None:
    assert CATALOG.python == "3.12"
    ids = [b.id for b in CATALOG.builds]
    assert {"cpu", "cuda-13", "cuda-12", "xpu", "rocm-gfx1151", "rocm-gfx1100", "rocm-gfx1201"} <= set(ids)
    check_locks(CATALOG)
    assert {b.id for b in CATALOG.builds if b.tested} == {"cpu", "rocm-gfx1151"}


def test_windows_has_its_own_locks_without_amds_builds() -> None:
    assert WINDOWS_CATALOG.python == CATALOG.python
    assert [b.id for b in WINDOWS_CATALOG.builds] == ["cpu", "cuda-13", "cuda-12", "xpu"]
    check_locks(WINDOWS_CATALOG)
    assert all(b.lock.parent.name == "windows" for b in WINDOWS_CATALOG.builds)
    for build in WINDOWS_CATALOG.builds:  # the same torch as on Linux
        linux = CATALOG.find(build.id)
        assert linux is not None and linux.torch == build.torch, build.id


@pytest.mark.parametrize(
    ("gpus", "build", "reason"),
    [
        ([Gpu("amd", "Strix Halo", "gfx1151", units=80)], "rocm-gfx1151", ""),
        ([Gpu("amd", "Old Radeon", "gfx803")], "cpu", "no build for this GPU (gfx803)"),
        ([Gpu("nvidia", "RTX 4090", "8.9", "590.44")], "cuda-13", ""),
        ([Gpu("nvidia", "RTX 4090", "8.9", "550.120")], "cuda-12", ""),
        ([Gpu("nvidia", "GTX 1080", "6.1", "590.44")], "cuda-12", ""),
        ([Gpu("nvidia", "RTX 5090", "12.0", "570.1")], "cpu", "needs Nvidia driver 580"),
        ([Gpu("nvidia", "GTX 680", "3.0", "470.2")], "cpu", "needs Nvidia driver 525"),
        ([Gpu("nvidia", "Some card", "", "")], "cpu", "could not be read"),
        ([Gpu("intel", "Arc", "7d55")], "xpu", ""),
        ([Gpu("intel", "Iris Xe", "9a49")], "cpu", "does not support this Intel GPU"),
        ([], "cpu", ""),
        # A laptop: the Nvidia card wins over the integrated graphics.
        ([Gpu("amd", "Radeon 780M", "gfx1103", units=24), Gpu("nvidia", "RTX 4070", "8.9", "590.1")], "cuda-13", ""),
        # Two AMD GPUs: the bigger one.
        ([Gpu("amd", "iGPU", "gfx1036", units=4), Gpu("amd", "RX 7900", "gfx1100", units=192)], "rocm-gfx1100", ""),
    ],
)
def test_the_build_suits_the_gpu(gpus: list[Gpu], build: str, reason: str) -> None:
    choice = choose(gpus, CATALOG)
    assert choice.build == build
    assert reason in choice.reason
    if build == "rocm-gfx1100":
        assert choice.gpu_index == 1


# ---- installing ------------------------------------------------------------------------------------


@pytest.fixture
def fakes(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    if sys.platform == "win32":
        pytest.skip("the stand-in uv and python are POSIX shell scripts")
    monkeypatch.setenv("FAKE_TORCH", str(FAKES))
    monkeypatch.setenv("REAL_PYTHON", sys.executable)
    monkeypatch.setenv("FAKE_PID", str(tmp_path / "uv.pid"))
    monkeypatch.setenv("PATH", f"{FAKES}{os.pathsep}{os.environ['PATH']}")
    monkeypatch.delenv("FAKE_UV", raising=False)
    return tmp_path


async def test_install_writes_the_marker_last_and_clears_uvs_cache(fakes: Path) -> None:
    environments = Environments(fakes / "python", CATALOG)
    build = CATALOG.find("rocm-gfx1151")
    assert build is not None
    steps: list[tuple[float, str]] = []

    async def progress(done: float, step: str) -> None:
        steps.append((done, step))

    await environments.install(build, progress)
    assert environments.is_installed(build) and not environments.is_outdated(build)
    marker = json.loads((environments.folder(build) / MARKER).read_text())
    assert marker["lock"] == lock_hash(build) and marker["size"] > 0
    assert not (fakes / "python/.uv-cache").exists()
    assert [s for _, s in steps][:2] == ["Getting Python", "Downloading PyTorch"]
    assert "Downloading PyTorch" in [s for d, s in steps if d > 0.01]  # the watcher saw files arrive
    assert [d for d, _ in steps] == sorted(d for d, _ in steps)
    assert environments.describe(build)["installed"]

    # Spinus was updated with a new lock: the old environment still works, and says it is outdated.
    (environments.folder(build) / MARKER).write_text(json.dumps({**marker, "lock": "old"}))
    assert environments.is_installed(build) and environments.is_outdated(build)

    await environments.remove(build)
    assert not environments.folder(build).exists() and not (fakes / "python/interpreters").exists()


async def test_a_failed_or_stopped_install_leaves_nothing_behind(fakes: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    environments = Environments(fakes / "python", CATALOG)
    build = CATALOG.find("cpu")
    assert build is not None

    async def progress(done: float, step: str) -> None:
        pass

    monkeypatch.setenv("FAKE_UV", "fail")
    with pytest.raises(InstallError, match="no matching wheel for torch"):
        await environments.install(build, progress)
    assert not environments.folder(build).exists() and not environments.is_installed(build)

    monkeypatch.setenv("FAKE_UV", "hang")
    task = asyncio.create_task(environments.install(build, progress))
    while not (fakes / "uv.pid").exists():
        await asyncio.sleep(0.05)
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    pid = int((fakes / "uv.pid").read_text())
    with pytest.raises(ProcessLookupError):
        os.kill(pid, 0)
    assert not environments.folder(build).exists() and environments.installing == ""


async def test_install_checks_for_uv_and_disk_space(fakes: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    environments = Environments(fakes / "python", CATALOG)
    build = CATALOG.find("rocm-gfx1151")
    assert build is not None

    async def progress(done: float, step: str) -> None:
        pass

    monkeypatch.setattr(shutil, "disk_usage", lambda _: shutil._ntuple_diskusage(10**12, 10**12 - 10**9, 10**9))
    with pytest.raises(InstallError, match=r"Not enough free disk space: this needs about \d+\.\d GB"):
        await environments.install(build, progress)
    monkeypatch.setenv("PATH", "/nonexistent")
    monkeypatch.setattr("spinus_ai.compute.environments.find_uv", lambda: None)
    with pytest.raises(InstallError, match="needs uv"):
        await environments.install(build, progress)


# ---- the engine ------------------------------------------------------------------------------------


async def install(engine: Engine, build_id: str) -> None:
    build = engine.catalog.find(build_id)
    assert build is not None

    async def progress(done: float, step: str) -> None:
        pass

    await engine.environments.install(build, progress)


def make_engine(root: Path, gpus: list[Gpu]) -> Engine:
    return Engine(root / "config", root / "python", root / "cache", gpus=lambda: gpus, cores=lambda: 6)


STRIX = [Gpu("amd", "Strix Halo", "gfx1151", units=80)]


async def test_the_gpu_is_used_when_it_passes_its_test(fakes: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("FAKE_GPU", "ok")
    monkeypatch.setenv("PYTORCH_HIP_ALLOC_CONF", "expandable_segments:True")
    engine = make_engine(fakes, STRIX)
    with pytest.raises(EngineError, match="not installed"):
        await engine.call("info", {})
    await install(engine, "rocm-gfx1151")
    info = await engine.call("info", {})
    assert info["device"] == "cuda" and info["deviceName"] == "Fake Radeon" and info["torch"] == "2.14.0+fake"
    assert engine.last_test is not None and engine.last_test["ok"]
    env = engine.worker_env(engine.plan())  # type: ignore[arg-type]
    assert "PYTORCH_HIP_ALLOC_CONF" not in env and env["HIP_VISIBLE_DEVICES"] == "0"
    assert env["MIOPEN_USER_DB_PATH"] == str(fakes / "cache/gpu/miopen")
    status = engine.status()
    assert status["suggested"] == "rocm-gfx1151" and status["uses"] == {
        "build": "rocm-gfx1151",
        "device": "auto",
        "note": "",
    }
    assert [b["id"] for b in status["builds"]] == ["rocm-gfx1151", "cpu"]

    # "Force CPU": the GPU build on the CPU, its threads limited to the physical cores.
    await engine.set_force_cpu(True)
    info = await engine.call("info", {})
    assert info["device"] == "cpu" and info["threads"] == 6
    await engine.stop()


@pytest.mark.parametrize(("gpu", "why"), [("wrong", "wrong result"), ("crash", "signal 11")])
async def test_a_failing_gpu_falls_back_to_the_cpu(
    fakes: Path, monkeypatch: pytest.MonkeyPatch, gpu: str, why: str
) -> None:
    monkeypatch.setenv("FAKE_GPU", gpu)
    engine = make_engine(fakes, STRIX)
    await install(engine, "rocm-gfx1151")
    await install(engine, "cpu")
    info = await engine.call("info", {})
    assert info["device"] == "cpu"
    uses = engine.status()["uses"]
    assert uses["build"] == "cpu" and why in uses["note"]
    assert engine.last_test is not None and not engine.last_test["ok"]

    # "Test again" tries the GPU once more, and reports.
    monkeypatch.setenv("FAKE_GPU", "ok")
    test = await engine.test()
    assert test["ok"] and test["device"] == "cuda" and test["build"] == "rocm-gfx1151"
    await engine.stop()


async def test_a_crashed_or_stopped_job_does_not_stop_the_next(fakes: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("FAKE_GPU", "none")
    engine = make_engine(fakes, [])
    await install(engine, "cpu")
    assert (await engine.call("info", {}))["device"] == "cpu"
    worker = engine.process
    assert worker is not None
    worker.kill()
    with pytest.raises(EngineError, match="stopped unexpectedly"):
        await engine.call("info", {})
    assert (await engine.call("info", {}))["device"] == "cpu"
    with pytest.raises(EngineError, match="Unknown method"):
        await engine.call("no-such-job", {})

    # Stopping a job stops the worker; the next job starts another.
    task = asyncio.create_task(engine.call("selftest", {}))
    await asyncio.sleep(0)
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert engine.process is None
    assert (await engine.call("selftest", {}))["ok"]
    await engine.stop()


async def test_the_service_installs_tests_and_removes(fakes: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("FAKE_GPU", "ok")
    engine = make_engine(fakes, STRIX)
    service = Service(fakes / "config", MemoryKeyStore(), None, None, models=fakes / "models", engine=engine)
    updates: list[dict[str, Any]] = []
    finished: asyncio.Queue[dict[str, Any]] = asyncio.Queue()

    async def notify(name: str, payload: dict[str, Any]) -> None:
        updates.append(payload)
        if payload["kind"] in ("done", "error"):
            await finished.put(payload)

    status = await service.handle("ai.v1.compute.status", {}, notify)
    assert status["suggested"] == "rocm-gfx1151" and status["uses"] is None
    assert status["builds"][0]["tested"] and status["builds"][0]["download"] > 1_000_000_000

    await service.handle("ai.v1.compute.install", {"build": "rocm-gfx1151"}, notify)
    done = await asyncio.wait_for(finished.get(), 30)
    assert done["kind"] == "done" and done["result"]["builds"][0]["installed"]
    assert any(u["kind"] == "progress" and u["step"] == "Downloading PyTorch" for u in updates)

    await service.handle("ai.v1.compute.test", {}, notify)
    done = await asyncio.wait_for(finished.get(), 30)
    assert done["result"]["test"]["ok"] and done["result"]["test"]["deviceName"] == "Fake Radeon"

    status = await service.handle("ai.v1.compute.setForceCpu", {"on": True}, notify)
    assert status["forceCpu"] and status["uses"]["device"] == "cpu"
    status = await service.handle("ai.v1.compute.remove", {"build": "rocm-gfx1151"}, notify)
    assert not status["builds"][0]["installed"] and status["uses"] is None
    await service.close()


@pytest.mark.skipif(not os.environ.get("SPINUS_TEST_COMPUTE_PYTHON"), reason="needs a real PyTorch environment")
async def test_the_real_worker_runs_its_self_test(tmp_path: Path) -> None:
    python = Path(os.environ["SPINUS_TEST_COMPUTE_PYTHON"])
    engine = make_engine(tmp_path, devices.detect())
    build = engine.catalog.find(engine.choice().build)
    assert build is not None
    folder = engine.environments.folder(build)
    (folder / "bin").mkdir(parents=True)
    (folder / "bin/python").symlink_to(python)
    write(folder / "pyvenv.cfg", (python.parent.parent / "pyvenv.cfg").read_text())
    (folder / "lib").symlink_to(python.parent.parent / "lib")
    write(folder / MARKER, json.dumps({"build": build.id, "lock": lock_hash(build)}))
    test = await engine.test()
    print(test)
    assert test["ok"]
    await engine.stop()
