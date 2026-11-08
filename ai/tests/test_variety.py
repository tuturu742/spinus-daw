"""Variety: how far a written song may move from the reference songs (close, balanced, adventurous), for the rules'
writer and the AI's (its brief and Spinus's checks)."""

from __future__ import annotations

import hashlib
import json
import statistics
from pathlib import Path
from typing import Any

import pytest
from test_compose import anthropic_tool, fake
from test_compose import connect as connect_compose
from test_harness import BRIEF
from test_service import make_service, no_notify, run_job
from test_songwriter import summary

from spinus_ai import compose, harness
from spinus_ai import songwriter as sw
from spinus_ai.service import RpcError

SEEDS = range(30)

# The songs the rules wrote for the test summary before there was a variety (sha256 of the song as JSON, without its
# `variety` and the vocabulary's order among chords heard as often, which is Python's set order): balanced must
# write exactly these.
BEFORE = {
    ("sketch", 0): "c438badc268246e9", ("sketch", 1): "62fb2662a3ba683e", ("sketch", 2): "7fbb8bbec91120fd",
    ("sketch", 3): "044eee6be3f4716f", ("sketch", 4): "202f76f3266c8891", ("sketch", 5): "715905125a062292",
    ("sketch", 6): "cb199aa8a639a411", ("sketch", 7): "7d4894388ffcf5c2",
    ("full", 0): "5706329d7b831701", ("full", 1): "2d177d1d1e357dd1", ("full", 2): "2fc73b6452d16da3",
    ("full", 3): "24b0bb6eb020ecc3", ("full", 4): "4ea6ea41effab45d", ("full", 5): "ff423e3a460a1d76",
    ("full", 6): "fc2926307c565c37", ("full", 7): "54b6de34e865daf4",
}  # fmt: skip


def digest(song: dict[str, Any]) -> str:
    song = json.loads(json.dumps(song))
    song.pop("variety", None)
    song["checks"]["chords"].pop("vocabulary")
    return hashlib.sha256(json.dumps(song, sort_keys=True).encode()).hexdigest()[:16]


def songs(variety: str) -> list[dict[str, Any]]:
    return [sw.write(summary(), seed, variety=variety) for seed in SEEDS]


def test_balanced_writes_exactly_the_songs_written_before() -> None:
    for (size, seed), before in BEFORE.items():
        song = sw.write(summary(), seed, size)
        assert digest(song) == before, (size, seed)
        assert sw.write(summary(), seed, size, variety="balanced") == song
        assert song["variety"] == "balanced" and not any("Variety" in line for line in song["plain"])
    # Its AI brief is the brief as it was.
    setup = compose.Setup.make(summary(), None, "sketch")
    assert setup.variety is sw.BALANCED and "variety" not in json.loads(compose.first_message(setup))
    assert "Variety" not in harness.Workspace.start(harness.setup_of(BRIEF), 5, "sketch").tool_brief()


@pytest.mark.parametrize("variety", ["close", "adventurous"])
def test_each_variety_writes_the_same_song_for_the_same_seed(variety: str) -> None:
    for seed in (3, 11):
        first = sw.write(summary(), seed, "full", variety=variety)
        assert sw.write(summary(), seed, "full", variety=variety) == first
        assert first["variety"] == variety
        assert digest(first) != digest(sw.write(summary(), seed, "full"))
        assert any(line.startswith("Variety: ") for line in first["plain"])


