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
        "Understood: 118 BPM, G major, sunny, upbeat, catchy; genre: indie pop; sounds: acoustic guitar, whistled"
    )
    assert described["genre"] == "indiepop"
    assert summary["tempo"]["median"] == 118.0
    assert summary["keys"] == [{"name": "G major", "count": 3}]
    assert summary["modes"] == {"major": 1.0}
    assert described["named"] == {"chords": "guitar", "lead": "whistle"}

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
    assert reading.sounds == {"chords": "piano", "pad": "string"}
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
    assert describe.read("an electric piano").sounds == {"chords": "piano"}  # one instrument, not two
    assert describe.read("tempo 90").tempo == 90.0
    assert describe.read("at 999 bpm").tempo is None
    # The genre's palette fills what the description doesn't name.
    assert describe.describe("darkwave")["described"]["sounds"]["bass"] == "synth"
    assert describe.describe("darkwave with a piano")["described"]["sounds"]["chords"] == "piano"


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
