"""The music harness (spinus_ai.harness) with scripted fake models, no API calls: a good run composes through the
tools and finishes, malformed calls come back as errors the model can fix, a model that never finishes (or is
stopped) leaves the best checked version, a revision starts from what Spinus heard, the request is cached and the
model's reasoning blocks go back unchanged, the MCP server serves the same tools, and the comparison with the
one-shot writer reports both ways' tokens and scores."""

from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any

import httpx
import pytest
from test_service import make_service, no_notify, run_job
from test_songwriter import summary

from spinus_ai import compose, harness
from spinus_ai import songwriter as sw
from spinus_ai.connections import ConnectionStore, MemoryKeyStore
from spinus_ai.routing import Routing
from spinus_ai.service import RpcError
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
    # The plain song still has open issues: finish was refused once with them, and the second finish took it.
    assert composed["calls"]["finish"] == 2 and composed["turns"] == len(fake.script)
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
    assert {t["name"] for t in sent[0]["tools"]} == {t.name for t in harness.tools_for(harness.DEFAULT_DEPTH)}
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
        [("set_form", {"sections": [{"id": "a", "kind": "kazoo", "bars": 8, "chords": ["i"]}], "order": ["a"]})],
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
    assert "'kazoo' is not a kind of section" in words[6] and "there is no tool 'dance'" in words[7]
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
    song = await run_job(service, "ai.v1.song.compose", params(tools="basic"))
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
    # form, then finish refused once (open issues), then finish: three requests.
    assert usage["input"] == 3 * 105 and usage["cached"] == 3 * 90  # all the input, the cache's part said
    assert "270 of the input tokens were read from the provider's cache" in song["note"]


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
        {  # refused once with the open issues: finish again
            "jsonrpc": "2.0",
            "id": 6,
            "method": "tools/call",
            "params": {"name": "finish", "arguments": {"summary": "s"}},
        },
    ]
    feed = [json.dumps(x) + "\n" for x in lines]
    out: list[dict[str, Any]] = []
    harness.serve_mcp(
        BRIEF, tmp_path / "song.json", lambda: feed.pop(0) if feed else "", lambda text: out.append(json.loads(text))
    )
    assert [o["id"] for o in out] == [1, 2, 3, 4, 5, 6]
    assert out[4]["result"]["isError"] is True and "check still finds" in out[4]["result"]["content"][0]["text"]
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


async def test_openais_reasoning_goes_back_unchanged_in_the_harness_loop(tmp_path: Path) -> None:
    """On OpenAI's Responses API, each turn's items (its encrypted reasoning with them) are sent again on the next
    request, as OpenAI asks; the harness reads only the tool calls."""
    from test_responses import USAGE, answer

    sent: list[dict[str, Any]] = []
    turns: list[list[dict[str, Any]]] = [
        [
            {"type": "reasoning", "id": "rs_0", "summary": [], "encrypted_content": "secret-0"},
            {"type": "function_call", "id": "fc_0", "call_id": "call_0", "name": "form", "arguments": "{}"},
        ],
        [
            {"type": "reasoning", "id": "rs_1", "summary": [], "encrypted_content": "secret-1"},
            {"type": "function_call", "id": "fc_1", "call_id": "call_1", "name": "rules_write",
             "arguments": json.dumps({"part": "bass"})},
            {"type": "function_call", "id": "fc_2", "call_id": "call_2", "name": "finish",
             "arguments": json.dumps({"summary": "Through the Responses API."})},
        ],
        [  # finish was refused once (open issues): asked again
            {"type": "reasoning", "id": "rs_2", "summary": [], "encrypted_content": "secret-2"},
            {"type": "function_call", "id": "fc_3", "call_id": "call_3", "name": "finish",
             "arguments": json.dumps({"summary": "Through the Responses API."})},
        ],
    ]  # fmt: skip

    def handler(request: httpx.Request) -> httpx.Response:
        assert request.url.path == "/v1/responses"
        sent.append(json.loads(request.content))
        return httpx.Response(200, content=answer(turns[len(sent) - 1]), headers={"content-type": "text/event-stream"})

    service = make_service(tmp_path, handler)
    saved = await service.handle(
        "ai.v1.connections.save", {"name": "OpenAI", "provider": "openai", "key": "k"}, no_notify
    )
    chain = [{"connection": saved["id"], "model": "gpt-6.1-sol"}]
    await service.handle("ai.v1.routing.set", {"feature": "patterns", "chain": chain}, no_notify)
    song = await run_job(service, "ai.v1.song.compose", params(effort="xhigh"))
    assert len(sent) == 3 and sent[0]["reasoning"] == {"effort": "xhigh"} and "max_output_tokens" not in sent[0]
    assert sent[1]["input"][1] == turns[0][0] and sent[1]["input"][2] == turns[0][1]  # the reasoning, unchanged
    assert sent[1]["input"][3]["type"] == "function_call_output" and sent[1]["input"][3]["call_id"] == "call_0"
    assert song["composed"]["reason"] == "Through the Responses API." and song["layerParts"]["bass"]
    usage = song["composed"]["usage"]
    assert usage["reasoning"] == 3 * USAGE["output_tokens_details"]["reasoning_tokens"]  # type: ignore[index]


def test_eval_saves_each_song(tmp_path: Path) -> None:
    brief = tmp_path / "brief.json"
    brief.write_text(json.dumps(BRIEF), encoding="utf-8")
    harness.main(["eval", "--brief", str(brief), "--fake", "--efforts", ",high", "--save", str(tmp_path / "songs")])
    saved = sorted(p.name for p in (tmp_path / "songs").iterdir())
    assert saved == [
        "anthropic-harness-default.song.json",
        "anthropic-harness-high.song.json",
        "anthropic-one-shot-default.song.json",
        "anthropic-one-shot-high.song.json",
    ]
    song = json.loads((tmp_path / "songs" / "anthropic-harness-high.song.json").read_text())
    assert song["kind"] == "spinus.song" and song["composed"]["harness"]


