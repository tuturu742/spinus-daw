"""A song described in words (spinus_ai.describe): the rules read the genre, tempo, key, moods, instruments and
what to leave out, and say what they understood; the summary they make writes a song in that genre's style; the
AI's reading, when asked, is checked, and an unusable answer leaves the rules' reading."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import httpx
import pytest
from test_service import anthropic_reply, make_service, no_notify, run_job

from spinus_ai import describe, songwriter
from spinus_ai.service import RpcError


def test_a_described_song_is_understood_as_written() -> None:
    summary = describe.describe(
        "A sunny, upbeat indie pop song at 118 BPM in G major, with acoustic guitar, handclaps and a catchy "
        "whistled lead"
    )
    described = summary["described"]
    assert described["understood"] == (
        "Understood: 118 BPM, G major, sunny, upbeat, catchy; genre: indie pop; sounds: acoustic guitar, handclaps, "
        "whistled"
    )
    assert described["genre"] == "indiepop"
    assert summary["tempo"]["median"] == 118.0
    assert summary["keys"] == [{"name": "G major", "count": 3}]
    assert summary["modes"] == {"major": 1.0}
    assert described["named"] == {"acoustic-guitar": "acoustic", "percussion": "clap", "lead": "whistle"}

    dsbm = describe.describe("A slow, dark depressive black metal song in D minor at 72 BPM")["described"]
    assert dsbm["understood"] == "Understood: 72 BPM, D minor, slow, dark; genre: depressive black metal"


@pytest.mark.parametrize(
    ("text", "genre", "tempo", "mode"),
    [
        ("slowburn darkwave", "darkwave", (100, 120), "minor"),
        ("slow-burning coldwave", "coldwave", (100, 125), "minor"),
        ("brooding trip-hop", "triphop", (70, 90), "minor"),
        ("euphoric trance anthem", "trance", (135, 150), "major"),
        ("laid-back bossa nova", "bossa", (105, 130), None),
        ("chill lofi beat to study to", "lofi", (65, 85), None),
        ("melancholic dream pop", "dreampop", (80, 100), "minor"),
        ("fast happy k-pop", "kpop", (125, 145), "major"),
        ("dreamy shoegaze with big guitars", "shoegaze", (95, 120), None),
        ("80s video game music", "chiptune", (130, 170), None),
        ("epic orchestral trailer", "cinematic", (80, 110), None),
        ("funky disco with strings", "disco", (110, 125), None),
        ("a reggaeton party", "reggaeton", (88, 100), None),
        ("moody post-punk", "postpunk", (120, 150), "minor"),
        ("heavy dub", "dub", (62, 85), None),
        ("drift phonk", "phonk", (120, 150), "minor"),
        ("laid-back city pop", "citypop", (90, 110), None),
        ("a bluegrass hoedown", "bluegrass", (120, 170), "major"),
        ("uk garage 2-step", "garage", (128, 138), None),
        ("raw black metal with blast beats", "blackmetal", (140, 200), "minor"),
        ("an R&B slow jam", "rnb", (60, 100), None),
        ("neo soul", "soul", (70, 110), None),
        ("afrobeats", "afrobeats", (95, 116), None),
        ("hyperpop", "hyperpop", (140, 180), None),
        ("doom metal", "doom", (50, 80), "minor"),
    ],
)
def test_genres_and_moods_from_short_phrases(
    text: str, genre: str, tempo: tuple[float, float], mode: str | None
) -> None:
    summary = describe.describe(text)
    described = summary["described"]
    assert described["genre"] == genre, described["understood"]
    assert tempo[0] <= summary["tempo"]["median"] <= tempo[1], described["understood"]
    if mode is not None:
        modes = summary["modes"]
        assert max(modes, key=lambda m: modes[m]) == mode, modes
    assert "not recognized" not in described["understood"]


def test_without_a_genre_it_says_so_and_leans_on_the_moods() -> None:
    sad = describe.describe("something melancholic and slow")
    assert sad["described"]["understood"].endswith("genre: not recognized (using a neutral style)")
    assert sad["modes"].get("minor", 0) > 0.8 and sad["tempo"]["median"] < 90
    happy = describe.describe("something happy and energetic")
    assert happy["modes"].get("major", 0) > 0.8 and happy["tempo"]["median"] > 100
    # No key said: several keys of the mode, not always C major or A minor.
    assert len({k["name"] for k in happy["keys"]}) >= 3
    assert "C major" not in describe.describe("something nice")["described"]["understood"]


@pytest.mark.parametrize(
    ("text", "tonic", "scale"),
    [
        ("in F# minor", 6, "minor"),
        ("in Bb major", 10, "major"),
        ("key of Eb min", 3, "minor"),
        ("C major please", 0, "major"),
        ("in Am", 9, "minor"),
        ("G sharp minor", 8, "minor"),
        ("a melancholic mood", None, None),  # "a m..." is not A minor
        ("be mine", None, None),
    ],
)
def test_keys_are_read_as_written(text: str, tonic: int | None, scale: str | None) -> None:
    reading = describe.read(text)
    assert (reading.tonic, reading.scale) == (tonic, scale)


def test_moods_tempo_length_instruments_and_leaving_out() -> None:
    reading = describe.read("A fast, happy, energetic pop song, 128 bpm, long, with piano and strings, no drums")
    assert reading.tempo == 128.0
    assert reading.pace > 1.1 and reading.minor_lean == -1 and reading.energy == 1 and reading.length > 1
    assert reading.moods == ["fast", "happy", "energetic"]
    assert reading.sounds == {"piano": "piano", "strings": "string", "drums": "drum"}
    assert [p["role"] for p in reading.parts] == ["piano", "strings", "drums"]
    assert reading.without == ["drums"]
    summary = describe.to_summary(reading, "x")
    assert summary["drums"]["songs"] == 0 and summary["without"] == ["drums"]
    assert "sounds: piano, strings" in summary["described"]["understood"]
    assert "without drums" in summary["described"]["understood"]

    # No tempo said: the genre's, moved by the pace; no key said: the mood leans the mode.
    slow = describe.describe("a slow, sad house track")
    assert slow["tempo"]["median"] < describe.BY_ID["house"].tempo
    assert slow["modes"]["minor"] > describe.BY_ID["house"].minor
    assert describe.read("slow burn").moods == ["slow burn"]  # not also "slow"
    assert describe.read("an electric piano").sounds == {"piano": "piano"}  # one instrument, not two
    assert describe.read("tempo 90").tempo == 90.0
    assert describe.read("at 999 bpm").tempo is None
    # The genre's palette fills what the description doesn't name.
    assert describe.describe("darkwave")["described"]["sounds"]["bass"] == "synth"
    assert describe.describe("darkwave with a piano")["described"]["sounds"]["piano"] == "piano"


@pytest.mark.parametrize("genre", [g.id for g in describe.GENRES])
@pytest.mark.parametrize("size", ["sketch", "full"])
def test_every_genre_writes_a_song_in_its_style(genre: str, size: str) -> None:
    g = describe.BY_ID[genre]
    assert describe.read(f"a {g.name} song").genre == genre  # its name finds it
    summary = describe.describe(f"a {g.name} song")
    song = songwriter.write(summary, 5, size)
    assert song["bars"] >= 32
    low, high = g.tempo_range
    assert low * 0.9 <= song["tempo"] <= high * 1.1
    checks = song["checks"]
    assert checks["notesFitChords"] >= 0.95 and checks["chords"]["inVocabulary"] == 1.0
    has_drums = any(song["parts"]["drums"].values())
    assert has_drums == g.drums
    if size == "full":
        families = {layer["family"] for layer in song["layers"]}
        assert ("Drums" in families) == g.drums
        assert len(song["layers"]) >= (6 if not g.drums else 8)
    json.dumps(summary)  # plain data


def test_a_song_without_a_lead_has_no_lead_or_counter_melody() -> None:
    song = songwriter.write(describe.describe("a synthwave instrumental bed, no melody"), 2, "full")
    assert not any(song["parts"]["lead"].values())
    assert {layer["id"] for layer in song["layers"]}.isdisjoint({"lead", "counter"})


def test_the_ai_reading_is_checked() -> None:
    reading = describe.check_reading(
        {
            "genre": "synthwave",
            "tempo": 300,  # out of range: not taken
            "key": "E minor",
            "pace": "slow",
            "mood": "dark",
            "energy": "calm",
            "instruments": {"pad": "Strings", "cowbell": "cowbell", "lead": "x" * 40},
            "without": ["lead", "everything"],
        }
    )
    assert reading is not None
    assert reading.genre == "synthwave" and reading.genre_named and reading.tempo is None
    assert (reading.tonic, reading.scale) == (4, "minor")
    assert reading.pace == 0.85 and reading.minor_lean == 1.0 and reading.energy == -1.0
    assert reading.sounds == {"pad": "strings"}
    assert reading.without == ["lead"]
    assert describe.check_reading({"genre": "polka"}) is None
    assert describe.check_reading("synthwave") is None


def ai_service(tmp_path: Path, answer: Any, sent: list[dict[str, Any]]) -> Any:
    def handler(request: httpx.Request) -> httpx.Response:
        sent.append(json.loads(request.content))
        tool = ("read_description", answer) if isinstance(answer, dict) else None
        return httpx.Response(
            200,
            content=anthropic_reply("" if tool else str(answer), tool),
            headers={"content-type": "text/event-stream"},
        )

    return make_service(tmp_path, handler)


async def test_the_service_reads_offline_by_default_and_with_the_ai_when_asked(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    service = ai_service(tmp_path, {"genre": "techno", "tempo": 135, "key": "F minor"}, sent)
    text = "something for a warehouse at 4 am"
    rules = await run_job(service, "ai.v1.song.describe", {"text": text})
    assert rules == describe.describe(text) and not sent
    no_connection = await run_job(service, "ai.v1.song.describe", {"text": text, "ai": True})
    assert "no AI connection" in no_connection["described"]["note"] and not sent

    await service.handle(
        "ai.v1.connections.save", {"name": "Claude", "provider": "anthropic", "key": "sk-test"}, no_notify
    )
    read = await run_job(service, "ai.v1.song.describe", {"text": text, "ai": True})
    assert read["described"]["genre"] == "techno" and read["described"]["readBy"] == "claude-test"
    assert read["tempo"]["median"] == 135.0 and read["keys"][0]["name"] == "F minor"
    assert sent[0]["tools"][0]["name"] == "read_description"
    assert text in sent[0]["messages"][0]["content"][0]["text"]

    for bad in ({"text": ""}, {"text": 5}, {"text": "x" * 5000}):
        with pytest.raises(RpcError):
            await service.handle("ai.v1.song.describe", bad, no_notify)


async def test_an_unusable_ai_answer_leaves_the_rules_reading(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    service = ai_service(tmp_path, {"genre": "polka"}, sent)
    await service.handle(
        "ai.v1.connections.save", {"name": "Claude", "provider": "anthropic", "key": "sk-test"}, no_notify
    )
    read = await run_job(service, "ai.v1.song.describe", {"text": "a pop song", "ai": True})
    assert read["described"]["genre"] == "pop"
    assert "not usable" in read["described"]["note"]


def test_the_parts_a_description_names_become_the_parts() -> None:
    summary = describe.describe("a song with guitar, bass and drums")
    assert [(p["role"], p["name"]) for p in summary["described"]["parts"]] == [
        ("rhythm-guitar", "Guitar"),
        ("bass", "Bass"),
        ("drums", "Drums"),
    ]
    example = describe.describe(
        "A sunny, upbeat indie pop song at 118 BPM in G major, with acoustic guitar, handclaps and a catchy "
        "whistled lead"
    )
    assert [p["role"] for p in example["described"]["parts"]] == ["acoustic-guitar", "percussion", "lead"]
    # Nothing named: no parts (the page's Suggest parts proposes the genre's).
    darkwave = describe.describe("slowburn darkwave")
    assert darkwave["described"]["parts"] == []
    assert darkwave["described"]["suggested"] == ["drums", "bass", "chords", "pad", "lead"]
    assert describe.describe("heavy metal")["described"]["suggested"] == [
        "drums",
        "bass",
        "rhythm-guitar",
        "rhythm-guitar",
        "lead-guitar",
    ]
    assert describe.read("a bass drum and a piano").parts[0]["role"] == "drums"  # not a bass
    assert all(p["role"] in songwriter.ROLES for p in describe.read("".join(w for w, *_ in describe.INSTRUMENTS)).parts)
    assert all(r in songwriter.ROLES for f in describe.FEELS.values() for r in f.suggest)


def test_each_role_plays_as_its_instrument_does() -> None:
    metal = describe.describe("aggressive heavy metal in E minor")
    roles = ["drums", "bass", "rhythm-guitar", "lead-guitar", "acoustic-guitar", "clean-guitar", "piano", "organ"]
    roles += ["strings", "brass", "woodwinds", "choir", "percussion"]
    parts = [{"id": r, "name": r, "role": r} for r in roles]
    song = songwriter.write(metal, 3, parts=parts)
    notes = song["layerParts"]
    chorus = next(s for s in song["sections"] if s["kind"] == "chorus")
    bar_ticks = song["barTicks"]
    step = bar_ticks // 16

    # Metal's rhythm guitar: palm-muted sixteenths on the root, power chords (root, fifth, octave) on accents.
    guitar = notes["rhythm-guitar"]["chorus"]
    by_start: dict[int, list[int]] = {}
    for n in guitar:
        by_start.setdefault(n["start"], []).append(n["pitch"])
    assert sum(1 for ps in by_start.values() if len(ps) == 1) >= chorus["bars"] * 6  # chugs
    power = [sorted(ps) for ps in by_start.values() if len(ps) == 3]
    assert power and all(ps[1] - ps[0] == 7 and ps[2] - ps[0] == 12 for ps in power)
    assert all(40 <= n["pitch"] <= 76 for n in guitar)
    # Double kick in the choruses.
    kicks = [n for n in notes["drums"]["chorus"] if n["drum"] == "kick" and n["start"] < bar_ticks]
    assert len(kicks) >= 12
    # The acoustic guitar strums: the strings of a stroke a few ticks apart.
    strum = sorted(n["start"] for n in notes["acoustic-guitar"]["verse"] if n["start"] < step * 2)
    assert len(strum) >= 4 and strum[1] - strum[0] == 12
    # The piano has two hands; strings hold; brass stabs are short; percussion names drums.
    piano = notes["piano"]["verse"]
    assert any(n["pitch"] < 52 for n in piano) and any(n["pitch"] >= 55 for n in piano)
    assert all(n["length"] >= bar_ticks * 0.9 for n in notes["strings"]["chorus"])
    assert all(n["length"] <= step * 2 for n in notes["brass"]["chorus"])
    assert all("drum" in n for ns in notes["percussion"].values() for n in ns)
    # Every pitched note of the harmonic roles fits the chord.
    by_id = {s["id"]: s for s in song["sections"]}
    for role in ("rhythm-guitar", "piano", "organ", "strings", "choir", "brass", "acoustic-guitar", "clean-guitar"):
        for sid, ns in notes[role].items():
            for n in ns:
                tones = by_id[sid]["chords"][min(by_id[sid]["bars"] - 1, n["start"] // bar_ticks)]["tones"]
                assert n["pitch"] % 12 in tones, (role, sid, n)
    families = {layer["id"]: layer["family"] for layer in song["layers"]}
    assert families["rhythm-guitar"] == "Guitars" and families["piano"] == "Keys" and families["choir"] == "Orchestra"


def test_reference_songs_become_a_description_whose_edits_win() -> None:
    from test_songwriter import summary as songs_summary

    songs = songs_summary()
    prompt = describe.prompt_from(songs)
    assert prompt == (
        "Like your songs: about 128 BPM, in A minor, four-on-the-floor drums; with drums, bass, synth chords and a "
        "lead melody."
    )
    # Left as it is: the songs' own numbers, nothing changed, their parts named.
    same = describe.describe(prompt, songs, prompt)
    assert same["described"]["understood"].startswith("Understood: like your songs;")
    assert same["tempo"] == songs["tempo"] and same["keys"] == songs["keys"] and same["chords"] == songs["chords"]
    assert same["perSong"] == songs["perSong"] and same["drums"] == songs["drums"]
    assert [p["role"] for p in same["described"]["parts"]] == ["drums", "bass", "chords", "lead"]

    # Edited: the words written in win; the rest of the songs' style stays underneath.
    text = prompt.replace("about 128 BPM", "140 BPM").replace("A minor", "D minor") + " Add a guitar, make it darker."
    edited = describe.describe(text, songs, prompt)
    assert edited["described"]["understood"].startswith(
        "Understood: like your songs, changed: 140 BPM, D minor, darker"
    )
    assert edited["tempo"] == {"median": 140.0, "min": 140.0, "max": 140.0}
    assert edited["keys"] == [{"name": "D minor", "count": 3}] and edited["keyFixed"]
    assert edited["perSong"] and edited["drums"]["lanes"] == songs["drums"]["lanes"]  # their groove and chords
    assert [p["role"] for p in edited["described"]["parts"]][-1] == "rhythm-guitar"
    for seed in range(4):
        song = songwriter.write(edited, seed, parts=[{"id": "g", "name": "Guitar", "role": "rhythm-guitar"}])
        assert song["tempo"] == 140.0 and song["key"]["name"] == "D minor"

    # A genre named: its groove and chords; energy and pace words move the numbers.
    trance = describe.describe(prompt + " Make it trance, more energetic.", songs, prompt)
    assert trance["described"]["genre"] == "trance"
    assert trance["drums"]["lanes"]["kick"][4] >= 0.9 and trance["perSong"] == []
    assert trance["parts"]["bass"]["notesPerBar"] > songs["parts"]["bass"]["notesPerBar"]
    slower = describe.describe(prompt + " Slower.", songs, prompt)
    assert slower["tempo"]["median"] < 128
    songwriter.write(trance, 1, parts=[{"id": "d", "name": "Drums", "role": "drums"}])


async def test_the_service_writes_the_prompt_and_reads_it_against_the_songs(tmp_path: Path) -> None:
    from test_songwriter import summary as songs_summary

    service = ai_service(tmp_path, {}, [])
    songs = songs_summary()
    prompt = await service.handle("ai.v1.song.prompt", {"summary": songs}, no_notify)
    assert prompt["text"].startswith("Like your songs: about 128 BPM")
    read = await run_job(
        service,
        "ai.v1.song.describe",
        {"text": prompt["text"] + " In E minor.", "summary": songs, "generated": prompt["text"]},
    )
    assert read["keys"] == [{"name": "E minor", "count": 3}] and read["described"]["references"]
    with pytest.raises(RpcError):
        await service.handle("ai.v1.song.prompt", {"summary": {"kind": "x"}}, no_notify)
