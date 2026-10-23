"""Samples from a song's stems: the song's own drum hits for the Drum Machine's pads, and a
melodic stem cut into phrases for the Sampler, with the notes that play them back where they were.

Only the choosing happens here; Spinus cuts the audio itself (it already has the stem's file)."""

from __future__ import annotations

from typing import Any

import numpy as np

# The longest a drum sample is kept (seconds): long enough for a cymbal, short enough to stay a hit.
LONGEST = {"crash": 2.0, "ride": 1.5, "open_hat": 0.8, "low_tom": 0.8, "mid_tom": 0.8, "high_tom": 0.8}
LONGEST_OTHER = 0.6
SHORTEST = 0.06
LEAD = 0.003  # a sample starts a little before the hit, so its attack is whole

# Phrases: quiet for at least this long ends one (seconds), and the Sampler has keys for this many.
GAP = 0.3
MOST_PHRASES = 64
FIRST_KEY = 60  # C5, like the Beat Slicer
TICKS_PER_BEAT = 960
GRID = 240  # sixteenths
SLACK = 30  # ticks


def choose_drum_samples(hits: list[dict[str, Any]], duration: float) -> list[dict[str, Any]]:
    """For each pad the song uses, its clearest hit: loud, with nothing else starting at the same time
    and room before the next hit. [{pad, kind, start, end}] in seconds, by pad."""
    times = np.array([h["time"] for h in hits], dtype=float)
    best: dict[int, tuple[float, dict[str, Any]]] = {}
    for h in hits:
        t = float(h["time"])
        others = np.abs(times - t)
        together = int(np.sum(others < 0.03)) - 1  # other hits at the same moment (kick and crash...)
        later = times[times > t + 0.03]
        gap = float(later.min() - t) if len(later) else duration - t
        longest = LONGEST.get(h["kind"], LONGEST_OTHER)
        score = float(h["velocity"]) * (0.3 if together else 1.0) * min(1.0, gap / longest)
        if gap < SHORTEST:
            continue
        if h["pad"] not in best or score > best[h["pad"]][0]:
            end = min(t + longest, t + gap, duration)
            best[h["pad"]] = (score, {"pad": h["pad"], "kind": h["kind"], "start": max(0.0, t - LEAD), "end": end})
    return [best[pad][1] for pad in sorted(best)]


def beat_clock(beats: list[float], first_downbeat: float) -> tuple[Any, Any]:
    """Seconds to ticks and back along the song's own beats (from the first downbeat), continuing at the
    first and last beat lengths past the ends; 120 BPM when there are no beats."""
    if len(beats) < 2:
        return (lambda t: (t - first_downbeat) * 2 * TICKS_PER_BEAT), (
            lambda k: first_downbeat + k / (2 * TICKS_PER_BEAT)
        )
    times = np.asarray(beats, dtype=float)
    first = int(np.argmin(np.abs(times - first_downbeat)))
    index = (np.arange(len(times)) - first).astype(float)
    head, tail = times[1] - times[0], times[-1] - times[-2]

    def to_ticks(t: float) -> float:
        if t < times[0]:
            return float((index[0] - (times[0] - t) / head) * TICKS_PER_BEAT)
        if t > times[-1]:
            return float((index[-1] + (t - times[-1]) / tail) * TICKS_PER_BEAT)
        return float(np.interp(t, times, index) * TICKS_PER_BEAT)

    def to_seconds(k: float) -> float:
        b = k / TICKS_PER_BEAT
        if b < index[0]:
            return float(times[0] - (index[0] - b) * head)
        if b > index[-1]:
            return float(times[-1] + (b - index[-1]) * tail)
        return float(np.interp(b, index, times))

    return to_ticks, to_seconds


def sounding_parts(mono: np.ndarray, rate: int) -> list[tuple[float, float]]:
    """Where a stem is not quiet (10 ms steps, quieter than 40 dB under its loudest is quiet), with gaps
    shorter than GAP joined: [(start, end)] in seconds."""
    hop = max(1, rate // 100)
    frames = len(mono) // hop
    if frames == 0:
        return []
    level = np.sqrt(np.mean(mono[: frames * hop].reshape(frames, hop) ** 2, axis=1))
    loud = level > max(float(level.max()) * 0.01, 1e-4)
    parts: list[list[float]] = []
    for i in np.flatnonzero(loud):
        t = i * hop / rate
        if parts and t - parts[-1][1] <= GAP:
            parts[-1][1] = t + hop / rate
        else:
            parts.append([t, t + hop / rate])
    return [(a, b) for a, b in parts if b - a >= 0.05]


def find_phrases(
    audio: np.ndarray, rate: int, beats: list[float], first_downbeat: float, beats_per_bar: int = 4
) -> list[dict[str, Any]]:
    """A melodic stem as phrases: each sounding part, from the sixteenth at or before it starts to the one
    after it ends, and no longer than 4 bars (cut at bar lines). Phrases before the first downbeat are
    left out, like notes. [{key, start, end (seconds of audio to cut), tick, length (ticks)}]."""
    mono = audio.mean(axis=0) if audio.ndim == 2 else audio
    to_ticks, to_seconds = beat_clock(beats, first_downbeat)
    bar = beats_per_bar * TICKS_PER_BEAT
    spans: list[list[int]] = []
    for start, end in sounding_parts(mono, rate):
        # (A sound found a few milliseconds early or late still counts as on its sixteenth.)
        a = int(np.floor((to_ticks(start) + SLACK) / GRID)) * GRID
        b = int(np.ceil((to_ticks(end) - SLACK) / GRID)) * GRID
        if b <= 0:
            continue
        a = max(a, 0)
        if spans and a < spans[-1][1]:  # touching after snapping: one phrase
            spans[-1][1] = max(spans[-1][1], b)
            continue
        spans.append([a, b])
    phrases: list[list[int]] = []
    for a, b in spans:
        while b - a > 4 * bar:
            cut = (a // bar + 4) * bar
            phrases.append([a, cut])
            a = cut
        phrases.append([a, b])
    # More phrases than keys: join the closest neighbours.
    while len(phrases) > MOST_PHRASES:
        gaps = [phrases[i + 1][0] - phrases[i][1] for i in range(len(phrases) - 1)]
        i = int(np.argmin(gaps))
        phrases[i : i + 2] = [[phrases[i][0], phrases[i + 1][1]]]
    return [
        {"key": FIRST_KEY + i, "start": round(to_seconds(a), 5), "end": round(to_seconds(b), 5), "tick": a,
         "length": b - a}
        for i, (a, b) in enumerate(phrases)
    ]  # fmt: skip


def phrase_notes(phrases: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """The notes that play the phrases back where they were."""
    return [{"start": p["tick"], "length": p["length"], "pitch": p["key"], "velocity": 1.0} for p in phrases]
