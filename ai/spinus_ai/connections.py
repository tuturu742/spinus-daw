"""Named connections to model providers: settings in connections.json, keys only in the
system keyring."""

from __future__ import annotations

import contextlib
import json
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Protocol

PROVIDERS = ("anthropic", "openrouter", "openai", "local")

DEFAULT_BASE_URLS = {
    "anthropic": "https://api.anthropic.com",
    "openrouter": "https://openrouter.ai/api/v1",
    "openai": "https://api.openai.com/v1",
    "local": "http://localhost:11434/v1",  # Ollama; llama.cpp's server is usually :8080/v1
}

KEYRING_SERVICE = "Spinus DAW"


class KeyStore(Protocol):
    def get(self, connection_id: str) -> str | None: ...
    def set(self, connection_id: str, key: str) -> None: ...
    def delete(self, connection_id: str) -> None: ...


class KeyringUnavailable(RuntimeError):
    """No system keyring (e.g. no Secret Service running): keys cannot be stored safely."""


class SystemKeyStore:
    """The desktop's keyring (Secret Service on Linux) through the keyring package."""

    def __init__(self) -> None:
        import keyring
        from keyring.backends import fail

        self._keyring = keyring
        if isinstance(keyring.get_keyring(), fail.Keyring):
            raise KeyringUnavailable("No system keyring is available to store keys safely.")

    def get(self, connection_id: str) -> str | None:
        value = self._keyring.get_password(KEYRING_SERVICE, connection_id)
        return value if isinstance(value, str) else None

    def set(self, connection_id: str, key: str) -> None:
        self._keyring.set_password(KEYRING_SERVICE, connection_id, key)

    def delete(self, connection_id: str) -> None:
        with contextlib.suppress(Exception):  # nothing stored is fine
            self._keyring.delete_password(KEYRING_SERVICE, connection_id)


class MemoryKeyStore:
    """For tests and for running without a keyring (keys forgotten when the service stops)."""

    def __init__(self) -> None:
        self.keys: dict[str, str] = {}

    def get(self, connection_id: str) -> str | None:
        return self.keys.get(connection_id)

    def set(self, connection_id: str, key: str) -> None:
        self.keys[connection_id] = key

    def delete(self, connection_id: str) -> None:
        self.keys.pop(connection_id, None)


@dataclass
class Connection:
    id: str
    name: str
    provider: str
    base_url: str
    models: list[str] = field(default_factory=list)  # filled by a connection test

    def public(self, has_key: bool) -> dict[str, object]:
        data = asdict(self)
        data["baseUrl"] = data.pop("base_url")
        data["hasKey"] = has_key
        return data


class ConnectionStore:
    def __init__(self, folder: Path, keys: KeyStore) -> None:
        self.file = folder / "connections.json"
        self.keys = keys
        self.connections: list[Connection] = []
        self.load()

    def load(self) -> None:
        if not self.file.exists():
            self.connections = []
            return
        data = json.loads(self.file.read_text(encoding="utf-8"))
        self.connections = [
            Connection(c["id"], c["name"], c["provider"], c["baseUrl"], list(c.get("models", [])))
            for c in data.get("connections", [])
        ]

    def save(self) -> None:
        self.file.parent.mkdir(parents=True, exist_ok=True)
        data = {"connections": [c.public(False) for c in self.connections]}
        for c in data["connections"]:
            c.pop("hasKey")
        tmp = self.file.with_suffix(".tmp")
        tmp.write_text(json.dumps(data, indent=2), encoding="utf-8")
        tmp.replace(self.file)

    def find(self, connection_id: str) -> Connection | None:
        return next((c for c in self.connections if c.id == connection_id), None)

    def key_for(self, connection: Connection) -> str | None:
        return self.keys.get(connection.id)

    def needs_key(self, connection: Connection) -> bool:
        return connection.provider != "local"

    def upsert(
        self, name: str, provider: str, base_url: str | None, key: str | None, connection_id: str | None = None
    ) -> Connection:
        if provider not in PROVIDERS:
            raise ValueError(f"Unknown provider '{provider}'.")
        name = name.strip() or provider.capitalize()
        url = (base_url or DEFAULT_BASE_URLS[provider]).rstrip("/")
        existing = self.find(connection_id) if connection_id else None
        if existing is None:
            existing = Connection(connection_id or uuid.uuid4().hex[:12], name, provider, url)
            self.connections.append(existing)
        else:
            if existing.provider != provider or existing.base_url != url:
                existing.models = []
            existing.name, existing.provider, existing.base_url = name, provider, url
        if key:
            self.keys.set(existing.id, key.strip())
        self.save()
        return existing

    def remove(self, connection_id: str) -> bool:
        connection = self.find(connection_id)
        if connection is None:
            return False
        self.connections.remove(connection)
        self.keys.delete(connection_id)
        self.save()
        return True
