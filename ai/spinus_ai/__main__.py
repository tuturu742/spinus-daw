"""python -m spinus_ai --port-file FILE --token-file FILE --config FOLDER [--docs FOLDER] [--schema FILE]
[--models FOLDER] [--parent-pid PID]

Spinus starts it through run_spinus_ai.py, which also takes --log FILE."""

from __future__ import annotations

import argparse
import asyncio
import signal
import sys
from pathlib import Path

from .connections import KeyringUnavailable, KeyStore, MemoryKeyStore, SystemKeyStore
from .parent import watch_parent
from .server import serve
from .service import Service


def main() -> None:
    parser = argparse.ArgumentParser(prog="spinus-ai")
    parser.add_argument("--port-file", required=True, type=Path, help="where the port is written once listening")
    parser.add_argument("--token-file", required=True, type=Path, help="the token clients send first (read, deleted)")
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--docs", type=Path)
    parser.add_argument("--schema", type=Path)
    parser.add_argument("--models", type=Path, help="downloaded models (default ~/.local/share/spinus/models)")
    parser.add_argument("--parent-pid", type=int, help="stop when this process (Spinus) has ended")
    parser.add_argument("--memory-keys", action="store_true", help="keep keys in memory only (tests)")
    args = parser.parse_args()
    token = args.token_file.read_text().strip()
    args.token_file.unlink(missing_ok=True)

    note = ""
    keys: KeyStore
    if args.memory_keys:
        keys = MemoryKeyStore()
    else:
        try:
            keys = SystemKeyStore()
        except KeyringUnavailable as error:
            keys, note = MemoryKeyStore(), str(error) + " Keys are kept only until Spinus closes."

    async def run() -> None:
        service = Service(args.config, keys, args.docs, args.schema, keyring_note=note, models=args.models)
        loop = asyncio.get_running_loop()
        if sys.platform != "win32":  # Windows has no signal handlers in asyncio; Spinus sends ai.v1.shutdown
            for sig in (signal.SIGTERM, signal.SIGINT):
                loop.add_signal_handler(sig, service.stopping.set)
        watcher = None
        if args.parent_pid:
            watcher = asyncio.create_task(watch_parent(args.parent_pid, service.stopping.set))
        await serve(service, args.port_file, token)
        if watcher is not None:
            watcher.cancel()

    asyncio.run(run())


if __name__ == "__main__":
    main()
