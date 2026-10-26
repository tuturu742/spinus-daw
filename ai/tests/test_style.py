"""The Track creator's style summary (spinus_ai.style) on songs made up for the test: known tempo, keys,
chords, drum grooves, sections and loudness."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import numpy as np
import pytest

from spinus_ai import style
from spinus_ai.service import RpcError

RATE = 22050

# Triads as MIDI notes (around middle C) with the bass's root an octave or two down.
CHORDS = {
    "C": (60, 64, 67),
    "G": (55, 59, 62),
    "Am": (57, 60, 64),
    "F": (53, 57, 60),
    "Dm": (50, 53, 57),
    "E7": (52, 56, 59, 62),
    "D": (62, 66, 69),
    "Em": (52, 55, 59),
}
ROOTS = {"C": 36, "G": 43, "Am": 45, "F": 41, "Dm": 38, "E7": 40, "D": 38, "Em": 40}


def song_with(
    chords: list[str],
    tempo: float = 120.0,
    key: tuple[int, str] = (0, "major"),
    name: str = "Song",
    per_bar: int = 4,
) -> dict[str, Any]:
    """A song of one chord per bar (held in "other", the root in the bass), on a steady beat."""
    beat = 60.0 / tempo
    bar = beat * per_bar
    beats = [0.5 + i * beat for i in range(len(chords) * per_bar)]
    other, bass = [], []
    for i, chord in enumerate(chords):
        start = 0.5 + i * bar
        other += [{"start": start, "end": start + bar * 0.95, "pitch": p, "velocity": 0.7} for p in CHORDS[chord]]
        bass += [{"start": start + b * beat, "end": start + (b + 0.9) * beat, "pitch": ROOTS[chord], "velocity": 0.9}
                 for b in range(per_bar)]  # fmt: skip
    tonic, scale = key
    return {
        "name": name,
        "tempo": tempo,
        "beatsPerBar": per_bar,
        "firstDownbeat": beats[0],
        "beats": beats,
        "downbeats": beats[::per_bar],
        "key": {"tonic": tonic, "scale": scale, "name": f"{style.NAMES[tonic]} {scale}"},
        "loudness": {"loudness": -9.0, "truePeak": -0.8, "crest": 8.5},
        "parts": {"other": other, "bass": bass},
        "drums": [],
    }


def add_drums(song: dict[str, Any], swing: float = 0.5) -> dict[str, Any]:
    """Four on the floor: kick on every beat, snare on 2 and 4, hats on every eighth (the off-beat ones
    late by `swing`, 0.5 straight)."""
    beats = song["beats"]
    beat = beats[1] - beats[0]
    hits = []
    for i, time in enumerate(beats):
        hits.append({"time": time, "kind": "kick", "velocity": 1.0})
        if i % 4 in (1, 3):
            hits.append({"time": time + 0.003, "kind": "snare", "velocity": 0.9})
        hits.append({"time": time, "kind": "closed_hat", "velocity": 0.5})
        hits.append({"time": time + beat * swing, "kind": "closed_hat", "velocity": 0.4})
    song["drums"] = hits
    return song


def test_chords_are_found_per_bar_and_named_from_the_key() -> None:
    song = song_with(["C", "G", "Am", "F"] * 4)
    features = style.analyze_song(song)
    assert [c["name"] for c in features["chords"][:4]] == ["C", "G", "Am", "F"]
    assert [c["roman"] for c in features["chords"][:4]] == ["I", "V", "vi", "IV"]
    assert features["bars"] == 16

    # The same chords in A minor are named from A; a dominant seventh is told apart.
    minor = style.analyze_song(song_with(["Am", "F", "C", "G", "Dm", "E7"] * 2, key=(9, "minor")))
    assert [c["roman"] for c in minor["chords"][:6]] == ["i", "bVI", "bIII", "bVII", "iv", "V7"]


def test_progressions_and_how_often_chords_change() -> None:
    songs = [
        style.analyze_song(song_with(["C", "G", "Am", "F"] * 4, name="One")),
        # Two bars per chord, transposed to G: the same progression in numerals.
        style.analyze_song(
            song_with(["G", "G", "D", "D", "Em", "Em", "C", "C"] * 2, key=(7, "major"), name="Two")
        ),
    ]
    summary = style.summarize(songs)
    top = summary["chords"]["progressions"][0]
    assert top["chords"] == ["I", "V", "vi", "IV"]
    assert top["songs"] == 2
    assert summary["chords"]["barsPerChord"] == pytest.approx(1.5)
    assert summary["chords"]["transitions"]["I"]["V"] == pytest.approx(1.0)
    assert summary["chords"]["qualities"]["maj"] > 0.4


def test_the_drum_groove_per_sixteenth_step() -> None:
    groove = style.analyze_song(add_drums(song_with(["C"] * 8)))["groove"]
    assert groove["steps"] == 16 and groove["bars"] == 8
    lanes = groove["lanes"]
    assert [i + 1 for i, p in enumerate(lanes["kick"]) if p > 0.9] == [1, 5, 9, 13]
    assert [i + 1 for i, p in enumerate(lanes["snare"]) if p > 0.9] == [5, 13]
    assert [i + 1 for i, p in enumerate(lanes["hats"]) if p > 0.9] == [1, 3, 5, 7, 9, 11, 13, 15]
    assert sum(lanes["kick"]) == pytest.approx(4.0)
    assert groove["swing"] == pytest.approx(0.5, abs=0.01)

    # Hats late on the off-beat sixteenths: swung (the sixteenth grid, off-beats at 0.62 of the eighth).
    swung_song = add_drums(song_with(["C"] * 8))
    beat = swung_song["beats"][1] - swung_song["beats"][0]
    swung_song["drums"] = [h for h in swung_song["drums"] if h["kind"] != "closed_hat"] + [
        {"time": t + beat * (k + 0.62) / 2, "kind": "closed_hat"} for t in swung_song["beats"] for k in (0, 1)
    ]
    swung = style.analyze_song(swung_song)["groove"]
    assert swung["swing"] == pytest.approx(0.62, abs=0.02)

    # A half-time feel in another song: the summary weighs both by their bars.
    half = add_drums(song_with(["C"] * 8, name="Half"))
    half["drums"] = [h for h in half["drums"] if not (h["kind"] == "snare" and h["time"] % 2 < 1.0)]
    summary = style.summarize([style.analyze_song(add_drums(song_with(["C"] * 8))), style.analyze_song(half)])
    assert summary["drums"]["songs"] == 2
    assert summary["drums"]["lanes"]["snare"][4] == pytest.approx(1.0)
    assert summary["drums"]["lanes"]["snare"][12] == pytest.approx(0.5)  # in one song of the two


def test_parts_register_and_density() -> None:
    features = style.analyze_song(song_with(["C", "G", "Am", "F"] * 4))
    bass = features["parts"]["bass"]
    assert bass["present"] and bass["notesPerBar"] == pytest.approx(4.0)
    assert 36 <= bass["low"] <= bass["high"] <= 45
    other = features["parts"]["other"]
    assert other["voices"] == pytest.approx(3.0, abs=0.1)  # chords
    assert bass["voices"] == pytest.approx(1.0)  # single notes
    summary = style.summarize([features])
    assert summary["parts"]["bass"]["songs"] == 1
    assert not features["parts"]["drums"]["present"]
    assert "drums" not in summary["parts"]


def chord_audio(notes: tuple[int, ...], seconds: float, level: float) -> np.ndarray:
    t = np.arange(int(seconds * RATE)) / RATE
    tone: np.ndarray = sum(level * np.sin(2 * np.pi * 440 * 2 ** ((n - 69) / 12) * t) for n in notes)  # type: ignore[assignment]
    return tone


def test_sections_from_the_mix() -> None:
    # 8 bars of C quietly, 8 bars of a loud Dm, 8 bars of C again (2 s bars at 120 BPM).
    bar = 2.0
    mix = np.concatenate([chord_audio(CHORDS["C"], 8 * bar, 0.05), chord_audio(CHORDS["Dm"], 8 * bar, 0.2),
                          chord_audio(CHORDS["C"], 8 * bar, 0.05)])  # fmt: skip
    song = song_with(["C"] * 8 + ["Dm"] * 8 + ["C"] * 8)
    song["beats"] = [i * 0.5 for i in range(24 * 4)]
    song["downbeats"] = song["beats"][::4]
    features = style.analyze_song(song, np.stack([mix, mix]), RATE)
    sections = features["sections"]
    assert [(s["start"], s["bars"], s["label"]) for s in sections] == [(0, 8, "A"), (8, 8, "B"), (16, 8, "A")]
    assert sections[1]["level"] == 0.0 and sections[0]["level"] < -10
    assert features["loudness"]["contrast"] == pytest.approx(12.0, abs=1.0)
    summary = style.summarize([features])
    assert summary["sections"]["lengths"] == [{"bars": 8, "count": 3}]
    assert summary["sections"]["forms"] == ["ABA"]


def test_the_summary_of_several_songs_and_its_plain_words() -> None:
    songs = [
        style.analyze_song(add_drums(song_with(["Am", "F", "C", "G"] * 4, 120.0, (9, "minor"), "One"))),
        style.analyze_song(add_drums(song_with(["Am", "F", "C", "G"] * 4, 124.0, (9, "minor"), "Two"))),
        style.analyze_song(song_with(["C", "G", "Am", "F"] * 4, 128.0, (0, "major"), "Three")),
    ]
    songs[2]["loudness"]["lufs"] = -12.0
    summary = style.summarize(songs)
    assert summary["kind"] == "spinus.style-summary" and summary["version"] == style.SUMMARY_VERSION
    assert summary["tempo"] == {"median": 124.0, "min": 120.0, "max": 128.0}
    assert summary["keys"][0] == {"name": "A minor", "count": 2}
    assert summary["modes"]["minor"] == pytest.approx(2 / 3, abs=0.001)
    assert summary["loudness"]["lufs"] == -9.0 and summary["loudness"]["lufsRange"] == [-12.0, -9.0]
    assert summary["drums"]["songs"] == 2
    assert len(summary["perSong"]) == 3
    json.dumps(summary)  # it is what Spinus saves

    text = "\n".join(summary["plain"])
    assert "3 songs: One, Two, Three." in text
    assert "Tempo: 120 to 128 BPM, typically 124 BPM; 4 beats per bar." in text
    assert "mostly minor" in text and "A minor (2)" in text
    assert "Favourite progression: i - bVI - bIII - bVII" in text
    assert "kick on steps 1, 5, 9, 13" in text and "straight" in text
    assert "Bass: in every song" in text
    assert "Loudness: about -9 LUFS (-12 to -9)" in text


def test_a_song_without_a_beat_or_parts() -> None:
    features = style.analyze_song({"name": "Drone", "tempo": None, "beats": [], "parts": {}, "drums": []})
    assert features["bars"] == 0 and features["chords"] == [] and features["groove"] is None
    summary = style.summarize([features])
    assert summary["tempo"] is None
    assert any("no steady beat" in line for line in summary["plain"])
    assert any("Chords: none found" in line for line in summary["plain"])


async def test_the_service_measures_each_song_once_then_summarizes(tmp_path: Path) -> None:
    from test_listening import write_wav
    from test_stems import collect, service_with_fake_engine

    service = service_with_fake_engine(tmp_path, [])
    mix = chord_audio(CHORDS["C"], 8.0, 0.2)
    wav = write_wav(tmp_path / "song.wav", mix, RATE)
    song = {**song_with(["C", "G", "Am", "F"]), "file": str(wav)}
    done, updates = await collect(service, "ai.v1.style.summarize", {"songs": [song]})
    assert done["kind"] == "done", done
    features = done["result"]["perSong"][0]
    assert features["version"] == style.FEATURES_VERSION and features["seconds"] == pytest.approx(8.0, abs=0.01)
    assert any(u.get("step", "").startswith("Measuring the style") for u in updates)

    # Kept features are not measured again (the file may be gone by then).
    wav.unlink()
    again, updates = await collect(service, "ai.v1.style.summarize", {"songs": [{"features": features}] * 2})
    assert again["kind"] == "done", again
    assert again["result"]["songs"] == 2
    assert not any(u.get("step", "").startswith("Measuring") for u in updates)

    old, _ = await collect(service, "ai.v1.style.summarize", {"songs": [{"features": {**features, "version": 0}}]})
    assert old["kind"] == "error" and "older Spinus" in old["message"]
    with pytest.raises(RpcError, match="no audio file"):
        await service.handle("ai.v1.style.summarize", {"songs": [song]}, None)  # type: ignore[arg-type]
    with pytest.raises(RpcError, match="list of songs"):
        await service.handle("ai.v1.style.summarize", {"songs": []}, None)  # type: ignore[arg-type]
    await service.close()


async def test_notes_and_drum_hits_kept_from_before_are_only_placed(tmp_path: Path) -> None:
    # Spinus's analysis library keeps what Basic Pitch and the drum recogniser found: given back, neither
    # runs again (Basic Pitch's model is not even downloaded here, nor the recogniser trained).
    from test_listening import write_wav
    from test_stems import collect, service_with_fake_engine

    service = service_with_fake_engine(tmp_path, [])
    beats = [0.5 * i for i in range(9)]
    known = [{"start": 0.0, "end": 0.45, "pitch": 45, "velocity": 0.8}, {"start": 1.0, "end": 1.4, "pitch": 48,
                                                                          "velocity": 0.7}]  # fmt: skip
    done, updates = await collect(
        service, "ai.v1.transcribe.notes", {"known": known, "beats": beats, "firstDownbeat": 0.0}
    )
    assert done["kind"] == "done", done
    assert [(n["start"], n["pitch"]) for n in done["result"]["notes"]] == [(0, 45), (1920, 48)]
    assert done["result"]["seconds"] == known

    stem = write_wav(tmp_path / "drums.wav", chord_audio((36,), 4.0, 0.3), RATE)
    hits = [{"time": 0.0, "kind": "kick", "pad": 60, "velocity": 1.0},
            {"time": 0.5, "kind": "snare", "pad": 62, "velocity": 0.9}]  # fmt: skip
    params = {"file": str(stem), "hits": hits, "beats": beats, "firstDownbeat": 0.0, "cacheFolder": str(tmp_path / "c")}
    done, updates = await collect(service, "ai.v1.transcribe.drums", params)
    assert done["kind"] == "done", done
    assert not any("Learning" in u.get("step", "") for u in updates)
    assert [(n["start"], n["pitch"]) for n in done["result"]["notes"]] == [(0, 60), (960, 62)]
    assert not list((tmp_path / "config" / "cache").glob("drum-recogniser-*"))  # never trained
    await service.close()
