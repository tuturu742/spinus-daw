"""Tempo, meter and key of a song, from Beat This!'s beats and from the audio itself.

The key needs no model: a chroma profile (how much of each of the 12 pitch classes the music has, from an
FFT of the audio) correlated with the 24 major and minor key profiles of Krumhansl and Kessler, as most
key finders do. NumPy only.
"""

from __future__ import annotations

import itertools
from collections import Counter
from typing import Any

import numpy as np

NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")

# Krumhansl-Kessler probe-tone profiles (1982), from C.
MAJOR = np.array([6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88])
MINOR = np.array([6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17])


def rhythm(beats: list[float], downbeats: list[float]) -> dict[str, Any]:
    """Tempo (BPM, from the median beat), beats per bar (the most common count between downbeats) and
    where the first bar starts."""
    if len(beats) < 2:
        return {"tempo": None, "beatsPerBar": None, "firstDownbeat": None, "beats": beats, "downbeats": downbeats}
    intervals = np.diff(np.asarray(beats))
    # The beats come on the model's 20 ms frames, so one interval is off by up to a frame (at 136 BPM the
    # median alone jumps between 130.4, 136.4 and 142.9). The mean of the steady intervals (within 10 % of
    # the median: no gaps or doubled beats) averages that out.
    middle = float(np.median(intervals))
    steady = intervals[np.abs(intervals - middle) <= 0.1 * middle]
    tempo = float(60.0 / (steady.mean() if len(steady) else middle))
    counts: Counter[int] = Counter()
    for a, b in itertools.pairwise(downbeats):
        counts[sum(1 for t in beats if a - 0.02 <= t < b - 0.02)] += 1
    per_bar = counts.most_common(1)[0][0] if counts else 4
    return {
        "tempo": round(tempo, 2),
        "beatsPerBar": per_bar if 2 <= per_bar <= 12 else 4,
        "firstDownbeat": downbeats[0] if downbeats else beats[0],
        "beats": beats,
        "downbeats": downbeats,
    }


def chroma(audio: np.ndarray, rate: int) -> np.ndarray:
    """Energy of each pitch class (C first), from 55 Hz to 5 kHz, each moment weighed equally."""
    mono = audio.mean(axis=0) if audio.ndim == 2 else audio
    size, hop = 8192, 4096
    if len(mono) < size:
        mono = np.pad(mono, (0, size - len(mono)))
    frames = np.lib.stride_tricks.sliding_window_view(mono, size)[::hop]
    spectrum = np.abs(np.fft.rfft(frames * np.hanning(size), axis=1))
    freqs = np.fft.rfftfreq(size, 1.0 / rate)
    band = (freqs >= 55.0) & (freqs <= 5000.0)
    pitch = np.round(12.0 * np.log2(freqs[band] / 440.0) + 69.0).astype(int) % 12
    weights = np.zeros((12, int(band.sum())))
    weights[pitch, np.arange(len(pitch))] = 1.0
    per_frame = spectrum[:, band] @ weights.T  # frames x 12
    loudness = per_frame.sum(axis=1, keepdims=True)
    per_frame = per_frame / np.maximum(loudness, 1e-9)
    per_frame[loudness[:, 0] < 1e-3 * max(float(loudness.max()), 1e-9)] = 0.0  # silence says nothing
    profile: np.ndarray = per_frame.sum(axis=0)
    return profile


def key_of(profile: np.ndarray) -> dict[str, Any]:
    """The best key for a chroma profile, with the runners-up: {tonic, scale, name, confidence, others}."""
    scores = []
    for tonic in range(12):
        for scale, shape in (("major", MAJOR), ("minor", MINOR)):
            r = float(np.corrcoef(profile, np.roll(shape, tonic))[0, 1]) if profile.std() > 0 else 0.0
            scores.append((r, tonic, scale))
    scores.sort(reverse=True)
    best = scores[0]
    return {
        "tonic": best[1],
        "scale": best[2],
        "name": f"{NAMES[best[1]]} {best[2]}",
        # How clearly it wins: the gap to the second best, as a 0-1 number (0.1 apart is already clear).
        "confidence": round(min(1.0, max(0.0, (best[0] - scores[1][0]) * 10.0)), 2),
        "others": [
            {"tonic": t, "scale": s, "name": f"{NAMES[t]} {s}", "score": round(r, 3)} for r, t, s in scores[1:4]
        ],
    }
