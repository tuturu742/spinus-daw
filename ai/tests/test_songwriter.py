"""The Track creator's song writer (spinus_ai.songwriter) on a style made up for the test: the same seed writes
the same song, the song keeps to the style's numbers, one part or section can be written again keeping the
rest, and the AI's variation is checked (a malformed answer leaves the rules' song)."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import httpx
import pytest
from test_service import anthropic_reply, make_service, no_notify, run_job
from test_style import add_drums, song_with

from spinus_ai import songwriter, style
from spinus_ai.service import RpcError

KICK = [0.95, 0, 0.1, 0, 0.9, 0, 0.3, 0, 0.92, 0, 0.1, 0.2, 0.9, 0, 0.1, 0]
SNARE = [0, 0, 0, 0, 0.95, 0, 0, 0.1, 0, 0, 0, 0, 0.93, 0, 0.2, 0.3]
HATS = [0.9, 0.1, 0.85, 0.1, 0.9, 0.1, 0.85, 0.3, 0.9, 0.1, 0.85, 0.1, 0.9, 0.1, 0.85, 0.4]


def made_song(name: str, key: str, scale: str, tonic: int, loop: list[str], times: int) -> dict[str, Any]:
    chords = [{"bar": i + 1, "roman": loop[i % len(loop)]} for i in range(len(loop) * times)]
    return {
        "version": style.FEATURES_VERSION,
        "name": name,
        "tempo": 128.0,
        "key": {"tonic": tonic, "scale": scale, "name": key},
        "bars": len(chords),
        "chords": chords,
        "groove": {
            "steps": 16,
            "bars": len(chords),
            "lanes": {"kick": KICK, "snare": SNARE, "hats": HATS},
            "swing": 0.5,
        },
    }


def summary() -> dict[str, Any]:
    """Three minor songs and a major one, as spinus_ai.style.summarize gives them (the parts the writer uses)."""
    songs = [
        made_song("One", "A minor", "minor", 9, ["i", "bVI", "bIII", "bVII"], 12),
        made_song("Two", "D minor", "minor", 2, ["i", "iv", "bVI", "V"], 10),
        made_song("Three", "E minor", "minor", 4, ["i", "bVII", "bVI", "bVII", "iv", "i", "bIII", "V"], 4),
        made_song("Four", "C major", "major", 0, ["I", "V", "vi", "IV"], 10),
    ]
    return {
        "kind": "spinus.style-summary",
        "version": style.SUMMARY_VERSION,
        "songs": len(songs),
        "names": [s["name"] for s in songs],
        "tempo": {"median": 128.0, "min": 122.0, "max": 134.0},
        "beatsPerBar": {"4": 4},
        "keys": [{"name": s["key"]["name"], "count": 1} for s in songs],
        "modes": {"minor": 0.75, "major": 0.25},
        "chords": {"barsPerChord": 1.0},
        "drums": {"songs": 4, "steps": 16, "lanes": {"kick": KICK, "snare": SNARE, "hats": HATS}, "swing": 0.5},
        "parts": {
            "bass": {"songs": 4, "notesPerBar": 5.0, "low": 31, "median": 38, "high": 47, "voices": 1.1},
            "drums": {"songs": 4, "notesPerBar": 13.0},
            "other": {"songs": 4, "notesPerBar": 12.0, "low": 48, "median": 58, "high": 72, "voices": 2.6},
            "vocals": {"songs": 4, "notesPerBar": 3.0, "low": 55, "median": 64, "high": 76, "voices": 1.0},
        },
        "sections": {
            "lengths": [
                {"bars": 8, "count": 9},
                {"bars": 16, "count": 6},
                {"bars": 17, "count": 2},
                {"bars": 4, "count": 2},
            ],
            "perSong": 8.0,
            "songBars": 120.0,
            "forms": [],
        },
        "loudness": {"lufs": -9.0},
        "perSong": songs,
        "plain": [],
    }


def notes_of(song: dict[str, Any], part: str) -> list[tuple[dict[str, Any], dict[str, Any]]]:
    """Every note of a part as played in the song, with its section."""
    sections = {s["id"]: s for s in song["sections"]}
    return [(sections[e["section"]], n) for e in song["arrangement"] for n in song["parts"][part][e["section"]]]


def test_the_same_seed_writes_the_same_song_and_another_seed_another() -> None:
    first = songwriter.write(summary(), 7)
    assert json.dumps(songwriter.write(summary(), 7), sort_keys=True) == json.dumps(first, sort_keys=True)
    other = songwriter.write(summary(), 8)
    assert other["parts"] != first["parts"]
    # Through JSON (as Spinus keeps it) it is the same song.
    assert json.loads(json.dumps(first)) == first


@pytest.mark.parametrize("seed", range(12))
def test_the_song_keeps_to_the_style(seed: int) -> None:
    s = summary()
    song = songwriter.write(s, seed)
    checks = song["checks"]

    assert 128 * 0.97 - 0.5 <= song["tempo"] <= 128 * 1.03 + 0.5
    assert song["key"]["scale"] in ("minor", "major")
    # Sections on 4-bar phrases, of lengths the songs have (17 bars counts as 16).
    assert all(sec["bars"] % 4 == 0 and sec["bars"] in (4, 8, 16) for sec in song["sections"])
    assert 5 <= len(song["arrangement"]) <= 10
    assert 48 <= song["bars"] <= 140
    assert song["arrangement"][0]["bar"] == 0

    # Chords only from the songs of the song's mode, as a new combination.
    allowed = {"minor": {"i", "bVI", "bIII", "bVII", "iv", "V"}, "major": {"I", "V", "vi", "IV"}}[song["key"]["scale"]]
    used = {c["roman"] for sec in song["sections"] for c in sec["chords"]}
    assert used <= allowed
    assert checks["chords"]["inVocabulary"] == 1.0
    assert checks["chords"]["loopsOfOneReference"] <= 1

    # Drums: the steps the songs always play are played in every bar the drums play (fills and the ending
    # aside), steps they never play stay silent; the per-step hits come close to the lanes.
    lanes = {"kick": KICK, "snare": SNARE, "hat": HATS}
    for section, n in notes_of(song, "drums"):
        step = round(n["start"] / 240)
        bar, on = divmod(step, 16)
        if n["drum"] in lanes and bar < section["bars"] - 1:
            assert lanes[n["drum"]][on] > 0.05, (n, section["id"])
    for sec in song["sections"]:
        hits = {(round(n["start"] / 240), n["drum"]) for n in song["parts"]["drums"][sec["id"]]}
        played = {round(n["start"] / 240) // 16 for n in song["parts"]["drums"][sec["id"]]}
        for bar in played - {sec["bars"] - 1}:
            for lane, probabilities in lanes.items():
                for step, p in enumerate(probabilities):
                    if p >= 0.6:
                        assert (bar * 16 + step, lane) in hits, (sec["id"], bar, lane, step)
    assert min(checks["groove"].values()) >= 0.85

    # Registers, densities, and notes that fit the key and the chords.
    for part, stem in (("bass", "bass"), ("chords", "other"), ("lead", "vocals")):
        target = s["parts"][stem]
        pitches = [n["pitch"] for _, n in notes_of(song, part)]
        assert pitches, part
        assert target["low"] <= min(pitches) and max(pitches) <= target["high"], part
        density = len(pitches) / song["bars"]
        assert 0.7 * target["notesPerBar"] <= density <= 1.3 * target["notesPerBar"], (part, density)
    assert checks["notesFitChords"] == 1.0
    assert checks["leadStrongBeatsOnChordNotes"] == 1.0
    for section, n in notes_of(song, "lead"):
        bar = n["start"] // 3840
        if n["start"] % 1920 == 0:  # on the bar's first or third beat: a note of the chord
            assert n["pitch"] % 12 in section["chords"][bar]["tones"]
    # The bass plays the chord's root on each bar's first beat.
    for section, n in notes_of(song, "bass"):
        if n["start"] % 3840 == 0:
            assert n["pitch"] % 12 == section["chords"][n["start"] // 3840]["tones"][0]
    assert song["plain"] and "BPM" in song["plain"][0]


def test_a_part_or_a_section_is_written_again_and_the_rest_kept() -> None:
    s = summary()
    song = songwriter.write(s, 3)
    bass = songwriter.redo(song, s, 41, part="bass")
    assert bass["parts"]["bass"] != song["parts"]["bass"]
    for part in ("drums", "chords", "lead"):
        assert bass["parts"][part] == song["parts"][part]
    assert bass["sections"] == song["sections"] and bass["arrangement"] == song["arrangement"]
    assert bass["tempo"] == song["tempo"] and bass["key"] == song["key"]
    assert songwriter.redo(song, s, 41, part="bass") == bass  # seeded too

    verse = songwriter.redo(song, s, 5, section="verse")
    changed = {p for p in songwriter.PARTS if verse["parts"][p]["verse"] != song["parts"][p]["verse"]}
    assert len(changed) >= 3
    for sec in song["sections"]:
        if sec["id"] != "verse":
            for p in songwriter.PARTS:
                assert verse["parts"][p][sec["id"]] == song["parts"][p][sec["id"]]
    old = {x["id"]: x for x in song["sections"]}
    new = {x["id"]: x for x in verse["sections"]}
    assert new["verse"]["bars"] == old["verse"]["bars"]
    assert all(new[k] == old[k] for k in old if k != "verse")
    # Still no loop of one reference twice.
    assert verse["checks"]["chords"]["loopsOfOneReference"] <= 1

    with pytest.raises(ValueError):
        songwriter.redo(song, s, 1, part="cowbell")
    with pytest.raises(ValueError):
        songwriter.redo({"kind": "other"}, s, 1, part="bass")


def test_a_full_arrangement_has_parts_that_come_and_go_by_section() -> None:
    s = summary()
    sketch = songwriter.write(s, 3)
    assert [layer["id"] for layer in sketch["layers"]] == ["drums", "bass", "chords", "lead"]
    full = songwriter.write(s, 3, "full")
    # The same song underneath: plan, chords and the four parts.
    assert full["parts"] == sketch["parts"] and full["sections"] == sketch["sections"]
    layers = {layer["id"]: layer for layer in full["layers"]}
    assert set(layers) == {"drums", "bass", "chords", "pad", "arp", "lead", "counter", "riser", "impact"}
    assert "sub" not in layers  # this bass sits low (its middle note 38): no room for a sub under it
    assert {layer["family"] for layer in full["layers"]} == {"Drums", "Bass", "Synths", "Leads", "FX"}
    assert all(
        layer["soundRole"] in ("drums", "bass", "keys", "pad", "pluck", "lead", "fx") for layer in layers.values()
    )
    # One drums part: every drum note of the sketch, kick, snare and hats together.
    assert full["layerParts"]["drums"] == full["parts"]["drums"]

    # Thin verses, full choruses.
    def playing(sid: str) -> set[str]:
        return {layer for layer, by in full["layerParts"].items() if by.get(sid)}

    assert {"pad", "arp", "counter"} <= playing("chorus")
    assert not {"pad", "arp", "counter"} & playing("verse")
    assert len(playing("chorus")) > len(playing("verse"))

    # The pad and arpeggio play the chords' notes; the counter-melody never rubs against the lead.
    by_id = {x["id"]: x for x in full["sections"]}
    bar_ticks = full["barTicks"]
    for layer in ("pad", "arp", "counter"):
        for sid, notes in full["layerParts"][layer].items():
            for n in notes:
                tones = by_id[sid]["chords"][n["start"] // bar_ticks]["tones"]
                assert n["pitch"] % 12 in tones, (layer, sid, n)
    for sid, notes in full["layerParts"]["counter"].items():
        for n in notes:
            sounding = [m for m in full["parts"]["lead"][sid] if m["start"] <= n["start"] < m["start"] + m["length"]]
            assert not any(abs(n["pitch"] - m["pitch"]) % 12 in (0, 1, 11) for m in sounding), (sid, n)

    # A riser over the two bars before each chorus, an impact where each chorus starts.
    choruses = [e["bar"] for e in full["arrangement"] if by_id[e["section"]]["kind"] == "chorus"]
    assert full["fx"]["riser"]["at"] == [b - 2 for b in choruses]
    assert set(choruses) <= set(full["fx"]["impact"]["at"])
    assert "9 parts, a channel each" in " ".join(full["plain"])
    assert songwriter.write(s, 3, "full") == full


def test_the_users_parts_are_written_two_of_a_role_unlike_each_other() -> None:
    s = summary()
    parts = [
        {"id": "kit", "name": "Drum Machine", "role": "drums"},
        {"id": "rhythm", "name": "Rhythm guitar", "role": "chords"},
        {"id": "picked", "name": "Picked guitar", "role": "chords"},
        {"id": "synth", "name": "Surge lead", "role": "lead"},
        {"id": "harmony", "name": "Harmony", "role": "lead"},
    ]
    song = songwriter.write(s, 4, parts=parts)
    assert [layer["id"] for layer in song["layers"]] == ["kit", "rhythm", "picked", "synth", "harmony"]
    assert [layer["name"] for layer in song["layers"]][:2] == ["Drum Machine", "Rhythm guitar"]
    assert "bass" not in song["layerParts"]  # not on the list: not written
    notes = song["layerParts"]
    assert notes["rhythm"] == song["parts"]["chords"] and notes["kit"] == song["parts"]["drums"]
    # The second chords part breaks the chords up, an octave higher, with a rhythm of its own.
    rhythm = [n for ns in notes["rhythm"].values() for n in ns]
    picked = [n for ns in notes["picked"].values() for n in ns]
    assert picked and sum(n["pitch"] for n in picked) / len(picked) > sum(n["pitch"] for n in rhythm) / len(rhythm)
    assert {n["start"] for n in picked} != {n["start"] for n in rhythm}
    # The harmony a few semitones under the lead, note for note.
    by_id = {x["id"]: x for x in song["sections"]}
    for sid, harmony in notes["harmony"].items():
        lead = {n["start"]: n["pitch"] for n in song["parts"]["lead"][sid]}
        assert not harmony or by_id[sid]["kind"] in ("chorus", "bridge")
        assert all(3 <= lead[n["start"]] - n["pitch"] <= 5 for n in harmony)

    # Redoing the second chords part changes only it.
    again = songwriter.redo(song, s, 77, part="picked")
    assert again["layerParts"]["picked"] != notes["picked"]
    assert all(again["layerParts"][k] == notes[k] for k in ("kit", "rhythm", "synth", "harmony"))
    # Redoing the first one is redoing the chords: what is made from them follows.
    chords = songwriter.redo(song, s, 77, part="rhythm")
    assert chords["layerParts"]["rhythm"] != notes["rhythm"] and chords["layerParts"]["kit"] == notes["kit"]

    for bad in ([], [{"id": "a", "role": "cowbell"}], [{"id": "a", "role": "bass"}, {"id": "a", "role": "pad"}]):
        with pytest.raises(ValueError):
            songwriter.write(s, 1, parts=bad)


def test_a_high_bass_gets_a_sub_and_redoing_a_part_redoes_its_layers() -> None:
    s = summary()
    s["parts"]["bass"] = {**s["parts"]["bass"], "low": 38, "median": 45, "high": 57}
    full = songwriter.write(s, 7, "full")
    assert any(layer["id"] == "sub" for layer in full["layers"])
    subs = [n for notes in full["layerParts"]["sub"].values() for n in notes]
    assert subs and all(songwriter.SUB_RANGE[0] <= n["pitch"] <= songwriter.SUB_RANGE[1] for n in subs)

    chords = songwriter.redo(full, s, 99, part="chords")
    assert chords["size"] == "full"
    assert chords["layerParts"]["drums"] == full["layerParts"]["drums"]
    assert chords["layerParts"]["lead"] == full["layerParts"]["lead"]
    assert chords["layerParts"]["chords"] != full["layerParts"]["chords"]
    assert (
        chords["layerParts"]["arp"] != full["layerParts"]["arp"]
        or chords["layerParts"]["pad"] != (full["layerParts"]["pad"])
    )

    with pytest.raises(ValueError):
        songwriter.write(s, 1, "huge")


def test_a_summary_made_by_the_style_measurer_writes_a_song() -> None:
    songs = [
        style.analyze_song(add_drums(song_with(["Am", "F", "C", "G"] * 6, key=(9, "minor")))),
        style.analyze_song(add_drums(song_with(["C", "G", "Am", "F"] * 6))),
    ]
    summary_made = style.summarize(songs)
    song = songwriter.write(summary_made, 1)
    assert song["bars"] > 0 and all(song["parts"][p] for p in songwriter.PARTS)
    assert song["checks"]["notesFitChords"] == 1.0


def test_roman_numerals_are_read_as_the_measurer_writes_them() -> None:
    assert songwriter.QUALITIES == style.QUALITIES
    for tonic in range(12):
        for root in range(12):
            for quality in style.QUALITIES:
                roman = style.roman(root, quality, tonic)
                parsed = songwriter.parse_roman(roman)
                assert parsed == ((root - tonic) % 12, quality), roman
                assert songwriter.chord_label(roman, tonic) == style.chord_name(root, quality)
    assert songwriter.parse_roman("Ix") is None and songwriter.parse_roman("iisus4") is None


def ai_service(tmp_path: Path, answer: Any, sent: list[dict[str, Any]]) -> Any:
    def handler(request: httpx.Request) -> httpx.Response:
        sent.append(json.loads(request.content))
        tool = ("refine_song", answer) if isinstance(answer, dict) else None
        return httpx.Response(
            200,
            content=anthropic_reply("" if tool else str(answer), tool),
            headers={"content-type": "text/event-stream"},
        )

    return make_service(tmp_path, handler)


async def test_without_a_connection_the_rules_song_stands(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    service = ai_service(tmp_path, {}, sent)
    s = summary()
    rules = await run_job(service, "ai.v1.song.write", {"summary": s, "seed": 4})
    assert rules == songwriter.write(s, 4)  # offline by default: no AI asked
    asked = await run_job(service, "ai.v1.song.write", {"summary": s, "seed": 4, "refine": True})
    assert "no AI connection" in asked["note"] and asked["parts"] == rules["parts"] and not sent

    redone = await run_job(
        service, "ai.v1.song.write", {"summary": s, "seed": 9, "song": rules, "redo": {"part": "lead"}}
    )
    assert redone["parts"]["drums"] == rules["parts"]["drums"] and redone["parts"]["lead"] != rules["parts"]["lead"]

    full = await run_job(service, "ai.v1.song.write", {"summary": s, "seed": 4, "arrangement": "full"})
    assert full == songwriter.write(s, 4, "full") and full["layers"]

    mine = await run_job(
        service,
        "ai.v1.song.write",
        {"summary": s, "seed": 4, "parts": [{"id": "g", "name": "Guitar", "role": "chords"}]},
    )
    assert [layer["name"] for layer in mine["layers"]] == ["Guitar"]

    for bad in (
        {"summary": s, "parts": [{"id": "x", "role": "kazoo"}]},
        {"summary": s, "arrangement": "huge"},
        {"summary": {"kind": "x"}},
        {"summary": s, "seed": "x"},
        {"summary": s, "redo": {}},
        {"summary": s, "redo": {"part": "bass"}, "song": {"kind": "x"}},
    ):
        with pytest.raises(RpcError):
            await service.handle("ai.v1.song.write", bad, no_notify)


async def test_the_ai_varies_the_plan_within_the_style(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    answer = {
        "sections": [
            {"kind": "intro", "bars": 4},
            {"kind": "verse", "bars": 16},
            {"kind": "chorus", "bars": 8},
            {"kind": "verse", "bars": 16},
            {"kind": "chorus", "bars": 8},
            {"kind": "chorus", "bars": 8},
            {"kind": "outro", "bars": 8},
        ],
        "loops": {"chorus": ["iv", "V", "i", "bVI"], "verse": ["i", "Imaj7#11", "bVI", "V"]},  # not in the style
        "contours": {"verse": "valley", "bridge": "rise", "chorus": "zigzag"},
        "reason": "A shorter intro, a stronger chorus.",
    }
    service = ai_service(tmp_path, answer, sent)
    await service.handle(
        "ai.v1.connections.save", {"name": "Claude", "provider": "anthropic", "key": "sk-test"}, no_notify
    )
    s = summary()
    song = await run_job(service, "ai.v1.song.write", {"summary": s, "seed": 4, "refine": True})
    assert song["refinedBy"] == "claude-test"
    assert [
        song_section["kind"]
        for song_section in (next(x for x in song["sections"] if x["id"] == e["section"]) for e in song["arrangement"])
    ] == ["intro", "verse", "chorus", "verse", "chorus", "chorus", "outro"]
    by_kind = {x["kind"]: x for x in song["sections"]}
    assert by_kind["intro"]["bars"] == 4
    assert by_kind["chorus"]["loop"] == ["iv", "V", "i", "bVI"] or song["key"]["scale"] == "major"
    assert by_kind["verse"]["contour"] == "valley"
    assert "Imaj7#11" not in json.dumps(song["sections"])  # a chord outside the style is not taken
    assert by_kind["chorus"]["contour"] != "zigzag"
    assert song["checks"]["notesFitChords"] == 1.0  # the rules still wrote every note
    assert "The AI changed" in song["note"]
    assert sent[0]["tools"][0]["name"] == "refine_song"
    assert "chordVocabulary" in sent[0]["messages"][0]["content"][0]["text"]


@pytest.mark.parametrize(
    "answer",
    [
        "I would rather not.",  # no tool call
        {"sections": [{"kind": "verse", "bars": 13}] * 5},  # off the phrases
        {"sections": [{"kind": "solo", "bars": 8}] * 5, "loops": {"verse": ["X", "Y"]}},
        {"sections": "verse chorus", "contours": {"verse": "spiral"}, "loops": {"nowhere": ["i", "iv"]}},
    ],
)
async def test_a_malformed_answer_leaves_the_rules_song(tmp_path: Path, answer: Any) -> None:
    sent: list[dict[str, Any]] = []
    service = ai_service(tmp_path, answer, sent)
    await service.handle(
        "ai.v1.connections.save", {"name": "Claude", "provider": "anthropic", "key": "sk-test"}, no_notify
    )
    s = summary()
    song = await run_job(service, "ai.v1.song.write", {"summary": s, "seed": 4, "refine": True})
    rules = songwriter.write(s, 4)
    assert sent and song["refinedBy"] == ""
    assert song["parts"] == rules["parts"] and song["sections"] == rules["sections"]
    assert "not usable" in song["note"]
