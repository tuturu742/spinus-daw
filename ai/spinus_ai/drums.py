"""Drum hits in a drum stem, and what each is (trained on Spinus's own sounds).

The recogniser is a small neural network (NumPy) that tells kick, snare, clap, closed and open hi-hat,
tom, rim, crash and ride apart, several at once. It is trained on this computer the first time it is
needed, in a few seconds, from thousands of drum hits made the way Spinus's Synth Kit makes its sounds
(src/dsp/drums/DrumSynth.cpp: sine sweeps, filtered noise), varied in tuning, length, tone, room and
level, mixed with other instruments' bleed and noise. So it ships no weights and owes nothing to anyone's
recordings. The trained numbers are kept in the AI service's cache.

Then: onsets (spectral flux, from listening.py), each hit's sound described by its spectrum shape and
envelope, the recogniser's guess, toms split into low, mid and high by pitch, and the hits placed on the
song's beats as Drum Machine pads.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import numpy as np

RATE = 22050
KINDS = ("kick", "snare", "clap", "closed_hat", "open_hat", "tom", "rim", "crash", "ride")
VERSION = 3  # change when the training sounds, features or network change

# The Drum Machine's pads (the Spinus Synth Kit's order, from C5 = MIDI 60).
PADS = {
    "kick": 60,
    "snare": 61,
    "clap": 62,
    "closed_hat": 63,
    "open_hat": 64,
    "low_tom": 65,
    "mid_tom": 66,
    "high_tom": 67,
    "rim": 68,
    "crash": 70,
    "ride": 74,
}

# ---------------------------------------------------------------------------------------------------
# Training sounds


def _one_pole_highpass(x: np.ndarray, cutoff: float) -> np.ndarray:
    a = np.exp(-2 * np.pi * cutoff / RATE)
    y = np.empty_like(x)
    previous_x = previous_y = 0.0
    for i, v in enumerate(x):
        previous_y = a * (previous_y + v - previous_x)
        previous_x = v
        y[i] = previous_y
    return y


def _band(x: np.ndarray, low: float, high: float) -> np.ndarray:
    """A band of `x` (FFT masking with soft edges): good enough for training sounds."""
    spectrum = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / RATE)
    mask = 1.0 / (1.0 + (low / np.maximum(f, 1.0)) ** 4) / (1.0 + (f / max(high, 1.0)) ** 4)
    result: np.ndarray = np.fft.irfft(spectrum * mask, len(x))
    return result


def _sweep(t: np.ndarray, start: float, end: float, speed: float) -> np.ndarray:
    frequency = end + (start - end) * np.exp(-t * speed)
    result: np.ndarray = np.sin(2 * np.pi * np.cumsum(frequency) / RATE)
    return result


def make_hit(kind: str, rng: np.random.Generator, seconds: float = 0.4) -> np.ndarray:
    """One drum hit of a kind, its tuning, length and tone picked at random within the kind's range."""
    t = np.arange(int(seconds * RATE)) / RATE
    noise = rng.standard_normal(len(t))
    u = rng.uniform
    if kind == "kick":
        length = u(0.12, 0.7)
        x = _sweep(t, u(90, 230), u(35, 70), u(15, 90)) * np.exp(-t * 4 / length)
        x += noise * u(0, 0.4) * np.exp(-t / 0.003)
    elif kind == "snare":
        x = 0.6 * _sweep(t, u(170, 320), u(140, 220), 40) * np.exp(-t * u(20, 45))
        x += u(0.3, 1.3) * _band(noise, u(150, 600), u(6000, 10000)) * np.exp(-t * u(8, 25))
    elif kind == "clap":
        bursts, spacing = rng.integers(2, 5), u(0.006, 0.016)
        envelope = np.zeros_like(t)
        for b in range(int(bursts)):
            envelope += ((t >= b * spacing) & (t < b * spacing + 0.004)).astype(float)
        tail_start = bursts * spacing
        envelope += np.where(t >= tail_start, 0.7 * np.exp(-(t - tail_start) * u(12, 30)), 0.0)
        centre = u(800, 2500)
        x = _band(noise, centre / 2, centre * 2) * envelope
    elif kind == "closed_hat":
        x = _band(noise, u(5000, 9000), 11000) * np.exp(-t * u(35, 120))
    elif kind == "open_hat":
        x = _band(noise, u(5000, 9000), 11000) * np.exp(-t * u(4, 12))
    elif kind == "tom":
        f = u(60, 260)
        x = _sweep(t, f * u(1.2, 1.6), f, u(10, 25)) * np.exp(-t * u(4, 12))
    elif kind == "rim":
        x = (np.sin(2 * np.pi * u(900, 2600) * t) + u(0.2, 0.8) * noise) * np.exp(-t * u(60, 150))
        x = _band(x, 300, 9000)
    elif kind == "crash":
        x = _band(noise, u(2000, 6000), 11000) * np.exp(-t * u(1.5, 3.5))
        x *= 1 - np.exp(-t / 0.002)
    elif kind == "ride":
        bell = sum(np.sin(2 * np.pi * u(2500, 5000) * t) for _ in range(2))
        x = (u(0.1, 0.4) * bell + u(0.2, 0.5) * _band(noise, 3000, 11000)) * np.exp(-t * u(2, 6))
    else:
        raise ValueError(kind)
    peak = np.max(np.abs(x))
    result: np.ndarray = x / peak if peak > 0 else x
    return result


