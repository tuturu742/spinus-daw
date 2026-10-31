"""The project harness: the music harness's tools on the open project, turning edits into Spinus commands."""

from __future__ import annotations

import copy
import json
from pathlib import Path
from typing import Any

import httpx
from test_service import anthropic_reply, collect, make_service

from spinus_ai.manual import Manual
from spinus_ai.project_harness import ProjectHarness, db_to_volume, moved_text, pan_value, pitch_of
from spinus_ai.project_tools import Snapshot, Tools
from spinus_ai.service import load_commands

ROOT = Path(__file__).resolve().parents[2]
EXAMPLE = json.loads((ROOT / "tests/model/files/example-v1.json").read_text())
COMMANDS = load_commands(ROOT / "docs/dev/commands.schema.json")
BAR = 3840


def device(name: str, kind: str, stock_type: str) -> dict[str, Any]:
    return {"name": name, "type": kind, "available": True, "device": {"kind": "stock", "stockType": stock_type}}


DEVICES = [
    device("3x Osc", "instrument", "spinus.3x-osc"),
    device("Sidechain Ducker", "effect", "spinus.ducker"),
    device("Limiter", "effect", "spinus.limiter"),
    device("Hall Reverb", "effect", "bundled.hall"),
]


def song() -> dict[str, Any]:
    """The example project with two sections marked, and a bass and a pad that play."""
    project = copy.deepcopy(EXAMPLE)
    project["settings"]["key"] = {"tonic": 9, "scale": "minor"}
    project["settings"]["timeSignature"] = {"numerator": 4, "denominator": 4}
    project["playlist"]["markers"] = [{"id": 90, "at": 0, "name": "Verse"}, {"id": 91, "at": 4 * BAR, "name": "Chorus"}]
    project["channels"] += [
        {
            "id": 30,
            "name": "Bass",
            "instrument": {"id": 31, "kind": "stock", "stockType": "spinus.3x-osc"},
            "targetInsert": 50,
        },
        {
            "id": 32,
            "name": "Pad",
            "instrument": {"id": 33, "kind": "stock", "stockType": "spinus.3x-osc"},
            "targetInsert": 51,
        },
        {
            "id": 34,
            "name": "Keys",
            "instrument": {"id": 35, "kind": "stock", "stockType": "spinus.3x-osc"},
            "targetInsert": 52,
        },
    ]
    project["mixer"]["inserts"] += [
        {"id": i, "name": n, "effects": [], "sends": [], "sidechainSources": []}
        for i, n in ((50, "Bass"), (51, "Pad"), (52, "Keys"))
    ]
    bass = [{"start": b * 960, "length": 480, "pitch": 33, "velocity": 0.8} for b in range(32)]
    held = [{"start": b * BAR, "length": BAR, "pitch": 69, "velocity": 0.7} for b in range(8)]
    project["patterns"].append(
        {
            "id": 40,
            "name": "Music",
            "length": 8 * BAR,
            "channels": [
                {"channel": 30, "notes": bass},
                {"channel": 32, "notes": held},
                {"channel": 34, "notes": [{**n, "pitch": 71} for n in held]},  # same register as the pad
            ],
        }
    )  # fmt: skip
    project["playlist"]["lanes"].append({"id": 41, "name": "Music"})
    project["playlist"]["clips"].append(
        {"id": 42, "lane": 41, "start": 0, "length": 8 * BAR, "type": "pattern", "pattern": 40, "offset": 0}
    )
    return project


def harness(project: dict[str, Any] | None = None, **extra: Any) -> ProjectHarness:
    return ProjectHarness({"project": project or song(), "devices": DEVICES, **extra})


def valid(steps: list[dict[str, Any]]) -> None:
    """The steps pass the assistant's own checks: known commands, parameters as their schemas say, references
    to earlier steps' results only."""
    tools = Tools(Snapshot({"project": EXAMPLE, "devices": DEVICES}), Manual(None), COMMANDS)
    text, failed = tools.propose({"summary": "x", "steps": steps})
    assert not failed, text


