"""The AI service's session log: redaction (the same rules as Spinus's, tests/core/LogTests.cpp) and the file."""

from __future__ import annotations

import asyncio
import json
import logging
import subprocess
import sys
import time
from collections.abc import Iterator
from pathlib import Path

import pytest

from spinus_ai import logs
from spinus_ai.connections import MemoryKeyStore
from spinus_ai.logs import Redactor

RUNNER = Path(__file__).resolve().parents[1] / "run_spinus_ai.py"


def fake_key(prefix: str) -> str:
    """Made from pieces, so no text that looks like a real key is in the repository."""
    return prefix + "Ab9_" * 10


ANTHROPIC = fake_key("sk-" + "ant-" + "api03-")
OPENROUTER = fake_key("sk-" + "or-" + "v1-")


def test_home_folder_becomes_tilde() -> None:
    r = Redactor(["/home/alice"])
    assert r.apply("Opened /home/alice/Music/Song/Song.spn") == "Opened ~/Music/Song/Song.spn"
    assert r.apply("in /home/alice") == "in ~"
    assert r.apply("Saved in /home/alice.") == "Saved in ~."
    assert r.apply("file:///home/alice/x.wav") == "file://~/x.wav"
    assert r.apply("/home/alice2/x") == "/home/<user>/x"
    assert r.apply("/mnt/home/alice/x") == "/mnt/home/<user>/x"
    assert Redactor(["/home/ден"]).apply("/home/ден/Music") == "~/Music"
    assert Redactor([]).apply("/home/ден/Music") == "/home/<user>/Music"
    assert Redactor(["/"]).apply("/usr/lib/spinus") == "/usr/lib/spinus"


def test_windows_user_folders() -> None:
    r = Redactor(["C:\\Users\\Alice"])
    assert r.apply("C:\\Users\\Alice\\Music\\Song.spn") == "~\\Music\\Song.spn"
    assert r.apply("c:\\users\\alice\\Music") == "~\\Music"
    assert r.apply("C:/Users/Alice/AppData/Roaming/Spinus DAW") == "~/AppData/Roaming/Spinus DAW"
    assert r.apply('{"path": "C:\\\\Users\\\\Alice\\\\Music"}') == '{"path": "~\\\\Music"}'
    none = Redactor([])
    assert none.apply("D:\\Users\\Jo Ann\\Music\\x.wav") == "D:\\Users\\<user>\\Music\\x.wav"
    assert none.apply("C:\\Users\\Bob") == "C:\\Users\\<user>"
    assert none.apply("C:\\Users\\Bob is here") == "C:\\Users\\<user> is here"
    assert none.apply("prefix/drive_c/users/alice/AppData") == "prefix/drive_c/users/<user>/AppData"
    assert none.apply("C:\\Users\\Public\\Documents") == "C:\\Users\\Public\\Documents"
    assert none.apply("3 users/day and /srv/users/x") == "3 users/day and /srv/users/x"


def test_keys_and_tokens() -> None:
    r = Redactor([])
    assert r.apply(f"key {ANTHROPIC} refused") == "key sk-ant-*** refused"
    assert r.apply(f"({OPENROUTER})") == "(sk-or-***)"
    assert r.apply("OpenAI " + fake_key("sk-" + "proj-")) == "OpenAI sk-***"
    assert r.apply("disk-cache-folder-for-the-render") == "disk-cache-folder-for-the-render"
    assert r.apply("task-1 and sk-12") == "task-1 and sk-12"
    assert r.apply("Authorization: Bearer abc.def-ghi_123=") == "Authorization: Bearer ***"
    assert r.apply("header bearer xyz/123+") == "header bearer ***"
    assert r.apply("x-api-key: abcdef123456") == "x-api-key: ***"
    assert r.apply('{"apiKey": "abcdef123456", "model": "m"}') == '{"apiKey": "***", "model": "m"}'
    assert r.apply("url?api_key=abc123&x=1") == "url?api_key=***&x=1"
    assert r.apply("access_token=abc password = 'hunter22'") == "access_token=*** password = '***'"
    assert r.apply("Authorization: Basic dXNlcjpwYXNz") == "Authorization: Basic ***"
    assert r.apply("token file missing; 120 tokens: fine; Secret Service is off") == (
        "token file missing; 120 tokens: fine; Secret Service is off"
    )
    assert r.apply("key: C minor") == "key: C minor"
    r.add_secret("0123456789abcdef0123")
    r.add_secret("short")
    assert r.apply("token file says 0123456789abcdef0123.") == "token file says ***."
    assert r.apply("a short line") == "a short line"


