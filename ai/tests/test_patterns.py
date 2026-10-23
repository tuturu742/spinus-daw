"""The pattern generator: note names, the key, drum pads, the grid, swing, humanize, and
the whole job with a made-up provider."""

from __future__ import annotations

import json
import random
from pathlib import Path
from typing import Any

import httpx
import pytest
from test_service import anthropic_reply, make_service, no_notify, run_job, sse

from spinus_ai.patterns import Pad, Settings, Target, describe, finish, nearest_in_key, note_name, parse_note

KEY_C_MAJOR = {"tonic": 0, "scale": "major"}
DRUM_PADS = [{"name": "Kick", "note": 60}, {"name": "Snare", "note": 61}, {"name": "Closed Hat", "note": 63}]


def settings(**changes: Any) -> Settings:
    data: dict[str, Any] = {
        "request": "a funky bass line",
        "targets": [{"channel": 7, "name": "Bass", "instrument": "3x Osc"}],
        "context": [
            {
                "name": "Drums",
                "instrument": "Drum Machine",
                "pads": DRUM_PADS,
                "notes": [{"start": 0, "length": 240, "pitch": 60}, {"start": 960, "length": 240, "pitch": 61}],
            }
        ],
        "tempo": 100,
        "lengthTicks": 3840,
        "key": KEY_C_MAJOR,
    }
    data.update(changes)
    return Settings.from_json(data)


def test_note_names_use_scientific_pitch() -> None:
    assert parse_note("C4") == 60
    assert parse_note("c#4") == 61
    assert parse_note("Bb3") == 58
    assert parse_note("E1") == 28
    assert parse_note("C-1") == 0
    assert parse_note("H2") is None
    assert parse_note("C11") is None
    assert note_name(60) == "C4"
    assert note_name(61) == "C#4"
    assert all(parse_note(note_name(n)) == n for n in range(128))


def test_out_of_key_notes_go_to_the_nearest_note_of_the_key() -> None:
    assert nearest_in_key(61, (0, "major")) == 60  # C# -> C (a tie goes down)
    assert nearest_in_key(66, (0, "major")) == 65  # F# -> F
    assert nearest_in_key(70, (9, "minor")) == 69
    assert nearest_in_key(64, (0, "major")) == 64


def test_what_the_model_wrote_is_checked_snapped_and_kept_in_the_pattern() -> None:
    s = settings()
    variation = finish(
        {
            "title": "Funky",
            "parts": [
                {
                    "channel": 7,
                    "notes": [
                        {"step": 0, "length": 2, "note": "C2", "velocity": 0.9},
                        {"step": 3.4, "length": 1, "note": "F#2"},  # off the grid and the key
                        {"step": 15, "length": 4, "note": "G2"},  # longer than what is left
                        {"step": 16, "length": 1, "note": "A2"},  # after the pattern's end
                        {"step": 4, "length": 1, "note": "nonsense"},
                        {"step": "x", "note": "C2"},
                        {"step": 0, "length": 1, "note": "C2"},  # the same note again
                    ],
                },
                {"channel": 99, "notes": [{"step": 0, "length": 1, "note": "C2"}]},  # not asked for
            ],
        },
        s,
        random.Random(0),
    )
    assert variation is not None
    assert variation["title"] == "Funky"
    notes = variation["parts"]["7"]
    assert [(n["start"], n["pitch"]) for n in notes] == [(0, 36), (720, 41), (3600, 43)]
    assert notes[0]["length"] == 240  # the later duplicate won
    assert notes[2]["length"] == 240  # cut at the pattern's end
    assert "99" not in variation["parts"]

    assert (
        finish({"parts": [{"channel": 7, "notes": [{"step": 40, "length": 1, "note": "C2"}]}]}, s, random.Random(0))
        is None
    )


