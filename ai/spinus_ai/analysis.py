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

from .keynames import key_name

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


def onsets(audio: np.ndarray, rate: int) -> tuple[np.ndarray, float]:
    """How much new sound starts at each moment (spectral flux of the log spectrum), and its frames per second."""
    mono = audio.mean(axis=0) if audio.ndim == 2 else audio
    step = max(1, round(rate / 22050))
    mono, rate = mono[::step], rate // step  # 22 kHz is plenty for where hits are
    size, hop = 1024, 128
    if len(mono) < size * 2:
        return np.zeros(1), rate / hop
    frames = np.lib.stride_tricks.sliding_window_view(mono, size)[::hop]
    spectrum = np.log1p(10.0 * np.abs(np.fft.rfft(frames * np.hanning(size), axis=1)))
    flux = np.maximum(0.0, np.diff(spectrum, axis=0)).sum(axis=1)
    # A hit shows in the first window that holds it, which starts most of a window before it: frames moved so
    # that frame i is at i / frames per second.
    late = 1 + (size - hop // 2) // hop
    return np.concatenate([np.zeros(late), flux]), rate / hop


def pulse(envelope: np.ndarray, frame_rate: float, bpm: float) -> float:
    """How strongly the hits repeat every beat, 2 and 4 beats at `bpm` (autocorrelation, 1 at most)."""
    seg = envelope - envelope.mean()
    energy = float(np.dot(seg, seg))
    if energy <= 0.0:
        return 0.0
    lags = [round(60.0 * frame_rate / bpm * m) for m in (1, 2, 4)]
    return float(np.mean([np.dot(seg[:-k], seg[k:]) / energy if 0 < k < len(seg) else 0.0 for k in lags]))


# How strongly the hits must repeat at a tempo for a stretch to get beats at it.
MEND_PULSE = 0.2
# How much more clearly than the song's own tempo a stretch's hits must keep a tempo to get it.
MEND_CLEARER = 1.3


def mend_beats(beats: list[float], downbeats: list[float], audio: np.ndarray, rate: int) -> dict[str, Any]:
    """Beats where Beat This! loses the beat, from the hits themselves.

    In fast, noisy stretches (a blast beat, lo-fi metal) its beats come 0.1 to 0.5 s apart at random, so no tempo
    can be read there, and a part played at a tempo of its own (a band's song going from 160 to 210 BPM for a few
    bars) is lost. Such a stretch (beats that keep no interval, between steady ones) gets a steady beat placed where
    its hits are: at the tempo they repeat at most, from 0.6 to 1.6 times the song's (so it keeps the song's way of
    counting), when they keep it clearly better than the song's own tempo, else at the song's. One whose hits repeat
    at no tempo is left as it is, and so are the beats that keep an interval. {beats, downbeats, mended: [{from, to,
    bpm}]}."""
    unchanged: dict[str, Any] = {"beats": beats, "downbeats": downbeats, "mended": []}
    if len(beats) < 16:
        return unchanged
    times = np.asarray(beats, dtype=float)
    intervals = np.diff(times)
    # An interval is off when it is more than 20 % from the intervals around it (9 of them).
    local = np.array([np.median(intervals[max(0, i - 4) : i + 5]) for i in range(len(intervals))])
    off = np.abs(intervals / local - 1.0) > 0.2
    # Stretches: off intervals fewer than 6 beats apart, 3 or more of them.
    stretches: list[tuple[int, int]] = []
    marks = np.flatnonzero(off)
    i = 0
    while i < len(marks):
        j = i
        while j + 1 < len(marks) and marks[j + 1] - marks[j] < 6:
            j += 1
        if j - i + 1 >= 3:
            stretches.append((int(marks[i]), int(marks[j]) + 1))  # beats[first] .. beats[last]: the anchors
        i = j + 1
    if not stretches:
        return unchanged
    envelope, frame_rate = onsets(audio, rate)
    # The song's own tempo (from its steady beats): beats twice or half as fast stay in their octave.
    near = float(rhythm(beats, downbeats)["tempo"] or 120.0)
    out = list(beats)
    out_downbeats = list(downbeats)
    mended = []
    for first, last in reversed(stretches):
        a, z = times[first], times[last]
        if z - a < 3.0:
            continue
        segment = envelope[int(a * frame_rate) : int(z * frame_rate)]
        # A band changes tempo by less than it takes to double or halve it: the tempos from 0.6 to 1.6 times
        # the song's. (Searched wider, the strongest is often 4/3 or 3/2 of the beat: triplets.)
        candidates = np.arange(max(40.0, near * 0.6), min(320.0, near * 1.6), 0.5)
        scores = np.array([pulse(segment, frame_rate, c) for c in candidates])
        if len(scores) == 0 or scores.max() < MEND_PULSE:
            continue  # no tempo there either: left as it is
        best = float(candidates[int(np.argmax(scores))])
        # A tempo of its own only when its hits keep it clearly better than the song's (or twice or half of it,
        # the same beat counted another way): else the song's tempo, its beats placed where the hits are.
        own = max((t for t in (near, near * 2.0, near / 2.0) if 40.0 <= t <= 320.0),
                  key=lambda t: pulse(segment, frame_rate, t))  # fmt: skip
        keeps = pulse(segment, frame_rate, own)
        # The strongest at the edge of the tempos searched is a slope towards one outside them: none of them.
        inside = best / candidates[0] - 1.0 > 0.02 and 1.0 - best / candidates[-1] > 0.02
        bpm = best if inside and scores.max() >= MEND_CLEARER * max(keeps, 0.0) else near
        period = 60.0 / bpm
        # Where the beat falls: the phase that lands on the most hits.
        frames = np.arange(len(segment)) / frame_rate
        best_phase, most = 0.0, -1.0
        for phase in np.arange(0.0, period, 0.005):
            got = float(np.interp(np.arange(phase, z - a, period), frames, segment).sum())
            if got > most:
                best_phase, most = phase, got
        grid = [a + t for t in np.arange(best_phase, z - a, period) if period * 0.5 <= t <= z - a - period * 0.5]
        out[first + 1 : last] = grid
        out_downbeats = [d for d in out_downbeats if not a < d < z]
        mended.append({"from": round(float(a), 3), "to": round(float(z), 3), "bpm": round(bpm, 2)})
    mended.reverse()
    return {"beats": out, "downbeats": out_downbeats, "mended": mended}


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
        "name": key_name(best[1], best[2]),
        # How clearly it wins: the gap to the second best, as a 0-1 number (0.1 apart is already clear).
        "confidence": round(min(1.0, max(0.0, (best[0] - scores[1][0]) * 10.0)), 2),
        "others": [{"tonic": t, "scale": s, "name": key_name(t, s), "score": round(r, 3)} for r, t, s in scores[1:4]],
    }
