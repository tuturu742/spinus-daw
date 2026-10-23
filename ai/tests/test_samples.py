"""Samples from stems: the clearest drum hits for the pads, and phrases for the Sampler."""

from __future__ import annotations

import itertools
from pathlib import Path

import numpy as np

from spinus_ai import drums
from spinus_ai.drums import RATE, Recogniser
from spinus_ai.samples import (
    FIRST_KEY,
    MOST_PHRASES,
    beat_clock,
    choose_drum_samples,
    find_phrases,
    phrase_notes,
    sounding_parts,
)


def test_the_clearest_hit_of_each_drum_is_chosen() -> None:
    hits = [
        {"time": 0.0, "kind": "kick", "pad": 60, "velocity": 0.9},
        {"time": 0.0, "kind": "crash", "pad": 70, "velocity": 0.8},  # with the kick: not clean
        {"time": 0.5, "kind": "snare", "pad": 61, "velocity": 0.7},
        {"time": 0.52, "kind": "closed_hat", "pad": 63, "velocity": 0.3},  # cut off by... nothing much
        {"time": 1.0, "kind": "kick", "pad": 60, "velocity": 0.7},  # alone and with room: the best kick
        {"time": 2.0, "kind": "crash", "pad": 70, "velocity": 0.5},  # alone, and rings to the end
        {"time": 2.6, "kind": "snare", "pad": 61, "velocity": 0.2},
    ]
    chosen = {c["kind"]: c for c in choose_drum_samples(hits, 3.0)}
    assert set(chosen) == {"kick", "snare", "crash", "closed_hat"}
    assert abs(chosen["kick"]["start"] - (1.0 - 0.003)) < 1e-9 and abs(chosen["kick"]["end"] - 1.6) < 1e-9
    assert chosen["crash"]["start"] > 1.9 and abs(chosen["crash"]["end"] - 2.6) < 1e-9  # up to the next hit
    assert abs(chosen["snare"]["start"] - 0.497) < 1e-9  # louder, though the hat follows soon
    assert [c["pad"] for c in choose_drum_samples(hits, 3.0)] == [60, 61, 63, 70]


def test_ticks_and_seconds_follow_the_beats() -> None:
    to_ticks, to_seconds = beat_clock([0.5, 1.0, 1.5, 2.1], 0.5)
    assert to_ticks(1.0) == 960 and to_ticks(1.8) == 2400 and to_ticks(0.25) == -480
    for k in (-480, 0, 1200, 2880, 4000):
        assert abs(to_ticks(to_seconds(k)) - k) < 1e-6


def tone(seconds: float) -> np.ndarray:
    t = np.arange(int(seconds * RATE)) / RATE
    result: np.ndarray = 0.3 * np.sin(2 * np.pi * 220 * t)
    return result


def test_phrases_are_the_sounding_parts_on_the_grid() -> None:
    # 120 BPM from 0, 4/4: sound from beat 1 to 2.9, and from beat 5.1 to beat 25.1 (longer than 4 bars).
    audio = np.zeros(int(16 * RATE))
    audio[int(0.5 * RATE) : int(1.45 * RATE)] = tone(0.95)
    audio[int(2.55 * RATE) : int(12.55 * RATE)] = tone(10.0)
    assert len(sounding_parts(audio, RATE)) == 2
    beats = [i * 0.5 for i in range(33)]
    phrases = find_phrases(np.stack([audio, audio]), RATE, beats, 0.0)
    bar = 4 * 960
    assert [(p["tick"], p["tick"] + p["length"]) for p in phrases] == [
        (960, 3 * 960),  # beats 1 to 3
        (5 * 960, 5 * bar),  # from beat 5 to the bar line 4 bars on
        (5 * bar, 25 * 960 + 240),  # the rest, to the sixteenth after it ends
    ]
    assert [p["key"] for p in phrases] == [FIRST_KEY, FIRST_KEY + 1, FIRST_KEY + 2]
    assert phrases[1]["start"] == 2.5 and phrases[1]["end"] == 10.0
    notes = phrase_notes(phrases)
    assert notes[0] == {"start": 960, "length": 1920, "pitch": FIRST_KEY, "velocity": 1.0}


def test_no_more_phrases_than_keys() -> None:
    audio = np.zeros(int(100 * RATE))
    for i in range(90):  # 90 short notes, one a second
        audio[int(i * RATE + 0.1 * RATE) : int(i * RATE + 0.4 * RATE)] = tone(0.3)
    phrases = find_phrases(audio, RATE, [i * 0.5 for i in range(201)], 0.0)
    assert len(phrases) == MOST_PHRASES
    assert all(a["tick"] + a["length"] <= b["tick"] for a, b in itertools.pairwise(phrases))


async def test_the_service_makes_samples(tmp_path: Path, drum_model: Recogniser) -> None:
    from test_drums import write_mono_wav
    from test_stems import collect, service_with_fake_engine

    service = service_with_fake_engine(tmp_path, [])
    drum_model.save(tmp_path / "config" / "cache" / f"drum-recogniser-{drums.VERSION}.json")
    rng = np.random.default_rng(5)
    audio = np.zeros(int(2.0 * RATE))
    for i, kind in enumerate(["kick", "snare", "kick", "snare"]):
        hit = drums.make_hit(kind, rng) * 0.8
        start = int(i * 0.5 * RATE)
        audio[start : start + len(hit)] += hit[: len(audio) - start]
    stem = tmp_path / "drums.wav"
    write_mono_wav(stem, audio)
    beats = [i * 0.5 for i in range(5)]
    done, _ = await collect(service, "ai.v1.samples.make", {"file": str(stem), "drums": True, "beats": beats})
    assert done["kind"] == "done", done
    pads = {s["kind"]: s for s in done["result"]["samples"]}
    assert {"kick", "snare"} <= set(pads) and pads["kick"]["end"] > pads["kick"]["start"]
    assert done["result"]["notes"]

    melody = tmp_path / "bass.wav"
    write_mono_wav(melody, np.concatenate([np.zeros(RATE // 2), tone(1.0), np.zeros(RATE // 2)]))
    done, _ = await collect(service, "ai.v1.samples.make", {"file": str(melody), "beats": beats})
    assert done["kind"] == "done", done
    assert [(s["tick"], s["length"]) for s in done["result"]["samples"]] == [(960, 1920)]
    await service.close()
