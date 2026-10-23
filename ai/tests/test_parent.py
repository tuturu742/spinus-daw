"""The service stops by itself once the Spinus that started it has gone (crashed or killed)."""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

import pytest

from spinus_ai.parent import is_alive, watch_parent

RUNNER = Path(__file__).resolve().parents[1] / "run_spinus_ai.py"


def test_is_alive() -> None:
    assert is_alive(os.getpid())
    child = subprocess.Popen([sys.executable, "-c", "pass"])
    child.wait()
    assert not is_alive(child.pid)
    assert not is_alive(0)


async def test_watch_calls_back_once_the_process_ends() -> None:
    child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(0.5)"])
    called: list[bool] = []
    await watch_parent(child.pid, lambda: called.append(True), interval=0.05)
    assert called == [True]
    assert child.poll() is not None or child.wait(5) == 0


@pytest.mark.skipif(sys.platform == "win32", reason="the stand-in parent uses a POSIX shell")
def test_service_exits_when_its_parent_is_killed(tmp_path: Path) -> None:
    """A stand-in Spinus (a shell) starts the service with --parent-pid and is killed: the service follows."""
    port_file, token_file = tmp_path / "service.port", tmp_path / "service.token"
    token_file.write_text("t" * 32)
    pid_file = tmp_path / "service.pid"
    args = [
        sys.executable,
        str(RUNNER),
        "--log",
        str(tmp_path / "service.log"),
        "--port-file",
        str(port_file),
        "--token-file",
        str(token_file),
        "--config",
        str(tmp_path / "config"),
        "--memory-keys",
    ]
    quoted = " ".join("'" + a.replace("'", "'\\''") + "'" for a in args)
    # The shell starts the service in the background, naming itself as the parent, and then waits.
    script = f"{quoted} --parent-pid $$ & echo $! > '{pid_file}'; wait"
    parent = subprocess.Popen(["sh", "-c", script], start_new_session=True)
    try:
        deadline = time.monotonic() + 30
        while not (port_file.exists() and pid_file.exists() and pid_file.read_text().strip()):
            assert time.monotonic() < deadline, (tmp_path / "service.log").read_text()
            time.sleep(0.1)
        service = int(pid_file.read_text())
        assert is_alive(service)

        parent.kill()  # as a crash: no shutdown message
        parent.wait(5)
        deadline = time.monotonic() + 15
        while is_alive(service):
            assert time.monotonic() < deadline, "the service kept running after its parent was killed"
            time.sleep(0.1)
        assert not port_file.exists()  # it stopped in order, not killed
    finally:
        if parent.poll() is None:
            parent.kill()
