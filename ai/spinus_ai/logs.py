"""The AI service's session log: one file per run of Spinus, beside Spinus's own (Spinus passes
--session-log <its log>-ai.log), through Python's logging.

Every line is redacted before it is written, with the same rules as Spinus's (src/core/Log.cpp): the home
folder becomes ~, other users' folders lose their names, API keys and tokens and email addresses are masked,
and so is every key the service reads from or stores in the keyring. Prompts, the user's text and answers are
never logged: only what the service does (it started, a connection failed, a job failed) and errors.

Each line is written and flushed at once, so the lines before a crash are on disk. The file stops growing
at a size limit, with a note.
"""

from __future__ import annotations

import asyncio
import logging
import platform
import re
import sys
import threading
import types
from pathlib import Path
from typing import Any

from . import __version__
from .connections import KeyStore

LOGGER = "spinus_ai"
MAX_BYTES = 4 * 1024 * 1024

log = logging.getLogger(LOGGER)

_TOKEN = r"[A-Za-z0-9_-]"
_NAMES = "api_key|api-key|apikey|authorization|access_token|refresh_token|token|client_secret|secret|password|passwd"
_NAMED_VALUE = re.compile(
    rf"(?<![A-Za-z0-9])((?:{_NAMES})[\"']? *[:=] *[\"']?(?:(?:bearer|basic) +|(?!(?:bearer|basic) )))"
    rf"(?!\*\*\*)([^\s\"',;&}})]+)",
    re.IGNORECASE,
)
_KEY_ANTHROPIC = re.compile(rf"(?<![A-Za-z0-9])(sk-ant-){_TOKEN}+")
_KEY_OPENROUTER = re.compile(rf"(?<![A-Za-z0-9])(sk-or-){_TOKEN}+")
_KEY_OTHER = re.compile(rf"(?<![A-Za-z0-9])(sk-){_TOKEN}{{20,}}")
_BEARER = re.compile(r"(?<![A-Za-z0-9])(bearer[ \t]+)(?!\*\*\*)[A-Za-z0-9._~+/=-]+", re.IGNORECASE)
_NAME_END = r"[^/\\ \t\r\n\"'<>:;,)\]}|*?]"
_HOME_FOLDER = re.compile(rf"(/home/)(?!<user>)({_NAME_END}+)")
_WINDOWS_USER = re.compile(rf"((?:[A-Za-z]:|drive_c)[/\\]+users[/\\]+)((?:{_NAME_END}| )+)", re.IGNORECASE)
_SHARED_FOLDERS = {"public", "default", "all users", "<user>"}
_EMAIL = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}(?![A-Za-z0-9-])")


class Redactor:
    """Masks the home folder, keys, tokens and email addresses in a line of text."""

    def __init__(self, homes: list[str] | None = None) -> None:
        self.homes: list[str] = []
        self.secrets: set[str] = set()
        self.lock = threading.Lock()
        for home in homes if homes is not None else [str(Path.home())]:
            home = home.rstrip("/\\")
            if len(home) < 3:  # "/" or "C:" would mask every path
                continue
            for spelling in (
                home,
                home.replace("\\", "/"),
                home.replace("/", "\\"),
                home.replace("/", "\\").replace("\\", "\\\\"),
            ):
                if spelling not in self.homes:
                    self.homes.append(spelling)
        self.homes.sort(key=len, reverse=True)

    def add_secret(self, secret: str | None) -> None:
        if secret is not None and len(secret.strip()) >= 8:
            with self.lock:
                self.secrets.add(secret.strip())

    def apply(self, text: str) -> str:
        with self.lock:
            secrets = sorted(self.secrets, key=len, reverse=True)
        for secret in secrets:
            text = text.replace(secret, "***")
        text = _KEY_ANTHROPIC.sub(r"\1***", text)
        text = _KEY_OPENROUTER.sub(r"\1***", text)
        text = _KEY_OTHER.sub(r"\1***", text)
        text = _BEARER.sub(r"\1***", text)
        text = _NAMED_VALUE.sub(self._named, text)
        for home in self.homes:
            text = self._replace_home(text, home)
        text = _HOME_FOLDER.sub(r"\1<user>", text)
        text = _WINDOWS_USER.sub(self._windows_user, text)
        return _EMAIL.sub("<email>", text)

    @staticmethod
    def _named(match: re.Match[str]) -> str:
        return match.group(1) + "***"

    @staticmethod
    def _windows_user(match: re.Match[str]) -> str:
        # A name may have spaces ("C:\\Users\\Jo Ann\\Music") as far as the next separator; without one it ends
        # at a space.
        name, rest = match.group(2), ""
        after = match.string[match.end() : match.end() + 1]
        if after not in ("/", "\\") and " " in name:
            name, space, tail = name.partition(" ")
            rest = space + tail
        if not name or name.lower() in _SHARED_FOLDERS:
            return match.group(0)
        return match.group(1) + "<user>" + rest

    @staticmethod
    def _replace_home(text: str, home: str) -> str:
        windows = len(home) >= 2 and home[1] == ":"
        pattern = re.compile(
            r"(?<![A-Za-z0-9_.-])" + re.escape(home) + r"(?=$|[^A-Za-z0-9_.\-\x80-\U0010ffff]|\.(?:$|[ \n]))",
            re.IGNORECASE if windows else 0,
        )
        return pattern.sub("~", text)