def test_drum_channels_use_their_pads_by_name() -> None:
    s = settings(targets=[{"channel": 3, "name": "Beat", "instrument": "Drum Machine", "pads": DRUM_PADS}])
    variation = finish(
        {
            "parts": [
                {
                    "channel": 3,
                    "notes": [
                        {"step": 0, "length": 1, "note": "Kick"},
                        {"step": 2, "length": 1, "note": "hat"},
                        {"step": 4, "length": 1, "note": "snare"},
                        {"step": 6, "length": 1, "note": "Cowbell"},  # no such pad
                        {"step": 8, "length": 1, "note": "C#4"},  # a pad's note
                    ],
                }
            ]
        },
        s,
        random.Random(0),
    )
    assert variation is not None
    assert [(n["start"], n["pitch"]) for n in variation["parts"]["3"]] == [(0, 60), (480, 63), (960, 61), (1920, 61)]


def test_swing_and_humanize() -> None:
    notes = {"parts": [{"channel": 7, "notes": [{"step": s, "length": 1, "note": "C2"} for s in range(4)]}]}
    swung = finish(notes, settings(swing=1.0), random.Random(0))
    assert swung is not None
    assert [n["start"] for n in swung["parts"]["7"]] == [0, 320, 480, 800]

    first = finish(notes, settings(humanize=1.0), random.Random(4))
    again = finish(notes, settings(humanize=1.0), random.Random(4))
    assert first == again  # the same seed, the same feel
    assert first is not None
    starts = [n["start"] for n in first["parts"]["7"]]
    assert starts != [0, 240, 480, 720]
    assert all(abs(s - 240 * i) <= 20 for i, s in enumerate(starts))


def test_the_request_describes_the_song_the_other_channels_and_the_notes_to_adjust() -> None:
    text = describe(settings())
    assert "a funky bass line" in text
    assert "Key: C major" in text
    assert "16 sixteenth steps (1 bars)" in text
    assert "'Drums' (Drum Machine): 0:Kick/1 4:Snare/1" in text
    assert "Call write_pattern once." in text

    adjusting = describe(
        settings(
            adjust=True,
            variations=4,
            targets=[{"channel": 7, "name": "Bass", "notes": [{"start": 480, "length": 480, "pitch": 36}]}],
        )
    )
    assert "its notes now (step:note/length): 2:C2/2" in adjusting
    assert "Call write_pattern 4 times" in adjusting
    assert Settings.from_json({"targets": [], "variations": 12}).variations == 4


# ---------------------------------------------------------------------------------------------------


def tool_reply(*inputs: dict[str, Any]) -> bytes:
    """An Anthropic answer calling write_pattern once per input."""
    events: list[tuple[str, dict[str, Any]]] = [
        ("message_start", {"type": "message_start", "message": {"model": "claude-test", "usage": {"input_tokens": 50}}})
    ]
    for index, data in enumerate(inputs):
        events += [
            (
                "content_block_start",
                {
                    "type": "content_block_start",
                    "index": index,
                    "content_block": {"type": "tool_use", "id": f"t{index}", "name": "write_pattern"},
                },
            ),
            (
                "content_block_delta",
                {
                    "type": "content_block_delta",
                    "index": index,
                    "delta": {"type": "input_json_delta", "partial_json": json.dumps(data)},
                },
            ),
            ("content_block_stop", {"type": "content_block_stop", "index": index}),
        ]
    events.append(
        (
            "message_delta",
            {"type": "message_delta", "delta": {"stop_reason": "tool_use"}, "usage": {"output_tokens": 30}},
        )
    )
    return sse(events)


