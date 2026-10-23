"""Against the real providers, with keys from the environment (never stored):

    SPINUS_TEST_ANTHROPIC_KEY=... SPINUS_TEST_OPENROUTER_KEY=... uv run pytest tests/test_real_providers.py -s

Skipped without keys. Uses small, cheap models; a run costs well under a cent."""

from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path
from typing import Any

import pytest

from spinus_ai.connections import MemoryKeyStore
from spinus_ai.service import Service

ROOT = Path(__file__).resolve().parents[2]
EXAMPLE = json.loads((ROOT / "tests/model/files/example-v1.json").read_text())


async def ask(service: Service, question: str) -> dict[str, Any]:
    updates: list[dict[str, Any]] = []
    done = asyncio.Event()

    async def notify(name: str, payload: dict[str, Any]) -> None:
        updates.append(payload)
        if payload.get("kind") in ("done", "error"):
            done.set()

    await service.handle(
        "ai.v1.assistant.ask",
        {"conversation": [{"role": "user", "text": question}], "context": {"project": EXAMPLE}},
        notify,
    )
    await asyncio.wait_for(done.wait(), 120)
    for u in updates:
        if u["kind"] == "status":
            print("  status:", u["text"])
    return updates[-1]


async def check(tmp_path: Path, provider: str, key: str, pick: str) -> None:
    async def no_notify(name: str, payload: dict[str, Any]) -> None:
        pass

    service = Service(
        tmp_path,
        MemoryKeyStore(),
        ROOT / "docs/user",
        ROOT / "docs/dev/commands.schema.json",
        models=tmp_path / "models",
    )
    saved = await service.handle(
        "ai.v1.connections.save", {"name": provider, "provider": provider, "key": key}, no_notify
    )
    tested = await service.handle("ai.v1.connections.test", {"id": saved["id"]}, no_notify)
    assert tested["ok"], tested["message"]
    model = next(m for m in tested["models"] if pick in m)
    await service.handle(
        "ai.v1.routing.set", {"feature": "assistant", "chain": [{"connection": saved["id"], "model": model}]}, no_notify
    )

    final = await ask(
        service,
        "What is the tempo of my song, and how many channels does it have? Then propose setting the tempo to 128 BPM.",
    )
    assert final["kind"] == "done", final
    result = final["result"]
    print(f"{provider} / {result['model']}: {result['text']}\n  usage: {result['usage']}")
    assert str(int(EXAMPLE["settings"]["tempoBpm"])) in result["text"]
    assert result["proposals"], "expected a proposal"
    step = result["proposals"][0]["steps"][0]
    print("  proposal:", json.dumps(result["proposals"]))
    assert step["command"] == "project.setTempo" and float(step["parameters"]["bpm"]) == 128
    await service.close()


@pytest.mark.skipif(not os.environ.get("SPINUS_TEST_ANTHROPIC_KEY"), reason="no Anthropic key")
async def test_anthropic(tmp_path: Path) -> None:
    await check(tmp_path, "anthropic", os.environ["SPINUS_TEST_ANTHROPIC_KEY"], "haiku")


@pytest.mark.skipif(not os.environ.get("SPINUS_TEST_OPENROUTER_KEY"), reason="no OpenRouter key")
async def test_openrouter(tmp_path: Path) -> None:
    await check(tmp_path, "openrouter", os.environ["SPINUS_TEST_OPENROUTER_KEY"], "anthropic/claude-haiku")
