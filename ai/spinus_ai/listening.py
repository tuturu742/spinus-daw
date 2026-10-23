"""Listening to a take: pitch, onsets, notes, beatbox sounds and tempo.

Everything runs on the CPU with NumPy only (pYIN here, PyTorch only in the analysis engine):

- Pitch: pYIN (Mauch and Dixon, 2014): YIN's difference function gives pitch candidates whose
  probabilities come from a distribution of thresholds; a hidden Markov model over pitch bins, voiced or
  not, picks the smoothest likely path (Viterbi).
- Onsets: spectral flux of the log spectrum with an adaptive threshold.
- Notes: voiced stretches split where the pitch moves to another note or a new onset starts one.
- Beatbox: each onset's sound described by a few spectral features, matched to the user's own taught
  examples (or to rough rules before any are taught).
- Tempo: the autocorrelation of the onset strength.
"""

from __future__ import annotations

import math
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
from numpy.typing import NDArray

Array = NDArray[np.float64]

VERSION = 1  # bump when results change, so cached ones are not reused

SOUNDS = ("kick", "snare", "hat")


# ---------------------------------------------------------------------------------------------------
# Audio files


def read_wav(path: Path) -> tuple[Array, int]:
    """A WAV file's audio as mono floats -1..1, and its sample rate (8, 16, 24 or 32-bit PCM)."""
    with wave.open(str(path), "rb") as w:
        rate, width, channels, frames = w.getframerate(), w.getsampwidth(), w.getnchannels(), w.getnframes()
        raw = w.readframes(frames)

    if width == 1:
        data = (np.frombuffer(raw, dtype=np.uint8).astype(np.float64) - 128.0) / 128.0
    elif width == 2:
        data = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif width == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        ints = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        ints = np.where(ints >= 1 << 23, ints - (1 << 24), ints)
        data = ints.astype(np.float64) / float(1 << 23)
    elif width == 4:
        data = np.frombuffer(raw, dtype="<i4").astype(np.float64) / float(1 << 31)
    else:
        raise ValueError(f"Unsupported WAV sample size: {width * 8} bits.")

    if channels > 1:
        data = data.reshape(-1, channels).mean(axis=1)
    return data, rate


# ---------------------------------------------------------------------------------------------------
# Pitch: pYIN


@dataclass
class PitchTrack:
    times: Array  # seconds, frame centres
    hz: Array  # 0 where unvoiced
    voiced_probability: Array

    def to_json(self) -> dict[str, Any]:
        return {
            "times": [round(float(t), 4) for t in self.times],
            "hz": [round(float(f), 2) for f in self.hz],
            "voiced": [round(float(p), 3) for p in self.voiced_probability],
        }