async def test_the_job_asks_once_and_returns_the_variations(tmp_path: Path) -> None:
    requests: list[dict[str, Any]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(json.loads(request.content))
        return httpx.Response(
            200,
            content=tool_reply(
                {"title": "One", "parts": [{"channel": 7, "notes": [{"step": 0, "length": 2, "note": "C2"}]}]},
                {"title": "Two", "parts": [{"channel": 7, "notes": [{"step": 2, "length": 2, "note": "E2"}]}]},
            ),
            headers={"content-type": "text/event-stream"},
        )

    service = make_service(tmp_path, handler)
    await service.handle(
        "ai.v1.connections.save", {"name": "Claude", "provider": "anthropic", "key": "sk-test"}, no_notify
    )
    request = {
        "request": "two takes of a bass line",
        "targets": [{"channel": 7, "name": "Bass"}],
        "lengthTicks": 3840,
        "variations": 2,
    }
    result = await run_job(service, "ai.v1.pattern.generate", request)
    assert [v["title"] for v in result["variations"]] == ["One", "Two"]
    assert result["variations"][1]["parts"]["7"] == [{"start": 480, "length": 480, "pitch": 40, "velocity": 0.8}]
    assert len(requests) == 1
    assert requests[0]["tools"][0]["name"] == "write_pattern"
    assert "two takes of a bass line" in requests[0]["messages"][0]["content"][0]["text"]

    # Nothing usable: a plain error the user can act on.
    def unhelpful(request: httpx.Request) -> httpx.Response:
        return httpx.Response(
            200, content=anthropic_reply("I can't write music."), headers={"content-type": "text/event-stream"}
        )

    service = make_service(tmp_path / "other", unhelpful)
    await service.handle(
        "ai.v1.connections.save", {"name": "Claude", "provider": "anthropic", "key": "sk-test"}, no_notify
    )
    with pytest.raises(RuntimeError, match="did not write any usable notes"):
        await run_job(service, "ai.v1.pattern.generate", request)


def test_pads_from_json() -> None:
    assert Target.from_json({"channel": 1, "pads": [{"name": "Kick", "note": 60}]}).pads == [Pad("Kick", 60)]


async def test_the_assistant_asks_the_pattern_generator_and_the_notes_become_a_suggestion(tmp_path: Path) -> None:
    from test_service import anthropic_reply

    replies = iter(
        [
            anthropic_reply("", ("write_notes", {"request": "a bass line", "pattern": 11, "channels": [9]})),
            tool_reply(
                {"title": "Walking", "parts": [{"channel": 9, "notes": [{"step": 0, "length": 4, "note": "A1"}]}]}
            ),
            anthropic_reply("I wrote a walking bass line: accept it in the Piano Roll if you like it."),
        ]
    )
    sent: list[dict[str, Any]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        sent.append(json.loads(request.content))
        return httpx.Response(200, content=next(replies), headers={"content-type": "text/event-stream"})

    service = make_service(tmp_path, handler)
    await service.handle(
        "ai.v1.connections.save", {"name": "Claude", "provider": "anthropic", "key": "sk-test"}, no_notify
    )
    project = json.loads((Path(__file__).parents[2] / "tests/model/files/example-v1.json").read_text())
    answer = await run_job(
        service,
        "ai.v1.assistant.ask",
        {"conversation": [{"role": "user", "text": "Write me a bass line"}], "context": {"project": project}},
    )

    assert "walking bass" in answer["text"]
    assert answer["proposals"] == []
    [suggestion] = answer["suggestions"]
    assert suggestion["pattern"] == 11
    assert suggestion["variations"][0]["parts"]["9"][0]["pitch"] == 33  # A1, in A minor
    # The generator was told about the synth and the kick it fits with.
    generator_request = sent[1]["messages"][0]["content"][0]["text"]
    assert "channel 9: 'Surge'" in generator_request
    assert "'Kick'" in generator_request
    assert any(t["name"] == "write_notes" for t in sent[0]["tools"])


def test_longer_patterns_get_more_room_for_the_answer() -> None:
    from spinus_ai.patterns import Settings, output_budget

    def budget(bars: int, variations: int = 1) -> int:
        return output_budget(Settings.from_json({"lengthTicks": bars * 3840, "variations": variations, "targets": []}))

    assert budget(1) == budget(4) == 8192
    assert budget(8) == 16384
    assert budget(16, 4) == 32000


def test_bars_follow_the_time_signature() -> None:
    from spinus_ai.patterns import Settings, describe

    settings = Settings.from_json(
        {"lengthTicks": 2 * 2880, "beatsPerBar": 6, "barTicks": 2880, "timeSignature": "6/8", "targets": []}
    )
    assert settings.bar_steps == 12
    assert "time signature 6/8 (a bar is 12 sixteenth steps)" in describe(settings)
    assert "(2 bars)" in describe(settings)
    old = Settings.from_json({"lengthTicks": 3840, "beatsPerBar": 4, "targets": []})  # older callers
    assert old.bar_steps == 16 and old.signature == "4/4"
