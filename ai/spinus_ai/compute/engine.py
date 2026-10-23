"""The analysis engine: runs PyTorch work in a worker process.

The AI service itself runs on the system's Python with few packages. PyTorch work (stem separation,
beat tracking, sound matching) runs in a worker started from one of the installed environments
(environments.py): a GPU build on its GPU when it passes the self-test, otherwise the CPU build, or
a GPU build on the CPU with its threads limited to the physical cores. A worker that crashes
(a GPU memory fault, say) takes no one with it: the next job starts a new one.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import os
from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .builds import Build, Catalog, Choice, choose
from .devices import Gpu, detect, physical_cores
from .environments import Environments, find_uv

PACKAGE_PARENT = Path(__file__).resolve().parent.parent.parent
START_TIMEOUT = 300.0  # importing PyTorch and the first GPU kernels can take a while

Progress = Callable[[dict[str, Any]], Awaitable[None]]


class EngineError(Exception):
    pass


@dataclass(frozen=True)
class Plan:
    build: Build
    device: str  # "auto" (its GPU) or "cpu"
    note: str = ""


class Engine:
    def __init__(
        self,
        config: Path,
        root: Path,
        cache: Path,
        catalog: Catalog | None = None,
        gpus: Callable[[], list[Gpu]] = detect,
        cores: Callable[[], int] = physical_cores,
    ) -> None:
        self.settings_file = config / "compute.json"
        self.catalog = catalog or Catalog.load()
        self.environments = Environments(root, self.catalog)
        self.cache = cache
        self.detect_gpus, self.cores = gpus, cores
        self._gpus: list[Gpu] | None = None
        self.gpu_failures: dict[str, str] = {}  # build -> why its GPU is not used this session
        self.last_test: dict[str, Any] | None = None
        self.process: asyncio.subprocess.Process | None = None
        self.running: Plan | None = None
        self.lock = asyncio.Lock()
        self.next_id = 1

    # -----------------------------------------------------------------------------------------------

    @property
    def force_cpu(self) -> bool:
        try:
            return bool(json.loads(self.settings_file.read_text(encoding="utf-8")).get("forceCpu", False))
        except (OSError, ValueError):
            return False

    async def set_force_cpu(self, on: bool) -> None:
        self.settings_file.parent.mkdir(parents=True, exist_ok=True)
        self.settings_file.write_text(json.dumps({"forceCpu": on}) + "\n", encoding="utf-8")
        await self.stop()

    def gpus(self, refresh: bool = False) -> list[Gpu]:
        if self._gpus is None or refresh:
            self._gpus = self.detect_gpus()
        return self._gpus

    def choice(self) -> Choice:
        return choose(self.gpus(), self.catalog)

    def plan(self) -> Plan | None:
        """Which environment and device the next job uses, or None when nothing is installed."""
        choice = self.choice()
        gpu_build = self.catalog.find(choice.build) if choice.build != "cpu" else None
        cpu_build = self.catalog.find("cpu")
        installed = self.environments.installed()
        if gpu_build in installed and not self.force_cpu and gpu_build.id not in self.gpu_failures:
            return Plan(gpu_build, "auto")
        why = self.gpu_failures.get(gpu_build.id, "") if gpu_build else ""
        if cpu_build in installed:
            return Plan(cpu_build, "cpu", why)
        for build in installed:
            if build.vendor != "cpu":
                return Plan(build, "cpu", why)
        return None

    def status(self) -> dict[str, Any]:
        choice = self.choice()
        shown = [b for b in (self.catalog.find(choice.build), self.catalog.find("cpu")) if b is not None]
        shown += [b for b in self.environments.installed() if b not in shown]
        plan = self.plan()
        return {
            "gpus": [g.to_json() for g in self.gpus()],
            "suggested": choice.build,
            "reason": choice.reason,
            "builds": [self.environments.describe(b) for b in shown],
            "uv": find_uv() is not None,
            "forceCpu": self.force_cpu,
            "folder": str(self.environments.root),
            "uses": None if plan is None else {"build": plan.build.id, "device": plan.device, "note": plan.note},
            "test": self.last_test,
        }

    # -----------------------------------------------------------------------------------------------

    def worker_env(self, plan: Plan) -> dict[str, str]:
        env = {k: v for k, v in os.environ.items() if k not in ("PYTORCH_HIP_ALLOC_CONF", "HSA_OVERRIDE_GFX_VERSION")}
        gpu_cache = self.cache / "gpu"
        env.update(
            {
                "PYTHONPATH": str(PACKAGE_PARENT),
                "PYTHONNOUSERSITE": "1",
                "MIOPEN_USER_DB_PATH": str(gpu_cache / "miopen"),
                "MIOPEN_CUSTOM_CACHE_DIR": str(gpu_cache / "miopen"),
                "AMD_COMGR_CACHE_DIR": str(gpu_cache / "comgr"),
            }
        )
        choice = self.choice()
        if plan.device == "auto" and choice.gpu is not None:
            index = str(choice.gpu_index)
            if choice.gpu.vendor == "amd":
                env["HIP_VISIBLE_DEVICES"] = index
            elif choice.gpu.vendor == "nvidia":
                env["CUDA_VISIBLE_DEVICES"] = index
        return env

    async def start(self, plan: Plan) -> None:
        await self.stop()
        log = self.environments.root / "worker.log"
        log.parent.mkdir(parents=True, exist_ok=True)
        threads = self.cores() if plan.device == "cpu" else 0
        with log.open("w", encoding="utf-8") as errors:
            self.process = await asyncio.create_subprocess_exec(
                str(self.environments.python(plan.build)), "-m", "spinus_ai.compute.worker",
                "--device", plan.device, "--threads", str(threads),
                stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=errors,
                env=self.worker_env(plan), limit=1 << 24,
            )  # fmt: skip
        self.running = plan
        ready = await self.read(None, START_TIMEOUT)
        if ready != "ready":
            await self.stop()
            raise EngineError(f"The analysis engine could not start: {ready}")

    async def read(self, progress: Progress | None, timeout: float | None = None) -> Any:
        assert self.process is not None and self.process.stdout is not None
        while True:
            try:
                line = await asyncio.wait_for(self.process.stdout.readline(), timeout)
            except TimeoutError:
                await self.stop()
                raise EngineError("The analysis engine did not answer in time.") from None
            if not line:
                code = await self.process.wait()
                self.process = None
                raise EngineError(self.crash_message(code))
            message = json.loads(line)
            if "progress" in message:
                if progress is not None:
                    await progress(message["progress"])
                continue
            if "error" in message:
                raise EngineError(str(message["error"]))
            return message.get("result")

    def crash_message(self, code: int) -> str:
        how = f"signal {-code}" if code < 0 else f"code {code}"
        log = self.environments.root / "worker.log"
        try:
            tail = [line for line in log.read_text(encoding="utf-8", errors="replace").splitlines() if line.strip()]
        except OSError:
            tail = []
        last = f" Last message: {tail[-1].strip()}" if tail else ""
        return f"The analysis engine stopped unexpectedly ({how}).{last} (The log is in {log}.)"

    async def send(self, method: str, params: dict[str, Any], progress: Progress | None) -> Any:
        assert self.process is not None and self.process.stdin is not None
        number = self.next_id
        self.next_id += 1
        self.process.stdin.write((json.dumps({"id": number, "method": method, "params": params}) + "\n").encode())
        await self.process.stdin.drain()
        return await self.read(progress)

    async def ensure(self) -> Plan:
        """A worker for the current plan; a GPU that fails its self-test is not used again this session."""
        plan = self.plan()
        if plan is None:
            raise EngineError("The analysis engine is not installed: install it in Options > AI > Analysis engine.")
        if self.process is not None and self.running == plan:
            return plan
        if plan.device == "cpu":
            await self.start(plan)
            return plan
        try:
            await self.start(plan)
            test = await self.send("selftest", {}, None)
            self.last_test = test
            if test.get("device") == "cpu":
                raise EngineError("PyTorch does not see the GPU.")
            if not test.get("ok"):
                raise EngineError(f"The GPU computed a wrong result (error {test.get('error'):.3g}).")
            return plan
        except EngineError as error:
            await self.stop()
            self.gpu_failures[plan.build.id] = f"The GPU failed its test, so the CPU is used instead: {error}"
            self.last_test = {"ok": False, "message": str(error)}
            return await self.ensure()

    async def call(self, method: str, params: dict[str, Any], progress: Progress | None = None) -> Any:
        """Runs one job in the worker; cancelling it stops the worker."""
        async with self.lock:
            await self.ensure()
            try:
                return await self.send(method, params, progress)
            except asyncio.CancelledError:
                await self.stop()
                raise

    async def test(self) -> dict[str, Any]:
        """Tries the GPU again (and the CPU otherwise): the result of the self-test, for Options > AI."""
        async with self.lock:
            await self.stop()
            self.gpu_failures.clear()
            self.gpus(refresh=True)
            plan = await self.ensure()
            info: dict[str, Any] = await self.send("selftest", {}, None)
            info["build"], info["note"] = plan.build.id, plan.note
            self.last_test = info
            return info

    async def stop(self) -> None:
        process, self.process, self.running = self.process, None, None
        if process is None or process.returncode is not None:
            return
        with contextlib.suppress(ProcessLookupError):
            process.kill()
        with contextlib.suppress(Exception):
            await asyncio.wait_for(process.wait(), 10)
