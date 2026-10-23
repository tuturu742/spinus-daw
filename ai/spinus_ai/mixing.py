"""Mix analysis: how loud, how peaky, which tone and how wide each insert is, and which inserts
cover each other up (masking), from recordings of each insert that Spinus makes.

Loudness follows ITU-R BS.1770-4: K-weighting, 400 ms blocks with 75 % overlap, an absolute gate at
-70 LUFS and a relative gate 10 LU below. It is written here from the standard with NumPy only: each
block's spectrum is weighted by the K filter's response (the standard's two biquads, evaluated on the unit
circle), and Parseval gives the block's mean square, which is within about 0.1 dB of filtering in time.
A 997 Hz sine at 0 dBFS in one channel reads -3.01 LUFS, as the standard requires. True peak uses 4x
oversampling.
"""

from __future__ import annotations

import math
from typing import Any

import numpy as np

# Bands for tone and masking: (name, low Hz, high Hz).
BANDS = (
    ("sub", 20.0, 60.0),
    ("bass", 60.0, 250.0),
    ("low mids", 250.0, 500.0),
    ("mids", 500.0, 2000.0),
    ("high mids", 2000.0, 4000.0),
    ("presence", 4000.0, 6000.0),
    ("air", 6000.0, 20000.0),
)
SILENT = -70.0  # LUFS: BS.1770's absolute gate; quieter than this counts as silence


def k_weighting_power(freqs: np.ndarray, rate: int) -> np.ndarray:
    """|H(f)|² of BS.1770's K-weighting (a high shelf of about +4 dB above 1.7 kHz for the head, then a high
    pass below about 38 Hz), from the standard's analogue parameters made into biquads for this rate."""
    z = np.exp(-2j * np.pi * freqs / rate)  # z^-1 on the unit circle

    def response(b: tuple[float, float, float], a: tuple[float, float, float]) -> np.ndarray:
        result: np.ndarray = (b[0] + b[1] * z + b[2] * z * z) / (a[0] + a[1] * z + a[2] * z * z)
        return result

    # Stage 1, the shelf.
    f0, gain, q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    k = math.tan(math.pi * f0 / rate)
    vh = 10 ** (gain / 20)
    vb = vh**0.4996667741545416
    a0 = 1 + k / q + k * k
    shelf = response(((vh + vb * k / q + k * k) / a0, 2 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0),
                     (1.0, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0))  # fmt: skip
    # Stage 2, the high pass.
    f0, q = 38.13547087602444, 0.5003270373238773
    k = math.tan(math.pi * f0 / rate)
    a0 = 1 + k / q + k * k
    high_pass = response((1.0, -2.0, 1.0), (1.0, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0))
    power: np.ndarray = np.abs(shelf * high_pass) ** 2
    return power


def block_powers(audio: np.ndarray, rate: int) -> np.ndarray:
    """K-weighted mean square of each 400 ms block (100 ms apart), summed over the channels: blocks."""
    size, hop = int(0.4 * rate), int(0.1 * rate)
    if audio.shape[-1] < size:
        audio = np.pad(audio, ((0, 0), (0, size - audio.shape[-1])))
    count = (audio.shape[-1] - size) // hop + 1
    weight = k_weighting_power(np.fft.rfftfreq(size, 1 / rate), rate)
    weight[1 : -1 if size % 2 == 0 else None] *= 2  # one-sided spectrum: the other half counts too
    total = np.zeros(count)
    for channel in audio:
        frames = np.lib.stride_tricks.sliding_window_view(channel, size)[::hop][:count]
        spectra = np.abs(np.fft.rfft(frames, axis=1)) ** 2
        total += (spectra * weight).sum(axis=1) / (size * size)
    return total


def integrated_loudness(audio: np.ndarray, rate: int) -> float:
    """BS.1770-4 integrated loudness of [channels, samples] audio (LUFS), or SILENT."""
    powers = block_powers(audio, rate)
    loudness = -0.691 + 10 * np.log10(powers + 1e-20)
    gated = powers[loudness > SILENT]
    if len(gated) == 0:
        return SILENT
    relative = -0.691 + 10 * math.log10(float(gated.mean())) - 10.0
    gated = powers[(loudness > SILENT) & (loudness > relative)]
    if len(gated) == 0:
        return SILENT
    return round(-0.691 + 10 * math.log10(float(gated.mean())), 2)


