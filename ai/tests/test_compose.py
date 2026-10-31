"""The Track creator's AI song writer (spinus_ai.compose) with fake providers, no real API calls: the model's song
is checked note by note, a part it botches or leaves out is written by the rules, an answer cut off at the length
limit is continued and joined, a revision changes only what it names, every request's tokens (reasoning too) are
reported as the provider gave them."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import httpx
import pytest
from test_service import make_service, no_notify, run_job
from test_songwriter import summary

from spinus_ai import compose, songwriter
from spinus_ai.connections import Connection
from spinus_ai.gateway import Message, Request, Text, anthropic_body, complete, openai_body

PARTS = [
    {"id": "drums", "name": "Drums", "role": "drums"},
    {"id": "bass", "name": "Bass", "role": "bass"},
    {"id": "gtr", "name": "Rhythm guitar", "role": "rhythm-guitar"},
    {"id": "lead", "name": "Lead", "role": "lead"},
    {"id": "riser", "name": "Riser", "role": "riser"},
]
CANDIDATES = {"bass": [{"id": "surge:Reese", "name": "Surge XT: Reese Bass"}, {"id": "obxf:Sub", "name": "OB-Xf: Sub"}]}


def song_answer() -> dict[str, Any]:
    beat = "0 kick 1 110; 4 snare 1 100; 8 36 1 105; 12 38 1 100; " + "; ".join(
        f"{s} hat 1 70" for s in range(0, 16, 2)
    )
    return {
        "tempo": 126,
        "key": "A minor",
        "sections": [
            {"id": "verse", "kind": "verse", "bars": 8, "chords": ["i", "bVI", "bIII", "bVII"], "energy": 0.6},
            {"id": "chorus", "kind": "chorus", "bars": 8, "chords": ["bVI", "bVII", "i", "i"], "barsPerChord": 1},
        ],
        "order": ["verse", "chorus", "verse", "chorus"],
        "parts": {
            "drums": {"sections": {"verse": {"bars": 1, "notes": beat}, "chorus": {"bars": 1, "notes": beat}}},
            # A bass an octave too low (folded up), a note off the grid, one beyond its loop (dropped).
            "bass": {
                "sound": "surge:Reese",
                "sections": {
                    "verse": {"bars": 1, "notes": "0 21 4 100; 4.5 33 2 90; 8 45 4 90; 99 45 1 90"},
                    "chorus": {"bars": 2, "notes": "0 41 8 110; 16 43 8 110"},
                },
            },
            "gtr": {"sound": "nowhere:Unknown", "sections": {"verse": "", "chorus": {"bars": 1, "notes": "0 57 4 90"}}},
            # The lead is left out: the rules write it.
        },
        "reason": "A brooding verse that opens up.",
    }


def sse(events: list[dict[str, Any]]) -> bytes:
    return "".join(f"event: {e['type']}\ndata: {json.dumps(e)}\n\n" for e in events).encode()


def anthropic_tool(
    arguments: str, stop: str = "tool_use", end: bool = True, usage: dict[str, Any] | None = None
) -> bytes:
    """A write_song call, its arguments in two pieces; cut off (no block end, stop max_tokens) when asked."""
    events: list[dict[str, Any]] = [
        {"type": "message_start", "message": {"model": "claude-test", "usage": {"input_tokens": 1200}}},
        {"type": "content_block_start", "index": 0, "content_block": {"type": "thinking", "thinking": ""}},
        {"type": "content_block_delta", "index": 0, "delta": {"type": "thinking_delta", "thinking": ""}},
        {"type": "content_block_stop", "index": 0},
        {"type": "content_block_start", "index": 1,
         "content_block": {"type": "tool_use", "id": "t1", "name": "write_song"}},
    ]  # fmt: skip
    for piece in (arguments[:40], arguments[40:]):
        events.append(
            {"type": "content_block_delta", "index": 1, "delta": {"type": "input_json_delta", "partial_json": piece}}
        )
    if end:
        events.append({"type": "content_block_stop", "index": 1})
    events.append(
        {
            "type": "message_delta",
            "delta": {"stop_reason": stop},
            "usage": usage or {"output_tokens": 3000, "output_tokens_details": {"thinking_tokens": 900}},
        }
    )
    return sse(events)


def fake(tmp_path: Path, replies: list[bytes], sent: list[dict[str, Any]], counted: int = 1500) -> Any:
    def handler(request: httpx.Request) -> httpx.Response:
        if request.url.path.endswith("/count_tokens"):
            return httpx.Response(200, json={"input_tokens": counted})
        sent.append(json.loads(request.content))
        return httpx.Response(200, content=replies.pop(0), headers={"content-type": "text/event-stream"})

    return make_service(tmp_path, handler)


async def connect(service: Any) -> None:
    await service.handle(
        "ai.v1.connections.save", {"name": "Claude", "provider": "anthropic", "key": "sk-test"}, no_notify
    )


def params(**more: Any) -> dict[str, Any]:
    return {"summary": summary(), "seed": 7, "parts": PARTS, "candidates": CANDIDATES, "way": "one-shot", **more}


def played(song: dict[str, Any], pid: str) -> list[dict[str, Any]]:
    return [n for e in song["arrangement"] for n in song["layerParts"][pid].get(e["section"], [])]


async def test_the_ais_song_is_checked_and_what_it_left_out_written_by_the_rules(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    service = fake(tmp_path, [anthropic_tool(json.dumps(song_answer()))], sent)
    await connect(service)
    song = await run_job(service, "ai.v1.song.compose", params(effort="high"))

    assert song["composedBy"] == "claude-test" and song["tempo"] == 126 and song["key"]["name"] == "A minor"
    assert [s["name"] for s in song["sections"]] == ["Verse", "Chorus"] and song["bars"] == 32
    assert sent[0]["output_config"] == {"effort": "high"} and sent[0]["max_tokens"] == 64000
    assert sent[0]["tools"][0]["name"] == "write_song"
    brief = json.loads(sent[0]["messages"][0]["content"][0]["text"])
    assert [p["id"] for p in brief["parts"]] == ["drums", "bass", "gtr", "lead"]  # the riser is the rules'
    assert brief["parts"][1]["sounds"] == CANDIDATES["bass"]

    # Drums: names and General MIDI notes both reach the kit's drums, repeated over the sections.
    drums = played(song, "drums")
    assert {n["drum"] for n in drums} == {"kick", "snare", "hat"} and len(drums) == 32 * 12
    bass = played(song, "bass")
    low, high = compose.RANGES["bass"]
    assert all(low <= n["pitch"] <= high for n in bass) and min(n["pitch"] for n in bass) == 33
    assert all(n["start"] % songwriter.TICKS_PER_STEP == 0 for n in bass)
    verse_bass = song["layerParts"]["bass"]["verse"]
    assert len(verse_bass) == 8 * 3  # one-bar loop of three notes (the fourth was past its loop)
    assert song["layerParts"]["gtr"]["verse"] == [] and len(song["layerParts"]["gtr"]["chorus"]) == 8
    assert song["parts"]["bass"]["verse"] == verse_bass  # what the checks and Redo part use
    # Sounds: a candidate taken, an unknown one ignored.
    assert song["sounds"] == {"bass": "surge:Reese"}
    # The lead, left out, is the rules', on the AI's chords; the riser is placed by the rules.
    assert played(song, "lead") and "riser" in song["fx"]
    assert "Lead (left out)" in song["note"] and "Written by the AI (claude-test, reasoning high)" in song["note"]
    findings = song["composed"]["findings"]
    assert any("outside its range" in f for f in findings) and any("off the sixteenth grid" in f for f in findings)
    assert any("Lead was written by Spinus's rules" in f for f in findings)

    usage = song["composed"]["usage"]
    assert (usage["input"], usage["output"], usage["reasoning"]) == (1200, 3000, 900)
    assert "1,200 in, 3,000 out (of them 900 reasoning)" in song["note"]
    assert not (tmp_path / "config" / "compose-usage.json").exists()  # nothing kept beyond the song itself

    # Redo part on the AI's song: only that part is written again.
    again = await run_job(service, "ai.v1.song.write", {"summary": summary(), "seed": 3, "song": song,
                                                        "redo": {"part": "bass"}})  # fmt: skip
    assert again["layerParts"]["drums"] == song["layerParts"]["drums"]
    assert again["layerParts"]["gtr"] == song["layerParts"]["gtr"]
    assert again["layerParts"]["bass"] != song["layerParts"]["bass"] and again["composedBy"] == "claude-test"


@pytest.mark.parametrize(
    "answer, why",
    [
        ({"sections": [{"id": "v", "kind": "kazoo", "bars": 8, "chords": ["i"]}], "order": ["v"]}, "kind of section"),
        ({"sections": [{"id": "v", "kind": "verse", "bars": 8, "chords": ["Xm9"]}], "order": ["v"]}, "Roman"),
        ({"sections": [{"id": "v", "kind": "verse", "bars": 8, "chords": ["i"]}], "order": ["w"]}, "aren't there"),
        ({"sections": [{"id": "v", "kind": "verse", "bars": 8, "chords": ["i"]}], "order": ["v"],
          "parts": {"bass": {"sections": {"v": {"notes": "nan 40 1 90; x y z"}}}}}, "usable notes"),
    ],
)  # fmt: skip
async def test_an_unusable_song_leaves_the_rules_song(tmp_path: Path, answer: dict[str, Any], why: str) -> None:
    sent: list[dict[str, Any]] = []
    service = fake(tmp_path, [anthropic_tool(json.dumps(answer))], sent)
    await connect(service)
    song = await run_job(service, "ai.v1.song.compose", params())
    assert "composed" not in song and why in song["note"] and song["usage"]["input"] == 1200
    assert song["layers"] == songwriter.write(summary(), 7, "sketch", PARTS)["layers"]


async def test_without_a_connection_nothing_is_sent_and_the_rules_write(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    service = fake(tmp_path, [], sent)
    song = await run_job(service, "ai.v1.song.compose", params())
    assert "no AI connection" in song["note"] and not sent
    who = await service.handle("ai.v1.song.composer", {}, no_notify)
    assert who["available"] is False and "Options > AI" in who["reason"]


async def test_an_answer_cut_off_is_continued_and_joined(tmp_path: Path) -> None:
    whole = json.dumps(song_answer())
    cut = whole[: whole.index('"gtr"') + 30]  # the guitar part unfinished
    rest = {"parts": {"gtr": song_answer()["parts"]["gtr"]}}
    sent: list[dict[str, Any]] = []
    service = fake(tmp_path, [anthropic_tool(cut, "max_tokens", end=False), anthropic_tool(json.dumps(rest))], sent)
    await connect(service)
    song = await run_job(service, "ai.v1.song.compose", params())
    assert len(sent) == 2 and "cut off at your length limit" in sent[1]["messages"][0]["content"][0]["text"]
    assert song["composed"]["continued"] == 1 and "continued (1x)" in song["note"]
    assert len(song["layerParts"]["gtr"]["chorus"]) == 8 and song["layerParts"]["bass"]["verse"]
    assert song["composed"]["usage"]["output"] == 6000  # both pieces counted


def test_cut_off_arguments_are_read_up_to_their_last_complete_value() -> None:
    assert compose.salvage('{"a": 1, "b": {"c": [1, 2, 3], "d": "unfin') == {"a": 1, "b": {"c": [1, 2, 3]}}
    assert compose.salvage('{"s": "a, b", "t": [{"x": 1}, {"y": 2') == {"s": "a, b", "t": [{"x": 1}]}
    assert compose.salvage("nothing") is None


async def test_a_revision_changes_what_it_names_and_counts_every_round(tmp_path: Path) -> None:
    revision = {"parts": {"gtr": {"sections": {"verse": {"bars": 1, "notes": "0 52 2 90; 8 52 2 90"}}}}}
    sent: list[dict[str, Any]] = []
    service = fake(tmp_path, [anthropic_tool(json.dumps(song_answer())), anthropic_tool(json.dumps(revision))], sent)
    await connect(service)
    first = await run_job(service, "ai.v1.song.compose", params())
    revised = await run_job(
        service,
        "ai.v1.song.compose",
        params(song=first, levels={"bass": -18.0, "gtr": -100.0}, problems={"drums": "the helper crashed"}),
    )
    asked = json.loads(sent[1]["messages"][0]["content"][0]["text"].split(compose.REVISE)[1])
    assert any("Rhythm guitar made no sound" in f for f in asked["findings"])
    assert any("Drums could not be played" in f for f in asked["findings"])
    assert asked["levelsDb"] == {"Bass": -18.0, "Rhythm guitar": -100.0}
    assert len(revised["layerParts"]["gtr"]["verse"]) == 16 and revised["layerParts"]["gtr"]["chorus"]
    assert revised["layerParts"]["bass"] == first["layerParts"]["bass"]  # not named: kept
    assert revised["composed"]["round"] == 1 and "revised 1 time" in revised["note"]
    rounds = [r["round"] for r in revised["composed"]["usage"]["requests"]]
    assert rounds == [0, 1] and revised["composed"]["usage"]["output"] == 6000

    # A revision that does not hold together leaves the song before it, its tokens still counted.
    service = fake(tmp_path / "b", [anthropic_tool('{"order": ["nowhere"]}')], sent)
    await connect(service)
    kept = await run_job(service, "ai.v1.song.compose", params(song=first))
    assert kept["layerParts"] == first["layerParts"] and "not usable" in kept["note"]
    assert kept["composed"]["usage"]["input"] == 2400


async def test_the_composer_names_the_connection(tmp_path: Path) -> None:
    service = fake(tmp_path, [], [])
    await connect(service)
    who = await service.handle("ai.v1.song.composer", {}, no_notify)
    assert who == {"available": True, "connection": "Claude", "model": "claude-sonnet-5-5", "reason": ""}


def test_effort_and_the_whole_allowance_reach_each_kind_of_provider() -> None:
    request = Request("m", "sys", [Message("user", [Text("hi")])], [], None, "medium")
    assert anthropic_body(request)["output_config"] == {"effort": "medium"} and anthropic_body(request)["max_tokens"]
    openai = Connection("o", "O", "openai", "https://api.openai.com/v1")
    body = openai_body(request, openai, "max_completion_tokens")
    assert body["reasoning_effort"] == "medium" and "max_completion_tokens" not in body and "max_tokens" not in body
    router = Connection("r", "R", "openrouter", "https://openrouter.ai/api/v1")
    assert openai_body(request, router)["reasoning"] == {"effort": "medium"}
    plain = Request("m", "sys", [Message("user", [Text("hi")])])
    assert "output_config" not in anthropic_body(plain) and "reasoning_effort" not in openai_body(plain, openai)


async def test_reasoning_tokens_are_read_from_both_kinds_of_usage() -> None:
    openai_usage = {
        "prompt_tokens": 50,
        "completion_tokens": 400,
        "completion_tokens_details": {"reasoning_tokens": 300},
    }
    chunks = [
        {"choices": [{"delta": {"content": "ok"}, "finish_reason": "stop"}]},
        {"choices": [], "usage": openai_usage},
    ]
    stream = ("".join(f"data: {json.dumps(c)}\n\n" for c in chunks) + "data: [DONE]\n\n").encode()
    replies = {
        "/v1/chat/completions": stream,
        "/v1/messages": anthropic_tool(
            "{}", usage={"output_tokens": 80, "output_tokens_details": {"thinking_tokens": 60}}
        ),
    }

    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(200, content=replies[request.url.path], headers={"content-type": "text/event-stream"})

    async def no_text(_: str) -> None:
        pass

    client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    request = Request("m", "sys", [Message("user", [Text("hi")])], [], None, "high")
    done = await complete(client, Connection("o", "O", "openai", "https://x.test/v1"), "k", request, no_text)
    assert (done.usage.output_tokens, done.usage.reasoning_tokens) == (400, 300)
    done = await complete(client, Connection("a", "A", "anthropic", "https://y.test"), "k", request, no_text)
    assert (done.usage.output_tokens, done.usage.reasoning_tokens) == (80, 60)


async def test_a_model_that_allows_a_shorter_answer_is_asked_again(tmp_path: Path) -> None:
    sizes: list[int] = []

    def handler(request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content)
        sizes.append(body["max_tokens"])
        if body["max_tokens"] > 8192:
            message = "max_tokens: 64000 > 8192, which is the maximum allowed number of output tokens for m"
            return httpx.Response(400, json={"type": "error", "error": {"message": message}})
        return httpx.Response(200, content=anthropic_tool("{}"), headers={"content-type": "text/event-stream"})

    async def no_text(_: str) -> None:
        pass

    client = httpx.AsyncClient(transport=httpx.MockTransport(handler))
    request = Request("m", "sys", [Message("user", [Text("hi")])], [], None)
    await complete(client, Connection("a", "A", "anthropic", "https://y.test"), "k", request, no_text)
    assert sizes == [64000, 8192]
