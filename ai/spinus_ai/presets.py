"""Closest preset: an index of how an instrument's presets sound, and the presets nearest a
stem.

Spinus renders each preset playing the same short phrase; the analysis engine turns each recording into a
CLAP vector (spinus_ai.compute.worker embed) and this module adds a few plain measurements (the spectrum's
shape, brightness, how fast it starts). Only these numbers are kept, in one file per instrument; the
recordings are deleted. A stem is measured the same way, from its loudest parts, in one-second windows
(CLAP hears notes rather than phrases this way, which matches across different melodies).

On recordings of 80 Surge XT presets playing a melody unlike the previews, this finds the same preset
first for 38 % of them, among the first five for 55 % and the first twenty for 74 % (CLAP alone: 23, 33
and 59 %). Presets that are not the same but sound alike are what the user is after anyway."""

from __future__ import annotations

import itertools
import json
import os
from pathlib import Path
from typing import Any

import numpy as np

from .samples import sounding_parts

INDEX_VERSION = 2
BANDS = 32
NOTE_STARTS = (0.0, 1.0, 2.0)  # the preview phrase's notes, a second each (Spinus's PresetPreviews::getPhrase)


def describe_sound(mono: np.ndarray, rate: int) -> list[float]:
    """Plain measurements of a sound: its spectrum's shape in 32 bands from 40 Hz to 16 kHz (dB, relative to
    the loudest band, floored at -60), brightness (the spectral centroid, in octaves above 40 Hz) and its
    attack (log10 of the seconds to reach 90 % of the loudest 10 ms)."""
    mono = np.asarray(mono, dtype=float)
    size = 4096
    if len(mono) < size:
        mono = np.pad(mono, (0, size - len(mono)))
    frames = (len(mono) - size) // (size // 2) + 1
    window = np.hanning(size)
    power = np.zeros(size // 2 + 1)
    for i in range(frames):
        power += np.abs(np.fft.rfft(mono[i * size // 2 : i * size // 2 + size] * window)) ** 2
    freqs = np.fft.rfftfreq(size, 1 / rate)
    edges = 40.0 * (16000.0 / 40.0) ** (np.arange(BANDS + 1) / BANDS)
    bands = np.array([power[(freqs >= a) & (freqs < b)].sum() for a, b in itertools.pairwise(edges)])
    db = 10 * np.log10(bands + 1e-12)
    shape = np.maximum(db - db.max(), -60.0)
    weights = power[freqs >= 40.0]
    octaves = np.log2(freqs[freqs >= 40.0] / 40.0)
    brightness = float((weights * octaves).sum() / max(weights.sum(), 1e-12))
    hop = max(1, rate // 100)
    count = len(mono) // hop
    level = np.sqrt(np.mean(mono[: count * hop].reshape(count, hop) ** 2, axis=1)) if count else np.zeros(1)
    attack = float(np.argmax(level >= 0.9 * level.max()) * hop / rate) if level.max() > 0 else 0.0
    return [round(float(v), 3) for v in shape] + [round(brightness, 3), round(float(np.log10(attack + 0.005)), 3)]


def stem_clips(audio: np.ndarray, rate: int, most: int = 4, longest: float = 6.0) -> list[tuple[float, float]]:
    """The parts of a stem to compare with presets: its loudest sounding parts (each at most `longest`
    seconds, from where it starts), in time order."""
    mono = audio.mean(axis=0) if audio.ndim == 2 else audio
    parts = sounding_parts(mono, rate)
    if not parts:
        return [(0.0, min(longest, len(mono) / rate))]

    def loudness(part: tuple[float, float]) -> float:
        a, b = int(part[0] * rate), int(min(part[1], part[0] + longest) * rate)
        return float(np.sqrt(np.mean(mono[a:b] ** 2))) * min(part[1] - part[0], longest)

    chosen = sorted(parts, key=loudness, reverse=True)[:most]
    return sorted((a, min(b, a + longest)) for a, b in chosen)


def windows(clips: list[tuple[float, float]], length: float = 1.0, most: int = 12) -> list[tuple[float, float]]:
    """Clips cut into windows of `length` seconds (a shorter last piece only if it is at least half as long),
    at most `most` of them."""
    result: list[tuple[float, float]] = []
    for start, end in clips:
        t = start
        while t + length / 2 <= end and len(result) < most:
            result.append((t, min(t + length, end)))
            t += length
    if not result and clips:
        result.append(clips[0])
    return result


class PresetIndex:
    """How one instrument's presets sound: {id: {version, notes (a CLAP vector per note of the phrase),
    features}}, kept in a JSON file."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.entries: dict[str, dict[str, Any]] = {}
        if path.exists():
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
                if data.get("version") == INDEX_VERSION:
                    self.entries = dict(data.get("presets", {}))
            except (OSError, ValueError):
                self.entries = {}

    def missing(self, presets: list[dict[str, Any]]) -> list[str]:
        """The presets ({id, version}) not measured yet, or changed since."""
        return [str(p["id"]) for p in presets if self.entries.get(str(p["id"]), {}).get("version") != p.get("version")]

    def add(self, preset_id: str, version: str, notes: list[list[float]], features: list[float]) -> None:
        self.entries[preset_id] = {"version": version, "notes": notes, "features": features}

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.path.with_suffix(".tmp")
        temporary.write_text(json.dumps({"version": INDEX_VERSION, "presets": self.entries}), encoding="utf-8")
        os.replace(temporary, self.path)

    def closest(
        self, windows: list[list[float]], features: list[float], top: int = 5, only: list[str] | None = None
    ) -> list[dict[str, Any]]:
        """The presets that sound most like a stem (CLAP vectors of its one-second windows, and its
        measurements): [{id, score}], best first. Each window is matched with the preset's closest note
        (vectors relative to the presets' average sound); that ranking and the one by measurements count
        half each. The score is the CLAP part (-1 to 1)."""
        ids = [i for i in self.entries if only is None or i in set(only)]
        if not ids or not windows:
            return []
        notes = np.array([self.entries[i]["notes"] for i in ids], dtype=float)  # presets x notes x 512
        measured = np.array([self.entries[i]["features"] for i in ids], dtype=float)
        average = notes.reshape(-1, notes.shape[-1]).mean(axis=0)

        def unit(x: np.ndarray) -> np.ndarray:
            x = x - average
            result: np.ndarray = x / np.maximum(np.linalg.norm(x, axis=-1, keepdims=True), 1e-9)
            return result

        similarity = np.einsum("pkd,qd->pqk", unit(notes), unit(np.asarray(windows, dtype=float)))
        by_clap = similarity.max(axis=2).mean(axis=1)
        spread = np.maximum(measured.std(axis=0), 1e-6)
        distance = np.linalg.norm((measured - np.asarray(features)) / spread, axis=1)
        place = np.argsort(np.argsort(-by_clap)) + np.argsort(np.argsort(distance))
        order = np.lexsort((-by_clap, place))
        return [{"id": ids[i], "score": round(float(by_clap[i]), 3)} for i in order[:top]]
