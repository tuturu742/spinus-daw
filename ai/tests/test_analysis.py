"""Tempo, meter and key, on synthetic music."""

from __future__ import annotations

import numpy as np
import pytest

from spinus_ai.analysis import chroma, key_of, mend_beats, rhythm

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


def test_the_tempo_is_not_stuck_on_the_models_frames() -> None:
    # Beat This! gives beats on 20 ms frames: 135 BPM beats rounded to them still read 135, not 136.4.
    beats = [round((0.3 + i * 60.0 / 135.0) / 0.02) * 0.02 for i in range(200)]
    assert rhythm(beats, beats[::4])["tempo"] == pytest.approx(135.0, abs=0.1)
    # A gap (a break with no beat found) does not pull it down.
    with_gap = beats[:100] + [b + 2.0 for b in beats[100:]]
    assert rhythm(with_gap, with_gap[::4])["tempo"] == pytest.approx(135.0, abs=0.1)


def clicks(times: list[float], seconds: float) -> np.ndarray:
    """Short bursts of noise at `times`, like drum hits."""
    audio = np.zeros(int(seconds * RATE), dtype=np.float32)
    burst = (np.random.default_rng(1).standard_normal(400) * np.exp(-np.arange(400) / 60.0)).astype(np.float32)
    for t in times:
        at = int(t * RATE)
        audio[at : at + 400] += burst[: len(audio) - at]
    return audio


def test_a_stretch_where_the_beat_is_lost_gets_the_tempo_of_its_hits() -> None:
    # A band's song: 160 BPM, a part at 210 where Beat This! gives beats at random (lo-fi blast beats), then 145.
    first = [0.5 + i * 60.0 / 160.0 for i in range(64)]
    fast_from = first[-1] + 60.0 / 160.0
    fast = [fast_from + i * 60.0 / 210.0 for i in range(42)]
    slow_from = fast[-1] + 60.0 / 210.0
    slow = [slow_from + i * 60.0 / 145.0 for i in range(56)]
    audio = clicks(first + fast + slow, slow[-1] + 2.0)
    rng = np.random.default_rng(7)
    lost = sorted(rng.uniform(fast_from + 0.2, slow_from - 0.2, 40).tolist())
    beats = first + lost + slow
    found = mend_beats(beats, beats[::4], audio, RATE)
    assert len(found["mended"]) == 1
    assert found["mended"][0]["bpm"] == pytest.approx(210.0, abs=1.5)
    # The beats in it now keep that tempo, on the hits.
    inside = [b for b in found["beats"] if fast_from + 0.5 < b < slow_from - 0.5]
    assert np.allclose(np.diff(inside), 60.0 / found["mended"][0]["bpm"], atol=0.01)
    assert all(min(abs(b - h) for h in fast) < 0.03 for b in inside)
    # The steady beats around it are kept as they were, and no downbeat is left inside it.
    assert found["beats"][:64] == first and found["beats"][-56:] == slow
    assert not any(found["mended"][0]["from"] < d < found["mended"][0]["to"] for d in found["downbeats"])


def test_steady_beats_and_a_stretch_at_the_songs_own_tempo_keep_it() -> None:
    hits = [0.5 + i * 60.0 / 168.0 for i in range(160)]
    audio = clicks(hits, hits[-1] + 2.0)
    unchanged = mend_beats(hits, hits[::4], audio, RATE)
    assert unchanged["mended"] == [] and unchanged["beats"] == hits
    # Beats lost for a while in a song that keeps its tempo: the stretch gets the song's own tempo back.
    rng = np.random.default_rng(3)
    lost = hits[:60] + sorted(rng.uniform(hits[60], hits[100], 30).tolist()) + hits[100:]
    found = mend_beats(lost, lost[::4], audio, RATE)
    assert len(found["mended"]) == 1 and found["mended"][0]["bpm"] == pytest.approx(168.0, abs=1.0)
