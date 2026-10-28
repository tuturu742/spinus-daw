"""The music harness (spinus_ai.harness) with scripted fake models, no API calls: a good run composes through the
tools and finishes, malformed calls come back as errors the model can fix, a model that never finishes (or is
stopped) leaves the best checked version, a revision starts from what Spinus heard, the request is cached and the
model's reasoning blocks go back unchanged, the MCP server serves the same tools, and the comparison with the
one-shot writer reports both ways' tokens and scores."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import httpx
import pytest
from test_service import make_service, no_notify, run_job
from test_songwriter import summary

from spinus_ai import compose, harness
from spinus_ai.connections import ConnectionStore, MemoryKeyStore
from spinus_ai.routing import Routing
from spinus_ai.usage import UsageLog

PARTS = [
    {"id": "drums", "name": "Drums", "role": "drums"},
    {"id": "bass", "name": "Bass", "role": "bass"},
    {"id": "keys", "name": "Piano", "role": "piano"},
    {"id": "lead", "name": "Lead", "role": "lead"},
]
BRIEF = {"summary": summary(), "parts": PARTS, "seed": 5, "arrangement": "sketch", "candidates": {}, "current": {}}


Script = list[list[tuple[str, dict[str, Any]]]]


def setup() -> compose.Setup:
    return harness.setup_of(BRIEF)


def scripted(tmp_path: Path, script: Script, sent: list[dict[str, Any]]) -> Any:
    model = harness.FakeModel(script, {})

    def handler(request: httpx.Request) -> httpx.Response:
        if not request.url.path.endswith("/count_tokens"):
            sent.append(json.loads(request.content))
        return model(request)

    return make_service(tmp_path, handler)


async def connect(service: Any) -> None:
    await service.handle(
        "ai.v1.connections.save", {"name": "Claude", "provider": "anthropic", "key": "sk-t"}, no_notify
    )


def params(**more: Any) -> dict[str, Any]:
    return {"summary": summary(), "seed": 5, "parts": PARTS, **more}


async def collect(service: Any, method: str, p: dict[str, Any]) -> tuple[Any, list[dict[str, Any]]]:
    import asyncio

    updates: list[dict[str, Any]] = []
    finished: asyncio.Future[Any] = asyncio.get_running_loop().create_future()

    async def notify(name: str, payload: dict[str, Any]) -> None:
        updates.append(payload)
        if payload.get("kind") == "done" and not finished.done():
            finished.set_result(payload["result"])
        elif payload.get("kind") == "error" and not finished.done():
            finished.set_exception(RuntimeError(payload["message"]))

    await service.handle(method, p, notify)
    return await asyncio.wait_for(finished, 60), updates


async def test_a_good_run_composes_through_the_tools_and_finishes(tmp_path: Path) -> None:
    ws = harness.Workspace.start(setup(), 5, "sketch")
    fake = harness.fake_runs(setup(), ws)
    sent: list[dict[str, Any]] = []
    service = scripted(tmp_path, fake.script, sent)
    await connect(service)
    song, updates = await collect(service, "ai.v1.song.compose", params(effort="medium"))

    composed = song["composed"]
    assert composed["harness"] and song["composedBy"] == "fake-model" and composed["reason"] == "A plain test song."
    assert composed["calls"]["write"] == len(ws.sections) and composed["calls"]["place"] == len(ws.sections)
    assert composed["calls"]["finish"] == 1 and composed["turns"] == len(fake.script)
    # The drums from a one-bar loop, over every bar; the bass from a motif fitted to the chords.
    first = next(iter(ws.sections))
    assert len(song["layerParts"]["drums"][first]) == 12 * ws.sections[first]["bars"]
    scale = {(int(song["key"]["tonic"]) + i) % 12 for i in (0, 2, 3, 5, 7, 8, 10, 0, 2, 4, 5, 7, 9, 11)}
    assert all(n["pitch"] % 12 in scale for n in song["layerParts"]["bass"][first])
    assert song["layerParts"]["keys"] and composed["byRules"] == ["keys", "lead"]
    # Every turn sent the request cached, with the tools; the history only grows (never edited).
    assert all(r.get("cache_control") == {"type": "ephemeral"} for r in sent)
    assert all(r["output_config"] == {"effort": "medium"} for r in sent)
    assert [len(r["messages"]) for r in sent] == [1 + 2 * k for k in range(len(sent))]
    assert {t["name"] for t in sent[0]["tools"]} == set(harness.TOOL_NAMES)
    assert "The brief:" in sent[0]["messages"][0]["content"][0]["text"]
    # Live progress in plain words, with the running total.
    steps = [u["step"] for u in updates if u.get("kind") == "progress"]
    assert any(s.startswith(f"Writing the drums for the {first}") for s in steps)
    assert any(s.startswith("Checking: ") and "tokens so far" in s for s in steps)
    usage = composed["usage"]
    assert usage["input"] == sum(r["input"] for r in usage["requests"]) and len(usage["requests"]) == len(sent)
    assert all(r["context_management"]["edits"][0]["type"] == "clear_tool_uses_20250919" for r in sent)

    # A revision starts from what Spinus heard, on the same working copy.
    sent.clear()
    service2 = scripted(
        tmp_path / "b",
        [
            [("edit", {"part": "bass", "section": first, "bars": "1", "notes": "0 45 4 100"})],
            [("finish", {"summary": "Fixed."})],
        ],
        sent,
    )
    await connect(service2)
    revised, _ = await collect(
        service2, "ai.v1.song.compose", params(song=song, levels={"bass": -100.0, "drums": -12.0})
    )
    opening = sent[0]["messages"][0]["content"][0]["text"]
    assert "bass made no sound" in opening and "drums -12.0 dB" in opening
    assert revised["composed"]["round"] == 1 and revised["layerParts"]["drums"] == song["layerParts"]["drums"]
    assert revised["layerParts"]["bass"][first][0]["pitch"] == 45
    assert [r["round"] for r in revised["composed"]["usage"]["requests"]][-1] == 1


async def test_malformed_calls_come_back_as_errors_to_fix(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    script: Script = [
        [("write", {"part": "kazoo", "section": "verse", "notes": "0 60 1 90"})],
        [("write", {"part": "bass", "section": "nowhere", "notes": "0 40 1 90"})],
        [("edit", {"part": "bass", "section": "verse", "bars": "9-99", "notes": "0 40 1 90"})],
        [("write", {"part": "bass", "section": "verse", "notes": "x y z w"})],
        [("place", {"part": "bass", "section": "verse", "motif": "none", "bar": 1})],
        [("write", {"part": "bass", "section": "verse", "notes": "0 40 1 90", "colour": "red"})],
        [("set_form", {"sections": [{"id": "a", "kind": "solo", "bars": 8, "chords": ["i"]}], "order": ["a"]})],
        [("dance", {})],
        [("finish", {"summary": "Done anyway."})],
    ]
    service = scripted(tmp_path, script, sent)
    await connect(service)
    song = await run_job(service, "ai.v1.song.compose", params())
    results = [r["messages"][-1]["content"][0] for r in sent[1:]]
    assert all(r["is_error"] for r in results)
    words = [r["content"] for r in results]
    assert "there is no part 'kazoo'" in words[0] and "there is no section 'nowhere'" in words[1]
    assert "the section has bars 1 to" in words[2] and "no note could be read" in words[3]
    assert "there is no motif 'none'" in words[4] and "Not done" in words[5]
    assert "'solo' is not a kind of section" in words[6] and "there is no tool 'dance'" in words[7]
    # The parts it never wrote are the rules'; the song is whole.
    assert song["composed"]["filled"] == ["drums", "bass", "keys", "lead"] and song["layerParts"]["bass"]


async def test_a_model_that_never_finishes_leaves_the_best_checked_version(tmp_path: Path, monkeypatch: Any) -> None:
    monkeypatch.setattr(harness, "MOST_TURNS", 6)
    sent: list[dict[str, Any]] = []
    good = [
        (
            "write",
            {
                "part": "drums",
                "section": s,
                "notes": "0 kick 1 100; 4 snare 1 100; 8 kick 1 100; "
                "12 snare 1 100; 0 hat 1 60; 4 hat 1 60; 8 hat 1 60; 12 hat 1 60",
                "loop_bars": 1,
            },
        )
        for s in harness.Workspace.start(setup(), 5, "sketch").sections
    ]
    script: Script = [
        good,
        [("check", {})],
        [("write", {"part": "bass", "section": "verse", "notes": "0 61 1 90; 4 63 1 90"})],
    ]
    script += [[("form", {})]] * 10
    service = scripted(tmp_path, script, sent)
    await connect(service)
    song = await run_job(service, "ai.v1.song.compose", params())
    assert len(sent) == 6 and "did not finish in 6 turns" in song["note"]
    # The best checked version: the drums, before the bass line full of notes outside the key.
    assert song["layerParts"]["drums"] and "bass" in song["composed"]["filled"]


async def test_stop_now_keeps_the_best_checked_version(tmp_path: Path) -> None:
    ws = harness.Workspace.start(setup(), 5, "sketch")
    fake = harness.fake_runs(setup(), ws)
    store = ConnectionStore(tmp_path, MemoryKeyStore())
    store.upsert("Fake", "anthropic", "https://fake.test", "k")
    routing = Routing(tmp_path, store)
    turns = {"n": 0}

    def handler(request: httpx.Request) -> httpx.Response:
        turns["n"] += 1
        return fake(request)

    folder = tmp_path / "run"
    if True:
        client = httpx.AsyncClient(transport=httpx.MockTransport(handler))

        async def say(_: dict[str, Any]) -> None:
            pass

        song = await harness.write(
            client,
            store,
            routing,
            UsageLog(Path(folder)),
            setup(),
            5,
            "sketch",
            "",
            say,
            lambda: turns["n"] >= 3,
        )
    assert turns["n"] == 3 and "as you stopped it" in song["note"] and song["layerParts"]["drums"]


async def test_reasoning_blocks_go_back_unchanged_in_the_loop(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []

    def reply(turn: int) -> bytes:
        call: tuple[str, dict[str, Any]] = ("form", {}) if turn == 0 else ("finish", {"summary": "x"})
        events: list[dict[str, Any]] = [
            {
                "type": "message_start",
                "message": {
                    "model": "m",
                    "usage": {"input_tokens": 10, "cache_read_input_tokens": 90, "cache_creation_input_tokens": 5},
                },
            },
            {"type": "content_block_start", "index": 0, "content_block": {"type": "thinking", "thinking": ""}},
            {
                "type": "content_block_delta",
                "index": 0,
                "delta": {"type": "signature_delta", "signature": f"sig{turn}"},
            },
            {"type": "content_block_stop", "index": 0},
            {
                "type": "content_block_start",
                "index": 1,
                "content_block": {"type": "tool_use", "id": f"u{turn}", "name": call[0]},
            },
            {
                "type": "content_block_delta",
                "index": 1,
                "delta": {"type": "input_json_delta", "partial_json": json.dumps(call[1])},
            },
            {"type": "content_block_stop", "index": 1},
            {"type": "message_delta", "delta": {"stop_reason": "tool_use"}, "usage": {"output_tokens": 20}},
        ]
        return "".join(f"event: {e['type']}\ndata: {json.dumps(e)}\n\n" for e in events).encode()

    def handler(request: httpx.Request) -> httpx.Response:
        sent.append(json.loads(request.content))
        return httpx.Response(200, content=reply(len(sent) - 1), headers={"content-type": "text/event-stream"})

    service = make_service(tmp_path, handler)
    await connect(service)
    song = await run_job(service, "ai.v1.song.compose", params())
    assert sent[1]["messages"][1]["content"][0] == {"type": "thinking", "thinking": "", "signature": "sig0"}
    usage = song["composed"]["usage"]
    assert usage["input"] == 2 * 105 and usage["cached"] == 2 * 90  # all the input, the cache's part said
    assert "180 of the input tokens were read from the provider's cache" in song["note"]


def test_the_mcp_server_serves_the_same_tools(tmp_path: Path) -> None:
    first = next(iter(harness.Workspace.start(setup(), 5, "sketch").sections))
    lines = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "tools/call",
            "params": {
                "name": "write",
                "arguments": {
                    "part": "drums",
                    "section": first,
                    "notes": "0 kick 1 100; 8 snare 1 100",
                    "loop_bars": 1,
                },
            },
        },
        {"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "write", "arguments": {"part": "x"}}},
        {
            "jsonrpc": "2.0",
            "id": 5,
            "method": "tools/call",
            "params": {"name": "finish", "arguments": {"summary": "s"}},
        },
    ]
    feed = [json.dumps(x) + "\n" for x in lines]
    out: list[dict[str, Any]] = []
    harness.serve_mcp(
        BRIEF, tmp_path / "song.json", lambda: feed.pop(0) if feed else "", lambda text: out.append(json.loads(text))
    )
    assert [o["id"] for o in out] == [1, 2, 3, 4, 5]
    assert "music harness" in out[0]["result"]["instructions"]
    assert [t["name"] for t in out[1]["result"]["tools"]] == harness.TOOL_NAMES
    assert out[2]["result"]["isError"] is False and "notes" in out[2]["result"]["content"][0]["text"]
    assert out[3]["result"]["isError"] is True
    saved = json.loads((tmp_path / "song.json").read_text())
    assert saved["kind"] == "spinus.song" and saved["layerParts"]["drums"][first]


async def test_the_comparison_reports_both_ways(tmp_path: Path) -> None:
    ws = harness.Workspace.start(setup(), 5, "sketch")
    client = httpx.AsyncClient(transport=httpx.MockTransport(harness.fake_runs(setup(), ws)))
    store = ConnectionStore(tmp_path, MemoryKeyStore())
    store.upsert("Fake", "anthropic", "https://fake.test", "k")
    rows = await harness.compare_ways(BRIEF, client, store, Routing(tmp_path, store), tmp_path, ["", "high"])
    assert [(r["way"], r["effort"]) for r in rows] == [
        ("one-shot", "default"),
        ("harness", "default"),
        ("one-shot", "high"),
        ("harness", "high"),
    ]
    one, tools = rows[0], rows[1]
    assert one["usedAi"] and tools["usedAi"] and one["requests"] == 1 and tools["requests"] > 1
    assert tools["output"] < one["output"]  # loops, motifs and the rules instead of every note spelled out
    assert {"issues", "notesInKey", "notesFitChords", "groove"} <= set(tools)
    assert "way" in harness.table(rows).splitlines()[0]


def test_a_brief_from_a_description(tmp_path: Path) -> None:
    harness.main(
        [
            "brief",
            "--describe",
            "a sunny indie pop song with guitar",
            "--parts",
            "drums:drums,gtr:rhythm-guitar",
            "-o",
            str(tmp_path / "b.json"),
        ]
    )
    brief = json.loads((tmp_path / "b.json").read_text())
    assert brief["summary"]["kind"] == "spinus.style-summary" and brief["parts"][1]["role"] == "rhythm-guitar"
    with pytest.raises(SystemExit):
        harness.main(["eval", "--brief", str(tmp_path / "b.json"), "--key-env", "SPINUS_NO_SUCH_KEY"])


def test_old_tool_calls_are_cleared_by_the_provider_past_the_threshold() -> None:
    """The fake applies Anthropic's documented clear_tool_uses_20250919 rule; the gateway reads its report."""
    calls, results = [], []
    for k in range(20):
        calls.append({"type": "tool_use", "id": f"u{k}", "name": "write", "input": {"notes": "0 60 4 90; " * 60}})
        results.append({"type": "tool_result", "tool_use_id": f"u{k}", "content": "bass: 60 notes." * 20})
    messages: list[dict[str, Any]] = [{"role": "user", "content": [{"type": "text", "text": "brief"}]}]
    for call, result in zip(calls, results, strict=True):
        messages += [{"role": "assistant", "content": [call]}, {"role": "user", "content": [result]}]
    body: dict[str, Any] = {"messages": messages, "context_management": {"edits": harness.context_edits(3000)}}
    whole, nothing = harness.cleared_size({**body, "context_management": {"edits": harness.context_edits(10**9)}})
    cut, cleared = harness.cleared_size(body)
    assert nothing == 0 and cleared > whole / 3 and abs(cut - (whole - cleared)) <= 2  # the trigger's digits differ
    assert harness.context_edits(0) == []