async def test_a_model_name_with_slashes_is_a_file_name(tmp_path: Path) -> None:
    ws = harness.Workspace.start(setup(), 5, "sketch")
    client = httpx.AsyncClient(transport=httpx.MockTransport(harness.fake_runs(setup(), ws)))
    store = ConnectionStore(tmp_path, MemoryKeyStore())
    store.upsert("Fake", "anthropic", "https://fake.test", "k")
    await harness.compare_ways(BRIEF, client, store, Routing(tmp_path, store), tmp_path, [""], tmp_path / "out",
                               "anthropic/claude-sonnet-5.5")  # fmt: skip
    assert sorted(p.name for p in (tmp_path / "out").iterdir()) == [
        "anthropic_claude-sonnet-5.5-harness-default.song.json",
        "anthropic_claude-sonnet-5.5-one-shot-default.song.json",
    ]


def test_a_refused_reasoning_level_becomes_the_lowest_offered() -> None:
    from spinus_ai.gateway import offered_effort

    said = "Unsupported value: 'reasoning.effort' does not support 'low' with this model. Supported values are: "
    assert offered_effort("low", said + "'medium', 'high' and 'xhigh'.") == "medium"
    assert offered_effort("low", "reasoning effort low is not available for this model") == "medium"
    assert offered_effort("xhigh", "reasoning effort xhigh is not available") is None  # nothing named
    assert offered_effort("high", "Unsupported value: 'reasoning_effort' 'high'.") is None  # not guessed at
    assert offered_effort("low", "max_tokens: 64000 > 8192") is None  # another refusal
    assert offered_effort("", "reasoning effort") is None