def test_the_song_is_read_with_its_sections_and_parts() -> None:
    h = harness()
    overview = json.loads(h.call("song_overview", {})[0])
    assert [s["name"] for s in overview["sections"]] == ["Verse", "Chorus"]
    assert overview["sections"][1]["bars"] == "5-8"
    assert overview["key"] == "A minor"
    parts = {p["name"]: p for p in overview["parts"]}
    assert parts["Bass"]["role"] == "bass" and parts["Bass"]["plays"] == "1-8"
    text, failed = h.call("read_part", {"part": "bass", "bars": "Chorus"})
    assert not failed and text.startswith("Bass, bars 5-8") and "0 33 2 102" in text


def test_writing_a_part_replaces_its_notes_there_through_commands() -> None:
    h = harness()
    text, failed = h.call(
        "write_part", {"part": "Bass", "bars": "5-8", "notes": "0 45 8 110; 8 43 8 90", "loop_bars": 1}
    )
    assert not failed, text
    assert "8 notes in bars 5-8" in text and "16 old ones taken out" in text
    commands = [s["command"] for s in h.steps]
    # The pattern plays only here, so the bass's notes come out of it (its length and clip kept as they were); a new
    # pattern holds the new ones.
    assert commands == [
        "pattern.setNotes",
        "pattern.setLength",
        "clip.set",
        "pattern.add",
        "pattern.setNotes",
        "pattern.setLength",
        "lane.add",
        "clip.addPattern",
    ]
    assert len(h.steps[0]["parameters"]["notes"]) == 16  # the verse's bass stays
    assert h.steps[-1]["parameters"]["start"] == 4 * BAR
    valid(h.steps)
    # The working copy follows: the chorus reads as written.
    again = h.call("read_part", {"part": "Bass", "bars": "5"})[0]
    assert again.endswith("0 45 8 110; 8 43 8 90")


def test_a_shared_pattern_is_copied_before_its_notes_change() -> None:
    project = song()
    project["playlist"]["clips"].append(
        {"id": 43, "lane": 41, "start": 8 * BAR, "length": 4 * BAR, "type": "pattern", "pattern": 40, "offset": 0}
    )
    h = harness(project)
    assert not h.call("write_part", {"part": "Bass", "bars": "1-2", "notes": "0 40 4"})[1]
    assert h.steps[0] == {"command": "clip.makeUnique", "parameters": {"clip": 42}}
    assert h.steps[1]["parameters"]["pattern"] == "$1.pattern"
    valid(h.steps)


def test_motifs_are_placed_repeated_moved_and_varied() -> None:
    h = harness()
    assert not h.call("motif", {"name": "hook", "notes": "0 C5 4; 4 E5 4; 8 G5 8", "bars": 1})[1]
    text, failed = h.call("place_motif", {"part": "Keys", "motif": "hook", "at_bar": 5, "times": 2, "transpose": 2})
    assert not failed, text
    assert h.call("read_part", {"part": "Keys", "bars": "6"})[0].endswith("0 62 4 100; 4 66 4 100; 8 69 8 100")
    valid(h.steps)


def test_a_new_part_and_a_bridge_inserted_after_a_bar() -> None:
    h = harness()
    assert not h.call(
        "add_part", {"name": "Strings", "instrument": "3x osc", "preset": "String Pad", "family": "Orchestra"}
    )[1]
    assert not h.call("write_part", {"part": "Strings", "bars": "5-8", "notes": "0 57 64"})[1]
    text, failed = h.call("insert_bars", {"after_bar": 4, "bars": 4, "name": "Bridge", "copy_from": "Verse"})
    assert not failed, text
    moved = [s for s in h.steps if s["command"] == "clip.set"]
    assert moved and all(s["parameters"]["start"] >= 8 * BAR for s in moved)
    assert any(s["command"] == "clip.split" and s["parameters"]["at"] == 4 * BAR for s in h.steps)
    assert {"command": "marker.set", "parameters": {"marker": 91, "at": 8 * BAR}} in h.steps
    assert any(s["command"] == "marker.add" and s["parameters"]["name"] == "Bridge" for s in h.steps)
    sections = json.loads(h.call("song_overview", {})[0])["sections"]
    assert [(s["name"], s["bars"]) for s in sections] == [("Verse", "1-4"), ("Bridge", "5-8"), ("Chorus", "9-12")]
    valid(h.steps)


