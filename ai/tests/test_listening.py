"""Listening: pitch, notes, onsets, beatbox sounds and tempo on made-up takes."""

from __future__ import annotations

import math
import wave
from collections.abc import Callable
from pathlib import Path

import numpy as np
import pytest

from spinus_ai.listening import (
    analyse_beats,
    analyse_melody,
    detect_onsets,
    estimate_tempo,
    learn_sound,
    pyin,
    read_wav,
)

RATE = 48000


def write_wav(path: Path, x: np.ndarray, rate: int = RATE, width: int = 3, channels: int = 1) -> Path:
    data = np.clip(x, -1.0, 1.0)
    if channels > 1:
        data = np.repeat(data[:, None], channels, axis=1).reshape(-1)
    scale = float((1 << (8 * width - 1)) - 1)
    ints = np.round(data * scale).astype(np.int64)
    if width == 3:
        raw = np.stack([ints & 0xFF, (ints >> 8) & 0xFF, (ints >> 16) & 0xFF], axis=1).astype(np.uint8).tobytes()
    else:
        raw = ints.astype({2: "<i2", 4: "<i4"}[width]).tobytes()
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(width)
        w.setframerate(rate)
        w.writeframes(raw)
    return path


def voice(seconds: float, hz: Callable[[np.ndarray], np.ndarray] | float, level: float = 0.4) -> np.ndarray:
    """A voice-like tone (a band-limited sawtooth with a soft start and end)."""
    t = np.arange(int(seconds * RATE)) / RATE
    f = hz(t) if callable(hz) else np.full_like(t, hz)
    phase = np.cumsum(f) / RATE
    x = np.zeros_like(t)
    for h in range(1, 16):
        x += np.where(h * f < RATE * 0.45, np.sin(2 * np.pi * h * phase) / h, 0.0)
    fade = np.minimum(1.0, np.minimum(t, seconds - t) / 0.01)
    return level * 0.5 * x * fade


def midi_hz(note: float) -> float:
    return float(440.0 * 2.0 ** ((note - 69.0) / 12.0))


def silence(seconds: float) -> np.ndarray:
    return np.zeros(int(seconds * RATE))


# ---------------------------------------------------------------------------------------------------


def test_wav_files_of_every_sample_size_and_stereo_are_read(tmp_path: Path) -> None:
    x = 0.5 * np.sin(2 * np.pi * 440.0 * np.arange(4800) / RATE)
    for width in (2, 3, 4):
        for channels in (1, 2):
            data, rate = read_wav(write_wav(tmp_path / f"{width}-{channels}.wav", x, width=width, channels=channels))
            assert rate == RATE
            assert np.max(np.abs(data - x)) < 1e-3