def make_example(rng: np.random.Generator) -> tuple[np.ndarray, np.ndarray]:
    """A training example: one to three hits at once (in a real mix, as from Demucs's drum stem: a little
    bleed of bass and tones, some room and noise), and which kinds are in it."""
    count = int(rng.choice([1, 1, 1, 2, 2, 3]))
    kinds = rng.choice(len(KINDS), size=count, replace=False)
    seconds = 0.4
    n = int(seconds * RATE)
    mix = np.zeros(n)
    for k in kinds:
        hit = make_hit(KINDS[k], rng, seconds) * rng.uniform(0.2, 1.0)
        shift = int(rng.integers(0, int(0.004 * RATE)))
        mix[shift:] += hit[: n - shift]
    t = np.arange(n) / RATE
    # The tail of an earlier hit still ringing underneath (a kick before a hi-hat, say).
    if rng.uniform() < 0.6:
        earlier = make_hit(KINDS[int(rng.integers(len(KINDS)))], rng, 1.0) * rng.uniform(0.2, 1.0)
        ago = int(rng.uniform(0.05, 0.5) * RATE)
        tail = earlier[ago : ago + n]
        mix[: len(tail)] += tail
    # Room: a little decaying noise shaped like the hit's own spectrum (cheap reverb).
    if rng.uniform() < 0.6:
        room = rng.standard_normal(int(0.08 * RATE)) * np.exp(-np.arange(int(0.08 * RATE)) / (0.02 * RATE))
        tail = np.convolve(mix, room, "same")
        mix += rng.uniform(0.02, 0.15) * tail / (np.max(np.abs(tail)) + 1e-9)
    # Bleed: what Demucs leaves of the bass and the music in a drum stem.
    if rng.uniform() < 0.5:
        mix += rng.uniform(0.0, 0.08) * np.sin(2 * np.pi * rng.uniform(40, 120) * t + rng.uniform(0, 6))
    if rng.uniform() < 0.3:
        mix += rng.uniform(0.0, 0.04) * np.sin(2 * np.pi * rng.uniform(200, 1000) * t)
    mix += rng.standard_normal(n) * 10 ** rng.uniform(-4, -2)
    # Tone and level of the mix as a whole.
    if rng.uniform() < 0.5:
        mix = np.tanh(mix * rng.uniform(1, 3))
    mix *= rng.uniform(0.1, 1.0) / (np.max(np.abs(mix)) + 1e-9)
    labels = np.zeros(len(KINDS))
    labels[kinds] = 1.0
    return mix, labels


# ---------------------------------------------------------------------------------------------------
# What a hit sounds like

N_BANDS = 24
WINDOW = 1024


def _band_edges() -> np.ndarray:
    def mel(f: float) -> float:
        return float(2595 * np.log10(1 + f / 700))

    edges = np.linspace(mel(40.0), mel(10000.0), N_BANDS + 1)
    result: np.ndarray = 700 * (10 ** (edges / 2595) - 1)
    return result


EDGES = _band_edges()
FREQS = np.fft.rfftfreq(WINDOW, 1.0 / RATE)
BAND_OF = np.digitize(FREQS, EDGES) - 1