def test_automation_effects_sends_and_ducking_in_real_units() -> None:
    h = harness()
    text, failed = h.call(
        "automate",
        {"part": "Pad", "what": "volume", "points": [{"bar": 1, "value": "-24 dB"}, {"bar": 5, "value": "0 dB"}]},
    )
    assert not failed, text
    points = h.steps[-1]["parameters"]["points"]
    assert points[0]["value"] == round(db_to_volume(-24), 4) and points[1]["time"] == 4 * BAR
    assert not h.call("add_effect", {"to": "master", "effect": "limiter", "settings": {"Ceiling": "-1 dB"}})[1]
    assert not h.call("send", {"part": "Pad", "to": "Reverb", "level": "-12 dB"})[1]
    assert not h.call("duck", {"part": "Pad", "source": "Kick", "amount": "40%"})[1]
    valid(h.steps)
    assert "master: no limiter" not in h.call("check", {})[0]


def test_shaping_a_sound_moves_its_settings_as_they_show() -> None:
    h = harness(
        deviceParameters={
            "31": [
                {"id": "attack", "name": "Attack", "text": "20 ms"},
                {"id": "decay", "name": "Decay", "text": "400 ms"},
                {"id": "sustain", "name": "Sustain", "text": "80%"},
                {"id": "cutoff", "name": "Cutoff", "text": "2.0 kHz"},
            ]
        }
    )
    text, failed = h.call("shape_sound", {"part": "Bass", "words": ["pluckier"]})
    assert not failed, text
    changed = h.steps[-1]["parameters"]["text"]
    assert changed == {"Attack": "4 ms", "Decay": "180 ms", "Sustain": "20 %"}
    assert h.call("shape_sound", {"part": "Bass", "words": ["brighter"]})[0].endswith("Cutoff -> 3.2 kHz")
    valid(h.steps)
    assert "didn't send" in h.call("shape_sound", {"part": "Pad", "words": ["darker"]})[0]


def test_the_checks_find_masking_and_a_missing_limiter() -> None:
    issues = harness().call("check", {})[0]
    assert "Keys and Pad, Verse: both held in the same register" in issues
    assert "master: no limiter" in issues


def test_a_mistake_changes_nothing_and_says_how() -> None:
    h = harness()
    text, failed = h.call("write_part", {"part": "Bass", "bars": "1", "notes": "zero C5 4"})
    assert failed and "could not be read" in text and not h.steps
    assert h.call("write_part", {"part": "Nobody", "bars": "1", "notes": "0 60 4"})[0].startswith(
        "Not done: there is no part"
    )
    assert "neither bars" in h.call("read_part", {"part": "Bass", "bars": "Solo"})[0]


def test_reading_values() -> None:
    assert pitch_of("C5") == 60 and pitch_of("Eb3") == 39 and pitch_of("60") == 60 and pitch_of("H2") is None
    assert pan_value("30% L") == 0.35 and pan_value("C") == 0.5
    assert moved_text("400 ms", "scale", 0.5, 1.0) == "200 ms" and moved_text("-6 dB", "scale", 0.5, 1.0) is None


async def test_the_assistant_collects_its_edits_into_one_proposal(tmp_path: Path) -> None:
    replies = iter(
        [
            anthropic_reply("", ("song_overview", {})),
            anthropic_reply("", ("add_part", {"name": "Strings", "instrument": "3x Osc", "preset": "String Pad"})),
            anthropic_reply("", ("write_part", {"part": "Strings", "bars": "Chorus", "notes": "0 57 64; 0 60 64"})),
            anthropic_reply("", ("propose", {"summary": "Strings in the chorus"})),
            anthropic_reply("If you apply it, strings hold the chorus's chords."),
        ]
    )
    service = make_service(tmp_path, lambda request: httpx.Response(200, content=next(replies)))
    await service.handle("ai.v1.connections.save", {"name": "A", "provider": "anthropic", "key": "k"}, None)  # type: ignore[arg-type]
    _, updates = await collect(
        service,
        "ai.v1.assistant.ask",
        {
            "conversation": [{"role": "user", "text": "add strings to the chorus"}],
            "context": {"project": song(), "devices": DEVICES},
        },
    )
    final = updates[-1]
    assert final["kind"] == "done", final
    proposals = final["result"]["proposals"]
    assert len(proposals) == 1 and proposals[0]["summary"] == "Strings in the chorus"
    assert [s["command"] for s in proposals[0]["steps"]][:2] == ["channel.add", "device.loadPreset"]
    valid(proposals[0]["steps"])
    assert any(u.get("text") == "Writing a part" for u in updates)


