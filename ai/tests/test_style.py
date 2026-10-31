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
        style.analyze_song(song_with(["G", "G", "D", "D", "Em", "Em", "C", "C"] * 2, key=(7, "major"), name="Two")),
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
    assert "Shared progression: i - bVI - bIII - bVII (in 2 songs)." in text
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


def test_loose_drums_and_busy_parts_are_said_briefly() -> None:
    # Real songs give many 20-40 % steps: only the likeliest few are named. Busy parts get whole numbers.
    summary = style.summarize([style.analyze_song(add_drums(song_with(["C"] * 8)))])
    summary["drums"]["lanes"]["kick"] = [0.3, 0.26, 0.45, 0.28, 0.27, 0.4, 0.3, 0.2] * 2
    summary["parts"]["other"]["notesPerBar"] = 26.49
    text = "\n".join(style.describe(summary))
    assert "kick with no steady place (most often on 3, 6, 11)" in text
    assert "about 26 notes a bar" in text
    assert "about 4 notes a bar" in text  # the bass


def test_hits_found_a_little_late_are_not_swing() -> None:
    # Onset detection finds hits a few milliseconds late. At 166 BPM, 25 ms late on every hit made a
    # straight sixteenth groove read as swung (0.64); measured from the eighths' own lateness it is straight.
    song = song_with(["C"] * 16, tempo=166.0)
    beat = song["beats"][1] - song["beats"][0]
    song["drums"] = [{"time": t + beat * k / 4 + 0.025, "kind": "closed_hat"} for t in song["beats"] for k in range(4)]
    assert style.analyze_song(song)["groove"]["swing"] == pytest.approx(0.5, abs=0.02)
    # Swung and late: still swung.
    song["drums"] = [
        {"time": t + beat * k + 0.025, "kind": "closed_hat"} for t in song["beats"] for k in (0, 0.31, 0.5, 0.81)
    ]
    assert style.analyze_song(song)["groove"]["swing"] == pytest.approx(0.62, abs=0.02)


def test_a_melody_note_on_the_fourth_does_not_make_a_sus4() -> None:
    # A C major chord with a passing F in the melody (a quarter of the bar) is still C.
    song = song_with(["C"] * 4)
    song["parts"]["other"] += [
        {"start": 0.5 + i * 2.0, "end": 1.0 + i * 2.0, "pitch": 65, "velocity": 0.8} for i in range(4)
    ]
    assert [c["name"] for c in style.analyze_song(song)["chords"]] == ["C"] * 4


# -----------------------------------------------------------------------------------------------
# The quality round: measured on real songs, checked here on made-up ones.

DRUM_RATE = 44100


def drum_stem(
    beats: list[float], seconds: float, late: float = 0.02, swing: float = 0.5, backbeat: int = 1
) -> np.ndarray:
    """A drum stem: a kick (a falling 60 Hz thump) on beats 1 and 3, a snare (a noise burst) on 2 and 4 (on
    1 and 3 with backbeat=0), hats (short high noise) on every sixteenth, the off-beat ones at `swing`; all
    `late` seconds after the beat times, as onset detection and beat tracking differ."""
    rng = np.random.default_rng(7)
    out = np.zeros(int(seconds * DRUM_RATE))

    def add(time: float, sound: np.ndarray) -> None:
        start = int((time + late) * DRUM_RATE)
        if 0 <= start < len(out):
            part = sound[: len(out) - start]
            out[start : start + len(part)] += part

    t = np.arange(int(0.25 * DRUM_RATE)) / DRUM_RATE
    kick = 0.9 * np.sin(2 * np.pi * (60 + 60 * np.exp(-t * 30)) * t) * np.exp(-t * 12)
    noise = rng.normal(size=int(0.15 * DRUM_RATE))
    snare = 0.5 * noise * np.exp(-np.arange(len(noise)) / DRUM_RATE * 25)
    hiss = np.diff(rng.normal(size=int(0.03 * DRUM_RATE) + 1), n=1)
    hat = 0.15 * np.diff(hiss, prepend=0.0) * np.exp(-np.arange(len(hiss)) / DRUM_RATE * 120)
    for i, beat in enumerate(beats[:-1]):
        length = beats[i + 1] - beat
        add(beat, kick if i % 2 != backbeat else snare)
        for k, place in enumerate((0.0, swing / 2, 0.5, 0.5 + swing / 2)):
            add(beat + place * length, hat * (1.0 if k % 2 == 0 else 0.7))
    return out


