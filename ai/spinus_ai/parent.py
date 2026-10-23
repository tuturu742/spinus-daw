"""The service ends with the Spinus that started it.

Spinus asks the service to shut down when it quits, but a Spinus that crashes, is killed or hangs in its
exit never does, and the service (with its compute worker, which can hold gigabytes) would run on with
nobody to talk to. Spinus passes its process id (--parent-pid); the service checks every second that the
process is still there and stops when it is gone.
"""

from __future__ import annotations

import asyncio
import os
import sys
from collections.abc import Callable


def is_alive(pid: int) -> bool:
    """True while the process `pid` exists (and is not a zombie left for its own parent to collect)."""
    if pid <= 0:
        return False
    if sys.platform == "win32":
        return _is_alive_windows(pid)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True  # exists, owned by someone else
    # A crashed Spinus whose own parent has not collected it yet is a zombie: gone for our purposes.
    try:
        with open(f"/proc/{pid}/stat", encoding="ascii", errors="replace") as stat:
            state = stat.read().rsplit(")", 1)[-1].split()[0]
        return state not in ("Z", "X")
    except (OSError, IndexError):
        return True  # no /proc (or it went away just now): the signal check said it exists


def _is_alive_windows(pid: int) -> bool:
    import ctypes

    synchronize, wait_timeout = 0x00100000, 0x00000102
    kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined,unused-ignore]
    handle = kernel32.OpenProcess(synchronize, False, pid)
    if not handle:
        return False
    try:
        return bool(kernel32.WaitForSingleObject(handle, 0) == wait_timeout)
    finally:
        kernel32.CloseHandle(handle)


async def watch_parent(pid: int, on_gone: Callable[[], None], interval: float = 1.0) -> None:
    """Calls `on_gone` once the process `pid` has ended (and returns)."""
    original_parent = os.getppid()
    while True:
        # On Linux a service whose parent died is handed to another process: a quicker sign than the pid.
        reparented = sys.platform != "win32" and original_parent == pid and os.getppid() != pid
        if reparented or not is_alive(pid):
            on_gone()
            return
        await asyncio.sleep(interval)