def test_pyin_follows_a_voice_and_finds_no_pitch_in_silence_or_noise() -> None:
    for note in (45, 57, 69, 76):  # A2 to E5
        track = pyin(voice(0.6, midi_hz(note)), RATE)
        voiced = track.hz[track.hz > 0]
        assert len(voiced) > 0.8 * len(track.hz)
        assert abs(12 * math.log2(float(np.median(voiced)) / midi_hz(note))) < 0.1

    rng = np.random.default_rng(1)
    assert np.mean(pyin(silence(0.5), RATE).hz > 0) < 0.05
    assert np.mean(pyin(0.3 * rng.standard_normal(RATE // 2), RATE).hz > 0) < 0.2


def test_a_hummed_melody_becomes_notes(tmp_path: Path) -> None:
    # C4 E4 G4 E4, each 0.4 s with a short breath between, then a held C5 with vibrato.
    parts, expected = [silence(0.2)], []
    time = 0.2
    for note in (60, 64, 67, 64):
        parts += [voice(0.4, midi_hz(note)), silence(0.06)]
        expected.append((time, note))
        time += 0.46

    def vibrato(t: np.ndarray) -> np.ndarray:
        return midi_hz(72) * 2.0 ** (0.4 / 12.0 * np.sin(2 * np.pi * 5.5 * t))

    parts += [voice(1.0, vibrato), silence(0.2)]
    expected.append((time, 72))

    result = analyse_melody(write_wav(tmp_path / "melody.wav", np.concatenate(parts)))
    notes = result["notes"]
    assert [round(n["pitch"]) for n in notes] == [n for _, n in expected]
    for note, (start, _) in zip(notes, expected, strict=True):
        assert abs(note["start"] - start) < 0.03
    assert abs(notes[-1]["end"] - notes[-1]["start"] - 1.0) < 0.06  # one held note, not many


def test_legato_notes_are_split_where_the_pitch_moves(tmp_path: Path) -> None:
    # A glide-free step from A3 to C4 to A3 without a gap.
    def steps(t: np.ndarray) -> np.ndarray:
        return np.where(t < 0.5, midi_hz(57), np.where(t < 1.0, midi_hz(60), midi_hz(57)))

    notes = analyse_melody(write_wav(tmp_path / "legato.wav", voice(1.5, steps)))["notes"]
    assert [round(n["pitch"]) for n in notes] == [57, 60, 57]


def clicks(times: list[float], seconds: float) -> np.ndarray:
    x = silence(seconds)
    burst = np.exp(-np.arange(480) / 60.0) * np.sin(2 * np.pi * 1000.0 * np.arange(480) / RATE)
    for t in times:
        start = int(t * RATE)
        x[start : start + len(burst)] += 0.6 * burst
    return x


def test_onsets_are_found_where_sounds_start_and_the_tempo_from_them() -> None:
    times = [0.25 + 0.5 * i for i in range(12)]  # 120 BPM
    onsets = detect_onsets(clicks(times, 6.5), RATE)
    assert len(onsets.times) == len(times)
    assert np.max(np.abs(onsets.times - np.array(times))) < 0.015
    assert abs(estimate_tempo(onsets)["bpm"] - 120.0) < 2.0

    # Faster than 0.05 s apart counts as one.
    assert len(detect_onsets(clicks([0.5, 0.52], 1.0), RATE).times) == 1


# ---------------------------------------------------------------------------------------------------
# Beatbox


def kick(rng: np.random.Generator) -> np.ndarray:
    t = np.arange(int(0.18 * RATE)) / RATE
    f = 55.0 + 90.0 * np.exp(-t / 0.02)
    body = np.sin(2 * np.pi * np.cumsum(f) / RATE) * np.exp(-t / 0.06)
    return 0.8 * body + 0.05 * rng.standard_normal(len(t)) * np.exp(-t / 0.005)


def snare(rng: np.random.Generator) -> np.ndarray:
    t = np.arange(int(0.15 * RATE)) / RATE
    noise = np.convolve(rng.standard_normal(len(t)), np.ones(4) / 4, mode="same") * np.exp(-t / 0.05)
    return 0.4 * noise + 0.3 * np.sin(2 * np.pi * 220.0 * t) * np.exp(-t / 0.03)


def hat(rng: np.random.Generator) -> np.ndarray:
    t = np.arange(int(0.05 * RATE)) / RATE
    noise = np.diff(rng.standard_normal(len(t) + 1))  # bright
    return 0.3 * noise * np.exp(-t / 0.012)


SOUND_MAKERS = {"kick": kick, "snare": snare, "hat": hat}


def sequence(sounds: list[str], every: float, rng: np.random.Generator) -> np.ndarray:
    x = silence(0.3 + every * len(sounds))
    for i, sound in enumerate(sounds):
        hit = SOUND_MAKERS[sound](rng) * rng.uniform(0.7, 1.0)
        hit *= np.minimum(1.0, np.arange(len(hit))[::-1] / (0.01 * RATE))  # fades out, as voices do
        start = int((0.2 + i * every) * RATE)
        x[start : start + len(hit)] += hit
    return x


def test_beatbox_hits_are_told_apart_after_teaching_and_roughly_before(tmp_path: Path) -> None:
    rng = np.random.default_rng(5)
    taught = {
        sound: learn_sound(write_wav(tmp_path / f"{sound}.wav", sequence([sound] * 5, 0.4, rng)))
        for sound in SOUND_MAKERS
    }
    assert all(len(examples) == 5 for examples in taught.values())

    beat = ["kick", "hat", "snare", "hat", "kick", "kick", "snare", "hat"] * 2
    take = write_wav(tmp_path / "beat.wav", sequence(beat, 0.25, rng))

    result = analyse_beats(take, taught)
    assert result["taught"]
    assert [h["sound"] for h in result["hits"]] == beat
    assert abs(result["hits"][2]["time"] - (0.2 + 2 * 0.25)) < 0.015

    rough = analyse_beats(take, {})
    assert not rough["taught"]
    correct = sum(h["sound"] == s for h, s in zip(rough["hits"], beat, strict=True))
    assert correct >= 0.8 * len(beat)


@pytest.mark.parametrize("seconds", [0.0, 0.01])
def test_tiny_or_empty_takes_give_nothing(tmp_path: Path, seconds: float) -> None:
    take = write_wav(tmp_path / "tiny.wav", silence(seconds))
    assert analyse_melody(take)["notes"] == []
    assert analyse_beats(take, {})["hits"] == []


def test_a_vocal_gives_its_pitch_curve_and_its_notes(tmp_path: Path) -> None:
    from spinus_ai.listening import analyse_vocal

    def wobble(t: np.ndarray) -> np.ndarray:
        return midi_hz(64.3) * 2.0 ** (0.3 / 12.0 * np.sin(2 * np.pi * 5.0 * t))

    take = write_wav(tmp_path / "vocal.wav", np.concatenate([silence(0.2), voice(0.8, wobble), silence(0.2)]))
    result = analyse_vocal(take)
    track = result["track"]
    assert len(track["times"]) == len(track["hz"]) == len(track["voiced"])
    assert track["times"][1] - track["times"][0] == pytest.approx(0.005, abs=1e-4)
    voiced = [hz for hz in track["hz"] if hz > 0]
    assert max(voiced) / min(voiced) > 2.0 ** (0.4 / 12.0)  # the vibrato is in the curve
    [note] = result["notes"]
    assert note["pitch"] == pytest.approx(64.3, abs=0.15)
    assert note["start"] == pytest.approx(0.2, abs=0.03)
