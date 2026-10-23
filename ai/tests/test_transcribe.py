"""Notes from melodic stems: the Basic Pitch port and placing notes on the song's beats.

SPINUS_TEST_BASIC_PITCH=<path of nmp.onnx> also runs the model on a test melody.
"""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest

from spinus_ai.transcribe import (
    MIN_NOTE_FRAMES,
    Model,
    frames_to_time,
    local_maxima,
    notes_from_output,
    place_on_beats,
    resample,
    transcribe,
)


def test_local_maxima_are_strict_peaks_in_time() -> None:
    values = np.array([[0.0, 1.0], [0.5, 1.0], [0.2, 0.0], [0.9, 0.3], [0.1, 0.2]])
    rows, cols = local_maxima(values)
    assert sorted(zip(rows.tolist(), cols.tolist(), strict=True)) == [(1, 0), (3, 0), (3, 1)]


def test_a_held_note_with_an_onset_becomes_one_note() -> None:
    frames = np.zeros((120, 88))
    onsets = np.zeros((120, 88))
    frames[10:60, 39] = 0.8  # MIDI 60
    onsets[10, 39] = 0.9
    notes = notes_from_output(frames, onsets)
    assert len(notes) == 1
    start, end, pitch, amplitude = notes[0]
    assert (start, pitch) == (10, 60) and 58 <= end <= 60 and abs(amplitude - 0.8) < 0.01

    # Too short: nothing.
    frames[:] = 0
    frames[10 : 10 + MIN_NOTE_FRAMES - 2, 39] = 0.8
    assert notes_from_output(frames, onsets) == []


def test_frame_times_and_resampling() -> None:
    times = frames_to_time(200)
    assert times[0] == 0.0 and abs(times[86] - 86 * 256 / 22050 + 0.0) < 0.05
    assert len(resample(np.zeros(44100), 44100)) == 22050
    assert len(resample(np.zeros(22050), 22050)) == 22050


def test_notes_follow_the_songs_beats() -> None:
    notes = [
        {"start": 0.0, "end": 0.4, "pitch": 40, "velocity": 0.5},  # before the first downbeat: dropped
        {"start": 0.52, "end": 0.98, "pitch": 43, "velocity": 0.7},
        {"start": 1.51, "end": 1.74, "pitch": 45, "velocity": 0.6},
    ]
    beats = [0.5 + i * 0.5 for i in range(8)]  # 120 BPM from 0.5 s
    placed = place_on_beats(notes, beats, 0.5)
    assert placed == [
        {"start": 0, "length": 960, "pitch": 43, "velocity": 0.7},
        {"start": 1920, "length": 480, "pitch": 45, "velocity": 0.6},
    ]

    # A song that slows down: the note on the fourth beat still lands on beat four.
    slowing = [0.0, 0.5, 1.05, 1.65, 2.3]
    placed = place_on_beats([{"start": 1.66, "end": 2.31, "pitch": 50, "velocity": 0.5}], slowing, 0.0)
    assert placed[0]["start"] == 3 * 960 and placed[0]["length"] == 960


@pytest.mark.skipif(not os.environ.get("SPINUS_TEST_BASIC_PITCH"), reason="needs Basic Pitch's nmp.onnx")
def test_basic_pitch_hears_a_melody() -> None:
    rate = 44100
    melody = [(0.0, 0.5, 60), (0.5, 1.0, 64), (1.0, 1.5, 67), (1.5, 2.5, 72), (2.5, 3.0, 71), (3.0, 4.0, 67)]
    t = np.arange(int(rate * 4.5)) / rate
    audio = np.zeros_like(t)
    for start, end, pitch in melody:
        f = 440 * 2 ** ((pitch - 69) / 12)
        inside = (t >= start) & (t < end)
        tt = t[inside] - start
        audio[inside] += 0.3 * sum(np.sin(2 * np.pi * f * h * tt) / h for h in (1, 2, 3)) * np.exp(-tt * 2)
    notes = transcribe(Model(Path(os.environ["SPINUS_TEST_BASIC_PITCH"])), audio[None, :], rate)
    assert [n["pitch"] for n in notes] == [p for _, _, p in melody]
    assert all(abs(n["start"] - s) < 0.03 for n, (s, _, _) in zip(notes, melody, strict=True))
