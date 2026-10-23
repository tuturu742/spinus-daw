"""Which connection and model each feature uses, with a fallback chain."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

from .connections import Connection, ConnectionStore

FEATURES = ("assistant", "patterns", "mixing")

# Used when a connection has never been tested (so its model list is unknown).
DEFAULT_MODELS = {"anthropic": "claude-sonnet-5-5", "openrouter": "openrouter/auto"}


@dataclass
class Route:
    connection: Connection
    model: str


def default_model(connection: Connection) -> str | None:
    """A sensible model for a connection: a Claude Sonnet where there is one, else the first listed."""
    models = connection.models
    for wanted in ("claude-sonnet-5-5", "anthropic/claude-sonnet-5.5", "sonnet"):
        for m in models:
            if wanted in m:
                return m
    if models:
        return models[0]
    return DEFAULT_MODELS.get(connection.provider)


class Routing:
    def __init__(self, folder: Path, connections: ConnectionStore) -> None:
        self.file = folder / "routing.json"
        self.connections = connections
        self.chains: dict[str, list[dict[str, str]]] = {}
        if self.file.exists():
            self.chains = json.loads(self.file.read_text(encoding="utf-8"))

    def get(self) -> dict[str, list[dict[str, str]]]:
        return {f: list(self.chains.get(f, [])) for f in FEATURES}

    def set(self, feature: str, chain: list[dict[str, str]]) -> None:
        if feature not in FEATURES:
            raise ValueError(f"Unknown feature '{feature}'.")
        for step in chain:
            if self.connections.find(step.get("connection", "")) is None:
                raise ValueError(f"Unknown connection '{step.get('connection')}'.")
        self.chains[feature] = [{"connection": s["connection"], "model": s.get("model", "")} for s in chain]
        self.file.parent.mkdir(parents=True, exist_ok=True)
        self.file.write_text(json.dumps(self.chains, indent=2), encoding="utf-8")

    def resolve(self, feature: str) -> list[Route]:
        """The connections to try, in order. Without a chosen chain: the assistant's (for other features), else
        every connection, in the order added."""
        routes: list[Route] = []
        chain = (
            self.chains.get(feature)
            or (self.chains.get("assistant") if feature != "assistant" else None)
            or [{"connection": c.id, "model": ""} for c in self.connections.connections]
        )
        for step in chain:
            connection = self.connections.find(step["connection"])
            if connection is None:
                continue
            model = step.get("model") or default_model(connection)
            if model:
                routes.append(Route(connection, model))
        return routes