def describe(hit: np.ndarray) -> np.ndarray:
    """A hit (from its onset, at 22.05 kHz) as numbers: the spectrum's shape in 24 bands early (0-46 ms)
    and late (46-180 ms), relative to its own loudness, and how fast it fades."""
    hit = np.pad(hit, (0, max(0, int(0.2 * RATE) - len(hit))))
    window = np.hanning(WINDOW)
    early = np.abs(np.fft.rfft(hit[:WINDOW] * window)) ** 2
    late = np.zeros_like(early)
    for start in (WINDOW, WINDOW + WINDOW // 2, 2 * WINDOW):
        segment = hit[start : start + WINDOW]
        if len(segment) == WINDOW:
            late += np.abs(np.fft.rfft(segment * window)) ** 2

    def bands(spectrum: np.ndarray) -> np.ndarray:
        values = np.zeros(N_BANDS)
        inside = (BAND_OF >= 0) & (BAND_OF < N_BANDS)
        np.add.at(values, BAND_OF[inside], spectrum[inside])
        return values

    e, lt = bands(early), bands(late)
    total = e.sum() + 1e-12
    shape_early = np.log10(e / total + 1e-6)
    shape_late = np.log10(lt / total + 1e-6)
    envelope = [np.sqrt(np.mean(hit[i : i + 220] ** 2)) for i in range(0, int(0.2 * RATE), 220)]
    envelope = np.log10(np.asarray(envelope) / (max(envelope) + 1e-12) + 1e-4)
    return np.concatenate([shape_early, shape_late, envelope[:20]])


# ---------------------------------------------------------------------------------------------------
# The network

HIDDEN = 96


class Recogniser:
    def __init__(self, params: dict[str, np.ndarray]) -> None:
        self.p = params

    def probabilities(self, features: np.ndarray) -> np.ndarray:
        x = (features - self.p["mean"]) / self.p["std"]
        h = np.maximum(0.0, x @ self.p["w1"] + self.p["b1"])
        result: np.ndarray = 1.0 / (1.0 + np.exp(-(h @ self.p["w2"] + self.p["b2"])))
        return result

    def save(self, file: Path) -> None:
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(json.dumps({"version": VERSION, **{k: v.tolist() for k, v in self.p.items()}}))

    @staticmethod
    def load(file: Path) -> Recogniser | None:
        try:
            data = json.loads(file.read_text())
        except (OSError, ValueError):
            return None
        if data.pop("version", None) != VERSION:
            return None
        return Recogniser({k: np.asarray(v) for k, v in data.items()})


def train(examples: int = 12000, epochs: int = 60, seed: int = 1) -> Recogniser:
    """Trains the recogniser on generated hits (the same every time: seeded)."""
    rng = np.random.default_rng(seed)
    features, labels = [], []
    for _ in range(examples):
        audio, label = make_example(rng)
        features.append(describe(audio))
        labels.append(label)
    x, y = np.asarray(features), np.asarray(labels)
    mean, std = x.mean(axis=0), x.std(axis=0) + 1e-6
    x = (x - mean) / std
    w1 = rng.standard_normal((x.shape[1], HIDDEN)) * np.sqrt(2 / x.shape[1])
    b1 = np.zeros(HIDDEN)
    w2 = rng.standard_normal((HIDDEN, len(KINDS))) * np.sqrt(1 / HIDDEN)
    b2 = np.zeros(len(KINDS))
    # Adam on the sigmoid cross-entropy, mini-batches of 128.
    params = [w1, b1, w2, b2]
    moments = [np.zeros_like(p) for p in params]
    squares = [np.zeros_like(p) for p in params]
    step, rate = 0, 0.003
    for _ in range(epochs):
        order = rng.permutation(len(x))
        for start in range(0, len(x), 128):
            batch = order[start : start + 128]
            xb, yb = x[batch], y[batch]
            h_pre = xb @ w1 + b1
            h = np.maximum(0.0, h_pre)
            out = 1.0 / (1.0 + np.exp(-(h @ w2 + b2)))
            d_out = (out - yb) / len(batch)
            grads = [None, None, h.T @ d_out, d_out.sum(axis=0)]
            d_h = (d_out @ w2.T) * (h_pre > 0)
            grads[0], grads[1] = xb.T @ d_h, d_h.sum(axis=0)
            step += 1
            for i, (p, g) in enumerate(zip(params, grads, strict=True)):
                moments[i] = 0.9 * moments[i] + 0.1 * g
                squares[i] = 0.999 * squares[i] + 0.001 * g * g
                m_hat = moments[i] / (1 - 0.9**step)
                v_hat = squares[i] / (1 - 0.999**step)
                p -= rate * m_hat / (np.sqrt(v_hat) + 1e-8)
    return Recogniser({"mean": mean, "std": std, "w1": w1, "b1": b1, "w2": w2, "b2": b2})


def recogniser(cache: Path) -> Recogniser:
    """The trained recogniser, trained now (a few seconds) if this computer has not done it yet."""
    file = cache / f"drum-recogniser-{VERSION}.json"
    found = Recogniser.load(file)
    if found is None:
        found = train()
        found.save(file)
    return found


# ---------------------------------------------------------------------------------------------------
# A drum stem


def fundamental(hit: np.ndarray) -> float:
    """The strongest frequency below 400 Hz in a tom's body (40-200 ms after the onset)."""
    body = hit[int(0.04 * RATE) : int(0.2 * RATE)]
    if len(body) < 512:
        return 0.0
    spectrum = np.abs(np.fft.rfft(body * np.hanning(len(body)), 8192))
    f = np.fft.rfftfreq(8192, 1.0 / RATE)
    inside = (f > 40) & (f < 400)
    return float(f[inside][np.argmax(spectrum[inside])])


def refine_onset(signal: np.ndarray, time: float) -> int:
    """Where a hit really starts, near a detected onset (which may be off by tens of milliseconds): the
    largest burst of new sound (spectral flux, 256-sample frames every 32 samples) within 30 ms either
    side. A hit over another's ringing tail adds new frequencies there while the tail only fades, so this
    one is described without the tail before it. `signal` starts with some silence."""
    size, hop = 256, 32
    begin = max(0, int((time - 0.03) * RATE) - size)
    segment = signal[begin : begin + int(0.06 * RATE) + size]
    frames = (len(segment) - size) // hop
    if frames < 3:
        return max(0, int(time * RATE))
    spectra = np.abs(
        np.fft.rfft(np.stack([segment[i * hop : i * hop + size] for i in range(frames)]) * np.hanning(size), axis=1)
    )
    flux = np.maximum(0.0, np.diff(spectra, axis=0)).sum(axis=1)
    best = int(np.argmax(flux)) + 1
    return begin + best * hop + size // 2 - int(0.002 * RATE)


def find_hits(audio: np.ndarray, rate: int, model: Recogniser, threshold: float = 0.5) -> list[dict[str, Any]]:
    """Every hit of a drum stem: {time (s), kind, pad, velocity}, several kinds at the same time allowed."""
    from .listening import detect_onsets
    from .transcribe import resample

    mono = audio.mean(axis=0) if audio.ndim == 2 else audio
    signal = resample(mono, rate, RATE)
    peak = float(np.max(np.abs(signal))) or 1.0
    lead = int(0.05 * RATE)  # a little silence first, so a hit right at the start is found too
    padded = np.concatenate([np.zeros(lead), signal])
    times = detect_onsets(padded, RATE).times
    hits: list[dict[str, Any]] = []
    tom_hits: list[tuple[int, float]] = []
    for time in times:
        start = refine_onset(padded, float(time)) - lead
        if start < 0:
            start = 0
        hit = signal[start : start + int(0.2 * RATE)]
        if len(hit) < 256:
            continue
        level = float(np.sqrt(np.mean(hit[:1024] ** 2)))
        if level < 0.003 * peak:
            continue
        probabilities = model.probabilities(describe(hit))
        found = [KINDS[i] for i in np.nonzero(probabilities >= threshold)[0]]
        if not found:
            found = [KINDS[int(np.argmax(probabilities))]]
        velocity = round(float(np.clip(0.3 + 0.7 * np.sqrt(level / peak) * 1.4, 0.1, 1.0)), 2)
        for kind in found:
            if kind == "tom":
                tom_hits.append((len(hits), fundamental(hit)))
            hits.append({"time": round(start / RATE, 4), "kind": kind, "velocity": velocity})

    # Toms: low, mid and high, by where each one's pitch falls among this song's toms.
    if tom_hits:
        pitches = np.asarray([f for _, f in tom_hits])
        low, high = np.percentile(pitches, [33, 67]) if len(set(pitches.round())) > 1 else (pitches[0], pitches[0])
        for index, f in tom_hits:
            hits[index]["kind"] = (
                "low_tom" if f <= low and len(set(pitches.round())) > 1 else ("high_tom" if f > high else "mid_tom")
            )
    for h in hits:
        h["pad"] = PADS[h["kind"]]
    return hits


# Pads the Synth Kit leaves free, for a second sound of the same kind.
FREE_PADS = (69, 71, 72, 73, 75)
NAMES = {"kick": "kick", "snare": "snare", "clap": "clap", "closed_hat": "closed hat", "open_hat": "open hat",
         "low_tom": "low tom", "mid_tom": "mid tom", "high_tom": "high tom", "rim": "rim", "crash": "crash",
         "ride": "ride"}  # fmt: skip


def group_hits(
    audio: np.ndarray, rate: int, hits: list[dict[str, Any]]
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Hits of one kind that sound clearly different (a song with two snares) become two groups on two pads,
    the second on a pad the Synth Kit leaves free; each group's loudest hit is full velocity and the others
    are accents relative to it. Returns (hits with "group" and their pad, groups [{pad, kind, name, count,
    start, end}], where start and end are the group's clearest hit, as for the song's own sounds)."""
    from .presets import describe_sound
    from .samples import choose_drum_samples

    mono = audio.mean(axis=0) if audio.ndim == 2 else audio
    duration = len(mono) / rate
    times = sorted(h["time"] for h in hits)
    hits = [dict(h) for h in hits]

    def measure(h: dict[str, Any]) -> np.ndarray:
        later = [t for t in times if t > h["time"] + 0.03]
        end = min(h["time"] + 0.25, later[0] if later else duration)
        piece = mono[int(h["time"] * rate) : int(end * rate)]
        return np.asarray(describe_sound(piece, rate)[:-1], dtype=float)  # without the attack: too short here

    measured = {id(h): measure(h) for h in hits}
    spread = np.maximum(np.std(np.array(list(measured.values())), axis=0), 1.0) if hits else np.ones(1)
    free = list(FREE_PADS)
    for kind in sorted({h["kind"] for h in hits}):
        same = [h for h in hits if h["kind"] == kind]
        for h in same:
            h["group"] = 0
        if len(same) < 4 or not free:
            continue
        points = np.array([measured[id(h)] / spread for h in same])
        # Two groups: start from the two hits furthest apart, then settle (a few rounds of 2-means).
        distances = np.linalg.norm(points[:, None, :] - points[None, :, :], axis=2)
        a, b = np.unravel_index(np.argmax(distances), distances.shape)
        centres = np.array([points[a], points[b]])
        for _ in range(10):
            labels = np.argmin(np.linalg.norm(points[:, None, :] - centres[None, :, :], axis=2), axis=1)
            if labels.min() == labels.max():
                break
            centres = np.array([points[labels == k].mean(axis=0) for k in (0, 1)])
        if labels.min() == labels.max() or min(np.sum(labels == 0), np.sum(labels == 1)) < 2:
            continue
        within = np.mean(np.linalg.norm(points - centres[labels], axis=1))
        apart = float(np.linalg.norm(centres[0] - centres[1]))
        if apart < 2.5 * max(within, 1e-6):
            continue  # the same sound played differently
        # The bigger group keeps the kind's pad.
        bigger = 0 if np.sum(labels == 0) >= np.sum(labels == 1) else 1
        pad = free.pop(0)
        for h, label in zip(same, labels, strict=True):
            if label != bigger:
                h["group"] = 1
                h["pad"] = pad

    groups = []
    for pad in sorted({h["pad"] for h in hits}):
        members = [h for h in hits if h["pad"] == pad]
        loudest = max(h["velocity"] for h in members)
        for h in members:
            h["velocity"] = round(float(np.clip(h["velocity"] / max(loudest, 1e-6), 0.2, 1.0)), 2)
        kind, group = members[0]["kind"], members[0]["group"]
        clearest = choose_drum_samples(members, duration)
        piece = clearest[0] if clearest else {"start": members[0]["time"], "end": members[0]["time"] + 0.2}
        groups.append({"pad": pad, "kind": kind, "name": NAMES.get(kind, kind) + (" 2" if group else ""),
                       "count": len(members), "start": piece["start"], "end": piece["end"]})  # fmt: skip
    return hits, groups


def place_hits(hits: list[dict[str, Any]], beats: list[float], first_downbeat: float) -> list[dict[str, Any]]:
    """Hits as Drum Machine notes: on sixteenths of the song's own beats, one per pad and step."""
    from .transcribe import place_on_beats

    notes = [{"start": h["time"], "end": h["time"] + 0.05, "pitch": h["pad"], "velocity": h["velocity"]} for h in hits]
    placed = place_on_beats(notes, beats, first_downbeat)
    for p in placed:
        p["length"] = 240
    return placed