def test_the_drum_grid_from_the_drum_stem() -> None:
    song = song_with(["C"] * 12, tempo=120.0)
    stem = drum_stem(song["beats"], 26.0)
    grid = style.drum_grid(stem, DRUM_RATE, song)
    assert grid is not None and grid["from"] == "drum stem"
    lanes = grid["lanes"]
    assert [i + 1 for i, p in enumerate(lanes["kick"]) if p >= 0.6] == [1, 9]
    assert [i + 1 for i, p in enumerate(lanes["snare"]) if p >= 0.6] == [5, 13]
    # Every sixteenth but those right after a snare, whose noise covers them.
    assert sum(p >= 0.6 for p in lanes["hats"]) >= 14
    assert grid["swing"] == pytest.approx(0.5, abs=0.03)  # straight, though every hit is 20 ms late
    assert 0 < grid["offsetMs"] <= 30  # moved onto the drums (to within a frame)
    assert not grid["barShifted"]


def test_swung_drums_and_a_downbeat_a_beat_off() -> None:
    song = song_with(["C"] * 12, tempo=100.0)
    swung = style.drum_grid(drum_stem(song["beats"], 30.0, swing=0.66), DRUM_RATE, song)
    assert swung is not None and swung["swing"] == pytest.approx(0.66, abs=0.04)
    # Snares on the beats the bar was said to start on: the bar starts a beat later.
    off = style.drum_grid(drum_stem(song["beats"], 30.0, backbeat=0), DRUM_RATE, song)
    assert off is not None and off["barShifted"]
    assert [i + 1 for i, p in enumerate(off["lanes"]["snare"]) if p >= 0.6] == [5, 13]


def test_sections_fall_on_phrases() -> None:
    novelty = np.zeros(64)
    novelty[[8, 16, 25]] = [1.0, 1.0, 0.6]  # the last change found a bar late
    novelty[12] = 0.9  # a fill: too short to be a section in a 64-bar song
    assert style.phrase_boundaries(novelty, 4) == [0, 8, 16, 24, 32]
    # A pickup: everything a bar later is on its own phrase grid.
    assert style.phrase_boundaries(np.roll(novelty, 1), 4)[:4] == [0, 9, 17, 25]


def test_loops_count_whichever_chord_they_start_on_and_shared_needs_two_songs() -> None:
    one = style.analyze_song(song_with(["C", "G", "Am", "F"] * 4, name="One"))
    two = style.analyze_song(song_with(["Am", "F", "C", "G"] * 4, name="Two"))
    alone = style.analyze_song(song_with(["C", "F", "G", "F"] * 4, name="Alone"))
    summary = style.summarize([one, two, alone])
    top = summary["chords"]["progressions"][0]
    assert top["shared"] and top["songs"] == 2
    assert "Shared progression:" in "\n".join(summary["plain"])
    lonely = "\n".join(style.summarize([alone])["plain"])
    assert "No progression is shared by two songs; the one repeated most is I - IV - V - IV" in lonely


def test_unlike_grooves_are_not_averaged() -> None:
    four = add_drums(song_with(["C"] * 8, name="Four"))
    half = song_with(["C"] * 8, name="Half")
    half["drums"] = [
        {"time": t, "kind": k}
        for i, t in enumerate(half["beats"])
        for k in ("kick" if i % 4 == 0 else "", "snare" if i % 4 == 2 else "")
        if k
    ]
    songs = [style.analyze_song(four), style.analyze_song({**four, "name": "Four again"}), style.analyze_song(half)]
    drums = style.summarize(songs)["drums"]
    assert [len(g["songs"]) for g in drums["grooves"]] == [2, 1]
    assert drums["lanes"]["kick"][4] == pytest.approx(1.0)  # the main groove's, not a blend
    text = "\n".join(style.summarize(songs)["plain"])
    assert "Main groove (2 of 3 songs" in text and "Another groove (Half)" in text