def test_email_addresses() -> None:
    r = Redactor([])
    assert r.apply("Signed in as jane.doe+spinus@example.co.uk.") == "Signed in as <email>."
    assert r.apply("<a.b@c.io>") == "<<email>>"
    assert r.apply("plugin@1.2.30 and user@localhost and @mention") == "plugin@1.2.30 and user@localhost and @mention"


def test_everything_in_one_line() -> None:
    assert Redactor(["/home/alice"]).apply(f"alice@example.com saved /home/alice/x with {ANTHROPIC}") == (
        "<email> saved ~/x with sk-ant-***"
    )


@pytest.fixture
def session_log(tmp_path: Path) -> Iterator[Path]:
    path = tmp_path / "logs" / "spinus-2026-10-24_14-03-12-ai.log"
    hook = sys.excepthook
    handler = logs.setup(path, max_bytes=4096)
    try:
        yield path
    finally:
        logs.log.removeHandler(handler)
        handler.close()
        sys.excepthook = hook


def test_the_log_file_is_redacted_flushed_and_capped(session_log: Path) -> None:
    keys = logs.RedactedKeys(MemoryKeyStore())
    keys.set("c1", "my-own-provider-key-1234")
    logging.getLogger("spinus_ai.test").warning("Refused my-own-provider-key-1234 and %s", ANTHROPIC)
    try:
        raise ValueError("bad value in /home/someone/file.wav")
    except ValueError:
        logging.getLogger("spinus_ai.test").exception("A job failed")
    text = session_log.read_text(encoding="utf-8")  # on disk at once
    assert "Spinus AI service" in text.splitlines()[0]
    assert " WARN  spinus_ai.test: Refused *** and sk-ant-***" in text
    assert "my-own-provider-key" not in text and "api03" not in text
    assert "ValueError: bad value in /home/<user>/file.wav" in text
    for i in range(200):
        logs.log.info("line %d %s", i, "x" * 40)
    capped = session_log.read_text(encoding="utf-8")
    assert "The log reached its size limit" in capped
    assert session_log.stat().st_size < 4096 + 512


def test_keys_read_from_the_keyring_are_masked() -> None:
    store = MemoryKeyStore()
    store.set("c2", "stored-before-this-run-5678")
    assert logs.RedactedKeys(store).get("c2") == "stored-before-this-run-5678"
    assert logs.redactor.apply("key stored-before-this-run-5678") == "key ***"


def test_the_service_writes_its_session_log(tmp_path: Path) -> None:
    """Started as Spinus starts it: the log has the start, a failed connection test and the stop, and no key."""
    port_file, token_file = tmp_path / "service.port", tmp_path / "service.token"
    token = "0123456789abcdef" * 2
    token_file.write_text(token)
    session_log = tmp_path / "logs" / "spinus-2026-10-24_14-03-12-ai.log"
    service = subprocess.Popen(
        [
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
            "--session-log",
            str(session_log),
        ]
    )
    try:
        deadline = time.monotonic() + 30
        while not port_file.exists():
            assert time.monotonic() < deadline, (tmp_path / "service.log").read_text()
            time.sleep(0.1)

        async def talk() -> None:
            reader, writer = await asyncio.open_connection("127.0.0.1", int(port_file.read_text()))
            writer.write(token.encode() + b"\n")

            async def call(number: int, method: str, params: dict[str, object]) -> dict[str, object]:
                message = {"jsonrpc": "2.0", "id": number, "method": method, "params": params}
                writer.write(json.dumps(message).encode() + b"\n")
                await writer.drain()
                reply: dict[str, object] = json.loads(await reader.readline())
                return reply

            saved = await call(
                1,
                "ai.v1.connections.save",
                # Nothing listens on port 9 here: the test fails at once, with the address in the error.
                {"provider": "openai", "name": "Work", "key": ANTHROPIC, "baseUrl": "http://127.0.0.1:9/v1"},
            )
            connection = saved["result"]
            assert isinstance(connection, dict)
            await call(2, "ai.v1.connections.test", {"id": connection["id"]})
            await call(3, "ai.v1.nope", {"prompt": "my secret song idea"})
            await call(4, "ai.v1.shutdown", {})
            writer.close()

        asyncio.run(talk())
        service.wait(15)
    finally:
        if service.poll() is None:
            service.kill()
    text = session_log.read_text(encoding="utf-8")
    assert "Spinus AI service" in text
    assert "Listening on 127.0.0.1:" in text
    assert "Connection test of Work (openai) failed: network" in text
    assert "ai.v1.nope failed: Unknown method ai.v1.nope" in text
    assert "The AI service stopped" in text
    assert "api03" not in text and token not in text
    assert "my secret song idea" not in text  # what was asked is never logged