async def test_the_harness_says_when_the_model_lacks_the_level_asked(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    script = [[("rules_write", {"part": "bass"})], [("finish", {"summary": "s"})], [("finish", {"summary": "s"})]]
    fake = harness.FakeModel(script, {})

    def handler(request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content)
        sent.append(body)
        if body.get("output_config", {}).get("effort") == "low":
            message = "output_config.effort: 'low' is not supported by this model. Supported values: 'medium', 'high'"
            return httpx.Response(400, json={"type": "error", "error": {"message": message}})
        return fake(request)

    service = make_service(tmp_path, handler)
    await connect(service)
    song = await run_job(service, "ai.v1.song.compose", params(effort="low"))
    efforts = [r["output_config"]["effort"] for r in sent]
    assert efforts == ["low", "medium", "medium", "medium"]  # asked once at low; the rest of the loop at medium
    assert "does not reason at the level 'low', so 'medium', the lowest it offers, was used" in song["note"]


def test_depths_are_groups_of_tools_with_basic_always() -> None:
    assert harness.check_groups(None) == ("basic", "arrangement")  # the default
    assert harness.check_groups("basic") == ("basic",)
    assert harness.check_groups("full") == harness.GROUP_ORDER
    assert harness.check_groups(["mix"]) == ("basic", "mix")  # Basic is always on
    assert harness.check_groups("transitions,basic") == ("basic", "transitions")
    with pytest.raises(ValueError, match="no group of tools 'kazoo'"):
        harness.check_groups("basic,kazoo")
    with pytest.raises(ValueError):
        harness.check_groups(7)
    assert harness.depth_words(("basic", "arrangement")) == "Basic + arrangement"
    assert harness.depth_words(harness.GROUP_ORDER) == "Full"
    basic = {t.name for t in harness.tools_for(("basic",))}
    assert basic == {t.name for t in harness.BASIC_TOOLS} and basic <= set(harness.TOOL_NAMES)


async def test_the_song_says_which_depth_wrote_it(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    service = scripted(tmp_path, [[("finish", {"summary": "s"})]], sent)
    await connect(service)
    song = await run_job(service, "ai.v1.song.compose", params(tools="basic"))
    assert song["composed"]["tools"] == ["basic"] and "Tools: Basic." in song["note"]
    assert "The tools you may use: Basic:" in sent[0]["messages"][0]["content"][0]["text"]
    assert {t["name"] for t in sent[0]["tools"]} == {t.name for t in harness.BASIC_TOOLS}
    with pytest.raises(RpcError):
        await service.handle("ai.v1.song.compose", params(tools=["basic", "kazoo"]), no_notify)


def test_eval_compares_depths(tmp_path: Path, capsys: Any) -> None:
    brief = tmp_path / "brief.json"
    brief.write_text(json.dumps(BRIEF), encoding="utf-8")
    harness.main(["eval", "--brief", str(brief), "--fake", "--tools", "basic;full", "--save", str(tmp_path / "s")])
    lines = capsys.readouterr().out.splitlines()
    assert lines[0].split()[:2] == ["way", "tools"]
    assert [line.split()[1] for line in lines[1:4]] == ["-", "Basic", "Full"]
    assert {p.name for p in (tmp_path / "s").iterdir()} == {
        "anthropic-one-shot-default.song.json",
        "anthropic-harness-basic-default.song.json",
        "anthropic-harness-basic+arrangement+transitions+mix-default.song.json",
    }


# ---------------------------------------------------------------------------------------------------
# Arrangement


def arranged() -> harness.Workspace:
    ws = harness.Workspace.start(setup(), 5, "sketch")
    ws.groups = harness.DEPTHS["arrangement"]
    return ws


def test_the_ai_adds_its_own_parts_with_a_family_and_colour() -> None:
    ws = arranged()
    assert "added" in ws.call("add_part", {"id": "shaker", "name": "Shaker", "role": "percussion"})
    ws.call("set_look", {"part": "shaker", "family": "Percussion", "colour": "#AA3300"})
    first = next(iter(ws.sections))
    ws.call("write", {"part": "shaker", "section": first, "notes": "0 hat 1 60; 2 hat 1 50", "loop_bars": 1})
    song = ws.song()
    layer = next(x for x in song["layers"] if x["id"] == "shaker")
    assert layer["family"] == "Percussion" and layer["colour"] == "#aa3300" and layer["drums"]
    assert song["layerParts"]["shaker"][first]
    # A copy (the best checked version) keeps it, and the user's parts are not the AI's to remove.
    assert any(p["id"] == "shaker" for p in ws.copy().setup.parts)
    assert not any(p["id"] == "shaker" for p in setup().parts)  # the request's own list is untouched
    for bad, why in [
        ({"id": "shaker", "role": "percussion"}, "already"),
        ({"id": "Big Pad!", "role": "pad"}, "lowercase"),
        ({"id": "kz", "role": "kazoo"}, "not a role"),
    ]:
        with pytest.raises(harness.Mistake, match=why):
            ws.call("add_part", bad)
    with pytest.raises(harness.Mistake, match="user's parts"):
        ws.call("remove_part", {"part": "bass"})
    with pytest.raises(harness.Mistake, match="'#rrggbb'"):
        ws.call("set_look", {"part": "bass", "colour": "red"})
    with pytest.raises(harness.Mistake, match="give a family"):
        ws.call("set_look", {"part": "bass"})
    assert "removed" in ws.call("remove_part", {"part": "shaker"})
    assert not any(x["id"] == "shaker" for x in ws.song()["layers"])


def test_motifs_vary_by_rhythm_notes_inversion_and_displacement() -> None:
    ws = arranged()
    ws.call("motif", {"name": "hook", "notes": "0 64 2 100; 4 67 2 90; 8 71 2 90; 12 69 2 80", "bars": 1})
    ws.call("vary", {"name": "up", "motif": "hook", "how": "invert"})
    assert ws.motifs["up"]["notes"].split("; ")[1].split()[1] == "61"  # 2*64 - 67
    ws.call("vary", {"name": "late", "motif": "hook", "how": "displace", "amount": 2})
    assert [t.split()[0] for t in ws.motifs["late"]["notes"].split("; ")] == ["2", "6", "10", "14"]
    ws.call("vary", {"name": "pushed", "motif": "hook", "how": "rhythm"})
    assert [t.split()[0] for t in ws.motifs["pushed"]["notes"].split("; ")] == ["0", "5", "8", "13"]
    ws.call("vary", {"name": "moved", "motif": "hook", "how": "notes", "amount": 2})
    assert ws.motifs["moved"]["notes"] != ws.motifs["hook"]["notes"]
    with pytest.raises(harness.Mistake, match="no motif"):
        ws.call("vary", {"name": "x", "motif": "nothing", "how": "invert"})
    with pytest.raises(harness.Mistake, match="one of"):
        ws.call("vary", {"name": "x", "motif": "hook", "how": "sideways"})


def test_fills_go_at_phrase_ends_with_a_crash_after() -> None:
    ws = arranged()
    section = next(s for s in ws.sections.values() if s["bars"] >= 8)
    sid, bars = section["id"], section["bars"]
    ws.call("write", {"part": "drums", "section": sid, "notes": "0 kick 1 100; 8 snare 1 100", "loop_bars": 1})
    ends = ", ".join(str(e) for e in sorted(set(range(4, bars + 1, 4)) | {bars}))
    said = ws.call("fill", {"part": "drums", "section": sid, "every": 4, "kind": "toms"})
    assert said.endswith(f"at the end of bars {ends}.")
    bar = ws.steps * 960 // 4
    notes = ws.notes["drums"][sid]
    in_fourth = [n for n in notes if 3 * bar <= n["start"] < 4 * bar]
    assert any(n["drum"].startswith("tom") for n in in_fourth)
    assert any(n["drum"] == "crash" and n["start"] == 4 * bar for n in notes)
    with pytest.raises(harness.Mistake, match="plays no drums"):
        ws.call("fill", {"part": "bass", "section": sid})
    with pytest.raises(harness.Mistake, match="a fill is one of"):
        ws.call("fill", {"part": "drums", "section": sid, "kind": "cowbell"})


def test_layers_enter_and_leave_and_come_back() -> None:
    ws = arranged()
    sid = next(s["id"] for s in ws.sections.values() if s["bars"] >= 8)
    ws.call("write", {"part": "bass", "section": sid, "notes": "0 45 4 100; 8 45 4 90", "loop_bars": 1})
    written = len(ws.notes["bass"][sid])
    ws.call("play_only", {"part": "bass", "section": sid, "bars": "5-8"})
    assert len(ws.notes["bass"][sid]) == 8  # two a bar, four bars
    ws.call("play_only", {"part": "bass", "section": sid, "bars": "none"})
    assert ws.notes["bass"][sid] == []
    ws.call("play_only", {"part": "bass", "section": sid, "bars": "all"})
    assert len(ws.notes["bass"][sid]) == written
    with pytest.raises(harness.Mistake, match="'3' or '5-8'"):
        ws.call("play_only", {"part": "bass", "section": sid, "bars": "sometimes"})


def test_the_energy_check_follows_the_references_sections() -> None:
    brief = json.loads(json.dumps(BRIEF))
    for i, song in enumerate(brief["summary"]["perSong"]):
        song["sections"] = [{"bars": 8, "level": -12.0 - i}, {"bars": 8, "level": -6.0}, {"bars": 8, "level": -14.0}]
    ws = harness.Workspace.start(harness.setup_of(brief), 5, "sketch")
    ws.groups = harness.DEPTHS["arrangement"]
    flat = "0 kick 1 100; 4 snare 1 100; 8 kick 1 100; 12 snare 1 100"
    for sid in ws.sections:
        ws.call("write", {"part": "drums", "section": sid, "notes": flat, "loop_bars": 1})
    said = ws.call("energy", {})
    assert "The reference songs' sections" in said and "50 %: 1.00" in said
    assert any("about as full as the others" in line for line in ws.issues())
    ws.groups = ("basic",)
    assert not any(line.startswith("energy") for line in ws.issues())  # only with the arrangement tools


async def test_arrangement_tools_through_a_run_and_not_at_basic(tmp_path: Path) -> None:
    first = next(iter(harness.Workspace.start(setup(), 5, "sketch").sections))
    script: Script = [
        [("add_part", {"id": "pad2", "role": "pad"}), ("set_look", {"part": "pad2", "colour": "#336699"})],
        [("rules_write", {"part": "pad2"}), ("play_only", {"part": "pad2", "section": first, "bars": "none"})],
        [("energy", {})],
        [("finish", {"summary": "s"})],
    ]
    sent: list[dict[str, Any]] = []
    service = scripted(tmp_path, script, sent)
    await connect(service)
    song = await run_job(service, "ai.v1.song.compose", params())
    assert {t["name"] for t in sent[0]["tools"]} >= {"add_part", "fill", "energy"}
    assert "Arrangement tools:" in sent[0]["system"]
    layer = next(x for x in song["layers"] if x["id"] == "pad2")
    assert layer["colour"] == "#336699" and song["layerParts"]["pad2"][first] == []
    # At Basic the same calls are refused, and their definitions are not sent.
    sent.clear()
    service = scripted(
        tmp_path / "b", [[("add_part", {"id": "x", "role": "pad"})], [("finish", {"summary": "s"})]], sent
    )
    await connect(service)
    await run_job(service, "ai.v1.song.compose", params(tools="basic"))
    assert "add_part" not in {t["name"] for t in sent[0]["tools"]}
    result = sent[1]["messages"][-1]["content"][0]
    assert result["is_error"] and "Arrangement tools, which are not allowed" in result["content"]


# ---------------------------------------------------------------------------------------------------
# Transitions and automation


def transitioned() -> harness.Workspace:
    ws = harness.Workspace.start(setup(), 5, "sketch")
    ws.groups = harness.DEPTHS["transitions"]
    return ws


def test_automation_values_are_read_as_spinus_reads_them() -> None:
    assert harness.normalised("volume", 6.0) == 1.0 and harness.normalised("volume", -100.0) == 0.0
    assert abs(harness.normalised("volume", 0.0) - math.exp(-6 / 20)) < 1e-9
    assert harness.normalised("pan", -1.0) == 0.0 and harness.normalised("pan", 0.0) == 0.5
    assert abs(harness.normalised("filter", 1000.0) - 0.5) < 1e-9  # the cutoff's middle
    assert harness.normalised("filter", 20.0) == 0.0 and abs(harness.normalised("filter", 20000.0) - 1.0) < 1e-9


def test_automation_clips_go_into_the_song() -> None:
    ws = transitioned()
    said = ws.call("automate", {"part": "bass", "what": "filter", "points": "1 300; 4.5 8000", "filter": "high-pass"})
    assert "from bar 1 to 4.5" in said
    ws.call("automate", {"part": "lead", "what": "volume", "points": "5 -30; 9 0", "shape": "ease-in"})
    song = ws.song()
    clips = song["production"]["automation"]
    assert clips[0]["part"] == "bass" and clips[0]["filter"] == "high-pass"
    assert clips[0]["points"][1] == {"bar": 3.5, "value": round(harness.normalised("filter", 8000), 4), "curve": 0.0}
    assert clips[1]["points"][0]["curve"] == 0.5
    assert "bass filter (high-pass)" in ws.call("automation", {})
    assert "Removed 1" in ws.call("remove_automation", {"part": "lead"})
    for bad, why in [
        ({"part": "bass", "what": "volume", "points": "1 0"}, "two points"),
        ({"part": "bass", "what": "volume", "points": "1 0; 999 0"}, "a point's bar is 1 to"),
        ({"part": "bass", "what": "volume", "points": "1 loud; 2 0"}, "'bar value'"),
        ({"part": "bass", "what": "volume", "points": "1 0; 2 40"}, "-100 to 6 dB"),
        ({"part": "bass", "what": "pan", "points": "1 0; 2 nan"}, "a point's bar"),
        ({"part": "bass", "what": "tempo", "points": "1 0; 2 1"}, "automate one of"),
        ({"part": "bass", "what": "filter", "points": "1 0; 2 1", "filter": "comb"}, "low-pass or high-pass"),
    ]:
        with pytest.raises(harness.Mistake, match=why):
            ws.call("automate", bad)


def test_transitions_lead_into_and_out_of_sections() -> None:
    ws = transitioned()
    chorus = next(sid for sid, s in ws.sections.items() if s["kind"] == "chorus")
    starts, at = [], 0
    for sid in ws.order:
        if sid == chorus:
            starts.append(at)
        at += ws.sections[sid]["bars"]
    said = ws.call("transition", {"kind": "riser", "section": chorus, "bars": 2})
    assert said.endswith(", ".join(f"bar {b - 1}" for b in starts) + ".")
    ws.call("transition", {"kind": "impact", "section": chorus, "sample": True})
    ws.call("transition", {"kind": "downlifter", "section": chorus, "bars": 2})
    ws.call("transition", {"kind": "swell", "section": chorus})
    song = ws.song()
    assert song["fx"]["riser"]["at"] == [b - 2 for b in starts] and song["fx"]["riser"]["bars"] == 2
    assert song["fx"]["impact"]["at"] == starts
    assert {layer["id"] for layer in song["layers"]} >= {"riser", "impact", "downlifter", "swell"}
    swell = next(x for x in song["layers"] if x["id"] == "swell")
    assert swell["part"] == "fx" and swell["family"] == "FX"
    kinds = [(a["part"], a["what"]) for a in song["production"]["automation"]]
    assert ("riser", "volume") in kinds and ("riser", "filter") in kinds and ("downlifter", "filter") in kinds
    rise = next(a for a in song["production"]["automation"] if a["part"] == "riser" and a["what"] == "volume")
    assert rise["points"][0]["value"] < rise["points"][1]["value"]  # it rises
    assert [x["file"] for x in song["production"]["samples"][:2]] == [
        "Drums/Cymbals/Crash Wide.flac", "Drums/808s/808 Long.flac"]  # fmt: skip
    first = ws.order[0]
    with pytest.raises(harness.Mistake, match="starts the song"):
        ws.call("transition", {"kind": "riser", "section": first, "bars": 2})
    with pytest.raises(harness.Mistake, match="are 2 bars"):
        ws.call("transition", {"kind": "riser", "section": chorus, "bars": 4})
    with pytest.raises(harness.Mistake, match="one of riser"):
        ws.call("transition", {"kind": "laser", "section": chorus})


def test_a_sampled_swell_is_a_reversed_cymbal_ending_on_the_beat() -> None:
    ws = transitioned()
    chorus = next(sid for sid, s in ws.sections.items() if s["kind"] == "chorus")
    starts, at = [], 0
    for sid in ws.order:
        if sid == chorus:
            starts.append(at)
        at += ws.sections[sid]["bars"]
    said = ws.call("transition", {"kind": "swell", "section": chorus, "sample": True})
    assert "reversed" in said
    ws.call("transition", {"kind": "swell", "section": chorus, "sample": True})  # again: no duplicates
    song = ws.song()
    reversed_ = [x for x in song["production"]["samples"] if x.get("reversed")]
    assert [x["bar"] for x in reversed_] == starts  # each ends on the chorus's first beat
    assert all(x["file"] == "Drums/Cymbals/Crash Wide.flac" for x in reversed_)
    assert "swell" not in {layer["id"] for layer in song["layers"]}  # no held synth note as well


async def test_transition_tools_only_with_their_group(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    script: Script = [
        [("automate", {"part": "bass", "what": "volume", "points": "1 -20; 3 0"})],
        [("finish", {"summary": "s"})],
    ]
    service = scripted(tmp_path, script, sent)
    await connect(service)
    song = await run_job(service, "ai.v1.song.compose", params(tools="arrangement"))
    assert "automate" not in {t["name"] for t in sent[0]["tools"]} and "production" not in song
    sent.clear()
    service = scripted(tmp_path / "b", script, sent)
    await connect(service)
    song = await run_job(service, "ai.v1.song.compose", params(tools="transitions"))
    assert "Transition and automation tools:" in sent[0]["system"]
    assert song["production"]["automation"][0]["part"] == "bass"


# ---------------------------------------------------------------------------------------------------
# Effects and mix


def mixed(effects: dict[str, list[str]] | None = None) -> harness.Workspace:
    brief = {**BRIEF, "effects": effects if effects is not None else {"reverb": ["Hall One", "Plate Two"]}}
    ws = harness.Workspace.start(harness.setup_of(brief), 5, "sketch")
    ws.groups = harness.GROUP_ORDER
    return ws


def test_effects_are_spinus_own_with_their_controls_checked() -> None:
    ws = mixed()
    said = ws.call(
        "add_effect", {"part": "bass", "effect": "eq", "settings": {"band3_freq": "250 Hz", "band3_type": "low CUT"}}
    )
    assert said.startswith("Effect bass-eq on bass") and "band3_type Low cut" in said
    ws.call("add_effect", {"part": "bass", "effect": "compressor", "settings": {"Threshold": "-18 dB", "ratio": "4:1"}})
    ws.call("set_effect", {"effect": "bass-compressor", "settings": {"makeup": "3 dB"}})
    assert "bass-eq2" in ws.call("add_effect", {"part": "bass", "effect": "eq"})
    ws.call("remove_effect", {"effect": "bass-eq2"})
    production = ws.song()["production"]
    assert [e["id"] for e in production["effects"]] == ["bass-eq", "bass-compressor"]
    assert production["effects"][1]["settings"] == {"threshold": "-18 dB", "ratio": "4:1", "makeup": "3 dB"}
    bads: list[tuple[dict[str, Any], str]] = [
        ({"part": "bass", "effect": "chorus"}, "an effect is one of"),
        ({"part": "bass", "effect": "eq", "settings": {"warmth": "lots"}}, "has no control 'warmth'"),
        ({"part": "bass", "effect": "filter", "settings": {"type": "Comb"}}, "Low pass, High pass"),
        ({"part": "bass", "effect": "compressor", "settings": {"threshold": "quiet"}}, "not a value Spinus reads"),
        ({"part": "bass", "effect": "eq", "settings": "loud"}, "settings are control ID"),
        ({"part": "bass", "effect": "reverb", "settings": {"size": "big"}}, "set by its send level"),
    ]
    for bad, why in bads:
        with pytest.raises(harness.Mistake, match=why):
            ws.call("add_effect", bad)
    with pytest.raises(harness.Mistake, match="no effect 'nope'"):
        ws.call("set_effect", {"effect": "nope", "settings": {}})


def test_sends_go_to_the_users_reverb_and_never_to_one_it_lacks() -> None:
    ws = mixed()
    assert "(Hall One)" in ws.call("send", {"part": "lead", "to": "reverb", "level": "-12 dB"})
    assert "(Plate Two)" in ws.call("send", {"part": "keys", "to": "reverb", "level": "-18", "plugin": "plate two"})
    production = ws.song()["production"]
    assert production["returns"] == {"reverb": "Plate Two"} and production["sends"]["lead"] == {"reverb": -12.0}
    assert "no longer" in ws.call("send", {"part": "lead", "to": "reverb", "level": "off"})
    with pytest.raises(harness.Mistake, match="-60 to 6 dB"):
        ws.call("send", {"part": "lead", "to": "reverb", "level": "loud"})
    with pytest.raises(harness.Mistake, match="reverbs: Hall One, Plate Two"):
        ws.call("send", {"part": "lead", "to": "reverb", "level": "-6", "plugin": "Cathedral"})
    assert "none (Spinus has none of its own)" in mixed({}).call("mix", {})
    with pytest.raises(harness.Mistake, match="set by its send level"):
        ws.call("send", {"part": "lead", "to": "reverb", "level": "-12", "settings": {"mix": "50%"}})


def test_delay_sends_use_spinus_delay_in_time_with_the_song() -> None:
    ws = mixed()  # the user has no delay plugin
    said = ws.call("send", {"part": "lead", "to": "delay", "level": "-14 dB"})
    assert "(Spinus Delay)" in said
    said = ws.call(
        "send",
        {"part": "keys", "to": "delay", "level": "-18", "settings": {"note": "1/4 TRIPLET", "mode": "ping-pong"}},
    )
    assert "note 1/4 triplet, mode Ping-pong" in said
    production = ws.song()["production"]
    assert production["returns"] == {"delay": "Spinus Delay"}
    assert production["returnSettings"] == {"delay": {"note": "1/4 triplet", "mode": "Ping-pong"}}
    assert production["sends"]["lead"] == {"delay": -14.0} and production["sends"]["keys"] == {"delay": -18.0}
    assert "the delay bus: Spinus Delay (note 1/4 triplet, mode Ping-pong)" in ws.call("mix", {})
    assert "The delays: Spinus Delay" in mixed({}).call("mix", {})
    with pytest.raises(harness.Mistake, match="note is one of"):
        ws.call("send", {"part": "lead", "to": "delay", "level": "-12", "settings": {"note": "3/7"}})
    with pytest.raises(harness.Mistake, match="has no control 'decay'"):
        ws.call("send", {"part": "lead", "to": "delay", "level": "-12", "settings": {"decay": "2 s"}})

    # As an effect on a part: Spinus Delay with its settings, or a delay of the user's by name (no settings).
    assert "(Spinus Delay): note 1/8, mix 25%" in ws.call(
        "add_effect", {"part": "lead", "effect": "delay", "settings": {"note": "1/8", "mix": "25%"}}
    )
    theirs = mixed({"delay": ["Tape Echo"]})
    assert "(Tape Echo)" in theirs.call("send", {"part": "lead", "to": "delay", "level": "-12", "plugin": "tape echo"})
    with pytest.raises(harness.Mistake, match="the delays there are: Spinus Delay, Tape Echo"):
        theirs.call("add_effect", {"part": "lead", "effect": "delay", "plugin": "Cathedral"})
    with pytest.raises(harness.Mistake, match="set by its send level"):
        theirs.call("add_effect", {"part": "lead", "effect": "delay", "plugin": "Tape Echo", "settings": {"mix": "1"}})
    # Switching the bus to another delay drops the Spinus Delay's settings.
    switched = mixed({"delay": ["Tape Echo"]})
    switched.call("send", {"part": "lead", "to": "delay", "level": "-12", "settings": {"note": "1/4"}})
    switched.call("send", {"part": "lead", "to": "delay", "level": "-12", "plugin": "Tape Echo"})
    assert not switched.song()["production"].get("returnSettings")


def test_ducking_and_buses() -> None:
    ws = mixed()
    assert "by 60 %" in ws.call("duck", {"part": "bass", "source": "drums", "amount": 60})
    ws.call("bus", {"name": "Low end", "parts": ["bass", "keys"]})
    production = ws.song()["production"]
    assert production["ducks"]["bass"] == {"source": "drums", "amount": 60.0, "release": "150 ms"}
    assert production["buses"] == {"Low end": ["bass", "keys"]}
    with pytest.raises(harness.Mistake, match="already play through the Low end bus"):
        ws.call("bus", {"name": "Other", "parts": ["keys"]})
    with pytest.raises(harness.Mistake, match="under itself"):
        ws.call("duck", {"part": "bass", "source": "bass"})
    with pytest.raises(harness.Mistake, match="0 to 100"):
        ws.call("duck", {"part": "bass", "source": "drums", "amount": 300})
    assert "removed" in ws.call("bus", {"name": "Low end", "parts": []})
    said = ws.call("mix", {})
    assert "bass ducks under drums by 60 %" in said and "The user's reverbs: Hall One, Plate Two" in said


def test_measure_gives_each_parts_level_per_section_with_the_mix() -> None:
    ws = mixed()
    first = next(iter(ws.sections))
    ws.call("write", {"part": "bass", "section": first, "notes": "0 40 4 100; 8 40 4 100", "loop_bars": 1})
    ws.call("write", {"part": "drums", "section": first, "notes": "0 kick 1 127; 4 snare 1 127", "loop_bars": 1})
    ws.call("add_effect", {"part": "bass", "effect": "compressor", "settings": {"makeup": "6 dB"}})
    said = ws.call("measure", {})
    line = next(x for x in said.splitlines() if x.startswith(f"{first}: loudness"))
    assert "bass 0 dB" in line  # with its 6 dB of makeup, the loudest
    ws.groups = ("basic",)
    assert "Per section" not in ws.call("measure", {})


def test_a_sound_is_picked_by_its_number_name_or_a_copied_line() -> None:
    sounds = [
        {"id": "stock-spinus.3x-osc|program:8", "name": "3x Osc: Bright Lead"},
        {"id": "ob-xf|/some/long/path/OB-Xf.clap|42", "name": "OB-Xf: 8VM Lead"},
    ]
    ws = harness.Workspace.start(harness.setup_of({**BRIEF, "candidates": {"lead": sounds}}), 5, "sketch")
    listed = ws.call("list_sounds", {"part": "lead"})
    assert listed.splitlines()[:2] == ["1. 3x Osc: Bright Lead", "2. OB-Xf: 8VM Lead"]
    assert "/some/long/path" not in listed  # no internal keys to garble
    assert "OB-Xf: 8VM Lead" in ws.call("pick_sound", {"part": "lead", "sound": "2"})
    assert ws.sounds["lead"] == sounds[1]["id"]
    for said in (
        "#1",
        "1. 3x Osc: Bright Lead",
        "3x osc: bright lead",
        "stock-spinus.3x-osc|program:8: 3x Osc: Bright Lead",
    ):
        ws.call("pick_sound", {"part": "lead", "sound": said})
        assert ws.sounds["lead"] == sounds[0]["id"], said
    assert "(picked)" in ws.call("list_sounds", {"part": "lead", "query": "bright"})
    with pytest.raises(harness.Mistake, match="number list_sounds shows"):
        ws.call("pick_sound", {"part": "lead", "sound": "7"})


def test_the_master_takes_effects_and_check_asks_for_its_limiter() -> None:
    ws = harness.Workspace.start(setup(), 5, "sketch")
    ws.groups = {"basic", "mix"}
    assert any("master: no limiter" in line for line in ws.issues())
    said = ws.call("add_effect", {"part": "master", "effect": "limiter", "settings": {"ceiling": "-1 dB"}})
    assert said.startswith("Effect master-limiter on master")
    assert not any("master: no limiter" in line for line in ws.issues())
    ws.groups = {"basic"}
    assert not any("master" in line for line in ws.issues())  # without the mix tools it can't, so it isn't asked


def test_chat_services_clear_old_tool_calls_in_batches_as_anthropic_does() -> None:
    from spinus_ai import gateway

    request = gateway.Request("m", "system", [gateway.Message("user", [gateway.Text("brief")])])
    request.context_edits = [
        {
            "type": "clear_tool_uses_20250919",
            "trigger": {"type": "input_tokens", "value": 2000},
            "keep": {"type": "tool_uses", "value": 2},
            "clear_at_least": {"type": "input_tokens", "value": 1000},
            "clear_tool_inputs": True,
            "exclude_tools": ["finish"],
        }
    ]

    def turn(n: int, name: str = "read") -> None:
        use = gateway.ToolUse(f"t{n}", name, {"part": "bass", "notes": "x" * 400})
        request.messages.append(gateway.Message("assistant", [use]))
        request.messages.append(gateway.Message("user", [gateway.ToolResult(f"t{n}", "y" * 2000)]))

    turn(0, "finish")
    for n in range(1, 3):
        turn(n)
    assert gateway.clear_old_tool_calls(request) == 0  # under the trigger: nothing
    request.token_ratio = 2.5  # notes count more tokens than their characters suggest: the provider said so
    assert gateway.clear_old_tool_calls(request) == 0  # past the trigger now, but too little to gain yet
    request.token_ratio = 0.0
    for n in range(3, 9):
        turn(n)
    cleared = gateway.clear_old_tool_calls(request)
    assert cleared > 1000
    results = [b for m in request.messages for b in m.blocks if isinstance(b, gateway.ToolResult)]
    assert [r.content == gateway.CLEARED for r in results] == [
        False,
        *[True] * 6,
        False,
        False,
    ]  # finish and the last 2
    uses = [b for m in request.messages for b in m.blocks if isinstance(b, gateway.ToolUse)]
    assert uses[1].input == {} and uses[-1].input["part"] == "bass"
    turn(9)  # one more call: less than clear_at_least to gain, so the history (and the cache) stays as it was
    assert gateway.clear_old_tool_calls(request) == 0


def test_the_sections_a_description_names_are_read_in_generic_words() -> None:
    assert harness.asked_sections(
        "an atmospheric intro; verse 1; a pre-chorus that builds; a chorus; a half-time breakdown; a build-up with a "
        "snare roll; a final double chorus; and an outro"
    ) == [["intro"], ["verse"], ["pre-chorus"], ["chorus"], ["breakdown"], ["build-up"], ["double chorus"], ["outro"]]
    assert harness.asked_sections("a breakdown or bridge, then the chorus; the bass drops back") == [
        ["breakdown", "bridge"],
        ["chorus"],
    ]  # either will do, and a verb isn't a drop
    assert harness.asked_sections("a dreamy song in D minor") == []


def described(text: str) -> dict[str, Any]:
    return {**BRIEF, "summary": {**summary(), "described": {"text": text}}}


async def test_check_names_the_sections_the_description_asked_for_and_the_model_adds_them(tmp_path: Path) -> None:
    text = "A song with a guitar solo and a breakdown, then the final chorus."
    ws = harness.Workspace.start(harness.setup_of(described(text)), 5, "sketch")
    missing = [line for line in ws.issues() if line.startswith("form:")]
    assert len(missing) == 2 and "a solo" in missing[0] and "a breakdown" in missing[1]
    sections = [
        {"id": sid, "kind": s["kind"], "bars": s["bars"], "chords": s["chords"]} for sid, s in ws.sections.items()
    ]
    chords = sections[-1]["chords"]
    fixed = {
        "sections": [
            *sections,
            {"id": "solo", "kind": "bridge", "bars": 8, "chords": chords},
            {"id": "breakdown", "kind": "bridge", "bars": 8, "chords": chords},
        ],
        "order": [*ws.order[:-1], "solo", "breakdown", ws.order[-1]],
    }
    # Through the fake gateway: the model checks, adds the sections by name, checks again and finishes.
    sent: list[dict[str, Any]] = []
    script: Script = [[("check", {})], [("set_form", fixed)], [("check", {})], [("finish", {"summary": "Done."})]]
    service = scripted(tmp_path, script, sent)
    await connect(service)
    song = await run_job(
        service,
        "ai.v1.song.compose",
        params(tools="basic", summary={**summary(), "described": {"text": text}}),
    )
    first, second = (sent[i]["messages"][-1]["content"][0]["content"] for i in (1, 3))
    assert "asks for a solo" in first and "asks for a breakdown" in first
    assert "asks for" not in second
    assert {"solo", "breakdown"} <= {s["id"] for s in song["sections"]}


def test_the_master_can_be_automated_for_a_fade_of_the_whole_mix() -> None:
    ws = harness.Workspace.start(setup(), 5, "sketch")
    ws.groups = {"basic", "transitions"}
    end = ws.song_bars()
    said = ws.call("automate", {"part": "master", "what": "volume", "points": f"{end - 3} 0; {end + 1} -60"})
    assert "master" in said
    fade = next(a for a in ws.production()["automation"] if a["part"] == "master")
    assert fade["what"] == "volume" and fade["points"][0]["value"] > fade["points"][-1]["value"]
    ws.call("remove_automation", {"part": "master"})
    assert not [a for a in ws.production()["automation"] if a["part"] == "master"]


def test_sections_are_named_by_their_ids_when_those_say_more_than_the_kind() -> None:
    ws = harness.Workspace.start(setup(), 5, "sketch")
    chords = next(iter(ws.sections.values()))["chords"]
    form = [("intro", "intro"), ("buildup", "verse"), ("drop", "chorus"), ("verse2", "verse"), ("drop2", "chorus")]
    ws.call(
        "set_form",
        {
            "sections": [{"id": i, "kind": k, "bars": 8, "chords": chords} for i, k in form],
            "order": [i for i, _ in form],
        },
    )
    names = {s["id"]: s["name"] for s in ws.song()["sections"]}
    assert names == {"intro": "Intro", "buildup": "Buildup", "drop": "Drop", "verse2": "Verse", "drop2": "Drop 2"}


def openai_tool_call(name: str, args: dict[str, Any], n: int, prompt_tokens: int = 10) -> bytes:
    """A chat-completions stream answering with one tool call."""
    chunks = [
        {"choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": f"call_{n}", "type": "function",
                      "function": {"name": name, "arguments": json.dumps(args)}}]}}]},
        {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}],
         "usage": {"prompt_tokens": prompt_tokens, "completion_tokens": 5}},
    ]  # fmt: skip
    return b"".join(f"data: {json.dumps(c)}\n\n".encode() for c in chunks) + b"data: [DONE]\n\n"


