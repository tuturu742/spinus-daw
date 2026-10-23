"""Notes from a melodic stem with Basic Pitch, on ONNX Runtime and NumPy.

The windowing, the model's outputs and the note decoding below follow Basic Pitch 0.4.0
(https://github.com/spotify/basic-pitch, basic_pitch/inference.py and note_creation.py),
Copyright 2022 Spotify AB, licensed under the Apache License, Version 2.0
(http://www.apache.org/licenses/LICENSE-2.0). Changes for Spinus: no TensorFlow, librosa, SciPy or
pretty_midi (resampling, frame times and peak picking are done here with NumPy), no pitch bends, notes
returned as plain tuples, and the result is placed on the song's beats.

The model file (nmp.onnx, 230 KB) is downloaded on request, pinned by SHA-256
(spinus_ai/compute/weights.py).
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

import numpy as np

# basic_pitch/constants.py
FFT_HOP = 256
AUDIO_SAMPLE_RATE = 22050
AUDIO_WINDOW_LENGTH = 2
ANNOTATIONS_FPS = AUDIO_SAMPLE_RATE // FFT_HOP
ANNOT_N_FRAMES = ANNOTATIONS_FPS * AUDIO_WINDOW_LENGTH
AUDIO_N_SAMPLES = AUDIO_SAMPLE_RATE * AUDIO_WINDOW_LENGTH - FFT_HOP
MIDI_OFFSET = 21
MAX_FREQ_IDX = 87
N_OVERLAPPING_FRAMES = 30

# basic_pitch/inference.py predict() defaults
ONSET_THRESHOLD = 0.5
FRAME_THRESHOLD = 0.3
MIN_NOTE_FRAMES = int(np.round(127.70 / 1000 * (AUDIO_SAMPLE_RATE / FFT_HOP)))  # 11
ENERGY_TOL = 11


def resample(audio: np.ndarray, rate: int, target: int = AUDIO_SAMPLE_RATE) -> np.ndarray:
    """Mono audio at `target` Hz: a windowed-sinc low-pass below the new Nyquist, then interpolation."""
    if rate == target:
        same: np.ndarray = audio.astype(np.float32)
        return same
    if rate > target:
        cutoff = 0.45 * target / rate  # of the old sample rate
        taps = 63
        n = np.arange(taps) - (taps - 1) / 2
        kernel = 2 * cutoff * np.sinc(2 * cutoff * n) * np.hamming(taps)
        audio = np.convolve(audio, kernel / kernel.sum(), mode="same")
    length = int(np.floor(len(audio) * target / rate))
    positions = np.arange(length) * (rate / target)
    result: np.ndarray = np.interp(positions, np.arange(len(audio)), audio).astype(np.float32)
    return result


class Model:
    def __init__(self, path: Path) -> None:
        import onnxruntime as ort

        self.session = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])

    def predict(self, windows: np.ndarray) -> dict[str, np.ndarray]:
        outputs = self.session.run(
            ["StatefulPartitionedCall:1", "StatefulPartitionedCall:2", "StatefulPartitionedCall:0"],
            {"serving_default_input_2:0": windows},
        )
        return dict(zip(["note", "onset", "contour"], outputs, strict=True))


def run_inference(model: Model, audio: np.ndarray) -> dict[str, np.ndarray]:
    """The model over overlapping 2-second windows, unwrapped into one matrix per output (frames x bins)."""
    overlap_len = N_OVERLAPPING_FRAMES * FFT_HOP
    hop_size = AUDIO_N_SAMPLES - overlap_len
    original_length = audio.shape[0]
    padded = np.concatenate([np.zeros(overlap_len // 2, dtype=np.float32), audio])
    windows = []
    for i in range(0, padded.shape[0], hop_size):
        window = padded[i : i + AUDIO_N_SAMPLES]
        if len(window) < AUDIO_N_SAMPLES:
            window = np.pad(window, (0, AUDIO_N_SAMPLES - len(window)))
        windows.append(window)
    output: dict[str, list[np.ndarray]] = {"note": [], "onset": [], "contour": []}
    for start in range(0, len(windows), 16):  # a batch at a time
        batch = np.stack(windows[start : start + 16])[:, :, None].astype(np.float32)
        for key, value in model.predict(batch).items():
            output[key].append(value)

    n_olap = N_OVERLAPPING_FRAMES // 2
    frames = int(np.floor(original_length * (ANNOTATIONS_FPS / AUDIO_SAMPLE_RATE)))
    unwrapped = {}
    for key, parts in output.items():
        joined = np.concatenate(parts)[:, n_olap:-n_olap, :]
        unwrapped[key] = joined.reshape(joined.shape[0] * joined.shape[1], joined.shape[2])[:frames]
    return unwrapped


def frames_to_time(n_frames: int) -> np.ndarray:
    """basic_pitch.note_creation.model_frames_to_time, without librosa."""
    original = np.arange(n_frames) * FFT_HOP / AUDIO_SAMPLE_RATE
    window_numbers = np.floor(np.arange(n_frames) / ANNOT_N_FRAMES)
    window_offset = (FFT_HOP / AUDIO_SAMPLE_RATE) * (ANNOT_N_FRAMES - (AUDIO_N_SAMPLES / FFT_HOP)) + 0.0018
    result: np.ndarray = original - window_offset * window_numbers
    return result


def inferred_onsets(onsets: np.ndarray, frames: np.ndarray, n_diff: int = 2) -> np.ndarray:
    diffs = []
    for n in range(1, n_diff + 1):
        appended = np.concatenate([np.zeros((n, frames.shape[1])), frames])
        diffs.append(appended[n:, :] - appended[:-n, :])
    frame_diff = np.min(diffs, axis=0)
    frame_diff[frame_diff < 0] = 0
    frame_diff[:n_diff, :] = 0
    if np.max(frame_diff) > 0:
        frame_diff = np.max(onsets) * frame_diff / np.max(frame_diff)
    result: np.ndarray = np.max([onsets, frame_diff], axis=0)
    return result


def local_maxima(values: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """scipy.signal.argrelmax(values, axis=0): strictly larger than both neighbours in time."""
    inner = (values[1:-1] > values[:-2]) & (values[1:-1] > values[2:])
    rows, cols = np.nonzero(inner)
    return rows + 1, cols


def notes_from_output(frames: np.ndarray, onsets: np.ndarray) -> list[tuple[int, int, int, float]]:
    """basic_pitch.note_creation.output_to_notes_polyphonic with its defaults (inferred onsets, the
    melodia trick): (start frame, end frame, MIDI pitch, amplitude)."""
    n_frames = frames.shape[0]
    onsets = inferred_onsets(onsets, frames)
    peaks = np.zeros(onsets.shape)
    rows, cols = local_maxima(onsets)
    peaks[rows, cols] = onsets[rows, cols]
    onset_time_idx, onset_freq_idx = np.where(peaks >= ONSET_THRESHOLD)
    remaining = frames.copy()
    notes: list[tuple[int, int, int, float]] = []

    def clear_neighbours(start: Any, end: Any, freq: Any) -> None:
        remaining[start:end, freq] = 0
        if freq < MAX_FREQ_IDX:
            remaining[start:end, freq + 1] = 0
        if freq > 0:
            remaining[start:end, freq - 1] = 0

    for start, freq in zip(onset_time_idx[::-1], onset_freq_idx[::-1], strict=True):
        if start >= n_frames - 1:
            continue
        i, k = start + 1, 0
        while i < n_frames - 1 and k < ENERGY_TOL:
            k = k + 1 if remaining[i, freq] < FRAME_THRESHOLD else 0
            i += 1
        i -= k
        if i - start <= MIN_NOTE_FRAMES:
            continue
        clear_neighbours(start, i, freq)
        notes.append((int(start), int(i), int(freq) + MIDI_OFFSET, float(np.mean(frames[start:i, freq]))))

    # The melodia trick: notes without a clear onset, from the strongest energy left.
    while np.max(remaining) > FRAME_THRESHOLD:
        mid, freq = np.unravel_index(np.argmax(remaining), remaining.shape)
        remaining[mid, freq] = 0
        i, k = mid + 1, 0
        while i < n_frames - 1 and k < ENERGY_TOL:
            k = k + 1 if remaining[i, freq] < FRAME_THRESHOLD else 0
            clear_neighbours(i, i + 1, int(freq))
            i += 1
        end = i - 1 - k
        i, k = mid - 1, 0
        while i > 0 and k < ENERGY_TOL:
            k = k + 1 if remaining[i, freq] < FRAME_THRESHOLD else 0
            clear_neighbours(i, i + 1, int(freq))
            i -= 1
        start = i + 1 + k
        if end - start <= MIN_NOTE_FRAMES:
            continue
        notes.append((int(start), int(end), int(freq) + MIDI_OFFSET, float(np.mean(frames[start:end, freq]))))
    return notes


def transcribe(model: Model, audio: np.ndarray, rate: int) -> list[dict[str, Any]]:
    """Notes of a (mono or stereo [channels, samples]) recording: [{start, end (seconds), pitch, velocity}],
    in time order."""
    mono = audio.mean(axis=0) if audio.ndim == 2 else audio
    output = run_inference(model, resample(mono, rate))
    times = frames_to_time(output["note"].shape[0])
    found = notes_from_output(output["note"], output["onset"])
    notes = [
        {"start": float(times[s]), "end": float(times[min(e, len(times) - 1)]), "pitch": p,
         "velocity": round(min(1.0, max(0.1, a)), 3)}
        for s, e, p, a in found
    ]  # fmt: skip
    return sorted(notes, key=lambda n: (n["start"], n["pitch"]))


def place_on_beats(
    notes: list[dict[str, Any]], beats: list[float], first_downbeat: float, grid: int = 240
) -> list[dict[str, Any]]:
    """Notes in ticks (960 a beat) from the first downbeat, following the song's own beats (so a song that
    speeds up or slows down still lands on the grid), snapped to `grid` ticks, at least a grid step long."""
    ticks_per_beat = 960
    if len(beats) >= 2:
        beat_times = np.asarray(beats, dtype=float)
        first = int(np.argmin(np.abs(beat_times - first_downbeat)))
        index = np.arange(len(beat_times)) - first

        # Past the ends: continue at the first and last beat lengths.
        def to_ticks(t: float) -> float:
            if t < beat_times[0]:
                return float((index[0] - (beat_times[0] - t) / (beat_times[1] - beat_times[0])) * ticks_per_beat)
            if t > beat_times[-1]:
                return float((index[-1] + (t - beat_times[-1]) / (beat_times[-1] - beat_times[-2])) * ticks_per_beat)
            return float(np.interp(t, beat_times, index) * ticks_per_beat)
    else:

        def to_ticks(t: float) -> float:
            return (t - first_downbeat) * 2 * ticks_per_beat  # 120 BPM when there is no beat

    placed = []
    for n in notes:
        start = round(to_ticks(n["start"]) / grid) * grid
        end = round(to_ticks(n["end"]) / grid) * grid
        if start < 0:
            continue
        placed.append(
            {"start": start, "length": max(grid, end - start), "pitch": n["pitch"], "velocity": n["velocity"]}
        )
    # One note per pitch and start.
    unique = {(p["start"], p["pitch"]): p for p in placed}
    return sorted(unique.values(), key=lambda p: (p["start"], p["pitch"]))