def test_the_key_by_votes_and_the_bass_tells_relative_keys_apart() -> None:
    # A minor and C major share their notes; the bass sitting on A decides.
    minor = song_with(["Am", "F", "C", "G", "Am", "Dm", "Am", "Am"] * 2, key=(0, "major"))
    key = style.estimate_key(minor, None, None)
    assert key is not None and key["name"] == "A minor"
    assert key["votes"]["Analyzer"] == "C major" and "C major" in key["alternatives"]
    major = style.estimate_key(song_with(["C", "F", "G", "C"] * 4, key=(0, "major")), None, None)
    assert major is not None and major["name"] == "C major" and major["confidence"] == "high"


def test_a_fast_song_with_a_snare_on_every_beat_may_be_half_as_fast() -> None:
    groove = {"steps": 16, "lanes": {"kick": [0.0] * 16, "snare": [1.0, 0, 0, 0] * 4, "hats": [0.0] * 16}}
    assert style.tempo_doubt(180.0, groove) == "half"
    assert style.tempo_doubt(120.0, groove) is None
    slow = {"steps": 16, "lanes": {"kick": [0.0] * 16, "snare": [0, 0, 1.0, 0] * 4, "hats": [0.0] * 16}}
    assert style.tempo_doubt(70.0, slow) == "double"


def test_short_notes_do_not_make_chords_and_the_longest_bass_note_is_the_root() -> None:
    song = song_with(["C"] * 2)
    # A flurry of short Bs and a short low E in the bass: still C.
    song["parts"]["other"] += [
        {"start": 0.5 + i * 0.1, "end": 0.55 + i * 0.1, "pitch": 71, "velocity": 1.0} for i in range(10)
    ]
    song["parts"]["bass"].append({"start": 0.5, "end": 0.6, "pitch": 28, "velocity": 1.0})
    assert [c["name"] for c in style.analyze_song(song)["chords"]] == ["C", "C"]


def test_the_recognised_hits_are_a_second_opinion_on_kick_and_snare() -> None:
    song = song_with(["C"] * 12, tempo=120.0)
    stem = drum_stem(song["beats"], 26.0)
    # The recogniser heard every snare as a kick: the grid's kick on 5 and 13 is then only half sure.
    song["drums"] = [{"time": t + 0.02, "kind": "kick"} for t in song["beats"]]
    grid = style.drum_grid(stem, DRUM_RATE, song)
    assert grid is not None and grid["from"] == "drum stem and recognised hits"
    assert grid["lanes"]["kick"][0] == pytest.approx(1.0) and 0.4 <= grid["lanes"]["kick"][4] <= 0.8
    assert [i + 1 for i, p in enumerate(grid["lanes"]["snare"]) if p >= 0.5] == [5, 13]


def test_a_key_most_estimates_agree_on_is_kept_whatever_the_bass() -> None:
    # As in A Forest: the mix, the stems and the Analyzer say A minor; the bass starting phrases on C does not
    # turn it into C major.
    from spinus_ai.analysis import MINOR

    a_minor = np.roll(MINOR, 9)
    song = song_with(["C", "Am", "C", "Am"] * 4, key=(9, "minor"))
    key = style.estimate_key(song, a_minor, a_minor)
    assert key is not None and key["name"] == "A minor" and key["confidence"] in ("high", "medium")


# -----------------------------------------------------------------------------------------------
# Guitars (a 6-stem analysis has a guitar stem)

E2 = 82.41


