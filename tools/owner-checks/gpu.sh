#!/usr/bin/env bash
# Does Spinus's analysis engine (PyTorch) work on your graphics card?
#
# What it does:
#   1. Shows the graphics card Spinus finds and the PyTorch build it suggests.
#   2. If that build is not installed yet, asks, then installs it exactly as Options > AI > Analysis
#      engine does, into ~/.local/share/spinus/python/ (about 1.8 GB download, 6.3 GB on disk for an
#      AMD gfx1151 card). Spinus uses the same installation afterwards; remove it there if you like.
#   3. Starts it, runs the graphics card self-test, and measures its speed against the CPU.
#   4. Prints a summary. Paste the whole output into the issue.
#
# Needs: uv (sudo pacman -S uv), internet for the first run. Run it from the spinus-daw folder:
#   tools/owner-checks/gpu.sh
set -u
cd "$(dirname "$0")/../.." || exit 1
if ! command -v uv >/dev/null; then
    echo "uv is missing. Install it with: sudo pacman -S uv"
    exit 1
fi
PYTHONPATH="$PWD/ai" exec python3 - "$@" <<'PY'
import asyncio, os, platform, sys
from pathlib import Path
from spinus_ai.compute.builds import Catalog
from spinus_ai.compute.devices import physical_cores
from spinus_ai.compute.engine import Engine, EngineError
from spinus_ai.compute.environments import InstallError

data = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share") / "spinus"
cache = Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") / "spinus"
config = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "spinus-gpu-check"

async def main() -> int:
    engine = Engine(config, data / "python", cache)
    print("== Computer")
    print("kernel", platform.release(), "| physical CPU cores", physical_cores())
    gpus = engine.gpus()
    for gpu in gpus:
        print(f"GPU: {gpu.vendor} | {gpu.name} | {gpu.target} | driver {gpu.driver or '-'} | units {gpu.units}")
        for note in gpu.notes:
            print("  note:", note)
    if not gpus:
        print("No GPU found that PyTorch could use.")
    choice = engine.choice()
    build = engine.catalog.find(choice.build)
    assert build is not None
    print(f"Suggested build: {build.id} ({build.label}), {'tested' if build.tested else 'UNTESTED'}",
          f"| download {build.download / 1e9:.2f} GB, {build.disk / 1e9:.2f} GB on disk")
    if choice.reason:
        print("Why no GPU build:", choice.reason)

    if not engine.environments.is_installed(build):
        answer = input(f"\nInstall {build.label} into {engine.environments.folder(build)}? [y/N] ")
        if answer.strip().lower() not in ("y", "yes"):
            print("Not installed; nothing else to check.")
            return 1
        print("== Installing (progress every 10 %)")
        shown = [-1]
        async def progress(done: float, step: str) -> None:
            if int(done * 10) != shown[0]:
                shown[0] = int(done * 10)
                print(f"  {step}: {done * 100:.0f} %", flush=True)
        try:
            await engine.environments.install(build, progress)
        except InstallError as error:
            print("FAILED:", error)
            return 1
    marker = engine.environments.marker(build) or {}
    print(f"Installed: {engine.environments.folder(build)} ({marker.get('size', 0) / 1e9:.2f} GB, {marker.get('installed', '?')})")

    print("== Self-test")
    try:
        test = await engine.test()
        print(f"{'PASS' if test['ok'] else 'FAIL'}: {test['deviceName']} | torch {test['torch']}",
              f"| HIP {test['hip'] or '-'} | CUDA {test['cuda'] or '-'} | XPU {test['xpu'] or '-'}",
              f"| max error {test['error']:.2g} | CPU threads {test['threads']}")
        if test.get("note"):
            print("  note:", test["note"])
        print("== Speed (2048x2048 matrix products, GFLOPS)")
        bench = await engine.call("benchmark", {})
        for device, gflops in bench["gflops"].items():
            print(f"  {device}: {gflops:.0f}")
    except EngineError as error:
        print("FAILED:", error)
        return 1
    finally:
        await engine.stop()
    print(f"\nWorker log: {engine.environments.root / 'worker.log'}")
    return 0

sys.exit(asyncio.run(main()))
PY