def test_close_is_tighter_and_adventurous_wider_over_many_seeds() -> None:
    by = {v: songs(v) for v in sw.VARIETIES}
    s = summary()
    low, high, median = s["tempo"]["min"], s["tempo"]["max"], s["tempo"]["median"]

    def spread(variety: str, value: Any) -> float:
        return statistics.pstdev([float(value(song)) for song in by[variety]])

    # Tempo: close within 1 % of the median; balanced within 3 % and the songs' range; adventurous beyond it.
    tempos = {v: [song["tempo"] for song in by[v]] for v in by}
    assert all(abs(t - median) <= median * 0.01 + 0.5 for t in tempos["close"])
    assert all(low <= t <= high for t in tempos["close"] + tempos["balanced"])
    assert any(not low <= t <= high for t in tempos["adventurous"])
    assert all(abs(t - median) <= median * 0.1 + 0.5 for t in tempos["adventurous"])
    assert spread("close", lambda x: x["tempo"]) < spread("balanced", lambda x: x["tempo"])
    assert spread("balanced", lambda x: x["tempo"]) < spread("adventurous", lambda x: x["tempo"])

    # Keys: close keeps the songs' own keys; adventurous goes to more keys than balanced.
    theirs = {k["name"] for k in s["keys"]}
    keys = {v: {song["key"]["name"] for song in by[v]} for v in by}
    assert keys["close"] <= theirs
    assert len(keys["close"]) < len(keys["balanced"]) < len(keys["adventurous"])

    # Chords: close and balanced only the songs' own; adventurous borrows some from the other mode.
    def outside(song: dict[str, Any]) -> list[str]:
        own = set(song["checks"]["chords"]["vocabulary"])
        return [c for x in song["sections"] for c in x["loop"] if c not in own]

    assert not any(outside(song) for song in by["close"] + by["balanced"])
    borrowed = [c for song in by["adventurous"] for c in outside(song)]
    assert borrowed and set(borrowed) <= set(sw.BORROWED["major"] + sw.BORROWED["minor"])

    # Form and length: adventurous takes more forms and a wider range of song lengths.
    forms = {v: {tuple(e["section"] for e in song["arrangement"]) for song in by[v]} for v in by}
    assert len(forms["adventurous"]) > len(forms["balanced"]) >= len(forms["close"])
    assert spread("close", lambda x: x["bars"]) <= spread("balanced", lambda x: x["bars"])
    assert spread("balanced", lambda x: x["bars"]) < spread("adventurous", lambda x: x["bars"])

    # Busyness: the lead's notes a bar vary much more between adventurous songs.
    def lead(song: dict[str, Any]) -> float:
        return float(song["checks"]["parts"]["lead"]["notesPerBar"])

    assert spread("adventurous", lead) > 2 * spread("balanced", lead)


def test_a_redo_keeps_the_songs_variety() -> None:
    song = sw.write(summary(), 4, variety="adventurous")
    again = sw.redo(song, summary(), 9, section="chorus")
    assert again["variety"] == "adventurous" and again["parts"]["drums"]["verse"] == song["parts"]["drums"]["verse"]
    # A song from before there was a variety is balanced.
    old = sw.write(summary(), 4)
    old.pop("variety")
    assert sw.redo(old, summary(), 9, part="lead")["variety"] == "balanced"


def test_an_unknown_variety_is_refused() -> None:
    with pytest.raises(ValueError, match="close, balanced, adventurous"):
        sw.variety_of("wild")
    assert sw.variety_of(None) is sw.BALANCED and sw.variety_of(" Close ") is sw.VARIETIES["close"]
    with pytest.raises(ValueError, match="variety"):
        compose.Setup.make(summary(), None, "sketch", variety="wild")


async def test_the_service_takes_the_variety_and_refuses_an_unknown_one(tmp_path: Path) -> None:
    service = make_service(tmp_path, lambda request: None)  # no AI connection: nothing is sent
    s = summary()
    song = await run_job(service, "ai.v1.song.write", {"summary": s, "seed": 4, "variety": "close"})
    assert song == sw.lift_last_chorus(sw.write(s, 4, variety="close"))
    assert await run_job(service, "ai.v1.song.write", {"summary": s, "seed": 4}) == sw.lift_last_chorus(sw.write(s, 4))
    redone = await run_job(service, "ai.v1.song.write", {"summary": s, "seed": 5, "song": song,
                                                         "redo": {"part": "lead"}})  # fmt: skip
    assert redone["variety"] == "close"
    # The AI writer without a connection: the rules' song, with the variety asked.
    ruled = await run_job(service, "ai.v1.song.compose", {"summary": s, "seed": 4, "variety": "adventurous"})
    assert ruled["variety"] == "adventurous" and "no AI connection" in ruled["note"]
    for method in ("ai.v1.song.write", "ai.v1.song.compose"):
        with pytest.raises(RpcError, match="The variety is close, balanced, adventurous, not 'wild'"):
            await service.handle(method, {"summary": s, "seed": 1, "variety": "wild"}, no_notify)


def test_the_ais_brief_says_how_free_it_is() -> None:
    close = compose.Setup.make(summary(), None, "sketch", variety="close")
    wide = compose.Setup.make(summary(), None, "sketch", variety="adventurous")
    assert "close to the references" in json.loads(compose.first_message(close))["variety"]
    said = json.loads(compose.first_message(wide))["variety"]
    assert "adventurous" in said and "relative major or minor" in said and "instruments added" in said
    ws = harness.Workspace.start(harness.setup_of({**BRIEF, "variety": "adventurous"}), 5, "sketch")
    assert "Variety: adventurous" in ws.tool_brief() and ws.setup.variety.name == "adventurous"
    # The offline plan's variation by the AI is told too.
    song = sw.write(summary(), 4, variety="close")
    assert (
        "close to the references" in json.loads(sw.describe_for_ai(sw.Style.from_summary(summary()), song))["variety"]
    )


