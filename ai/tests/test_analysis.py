"""Tempo, meter and key, on synthetic music."""

from __future__ import annotations

import numpy as np

from spinus_ai.analysis import chroma, key_of, rhythm

RATE = 22050


def chords(progression: list[list[int]], seconds: float = 1.0) -> np.ndarray:
    """Each chord (MIDI notes) for `seconds`, as decaying sawtooth-ish tones with a bass an octave down."""
    parts = []
    t = np.arange(int(seconds * RATE)) / RATE
    for chord in progression:
        tone = np.zeros_like(t)
        for i, note in enumerate([chord[0] - 12, *chord]):
            f = 440.0 * 2 ** ((note - 69) / 12)
            tone += sum(np.sin(2 * np.pi * f * h * t) / h for h in (1, 2, 3)) * (0.8 if i == 0 else 0.5)
        parts.append(tone * np.exp(-t * 1.5))
    return np.concatenate(parts)[None, :] * 0.1


def test_keys_of_chord_progressions() -> None:
    a_minor = [[57, 60, 64], [62, 65, 69], [64, 68, 71], [57, 60, 64], [65, 69, 72], [64, 68, 71], [57, 60, 64]]
    found = key_of(chroma(chords(a_minor), RATE))
    assert found["name"] == "A minor", found
    c_major = [[60, 64, 67], [65, 69, 72], [67, 71, 74], [60, 64, 67]]
    c_major += [[69, 72, 76], [62, 65, 69], [67, 71, 74], [60, 64, 67]]
    found = key_of(chroma(chords(c_major), RATE))
    assert found["name"] == "C major", found
    assert 0.0 <= found["confidence"] <= 1.0 and len(found["others"]) == 3
    # E major, a transposition: the key moves with it.
    found = key_of(chroma(chords([[n + 4 for n in c] for c in c_major]), RATE))
    assert (found["tonic"], found["scale"]) == (4, "major"), found


def test_silence_has_no_clear_key() -> None:
    found = key_of(chroma(np.zeros((1, RATE * 2)), RATE))
    assert found["confidence"] == 0.0


def test_tempo_and_bars_from_beats() -> None:
    beats = [0.1 + i * 0.5 for i in range(24)]  # 120 BPM
    downbeats = beats[::3]  # 3/4
    found = rhythm(beats, downbeats)
    assert found["tempo"] == 120.0 and found["beatsPerBar"] == 3 and found["firstDownbeat"] == 0.1
    assert rhythm([1.0], [])["tempo"] is None
    assert rhythm(beats, [])["beatsPerBar"] == 4  # no downbeats: assume 4