async def test_the_live_interface_gets_the_tools_and_each_calls_steps(tmp_path: Path) -> None:
    service = make_service(tmp_path, lambda request: httpx.Response(500))
    listed, _ = await collect(service, "ai.v1.project.tools", {})
    names = [t["name"] for t in listed["tools"]]
    assert "write_part" in names and all("inputSchema" in t for t in listed["tools"])
    result, _ = await collect(
        service,
        "ai.v1.project.tool",
        {
            "tool": "insert_bars",
            "arguments": {"after_bar": 4, "bars": 2, "name": "Break"},
            "context": {"project": song()},
        },
    )
    assert not result["isError"] and result["proposal"]["summary"].startswith("insert_bars")
    valid(result["proposal"]["steps"])
    looked, _ = await collect(
        service, "ai.v1.project.tool", {"tool": "check", "arguments": {}, "context": {"project": song()}}
    )
    assert looked["proposal"] is None and "no limiter" in looked["text"]


def example_edits() -> dict[str, Any]:
    """Edits on the unchanged example project (3/4, Kick and Surge): the proposal tests/ai/AiTests.cpp applies in
    Spinus, so the steps the harness makes are checked against the real commands too."""
    h = ProjectHarness({"project": copy.deepcopy(EXAMPLE), "devices": DEVICES})
    for name, arguments in [
        ("add_part", {"name": "Pads", "instrument": "3x Osc", "preset": "Warm Pad", "family": "Synths"}),
        ("write_part", {"part": "Pads", "bars": "2-3", "notes": "0 57 12 90; 0 60 12 90", "loop_bars": 1}),
        ("write_part", {"part": "Surge", "bars": "1", "notes": "0 C5 4; 4 D5 4; 8 E5 4"}),
        ("insert_bars", {"after_bar": 2, "bars": 2, "name": "Bridge", "copy_from": "1-2"}),
        (
            "automate",
            {"part": "Pads", "what": "volume", "points": [{"bar": 3, "value": "-18 dB"}, {"bar": 5, "value": "0 dB"}]},
        ),
        ("send", {"part": "Kick", "to": "Reverb", "level": "-14 dB"}),
        ("duck", {"part": "Surge", "source": "Kick", "amount": "40%"}),
    ]:
        text, failed = h.call(name, arguments)
        assert not failed, (name, text)
    proposal = h.take_proposal("Harness edits")
    assert proposal is not None
    return proposal


def test_the_example_proposal_spinus_applies_is_up_to_date() -> None:
    made = example_edits()
    valid(made["steps"])
    stored = json.loads((ROOT / "tests/ai/files/harness-proposal.json").read_text())
    assert stored == made, "run: python -c 'from test_project_harness import write_example; write_example()'"


def write_example() -> None:
    target = ROOT / "tests/ai/files/harness-proposal.json"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(example_edits(), indent=1) + "\n")


def test_a_part_changes_instrument_keeping_its_notes() -> None:
    project = song()
    project["playlist"]["clips"].append(
        {
            "id": 44,
            "lane": 41,
            "start": 0,
            "length": BAR,
            "type": "automation",
            "target": {"kind": "deviceParameter", "device": 10, "parameterId": "cutoff"},
            "points": [{"time": 0, "value": 0.5}],
        }
    )
    h = harness(project)
    text, failed = h.call("set_sound", {"part": "Surge", "instrument": "3x Osc", "preset": "Reese Bass"})
    assert not failed, text
    assert h.steps[0] == {
        "command": "channel.replaceInstrument",
        "parameters": {"channel": 9, "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}, "name": "Surge"},
    }
    # The preset is the new instrument's.
    assert h.steps[1] == {"command": "device.loadPreset", "parameters": {"device": "$1.device", "preset": "Reese Bass"}}
    assert "now 3x Osc" in text
    assert not any(c["id"] == 44 for c in h.clips())  # the old instrument's automation went
    valid(h.steps)
    # Only a new instrument, no preset or settings: one step.
    h = harness()
    text, failed = h.call("set_sound", {"part": "Bass", "instrument": "3x Osc"})
    assert not failed and [s["command"] for s in h.steps] == ["channel.replaceInstrument"], text