def plucked(freq: float, seconds: float, decay: float, bright: float = 1.0) -> np.ndarray:
    t = np.arange(int(seconds * RATE)) / RATE
    x = np.zeros_like(t)
    for h in range(1, 25):
        if freq * h > RATE / 2:
            break
        x += (bright**h) / h * np.sin(2 * np.pi * freq * h * t) * np.exp(-t * decay * (1 + 0.15 * h))
    return x


def strummed(chord: list[float], seconds: float, hits: int, distortion: float = 0.0) -> np.ndarray:
    """A guitar strumming `chord` (Hz) `hits` times, with a pick's noise at each hit, clean or through a tanh
    distortion of that gain (which also amplifies the strings' and the amplifier's noise)."""
    length = int(seconds / hits * RATE)
    rng = np.random.default_rng(7)
    one = sum(plucked(f, seconds / hits, 0.5 if distortion else 2.5, 1.0 if distortion else 0.85) for f in chord)
    pick = rng.standard_normal(length) * np.exp(-np.arange(length) / (0.004 * RATE)) * 0.3
    one = one + pick + rng.standard_normal(length) * 0.03  # the strings' and the room's noise
    if distortion:
        one = np.tanh(distortion * (one + rng.standard_normal(length) * 0.3))
    out = np.tile(one, hits)
    return out / np.max(np.abs(out)) * 0.5


def test_a_distorted_guitar_sounds_flat_and_a_clean_one_does_not() -> None:
    power = [E2, E2 * 1.5, E2 * 2]
    clean = [E2 * 2, E2 * 2 * 1.26, E2 * 2 * 1.5, E2 * 4]
    distorted = style.guitar_tone(strummed(power, 8.0, 32, distortion=12.0), RATE)
    light = style.guitar_tone(strummed(power, 8.0, 32, distortion=3.0), RATE)
    bright = style.guitar_tone(strummed(clean, 8.0, 32), RATE)
    assert distorted >= style.DISTORTED and light >= style.DISTORTED
    assert style.TONAL <= bright < style.DISTORTED / 4
    # A synth's pure tones (what a separation may put in the guitar stem) are below a guitar's.
    t = np.arange(8 * RATE) / RATE
    synth = sum(np.sin(2 * np.pi * f * t) / k for k, f in enumerate([220.0, 277.2, 329.6], start=1)) * 0.3
    assert style.guitar_tone(synth, RATE) < style.TONAL
    # Reverb (noise tails) does not make a clean guitar sound distorted.
    rng = np.random.default_rng(1)
    tail = rng.standard_normal(int(1.5 * RATE)) * np.exp(-np.arange(int(1.5 * RATE)) / RATE * 4) * 0.03
    wet = strummed(clean, 8.0, 32)
    wet = wet + np.convolve(wet, tail)[: len(wet)]
    assert style.guitar_tone(wet, RATE) < style.DISTORTED


def guitar_notes(bars: int, bar: float, kind: str) -> list[dict[str, Any]]:
    """Power chords (E5 on E2), strummed open chords, a lead line above D4 or a low single-note riff."""
    notes = []
    for b in range(bars):
        for k in range(4):
            start = 0.5 + b * bar + k * bar / 4
            end = start + bar / 4 * 0.9
            if kind == "power":
                pitches = [40, 47, 52]
            elif kind == "open":
                pitches = [45, 52, 57, 60, 64]
            elif kind == "lead":
                pitches = [67 + (k * 2) % 7]
            else:  # riff
                pitches = [40 + (k * 3) % 5]
            notes += [{"start": start, "end": end, "pitch": p, "velocity": 0.8} for p in pitches]
    return notes