async def test_a_long_session_on_a_chat_service_has_its_old_tool_calls_cleared(
    tmp_path: Path, monkeypatch: Any
) -> None:
    monkeypatch.setattr(harness, "CLEAR_AFTER", 8000)  # more than the conversation's characters over four reach
    sections = list(harness.Workspace.start(setup(), 5, "sketch").sections)
    big = "; ".join(f"{s % 16} {40 + s % 12} 1 {60 + s}" for s in range(48))
    calls = [("write", {"part": "bass", "section": sections[i % len(sections)], "notes": big}) for i in range(30)]
    calls += [("finish", {"summary": "Done."})]
    bodies: list[dict[str, Any]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content)
        bodies.append(body)
        turn = sum(1 for m in body["messages"] if m["role"] == "assistant")
        name, args = calls[min(turn, len(calls) - 1)]
        # Notes count about 2.5 tokens per 4 characters, as DeepSeek counted them.
        tokens = int(len(request.content) / 4 * 2.5)
        return httpx.Response(200, content=openai_tool_call(name, args, turn, tokens))

    service = make_service(tmp_path, handler)
    saved = await service.handle(
        "ai.v1.connections.save",
        {"name": "DeepSeek", "provider": "openai", "key": "sk-test", "baseUrl": "https://chat.test/v1"},
        no_notify,
    )
    service.routing.set(compose.FEATURE, [{"connection": saved["id"], "model": "chat-model"}])  # as the user picks
    song = await run_job(service, "ai.v1.song.compose", params(tools="basic"))
    assert bodies, song.get("note")
    last = bodies[-1]["messages"]
    tool_messages = [m for m in last if m["role"] == "tool"]
    assert any(m["content"] == "[cleared]" for m in tool_messages)
    assert all(m["content"] != "[cleared]" for m in tool_messages[-8:])  # the newest are kept
    assert song["composed"]["usage"]["cleared"] > 0