def test_the_tempo_the_ai_says_is_held_closer_or_let_go_further() -> None:
    answer = {"tempo": 300, "sections": [{"id": "v", "kind": "verse", "bars": 8, "chords": ["i"]}], "order": ["v"]}
    tempos = {
        v: compose.check_plan(answer, compose.Setup.make(summary(), None, "sketch", variety=v))["tempo"]
        for v in sw.VARIETIES
    }
    high = summary()["tempo"]["max"]
    assert tempos["balanced"] == round(high * 1.1)  # as it always was
    assert tempos["close"] == round(high * 1.05) and tempos["adventurous"] == round(high * 1.25)


def workspace(variety: str, lead_per_bar: float) -> harness.Workspace:
    """The harness's working copy with drums, a bass and a lead playing `lead_per_bar` notes a bar (a two-bar loop)."""
    ws = harness.Workspace.start(harness.setup_of({**BRIEF, "variety": variety}), 5, "sketch")
    tonic = int(ws.key["tonic"])
    count = round(2 * lead_per_bar)
    lead = "; ".join(f"{32 * i // count} {(60 if i % 2 else 67) + tonic} 2 90" for i in range(count))
    for sid in ws.sections:
        ws.call("write", {"part": "drums", "section": sid, "notes": "0 kick 1 100; 4 snare 1 100", "loop_bars": 1})
        ws.call("write", {"part": "bass", "section": sid, "notes": f"0 {36 + tonic} 4 100", "loop_bars": 1})
        ws.call("write", {"part": "lead", "section": sid, "notes": lead, "loop_bars": 2})
    return ws


def lead_issue(ws: harness.Workspace) -> bool:
    return any(line.startswith("lead: ") and "notes a bar" in line for line in ws.issues())


def test_the_checks_loosen_for_adventurous_and_tighten_for_close() -> None:
    # The references' vocals play 3 notes a bar: 8 is too busy for balanced and close, not for adventurous.
    assert lead_issue(workspace("balanced", 8)) and lead_issue(workspace("close", 8))
    assert not lead_issue(workspace("adventurous", 8))
    # 1.5 a bar is fine for balanced, too sparse for close.
    assert not lead_issue(workspace("balanced", 1.5)) and lead_issue(workspace("close", 1.5))

    # Close also finds a tempo outside the references' range and chords that are not theirs; the others don't.
    for variety in sw.VARIETIES:
        ws = workspace(variety, 4)
        ws.tempo = 150.0
        first = next(iter(ws.sections))
        ws.call("set_chords", {"section": first, "chords": ["i", "bII", "i", "bII"]})
        found = ws.issues()
        close = variety == "close"
        assert any("tempo 150 BPM is outside the references'" in f for f in found) == close
        assert any("bII" in f and "not the references' own" in f for f in found) == close


async def test_the_one_shot_writer_sends_the_variety_and_checks_with_it(tmp_path: Path) -> None:
    answer = {
        "tempo": 150,
        "key": "A minor",
        "sections": [{"id": "verse", "kind": "verse", "bars": 8, "chords": ["i", "bII", "bVI", "V"]}],
        "order": ["verse"] * 4,
        "parts": {"lead": {"sections": {"verse": {"bars": 1, "notes": "0 69 4 90; 8 76 4 90"}}}},
    }
    found: dict[str, list[str]] = {}
    for variety in ("close", "adventurous"):
        sent: list[dict[str, Any]] = []
        service = fake(tmp_path / variety, [anthropic_tool(json.dumps(answer))], sent)
        await connect_compose(service)
        song = await run_job(service, "ai.v1.song.compose", {"summary": summary(), "seed": 7, "way": "one-shot",
                                                             "variety": variety})  # fmt: skip
        brief = json.loads(sent[0]["messages"][0]["content"][0]["text"])
        assert variety in brief["variety"] and song["variety"] == variety
        found[variety] = song["composed"]["findings"]
    assert any("outside the references'" in f for f in found["close"])
    assert not any("outside the references'" in f or "references' own" in f for f in found["adventurous"])