def test_how_a_guitar_is_played() -> None:
    bars = [0.5 + i * 2.0 for i in range(8)]
    power = style.guitar_playing(guitar_notes(8, 2.0, "power"), bars)
    assert power["rhythm"] == 1.0 and power["lead"] == 0.0 and power["powerChords"] == 1.0
    open_chords = style.guitar_playing(guitar_notes(8, 2.0, "open"), bars)
    assert open_chords["rhythm"] == 1.0 and open_chords["powerChords"] == 0.0
    lead = style.guitar_playing(guitar_notes(8, 2.0, "lead"), bars)
    assert lead["lead"] == 1.0 and lead["rhythm"] == 0.0
    riff = style.guitar_playing(guitar_notes(8, 2.0, "riff"), bars)
    assert riff["rhythm"] == 1.0 and riff["lead"] == 0.0  # a low single-note riff is rhythm, not a lead
    half = style.guitar_playing(
        guitar_notes(4, 2.0, "power")
        + [{**n, "start": n["start"] + 8.0, "end": n["end"] + 8.0} for n in guitar_notes(4, 2.0, "lead")],
        bars,
    )
    assert half["rhythm"] == 0.5 and half["lead"] == 0.5


def guitar_song(
    name: str, distortion: float, lead: bool
) -> tuple[dict[str, Any], dict[str, tuple[np.ndarray, int]], np.ndarray]:
    """A 16-bar song with a guitar part and stem (power chords, distorted or not; a lead line in its second
    half), and its mix (the guitar and an organ-like chord an octave up)."""
    song = add_drums(song_with(["Em", "C", "G", "D"] * 4, 120.0, (4, "minor"), name))
    notes = guitar_notes(8, 2.0, "power")
    second = guitar_notes(8, 2.0, "lead" if lead else "power")
    song["parts"]["guitar"] = notes + [{**n, "start": n["start"] + 16.0, "end": n["end"] + 16.0} for n in second]
    seconds = 0.5 + 16 * 2.0
    stem = np.concatenate([np.zeros(int(0.5 * RATE)), strummed([E2, E2 * 1.5, E2 * 2], 32.0, 64, distortion)])
    stem = np.vstack([stem, stem])
    mix = stem + 0.2 * np.sin(2 * np.pi * 330.0 * np.arange(stem.shape[1]) / RATE)
    assert abs(stem.shape[1] / RATE - seconds) < 0.1
    return song, {"guitar": (stem, RATE)}, mix


def test_a_songs_guitars_and_the_summary_of_them() -> None:
    metal, metal_stems, metal_mix = guitar_song("Metal", 12.0, lead=True)
    loud = style.analyze_song(metal, metal_mix, RATE, metal_stems)
    g = loud["guitars"]
    assert g["present"] and g["measured"] and g["distorted"]
    assert g["rhythm"] >= 0.4 and g["lead"] >= 0.4 and g["powerChords"] == 1.0
    assert g["low"] == 40 and g["level"] is not None and g["level"] > style.GUITAR_LEVEL

    jangle, jangle_stems, jangle_mix = guitar_song("Jangle", 0.0, lead=False)
    clean = style.analyze_song(jangle, jangle_mix, RATE, jangle_stems)
    assert clean["guitars"]["present"] and not clean["guitars"]["distorted"] and clean["guitars"]["lead"] == 0.0

    # Only the separation's bleed: too quiet against the mix.
    quiet, quiet_stems, quiet_mix = guitar_song("Bleed", 12.0, lead=False)
    bleed = style.analyze_song(quiet, quiet_mix, RATE, {"guitar": (quiet_stems["guitar"][0] * 0.01, RATE)})
    assert not bleed["guitars"]["present"] and bleed["guitars"]["measured"]

    # An older analysis (4 stems): not measured, said nowhere.
    older = style.analyze_song(song_with(["Am", "F"] * 4))
    assert older["guitars"] == {"present": False, "measured": False}

    summary = style.summarize([loud, bleed, clean])
    assert summary["guitars"]["measured"] == 3 and summary["guitars"]["songs"] == 2
    assert summary["guitars"]["distorted"] == 1 and summary["guitars"]["clean"] == 1
    assert summary["guitars"]["lead"] == 1
    text = "\n".join(summary["plain"])
    assert "Guitars: in 2 of 3 songs, distorted and clean, mostly power chords, with a lead guitar in 1." in text
    assert "Guitars" not in "\n".join(style.summarize([older])["plain"])
    json.dumps(summary)