async def test_the_gateway_sends_context_editing_and_reads_what_was_cleared() -> None:
    from spinus_ai.connections import Connection
    from spinus_ai.gateway import Message, Request, Text, complete

    seen: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        seen.append(request)
        events = [
            {"type": "message_start", "message": {"model": "m", "usage": {"input_tokens": 100}}},
            {
                "type": "message_delta",
                "delta": {"stop_reason": "end_turn"},
                "usage": {"output_tokens": 1},
                "context_management": {
                    "applied_edits": [
                        {"type": "clear_tool_uses_20250919", "cleared_tool_uses": 8, "cleared_input_tokens": 5000}
                    ]
                },
            },
        ]
        data = "".join(f"event: {e['type']}\ndata: {json.dumps(e)}\n\n" for e in events).encode()
        return httpx.Response(200, content=data, headers={"content-type": "text/event-stream"})

    async def no_text(_: str) -> None:
        pass

    client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    request = Request("m", "s", [Message("user", [Text("hi")])], [], None, context_edits=harness.context_edits(30000))
    done = await complete(client, Connection("a", "A", "anthropic", "https://y.test"), "k", request, no_text)
    assert done.usage.cleared_tokens == 5000
    assert seen[0].headers["anthropic-beta"] == "context-management-2025-06-27"
    assert json.loads(seen[0].content)["context_management"]["edits"][0]["trigger"]["value"] == 30000
