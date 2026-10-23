"""Tokens (and cost, where the provider reports it) per month, connection and feature."""

from __future__ import annotations

import datetime
import json
from pathlib import Path
from typing import Any

from .gateway import Usage


class UsageLog:
    def __init__(self, folder: Path) -> None:
        self.file = folder / "usage.json"
        self.data: dict[str, Any] = {}
        if self.file.exists():
            self.data = json.loads(self.file.read_text(encoding="utf-8"))

    @staticmethod
    def month(now: datetime.date | None = None) -> str:
        return (now or datetime.date.today()).strftime("%Y-%m")

    def add(self, connection_id: str, feature: str, usage: Usage, now: datetime.date | None = None) -> None:
        entry = (
            self.data.setdefault(self.month(now), {})
            .setdefault(connection_id, {})
            .setdefault(feature, {"requests": 0, "inputTokens": 0, "outputTokens": 0, "cost": None})
        )
        entry["requests"] += 1
        entry["inputTokens"] += usage.input_tokens
        entry["outputTokens"] += usage.output_tokens
        if usage.cost is not None:
            entry["cost"] = (entry["cost"] or 0.0) + usage.cost
        self.file.parent.mkdir(parents=True, exist_ok=True)
        self.file.write_text(json.dumps(self.data, indent=2), encoding="utf-8")

    def get(self) -> dict[str, Any]:
        return self.data