def test_finish_is_refused_once_while_check_finds_issues() -> None:
    ws = harness.Workspace.start(setup(), 5, "sketch")
    open_issues = ws.issues()
    assert open_issues  # nothing written yet
    with pytest.raises(harness.Mistake, match=f"check still finds {len(open_issues)} issues"):
        ws.call("finish", {"summary": "Too soon."})
    assert not ws.finished
    assert ws.call("finish", {"summary": "As it is."}).startswith("Finished")  # asked again: taken as meant
    assert ws.finished


def test_with_the_rules_finding_check_asks_the_model_to_write_the_parts_carrying_the_song() -> None:
    ws = harness.Workspace.start(setup(), 5, "sketch")
    ws.call("rules_write", {"part": "bass"})
    ws.call("rules_write", {"part": "drums"})
    assert not any("written by Spinus's rules" in line for line in ws.issues())  # off by default
    ws.rules_finding = True
    found = [line for line in ws.issues() if "written by Spinus's rules" in line]
    assert found == ["bass was written by Spinus's rules, not by you: write it"]  # the drums may stay the rules'
    assert all("written by" not in line for line in ws.issues("drums"))


async def test_the_rules_finding_reaches_the_harness_and_stays_for_the_revision(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    script: Script = [[("rules_write", {"part": "bass"})], [("check", {})], [("finish", {"summary": "s"})]]
    script += [[("finish", {"summary": "s"})]]
    service = scripted(tmp_path, script, sent)
    await connect(service)
    song = await run_job(service, "ai.v1.song.compose", params(tools="basic", rulesFinding=True))
    checked = sent[2]["messages"][-1]["content"][0]["content"]
    assert "bass was written by Spinus's rules, not by you: write it" in checked
    assert song["composed"]["rulesFinding"] is True


def test_section_kinds_cover_the_structure_tags_and_play_as_their_base() -> None:
    from spinus_ai import songwriter as sw

    for tag in ("pre-chorus", "post-chorus", "hook", "break", "breakdown", "build", "drop", "interlude"):
        assert tag in sw.KINDS
    for tag in ("instrumental", "solo", "end", "fade-out"):
        assert tag in sw.KINDS
    assert all(k in sw.KINDS for k in sw.BASE_KINDS)  # old songs' kinds are still kinds, unchanged
    assert sw.base_kind("drop") == "chorus" and sw.base_kind("breakdown") == "bridge" and sw.base_kind("x") == "verse"
    assert sw.ENERGY["drop"] > sw.ENERGY["verse"] > sw.ENERGY["breakdown"] and sw.ENERGY["break"] < 0.5
    assert sw.KIND_CONTOUR["build"] == "rise" and sw.KIND_CONTOUR["fade-out"] == "fall"
    assert sw.asked_sections("intro, a post-chorus, a drop, then a fade out") == [
        ["intro"],
        ["post-chorus"],
        ["drop"],
        ["fade out"],
    ]
    assert describe_sections("a drop, then a breakdown") == [["drop"], ["breakdown"]]


def describe_sections(text: str) -> Any:
    from spinus_ai import describe

    return describe.describe(text)["described"]["sections"]


async def test_new_kinds_and_cues_go_through_the_tools_into_the_song(tmp_path: Path) -> None:
    ws = harness.Workspace.start(harness.setup_of(described("Intro, a build, a drop and a breakdown.")), 5, "sketch")
    chords = next(iter(ws.sections.values()))["chords"]
    form = [
        ("intro", "intro", ""),
        ("build", "build", "snare roll"),
        ("drop", "drop", ""),
        ("breakdown", "breakdown", "drums fall away"),
    ]
    said = ws.call(
        "set_form",
        {
            "sections": [{"id": i, "kind": k, "bars": 8, "chords": chords, "cue": c} for i, k, c in form],
            "order": [i for i, _, _ in form],
        },
    )
    assert "form" in said.lower() or said
    assert "breakdown (drums fall away)" in ws.call("form", {})
    assert not [line for line in ws.issues() if line.startswith("form:")]  # the kinds are what was asked for
    song = ws.song()
    by_id = {s["id"]: s for s in song["sections"]}
    assert by_id["breakdown"]["kind"] == "breakdown" and by_id["breakdown"]["cue"] == "drums fall away"
    assert by_id["drop"]["name"] == "Drop" and by_id["build"]["contour"] == "rise"
    assert by_id["drop"]["energy"] > by_id["breakdown"]["energy"]
    assert any("drums fall away." in line for line in sw.describe(song))  # the page shows the cue