redactor = Redactor()


def add_secret(secret: str | None) -> None:
    """Masks `secret` in every later line (a key, a token)."""
    redactor.add_secret(secret)


class RedactingFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        return redactor.apply(super().format(record))


class SessionLogHandler(logging.FileHandler):
    """Appends to the run's file, flushing each line; past `max_bytes` lines are left out, with a note."""

    def __init__(self, path: Path, max_bytes: int = MAX_BYTES) -> None:
        super().__init__(path, mode="a", encoding="utf-8")
        self.max_bytes = max_bytes
        self.full = False

    def emit(self, record: logging.LogRecord) -> None:
        if self.full:
            return
        try:
            if self.stream is not None and self.stream.tell() >= self.max_bytes:
                self.full = True
                self.stream.write(
                    f"The log reached its size limit ({self.max_bytes >> 20} MB): later lines are left out.\n"
                )
                self.flush()
                return
        except OSError:
            return
        super().emit(record)  # StreamHandler flushes after each line


class RedactedKeys:
    """A key store whose keys are masked in the log wherever they would appear."""

    def __init__(self, keys: KeyStore) -> None:
        self.keys = keys

    def get(self, connection_id: str) -> str | None:
        key = self.keys.get(connection_id)
        add_secret(key)
        return key

    def set(self, connection_id: str, key: str) -> None:
        add_secret(key)
        self.keys.set(connection_id, key)

    def delete(self, connection_id: str) -> None:
        self.keys.delete(connection_id)


def setup(path: Path, max_bytes: int = MAX_BYTES) -> logging.Handler:
    """Starts logging to `path`: a header, then a line per event; uncaught errors are logged too."""
    path.parent.mkdir(parents=True, exist_ok=True)
    handler = SessionLogHandler(path, max_bytes)
    handler.setFormatter(
        RedactingFormatter("%(asctime)s.%(msecs)03d %(levelname)-5s %(name)s: %(message)s", "%Y-%m-%d %H:%M:%S")
    )
    logging.addLevelName(logging.WARNING, "WARN")
    log.addHandler(handler)
    log.setLevel(logging.INFO)
    log.propagate = False
    log.info(
        "Spinus AI service %s started (Python %s, %s)",
        __version__,
        platform.python_version(),
        platform.platform(terse=True),
    )

    def uncaught(kind: type[BaseException], error: BaseException, trace: types.TracebackType | None) -> None:
        log.critical("The AI service stops: nothing caught this error", exc_info=(kind, error, trace))

    sys.excepthook = uncaught
    return handler


def log_loop_errors(loop: asyncio.AbstractEventLoop) -> None:
    """Errors in tasks nobody waited for (asyncio's own handler would print them to stderr only)."""

    def handler(loop: asyncio.AbstractEventLoop, context: dict[str, Any]) -> None:
        error = context.get("exception")
        message = str(context.get("message", "an error in a background task"))
        if isinstance(error, BaseException):
            log.error("%s", message, exc_info=error)
        else:
            log.error("%s", message)
        loop.default_exception_handler(context)

    loop.set_exception_handler(handler)
