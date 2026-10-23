"""Newline-delimited JSON-RPC 2.0 over TCP on 127.0.0.1 with a token."""

from __future__ import annotations

import asyncio
import contextlib
import hmac
import json
import os
from collections.abc import AsyncIterator
from pathlib import Path
from typing import Any

from .service import RpcError, Service

# The longest request read (Spinus sends long lists, such as every Surge XT preset and sample it might use).
MAX_REQUEST = 64 << 20


async def read_requests(reader: asyncio.StreamReader, limit: int) -> AsyncIterator[bytes | None]:
    """Yields each line, or None for one longer than `limit` (skipped up to its end)."""
    buffer = bytearray()
    skipping = False
    while chunk := await reader.read(1 << 20):
        buffer += chunk
        while (end := buffer.find(b"\n")) >= 0:
            line = bytes(buffer[:end])
            del buffer[: end + 1]
            if skipping:
                skipping = False
            elif len(line) > limit:
                yield None
            else:
                yield line
        if len(buffer) > limit:
            if not skipping:
                skipping = True
                yield None
            buffer.clear()
    if buffer and not skipping:
        yield bytes(buffer)


async def serve_client(
    service: Service, reader: asyncio.StreamReader, writer: asyncio.StreamWriter, limit: int = MAX_REQUEST
) -> None:
    lock = asyncio.Lock()

    async def send(message: dict[str, Any]) -> None:
        async with lock:
            writer.write(json.dumps(message, separators=(",", ":")).encode() + b"\n")
            await writer.drain()

    async def notify(method: str, params: dict[str, Any]) -> None:
        await send({"jsonrpc": "2.0", "method": method, "params": params})

    async def answer(request: dict[str, Any]) -> None:
        request_id = request.get("id")
        try:
            result = await service.handle(str(request.get("method", "")), dict(request.get("params") or {}), notify)
            if request_id is not None:
                await send({"jsonrpc": "2.0", "id": request_id, "result": result})
        except RpcError as error:
            if request_id is not None:
                await send({"jsonrpc": "2.0", "id": request_id, "error": {"code": error.code, "message": str(error)}})
        except Exception as error:
            if request_id is not None:
                await send({"jsonrpc": "2.0", "id": request_id, "error": {"code": -32603, "message": str(error)}})

    tasks: set[asyncio.Task[None]] = set()
    try:
        async for line in read_requests(reader, limit):
            if line is None:
                message = f"The request is longer than {limit >> 20} MB."
                await send({"jsonrpc": "2.0", "id": None, "error": {"code": -32600, "message": message}})
                continue
            if not line.strip():
                continue
            try:
                request = json.loads(line)
            except json.JSONDecodeError:
                await send({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "Parse error"}})
                continue
            task = asyncio.create_task(answer(request))
            tasks.add(task)
            task.add_done_callback(tasks.discard)
    finally:
        writer.close()


async def serve(service: Service, port_file: Path, token: str, limit: int = MAX_REQUEST) -> None:
    """Listens on 127.0.0.1 (a port the system picks, written to `port_file` once listening) for clients
    whose first line is `token`; others are disconnected."""

    clients: set[asyncio.StreamWriter] = set()

    async def client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            first = await asyncio.wait_for(reader.readline(), 10)
        except (TimeoutError, ValueError):
            first = b""
        if not hmac.compare_digest(first.strip(), token.encode()):
            writer.close()
            return
        clients.add(writer)
        try:
            await serve_client(service, reader, writer, limit)
        finally:
            clients.discard(writer)

    server = await asyncio.start_server(client, host="127.0.0.1", port=0, limit=1 << 20)
    port = server.sockets[0].getsockname()[1]
    # Written whole, then renamed, so Spinus never reads half a number.
    port_file.parent.mkdir(parents=True, exist_ok=True)
    partial = port_file.with_suffix(".partial")
    partial.write_text(str(port))
    os.replace(partial, port_file)
    service.start()
    await service.stopping.wait()
    # Stopping closes the connections too, rather than waiting for Spinus to: Python 3.12's Server waits for
    # every connection to end, and on Windows (the Proactor loop; seen under Wine) the end of one Spinus
    # already closed is not always noticed.
    server.close()
    for writer in list(clients):
        writer.close()
    with contextlib.suppress(TimeoutError):
        await asyncio.wait_for(server.wait_closed(), 2)
    await service.close()
    port_file.unlink(missing_ok=True)