def true_peak(audio: np.ndarray) -> float:
    """The highest level between the samples too (4x oversampled with a windowed sinc), dBTP."""
    taps = 48
    n = np.arange(-taps // 2, taps // 2)
    peak = float(np.max(np.abs(audio))) if audio.size else 0.0
    for phase in (0.25, 0.5, 0.75):
        kernel = np.sinc(n + phase) * np.hanning(len(n))
        for channel in audio:
            peak = max(peak, float(np.max(np.abs(np.convolve(channel, kernel, mode="same")))))
    return round(20 * math.log10(peak), 2) if peak > 0 else -120.0


def band_levels(audio: np.ndarray, rate: int, hop_seconds: float = 0.1) -> tuple[np.ndarray, np.ndarray]:
    """Energy in each band over time ([bands, frames], linear) and on average (dB), from 4096-sample
    frames of the mono sum."""
    mono = audio.mean(axis=0)
    size, hop = 4096, max(1, int(hop_seconds * rate))
    if len(mono) < size:
        mono = np.pad(mono, (0, size - len(mono)))
    frames = np.lib.stride_tricks.sliding_window_view(mono, size)[::hop]
    spectra = np.abs(np.fft.rfft(frames * np.hanning(size), axis=1)) ** 2
    freqs = np.fft.rfftfreq(size, 1 / rate)
    over_time = np.array([spectra[:, (freqs >= lo) & (freqs < hi)].sum(axis=1) for _, lo, hi in BANDS])
    average = 10 * np.log10(over_time.mean(axis=1) + 1e-20)
    return over_time, average


def width(audio: np.ndarray) -> float:
    """How wide: the side's share of the energy, 0 (mono) to 1 (all side); 0.5 for two unrelated sides."""
    if audio.shape[0] < 2:
        return 0.0
    mid, side = (audio[0] + audio[1]) / 2, (audio[0] - audio[1]) / 2
    energy = float(np.sum(mid * mid) + np.sum(side * side))
    return round(float(np.sum(side * side)) / energy, 3) if energy > 0 else 0.0


def analyze_insert(audio: np.ndarray, rate: int) -> dict[str, Any]:
    """{loudness (LUFS), truePeak (dBTP), crest (dB: peak above the loudness), bands {name: dB relative to
    the loudest band}, width}."""
    loudness = integrated_loudness(audio, rate)
    peak = true_peak(audio)
    _, average = band_levels(audio, rate)
    relative = average - average.max()
    return {
        "loudness": loudness,
        "truePeak": peak,
        "crest": round(peak - loudness, 2) if loudness > SILENT else 0.0,
        "bands": {name: round(float(v), 1) for (name, _, _), v in zip(BANDS, relative, strict=True)},
        "width": width(audio),
    }


def masking(levels: dict[str, np.ndarray], threshold: float = 0.4) -> list[dict[str, Any]]:
    """Which inserts cover each other up, and where: for each pair and band where both put a fair share of
    their energy (within 12 dB of their loudest band), how much of the time they sound there at once at
    similar levels (0 to 1). Pairs above `threshold`, worst first: [{a, b, band, amount}]."""
    names = sorted(levels)
    found = []
    for i, a in enumerate(names):
        for b in names[i + 1 :]:
            la, lb = levels[a], levels[b]
            frames = min(la.shape[1], lb.shape[1])
            for k, (band, _, _) in enumerate(BANDS):
                share_a = 10 * np.log10(la[k].mean() + 1e-20) - 10 * np.log10(la.mean(axis=1).max() + 1e-20)
                share_b = 10 * np.log10(lb[k].mean() + 1e-20) - 10 * np.log10(lb.mean(axis=1).max() + 1e-20)
                if share_a < -12 or share_b < -12:
                    continue
                x, y = la[k, :frames], lb[k, :frames]
                both = (x > x.max() * 1e-3) & (y > y.max() * 1e-3)
                if not both.any():
                    continue
                overlap = float(np.minimum(x[both], y[both]).sum() / np.maximum(x[both], y[both]).sum())
                amount = overlap * both.mean()
                if amount >= threshold:
                    found.append({"a": a, "b": b, "band": band, "amount": round(float(amount), 2)})
    return sorted(found, key=lambda m: -m["amount"])


def analyze_mix(inserts: dict[str, tuple[np.ndarray, int]]) -> dict[str, Any]:
    """Every insert's measurements and the masking between them: {inserts: {name: {...}}, masking: [...]}."""
    result: dict[str, Any] = {"inserts": {}, "masking": []}
    levels = {}
    for name, (audio, rate) in inserts.items():
        result["inserts"][name] = analyze_insert(audio, rate)
        if result["inserts"][name]["loudness"] > SILENT:
            levels[name] = band_levels(audio, rate)[0]
    result["masking"] = masking(levels)
    return result