def frame_signal(x: Array, frame: int, hop: int) -> Array:
    """Frames centred on multiples of hop (the signal is padded with zeros at both ends)."""
    padded = np.concatenate([np.zeros(frame // 2), x, np.zeros(frame // 2)])
    count = 1 + max(0, (len(padded) - frame) // hop)
    index = np.arange(frame)[None, :] + hop * np.arange(count)[:, None]
    framed: Array = padded[index]
    return framed


def cumulative_mean_normalized_difference(frames: Array, max_lag: int) -> Array:
    """YIN's d'(tau) for each frame, tau = 0..max_lag (the window is the frame without max_lag)."""
    window = frames.shape[1] - max_lag
    size = 1 << math.ceil(math.log2(frames.shape[1] + window))
    a = np.fft.rfft(frames[:, :window], size)
    b = np.fft.rfft(frames, size)
    correlation = np.fft.irfft(np.conj(a) * b, size)[:, : max_lag + 1]

    squares = np.concatenate([np.zeros((frames.shape[0], 1)), np.cumsum(frames**2, axis=1)], axis=1)
    lags = np.arange(max_lag + 1)
    moved = squares[:, lags + window] - squares[:, lags]  # energy of the window moved by tau
    difference = np.maximum(moved[:, :1] + moved - 2.0 * correlation, 0.0)
    difference[:, 0] = 0.0

    running = np.cumsum(difference[:, 1:], axis=1)
    normalized: Array = np.ones_like(difference)
    with np.errstate(divide="ignore", invalid="ignore"):
        normalized[:, 1:] = np.where(running > 0, difference[:, 1:] * lags[1:] / running, 1.0)
    return normalized


def beta_thresholds(count: int = 100, a: float = 2.0, b: float = 18.0) -> tuple[Array, Array]:
    """Thresholds 0..1 and the cumulative probability of each under a Beta(a, b) prior (mean 0.1)."""
    edges = np.linspace(0.0, 1.0, count + 1)
    centres = (edges[:-1] + edges[1:]) / 2.0
    density = centres ** (a - 1.0) * (1.0 - centres) ** (b - 1.0)
    cdf = np.concatenate([[0.0], np.cumsum(density / density.sum())])
    return edges, cdf


def pyin(
    x: Array,
    rate: int,
    fmin: float = 60.0,
    fmax: float = 1200.0,
    bins_per_semitone: int = 5,
    switch_probability: float = 0.01,
    max_semitones_per_second: float = 35.92 * 12.0,
) -> PitchTrack:
    max_lag = math.ceil(rate / fmin)
    min_lag = max(2, math.floor(rate / fmax))
    frame = 1 << math.ceil(math.log2(2 * max_lag + 1))
    hop = max(1, round(rate * 0.005))  # 5 ms
    frames = frame_signal(x, frame, hop)
    times = np.arange(frames.shape[0]) * hop / rate
    d = cumulative_mean_normalized_difference(frames, max_lag)

    # Troughs of d' between min_lag and max_lag, and where each lies between samples.
    inner = d[:, min_lag : max_lag + 1]
    left = d[:, min_lag - 1 : max_lag]
    right = np.concatenate([d[:, min_lag + 1 : max_lag + 1], np.full((d.shape[0], 1), np.inf)], axis=1)
    trough = (inner < left) & (inner <= right)

    edges, cdf = beta_thresholds()
    n_bins = math.floor(12 * bins_per_semitone * math.log2(fmax / fmin)) + 1
    observation = np.zeros((frames.shape[0], 2 * n_bins))
    candidates: list[Array] = [np.zeros(0)] * frames.shape[0]  # each frame's exact pitch candidates (Hz)

    for i in range(frames.shape[0]):
        lags = np.nonzero(trough[i])[0]
        if len(lags) == 0:
            observation[i, n_bins:] = 1.0 / n_bins
            continue
        values = inner[i, lags]
        # A trough gets the thresholds above its value but not above any earlier trough's.
        earlier = np.concatenate([[np.inf], np.minimum.accumulate(values)[:-1]])
        upper = np.interp(np.minimum(earlier, 1.0), edges, cdf)
        lower = np.interp(np.minimum(values, 1.0), edges, cdf)
        weights = np.maximum(upper - lower, 0.0)
        if weights.sum() <= 0.0:  # nothing under any threshold: a little weight on the deepest trough
            weights[np.argmin(values)] = 0.01

        lag = lags + min_lag
        a, b, c = d[i, lag - 1], d[i, lag], d[i, np.minimum(lag + 1, max_lag)]
        denominator = a - 2.0 * b + c
        shift = np.where(np.abs(denominator) > 1e-12, 0.5 * (a - c) / np.where(denominator == 0, 1, denominator), 0.0)
        hz = rate / (lag + np.clip(shift, -0.5, 0.5))
        candidates[i] = hz
        bins = np.round(12 * bins_per_semitone * np.log2(hz / fmin)).astype(int)
        ok = (bins >= 0) & (bins < n_bins)
        np.add.at(observation[i], bins[ok], weights[ok])
        voiced = min(1.0, observation[i, :n_bins].sum())
        observation[i, n_bins:] = (1.0 - voiced) / n_bins

    path = viterbi(observation, n_bins, bins_per_semitone, hop / rate, switch_probability, max_semitones_per_second)
    voiced_states = path < n_bins
    frequencies = fmin * 2.0 ** ((path % n_bins) / (12.0 * bins_per_semitone))

    # The path is in bins (20 cents); the exact candidate near it keeps fine detail such as vibrato.
    for voiced_frame in np.nonzero(voiced_states)[0]:
        found = candidates[int(voiced_frame)]
        if len(found):
            nearest = found[np.argmin(np.abs(np.log2(found / frequencies[voiced_frame])))]
            if abs(12.0 * math.log2(nearest / frequencies[voiced_frame])) < 1.0 / bins_per_semitone:
                frequencies[voiced_frame] = nearest
    return PitchTrack(times, np.where(voiced_states, frequencies, 0.0), observation[:, :n_bins].sum(axis=1))


def viterbi(
    observation: Array,
    n_bins: int,
    bins_per_semitone: int,
    seconds_per_frame: float,
    switch: float,
    max_semitones_per_second: float,
) -> NDArray[np.int64]:
    """The most likely state per frame: pitch bins 0..n-1 voiced, n..2n-1 the same bins unvoiced."""
    width = max(1, round(max_semitones_per_second * seconds_per_frame * bins_per_semitone))
    shifts = np.arange(-width, width + 1)
    triangle = (width + 1 - np.abs(shifts)).astype(np.float64)
    log_move = np.log(triangle / triangle.sum())
    stay, change = math.log(1.0 - switch), math.log(switch)

    with np.errstate(divide="ignore"):
        log_obs = np.log(observation + 1e-300)
    count = observation.shape[0]
    score = log_obs[0] - math.log(2 * n_bins)
    back = np.zeros((count, 2 * n_bins), dtype=np.int64)
    columns = np.arange(n_bins)

    def best_move(values: Array) -> tuple[Array, NDArray[np.int64]]:
        # For each bin j: max over k of values[j - k] + log_move[k], and which source bin that was.
        candidates = np.full((len(shifts), n_bins), -np.inf)
        for row, k in enumerate(shifts):
            source = columns - k
            ok = (source >= 0) & (source < n_bins)
            candidates[row, ok] = values[source[ok]] + log_move[row]
        best = np.argmax(candidates, axis=0)
        return candidates[best, columns], columns - shifts[best]

    for t in range(1, count):
        voiced_best, voiced_from = best_move(score[:n_bins])
        unvoiced_best, unvoiced_from = best_move(score[n_bins:])
        to_voiced = np.stack([voiced_best + stay, unvoiced_best + change])
        to_unvoiced = np.stack([voiced_best + change, unvoiced_best + stay])
        pick_v, pick_u = np.argmax(to_voiced, axis=0), np.argmax(to_unvoiced, axis=0)
        back[t, :n_bins] = np.where(pick_v == 0, voiced_from, unvoiced_from + n_bins)
        back[t, n_bins:] = np.where(pick_u == 0, voiced_from, unvoiced_from + n_bins)
        score = np.concatenate([to_voiced[pick_v, columns], to_unvoiced[pick_u, columns]]) + log_obs[t]

    path = np.zeros(count, dtype=np.int64)
    path[-1] = int(np.argmax(score))
    for t in range(count - 1, 0, -1):
        path[t - 1] = back[t, path[t]]
    return path


# ---------------------------------------------------------------------------------------------------
# Onsets


@dataclass
class Onsets:
    times: Array
    strengths: Array  # 0..1
    envelope: Array  # onset strength per frame
    frame_seconds: float


def spectrogram(x: Array, rate: int, size: int = 1024) -> tuple[Array, float]:
    hop = max(1, round(rate * 0.005))
    frames = frame_signal(x, size, hop) * np.hanning(size)[None, :]
    return np.abs(np.fft.rfft(frames, axis=1)), hop / rate


def detect_onsets(x: Array, rate: int, minimum_gap: float = 0.05) -> Onsets:
    magnitude, seconds = spectrogram(x, rate)
    log_magnitude = np.log1p(100.0 * magnitude)
    flux = np.concatenate([[0.0], np.maximum(np.diff(log_magnitude, axis=0), 0.0).sum(axis=1)])
    peak = flux.max() if len(flux) else 0.0
    envelope = flux / peak if peak > 0 else flux

    # A peak counts when it stands above the local median by a margin and is the largest nearby.
    span = max(1, round(0.1 / seconds))
    padded = np.pad(envelope, span, mode="edge")
    windows = np.lib.stride_tricks.sliding_window_view(padded, 2 * span + 1)
    threshold = np.median(windows, axis=1) + 0.08
    near = max(1, round(0.03 / seconds))
    local_max = np.lib.stride_tricks.sliding_window_view(np.pad(envelope, near, mode="constant"), 2 * near + 1).max(
        axis=1
    )
    candidates = np.nonzero((envelope >= threshold) & (envelope >= local_max) & (envelope > 0.05))[0]

    picked: list[int] = []
    for index in candidates:
        if not picked or (index - picked[-1]) * seconds >= minimum_gap:
            picked.append(int(index))
        elif envelope[index] > envelope[picked[-1]]:
            picked[-1] = int(index)

    frames = np.array(picked, dtype=np.int64)
    times: Array = frames.astype(np.float64) * seconds
    return Onsets(times, envelope[frames] if len(frames) else np.zeros(0), envelope, seconds)


# ---------------------------------------------------------------------------------------------------
# Notes from a sung or hummed take


def midi_of(hz: float) -> float:
    return 69.0 + 12.0 * math.log2(hz / 440.0)


def levels_db(x: Array, rate: int, times: Array) -> Array:
    """Loudness (RMS in dB) over 20 ms around each time."""
    half = max(1, round(rate * 0.01))
    squares = np.concatenate([[0.0], np.cumsum(x**2)])
    centres = np.round(times * rate).astype(np.int64)
    first, last = np.clip(centres - half, 0, len(x)), np.clip(centres + half, 0, len(x))
    mean = (squares[last] - squares[first]) / np.maximum(last - first, 1)
    db: Array = 10.0 * np.log10(mean + 1e-10)
    return db


def segment_notes(
    track: PitchTrack, onsets: Onsets, levels: Array, minimum_length: float = 0.07
) -> list[dict[str, Any]]:
    """Notes (seconds and MIDI pitch, fractional) from the voiced stretches of a pitch track."""
    if len(track.times) < 2:
        return []
    seconds = float(track.times[1] - track.times[0])
    voiced = track.hz > 0
    pitch = np.where(voiced, 69.0 + 12.0 * np.log2(np.where(voiced, track.hz, 440.0) / 440.0), np.nan)

    # A new attack on the same breath ("da-da"): an onset where the level jumps up by 6 dB or more.
    lookback = max(1, round(0.06 / seconds))
    onset_frames = set()
    for t in onsets.times:
        frame = round(t / seconds)
        near = range(max(0, frame - 2), min(len(levels), frame + 3))
        if any(levels[f] - levels[max(0, f - lookback) : f + 1].min() >= 6.0 for f in near):
            onset_frames.add(frame)

    notes: list[dict[str, Any]] = []
    i = 0
    while i < len(pitch):
        if not voiced[i]:
            i += 1
            continue
        start = i
        reference = pitch[i]
        j = i + 1
        while j < len(pitch) and voiced[j]:
            # Another note: the pitch settles a semitone-ish away, or a new onset after a while.
            ahead = pitch[j : j + max(1, round(0.04 / seconds))]
            moved = np.all(np.abs(ahead[~np.isnan(ahead)] - reference) > 0.7) if np.any(~np.isnan(ahead)) else False
            reattacked = j in onset_frames and (j - start) * seconds > 0.08
            if moved or reattacked:
                break
            if (j - start) * seconds < 0.06:  # the start of a note may glide: follow it
                reference = float(np.nanmedian(pitch[start : j + 1]))
            j += 1

        length = (j - start) * seconds
        if length >= minimum_length:
            # The middle of the note: glides in and out do not count.
            body = pitch[start:j]
            trim = len(body) // 5
            middle = body[trim : len(body) - trim] if len(body) - 2 * trim >= 3 else body
            notes.append(
                {
                    "start": round(float(track.times[start]), 4),
                    "end": round(float(track.times[j - 1]) + seconds, 4),
                    "pitch": round(float(np.nanmedian(middle)), 3),
                    "confidence": round(float(np.mean(track.voiced_probability[start:j])), 3),
                }
            )
        i = j
    return notes


# ---------------------------------------------------------------------------------------------------
# Beatbox sounds


FEATURES = ("centre", "low", "high", "flatness", "crossings", "decay")


def describe_hit(x: Array, rate: int, start: float, end: float) -> list[float]:
    """A hit's sound in a few numbers: brightness, low and high energy, noisiness, crossings, decay."""
    first = int(start * rate)
    last = min(len(x), int(min(end, start + 0.12) * rate))
    segment = x[first:last]
    if len(segment) < 64:
        segment = np.pad(segment, (0, 64 - len(segment)))
    size = 1 << math.ceil(math.log2(len(segment)))
    power = np.abs(np.fft.rfft(segment * np.hanning(len(segment)), size)) ** 2 + 1e-12
    hz = np.fft.rfftfreq(size, 1.0 / rate)
    total = power.sum()
    centre = float((hz * power).sum() / total)
    low = float(power[hz < 200.0].sum() / total)
    high = float(power[hz > 4000.0].sum() / total)
    flatness = float(np.exp(np.mean(np.log(power))) / np.mean(power))
    crossings = float(np.mean(np.abs(np.diff(np.sign(segment))) > 0))
    half = len(segment) // 2
    early, late = np.sqrt(np.mean(segment[:half] ** 2) + 1e-12), np.sqrt(np.mean(segment[half:] ** 2) + 1e-12)
    decay = float(20.0 * math.log10(late / early))
    return [math.log2(max(centre, 20.0)), low, high, flatness, crossings, decay]


def classify_rough(features: list[float]) -> tuple[str, float]:
    """Before any sounds are taught: low and dark is a kick, bright and noisy a hi-hat, the rest a snare."""
    centre, low, high = 2.0 ** features[0], features[1], features[2]
    if low > 0.3 or centre < 400.0:
        return "kick", 0.5
    if high > 0.4 or centre > 5000.0:
        return "hat", 0.5
    return "snare", 0.4


def classify(features: list[float], taught: dict[str, list[list[float]]]) -> tuple[str, float]:
    """The taught sound nearest to these features (each feature scaled by how much it varies)."""
    groups = {sound: np.array(examples) for sound, examples in taught.items() if examples}
    if len(groups) < 2:
        return classify_rough(features)

    everything = np.concatenate(list(groups.values()))
    spread = everything.std(axis=0) + 1e-6
    point = np.array(features)
    distances = {
        sound: float(np.min(np.linalg.norm((group - point) / spread, axis=1))) for sound, group in groups.items()
    }
    ranked = sorted(distances.items(), key=lambda item: item[1])
    best, second = ranked[0], ranked[1] if len(ranked) > 1 else (ranked[0][0], ranked[0][1] * 2 + 1)
    confidence = 1.0 - best[1] / (best[1] + second[1] + 1e-9)
    return best[0], round(confidence, 3)


# ---------------------------------------------------------------------------------------------------
# Tempo


def estimate_tempo(onsets: Onsets, low: float = 60.0, high: float = 180.0) -> dict[str, float]:
    """Beats per minute from how the onset strength repeats; 0 when there is too little to tell."""
    envelope = onsets.envelope - onsets.envelope.mean()
    if len(onsets.times) < 4 or len(envelope) < 8:
        return {"bpm": 0.0, "confidence": 0.0}
    size = 1 << math.ceil(math.log2(2 * len(envelope)))
    spectrum = np.fft.rfft(envelope, size)
    autocorrelation = np.fft.irfft(np.abs(spectrum) ** 2, size)[: len(envelope)]
    lags = np.arange(len(autocorrelation))
    with np.errstate(divide="ignore"):
        bpm = np.where(lags > 0, 60.0 / (lags * onsets.frame_seconds + 1e-12), 0.0)
    usable = (bpm >= low) & (bpm <= high)
    if not np.any(usable):
        return {"bpm": 0.0, "confidence": 0.0}
    # A gentle preference for tempos near 110, as people tend to hum.
    weight = np.exp(-0.5 * (np.log2(np.where(usable, bpm, 110.0) / 110.0) / 0.9) ** 2)
    scores = np.where(usable, autocorrelation * weight, -np.inf)
    best = int(np.argmax(scores))
    confidence = float(autocorrelation[best] / autocorrelation[0]) if autocorrelation[0] > 0 else 0.0
    return {"bpm": round(float(bpm[best]), 2), "confidence": round(max(0.0, confidence), 3)}


# ---------------------------------------------------------------------------------------------------
# What the service returns


def analyse_melody(path: Path) -> dict[str, Any]:
    x, rate = read_wav(path)
    onsets = detect_onsets(x, rate)
    track = pyin(x, rate)
    return {
        "duration": round(len(x) / rate, 4),
        "notes": segment_notes(track, onsets, levels_db(x, rate, track.times)),
        "tempo": estimate_tempo(onsets),
    }


def analyse_vocal(path: Path) -> dict[str, Any]:
    """For Spinus Tune's editor: the pitch as sung (every 5 ms) and the notes in it."""
    x, rate = read_wav(path)
    onsets = detect_onsets(x, rate)
    track = pyin(x, rate)
    return {
        "duration": round(len(x) / rate, 4),
        "track": track.to_json(),
        "notes": segment_notes(track, onsets, levels_db(x, rate, track.times)),
    }


def analyse_beats(path: Path, taught: dict[str, list[list[float]]]) -> dict[str, Any]:
    x, rate = read_wav(path)
    onsets = detect_onsets(x, rate)
    ends = [*onsets.times[1:], len(x) / rate][: len(onsets.times)]
    hits = []
    for time, strength, end in zip(onsets.times, onsets.strengths, ends, strict=True):
        sound, confidence = classify(describe_hit(x, rate, float(time), float(end)), taught)
        hits.append(
            {
                "time": round(float(time), 4),
                "sound": sound,
                "confidence": confidence,
                "strength": round(float(strength), 3),
            }
        )
    return {
        "duration": round(len(x) / rate, 4),
        "hits": hits,
        "tempo": estimate_tempo(onsets),
        "taught": sum(1 for examples in taught.values() if examples) >= 2,
    }


def learn_sound(path: Path) -> list[list[float]]:
    """The features of every hit in a take of one sound repeated (for teaching it)."""
    x, rate = read_wav(path)
    onsets = detect_onsets(x, rate)
    ends = [*onsets.times[1:], len(x) / rate][: len(onsets.times)]
    return [
        [round(v, 5) for v in describe_hit(x, rate, float(t), float(e))]
        for t, e in zip(onsets.times, ends, strict=True)
    ]
