"""The style of a few reference songs, for the Track creator: what the songs share, as plain numbers.

Spinus analyzes each song with the Analyzer's steps (tempo and beats, key, loudness, stems, the stems'
notes and drum hits) and gives the results here. `analyze_song` turns one song's results into its
*features* (kept by Spinus per song, so a song added again is not analyzed again); `summarize` combines
the features of all the songs into the *style summary*. The summary is the input of the Track creator's
later steps (choosing sounds, writing a new song with rules), so it holds explicit numbers: the drums'
probability per sixteenth step, how often each chord progression comes, section lengths in bars, and each
part's register and note density. It also says it all in plain words (`plain`).

Everything here is measured with NumPy: no model, nothing downloaded.

- **Chords**, one per bar: the notes of the harmonic parts (bass, piano, guitar, "other") sounding in the
  bar, weighed by how long they sound, make a chroma (how much of each of the 12 pitch classes), which is
  compared with chord templates (major, minor, diminished, suspended, sevenths); the bass's lowest note
  favours the chord with that root. Chords are named as Roman numerals from the song's key (in A minor,
  A minor is `i`, C major `bIII`), so songs in different keys can be compared.
- **Drum groove**: each drum hit is placed on the sixteenth-note grid of its bar (from the beats), and
  for each step the share of bars with drums that have a kick, a snare (snare, clap, rim) or hats
  (closed, open, ride) there. Swing: where the off-beat sixteenths fall between two eighths (0.5 is
  straight, 0.67 a triplet feel).
- **Sections**: the mix's chroma and loudness per bar; a checkerboard kernel along the bars'
  self-similarity finds where the music changes (Foote's novelty), and sections that sound alike get the
  same letter (A, B, A, ...).
"""

from __future__ import annotations

import itertools
import math
import statistics
from collections import Counter
from typing import Any

import numpy as np

FEATURES_VERSION = 2  # change when what analyze_song measures changes (Spinus then analyzes songs again)
SUMMARY_VERSION = 1  # change when the summary's format changes

NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")

# Chord templates: intervals from the root. Four-note chords and sus4 must fit clearly better to be chosen.
QUALITIES: dict[str, tuple[int, ...]] = {
    "maj": (0, 4, 7),
    "min": (0, 3, 7),
    "dim": (0, 3, 6),
    "sus4": (0, 5, 7),  # a melody note on the fourth easily looks like one: it must fit clearly better
    "7": (0, 4, 7, 10),
    "maj7": (0, 4, 7, 11),
    "m7": (0, 3, 7, 10),
}
SEVENTH_PENALTY = 0.04
BASS_BONUS = 0.15
# Stems whose notes make the harmony (vocals and leads are left out: they pass through other notes).
HARMONIC = ("bass", "other", "piano", "guitar", "keys", "synth", "strings")

# Drum kinds (spinus_ai.drums.KINDS) in the groove's three lanes.
LANES = {
    "kick": ("kick",),
    "snare": ("snare", "clap", "rim"),
    "hats": ("closed_hat", "open_hat", "ride", "hh", "hat"),
}

DEGREES = {
    0: "I",
    1: "bII",
    2: "II",
    3: "bIII",
    4: "III",
    5: "IV",
    6: "bV",
    7: "V",
    8: "bVI",
    9: "VI",
    10: "bVII",
    11: "VII",
}


# -----------------------------------------------------------------------------------------------
# One song.


def bar_starts(song: dict[str, Any]) -> list[float]:
    """The times bars start (seconds): the downbeats, or every beatsPerBar-th beat from the first
    downbeat. Empty without a beat."""
    beats = [float(b) for b in song.get("beats") or []]
    downbeats = [float(b) for b in song.get("downbeats") or []]
    if len(downbeats) >= 2:
        return downbeats
    if len(beats) < 2:
        return []
    per_bar = max(1, int(song.get("beatsPerBar") or 4))
    given = song.get("firstDownbeat")
    first = float(given) if given is not None else beats[0]
    start = int(np.argmin([abs(b - first) for b in beats]))
    return beats[start::per_bar]


def roman(root: int, quality: str, tonic: int) -> str:
    """A chord as a Roman numeral from the key's tonic: upper case major, lower case minor."""
    numeral = DEGREES[(root - tonic) % 12]
    accidental = numeral[0] if numeral[0] == "b" else ""
    numeral = numeral[len(accidental) :]
    if quality in ("min", "m7", "dim"):
        numeral = numeral.lower()
    suffix = {"dim": "°", "sus4": "sus4", "7": "7", "maj7": "maj7", "m7": "7"}.get(quality, "")
    return accidental + numeral + suffix


def chord_name(root: int, quality: str) -> str:
    return (
        NAMES[root]
        + {"maj": "", "min": "m", "dim": "dim", "sus4": "sus4", "7": "7", "maj7": "maj7", "m7": "m7"}[quality]
    )


def best_chord(profile: np.ndarray, bass: int | None) -> tuple[int, str, float] | None:
    """The chord template closest to a chroma profile: (root, quality, score), or None for silence."""
    total = float(np.linalg.norm(profile))
    if total <= 1e-9:
        return None
    unit = profile / total
    best: tuple[int, str, float] | None = None
    for root in range(12):
        for quality, intervals in QUALITIES.items():
            template = np.zeros(12)
            template[[(root + i) % 12 for i in intervals]] = 1.0
            score = float(unit @ template) / math.sqrt(len(intervals))
            if len(intervals) > 3 or quality == "sus4":
                score -= SEVENTH_PENALTY
            if bass is not None and bass == root:
                score += BASS_BONUS
            if best is None or score > best[2] + 1e-9:
                best = (root, quality, score)
    return best


def chords_of(song: dict[str, Any], bars: list[float]) -> list[dict[str, Any]]:
    """One chord per bar from the harmonic parts' notes: [{bar, root, quality, name, roman}] (bars with
    no harmony are left out)."""
    parts = song.get("parts") or {}
    notes = [(n, stem) for stem, found in parts.items() if stem in HARMONIC for n in found or []]
    if not notes or len(bars) < 2:
        return []
    tonic = int((song.get("key") or {}).get("tonic", 0))
    length = bars[-1] - bars[-2]
    edges = [*bars, bars[-1] + length]
    found = []
    for index in range(len(edges) - 1):
        start, end = edges[index], edges[index + 1]
        profile = np.zeros(12)
        lowest: tuple[int, float] | None = None
        for note, stem in notes:
            overlap = min(float(note["end"]), end) - max(float(note["start"]), start)
            if overlap <= 0:
                continue
            pitch = int(note["pitch"])
            profile[pitch % 12] += overlap * max(0.1, float(note.get("velocity", 0.8)))
            if stem == "bass" and overlap > 0.1 * (end - start) and (lowest is None or pitch < lowest[0]):
                lowest = (pitch, overlap)
        chord = best_chord(profile, lowest[0] % 12 if lowest is not None else None)
        if chord is None:
            continue
        root, quality, _ = chord
        found.append(
            {
                "bar": index,
                "root": root,
                "quality": quality,
                "name": chord_name(root, quality),
                "roman": roman(root, quality, tonic),
            }
        )
    return found


def beat_position(time: float, beats: list[float]) -> float:
    """Where a time is in beats (0 at the first beat), the beats' own length carried on just outside them."""
    if time < beats[0]:
        return (time - beats[0]) / (beats[1] - beats[0])
    if time > beats[-1]:
        return len(beats) - 1 + (time - beats[-1]) / (beats[-1] - beats[-2])
    return float(np.interp(time, beats, np.arange(len(beats), dtype=float)))


def groove_of(song: dict[str, Any]) -> dict[str, Any] | None:
    """The drums on the sixteenth grid: {steps (per bar), bars (with drums), lanes {kick, snare, hats:
    [probability per step]}, swing, hits}. None without drums or beats."""
    hits = song.get("drums") or []
    beats = [float(b) for b in song.get("beats") or []]
    if len(hits) < 4 or len(beats) < 2:
        return None
    per_bar = max(1, int(song.get("beatsPerBar") or 4))
    steps = per_bar * 4
    bars = bar_starts(song)
    first = beat_position(bars[0], beats) if bars else 0.0
    used: dict[str, set[tuple[int, int]]] = {lane: set() for lane in LANES}
    active: set[int] = set()
    step_seconds = float(np.median(np.diff(beats))) / 4
    found = []  # (position in beats from the first bar, kind)
    for hit in hits:
        time = float(hit["time"])
        if beats[0] - step_seconds <= time <= beats[-1] + 4 * step_seconds:
            found.append((beat_position(time, beats) - first, str(hit.get("kind", ""))))
    # Hits are found a little late (onset detection), the same for all: how late, from the hits close to
    # the eighths, is taken off before they are placed. Otherwise a fast straight song reads as swung, and
    # swung off-beats slip onto the next sixteenth.
    lags = [((p * 2 + 0.5) % 1.0) - 0.5 for p, _ in found]
    close = [lag for lag in lags if abs(lag) <= 0.15]
    late = float(np.median(close)) / 2 if len(close) >= 8 else 0.0
    offbeats: list[float] = []
    for position, kind in found:
        relative = position - late
        sixteenth = round(relative * 4)
        if sixteenth < 0:
            continue
        bar, step = divmod(sixteenth, steps)
        active.add(bar)
        for lane, kinds in LANES.items():
            if kind in kinds:
                used[lane].add((bar, step))
        if sixteenth % 2 == 1:
            # Where between two eighths it falls: 0.5 straight, more when swung.
            share = (relative * 2) % 1.0
            if 0.35 <= share <= 0.8:
                offbeats.append(share)
    if not active:
        return None
    count = len(active)
    lanes = {
        lane: [round(sum(1 for b in active if (b, s) in places) / count, 3) for s in range(steps)]
        for lane, places in used.items()
    }
    swing = round(float(np.median(offbeats)), 3) if len(offbeats) >= 8 else 0.5
    return {"steps": steps, "bars": count, "lanes": lanes, "swing": swing, "hits": len(hits)}


def parts_of(song: dict[str, Any], bar_count: int, seconds: float) -> dict[str, Any]:
    """Each part's notes: {stem: {notes, notesPerBar, low, median, high (MIDI pitches: 10th, 50th and
    90th percentile), voices (notes sounding at once, on average), present}}; the drums' hits per bar."""
    bars = max(1.0, float(bar_count) if bar_count > 0 else seconds / 2.0)
    result: dict[str, Any] = {}
    for stem, notes in (song.get("parts") or {}).items():
        notes = notes or []
        if not notes:
            result[stem] = {"notes": 0, "notesPerBar": 0.0, "present": False}
            continue
        pitches = np.array([int(n["pitch"]) for n in notes])
        spans = sorted((float(n["start"]), float(n["end"])) for n in notes)
        total = sum(max(0.0, e - s) for s, e in spans)
        union, until = 0.0, -math.inf
        for s, e in spans:
            if e > until:
                union += e - max(s, until)
                until = e
        density = len(notes) / bars
        result[stem] = {
            "notes": len(notes),
            "notesPerBar": round(density, 2),
            "low": int(np.percentile(pitches, 10)),
            "median": int(np.median(pitches)),
            "high": int(np.percentile(pitches, 90)),
            "voices": round(total / union, 2) if union > 0 else 1.0,
            "present": len(notes) >= 8 and density >= 0.25,
        }
    hits = song.get("drums") or []
    result["drums"] = {"notes": len(hits), "notesPerBar": round(len(hits) / bars, 2), "present": len(hits) >= 8}
    return result


def frames_of(audio: np.ndarray, rate: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Chroma (frames x 12, each frame summing to 1), loudness (dB) and the frames' times, from 4096-sample
    frames of the mono mix."""
    mono = audio.mean(axis=0) if audio.ndim == 2 else audio
    size, hop = 4096, 2048
    if len(mono) < size:
        mono = np.pad(mono, (0, size - len(mono)))
    frames = np.lib.stride_tricks.sliding_window_view(mono, size)[::hop]
    spectrum = np.abs(np.fft.rfft(frames * np.hanning(size), axis=1))
    freqs = np.fft.rfftfreq(size, 1.0 / rate)
    band = (freqs >= 55.0) & (freqs <= 5000.0)
    pitch = np.round(12.0 * np.log2(freqs[band] / 440.0) + 69.0).astype(int) % 12
    weights = np.zeros((12, int(band.sum())))
    weights[pitch, np.arange(len(pitch))] = 1.0
    chroma = spectrum[:, band] @ weights.T
    chroma = chroma / np.maximum(chroma.sum(axis=1, keepdims=True), 1e-9)
    rms = np.sqrt(np.mean(frames * frames, axis=1))
    loudness = 20 * np.log10(np.maximum(rms, 1e-6))
    times = (np.arange(len(frames)) * hop + size / 2) / rate
    return chroma, loudness, times


def sections_of(audio: np.ndarray, rate: int, bars: list[float]) -> list[dict[str, Any]]:
    """Where the music changes, in bars: [{start (bar), bars, label (A, B, ...), level (dB below the
    loudest section)}]."""
    if len(bars) < 4:
        return []
    chroma, loudness, times = frames_of(audio, rate)
    length = bars[-1] - bars[-2]
    edges = [*bars, bars[-1] + length]
    per_bar_chroma, per_bar_level = [], []
    for start, end in itertools.pairwise(edges):
        inside = (times >= start) & (times < end)
        if not inside.any():
            inside = np.abs(times - (start + end) / 2) == np.min(np.abs(times - (start + end) / 2))
        per_bar_chroma.append(chroma[inside].mean(axis=0))
        per_bar_level.append(float(np.mean(loudness[inside])))
    tone = np.array(per_bar_chroma)
    level = np.array(per_bar_level)
    count = len(tone)
    unit = tone / np.maximum(np.linalg.norm(tone, axis=1, keepdims=True), 1e-9)
    similar = unit @ unit.T
    # Loudness changes count too: 6 dB apart halves the likeness.
    similar = similar * np.exp(-np.abs(level[:, None] - level[None, :]) / 8.66)
    half = 4 if count >= 16 else 2
    kernel = np.ones((2 * half, 2 * half))
    kernel[:half, half:] = -1
    kernel[half:, :half] = -1
    padded = np.pad(similar, half, mode="edge")
    novelty = np.array([float((padded[i : i + 2 * half, i : i + 2 * half] * kernel).sum()) for i in range(count)])
    novelty = np.maximum(novelty, 0.0)
    threshold = float(novelty.mean() + 0.5 * novelty.std()) if novelty.max() > 0 else math.inf
    boundaries = [0]
    shortest = 4 if count >= 16 else 2
    for i in sorted(range(1, count), key=lambda k: -novelty[k]):
        if novelty[i] < threshold or novelty[i] <= 1e-6:
            break
        if all(abs(i - b) >= shortest for b in boundaries) and count - i >= shortest:
            boundaries.append(i)
    boundaries = sorted(boundaries)
    sections: list[dict[str, Any]] = []
    labels: list[tuple[str, np.ndarray, float]] = []
    loudest = -math.inf
    for start, end in itertools.pairwise([*boundaries, count]):
        mean_tone = unit[start:end].mean(axis=0)
        mean_tone = mean_tone / max(float(np.linalg.norm(mean_tone)), 1e-9)
        mean_level = float(level[start:end].mean())
        loudest = max(loudest, mean_level)
        label = None
        for name, tone_of, level_of in labels:
            if float(mean_tone @ tone_of) > 0.95 and abs(mean_level - level_of) < 3.0:
                label = name
                break
        if label is None:
            label = chr(ord("A") + min(len(labels), 25))
            labels.append((label, mean_tone, mean_level))
        sections.append({"start": start, "bars": end - start, "label": label, "level": mean_level})
    for section in sections:
        section["level"] = round(section["level"] - loudest, 1)
    return sections


def analyze_song(song: dict[str, Any], audio: np.ndarray | None = None, rate: int = 0) -> dict[str, Any]:
    """One song's features from the Analyzer's results: {name, tempo, beatsPerBar, key, bars, seconds,
    chords, groove, parts, sections, loudness}. `song`: {name, tempo, beatsPerBar, firstDownbeat, beats,
    downbeats (seconds), key {tonic, scale, name}, loudness (an ai.v1.mix.analyze insert: loudness,
    truePeak, crest), parts {stem: [{start, end, pitch, velocity}] (seconds)}, drums [{time, kind}]}.
    `audio` (the mix, [channels, samples]) gives the sections."""
    bars = bar_starts(song)
    key = song.get("key") or {}
    seconds = float(audio.shape[-1] / rate) if audio is not None and rate > 0 else float(song.get("seconds") or 0.0)
    loudness = song.get("loudness") or {}
    sections = sections_of(audio, rate, bars) if audio is not None and rate > 0 else []
    levels = [s["level"] for s in sections]
    return {
        "version": FEATURES_VERSION,
        "name": str(song.get("name", "")),
        "tempo": float(song["tempo"]) if song.get("tempo") else None,
        "beatsPerBar": int(song.get("beatsPerBar") or 4),
        "key": {
            "tonic": int(key.get("tonic", 0)),
            "scale": str(key.get("scale", "major")),
            "name": str(key.get("name", f"{NAMES[int(key.get('tonic', 0)) % 12]} {key.get('scale', 'major')}")),
        }
        if key
        else None,
        "bars": len(bars),
        "seconds": round(seconds, 2),
        "chords": chords_of(song, bars),
        "groove": groove_of(song),
        "parts": parts_of(song, len(bars), seconds),
        "sections": sections,
        "loudness": {
            "lufs": loudness.get("loudness"),
            "truePeak": loudness.get("truePeak"),
            "crest": loudness.get("crest"),
            "contrast": round(-min(levels), 1) if levels else None,  # loudest to quietest section, dB
        },
    }


# -----------------------------------------------------------------------------------------------
# All the songs.


def note_name(pitch: float) -> str:
    """60 -> C4 (scientific pitch, as the Piano Roll names notes)."""
    p = round(pitch)
    return f"{NAMES[p % 12]}{p // 12 - 1}"


def collapse(chords: list[dict[str, Any]]) -> list[str]:
    """A song's chord sequence with repeats joined (I I V V -> I V)."""
    sequence: list[str] = []
    for chord in chords:
        if not sequence or sequence[-1] != chord["roman"]:
            sequence.append(chord["roman"])
    return sequence


def median(values: list[float]) -> float | None:
    return float(statistics.median(values)) if values else None


def summarize(songs: list[dict[str, Any]]) -> dict[str, Any]:
    """What the songs (analyze_song's features) share. Every number is explicit, for a rules-based song
    writer: see the module's notes. `plain` says it in words."""
    count = len(songs)
    tempos = [float(s["tempo"]) for s in songs if s.get("tempo")]
    meters = Counter(int(s.get("beatsPerBar") or 4) for s in songs if s.get("tempo"))
    keys = Counter(str(s["key"]["name"]) for s in songs if s.get("key"))
    modes = Counter(str(s["key"]["scale"]) for s in songs if s.get("key"))

    # Chords.
    roman_counts: Counter[str] = Counter()
    quality_counts: Counter[str] = Counter()
    progressions: Counter[tuple[str, ...]] = Counter()
    progression_songs: dict[tuple[str, ...], set[int]] = {}
    transitions: dict[str, Counter[str]] = {}
    bars_per_chord: list[float] = []
    for index, song in enumerate(songs):
        chords = song.get("chords") or []
        roman_counts.update(c["roman"] for c in chords)
        quality_counts.update(c["quality"] for c in chords)
        sequence = collapse(chords)
        if sequence:
            bars_per_chord.append(len(chords) / len(sequence))
        for a, b in itertools.pairwise(sequence):
            transitions.setdefault(a, Counter())[b] += 1
        for i in range(len(sequence) - 3):
            window = tuple(sequence[i : i + 4])
            progressions[window] += 1
            progression_songs.setdefault(window, set()).add(index)
    chord_total = sum(roman_counts.values())
    progression_total = sum(progressions.values())
    chords_summary = {
        "barsPerChord": round(median(bars_per_chord) or 0.0, 2) if bars_per_chord else None,
        "common": [{"roman": r, "share": round(n / chord_total, 3)} for r, n in roman_counts.most_common(8)],
        "qualities": {q: round(n / chord_total, 3) for q, n in quality_counts.most_common()},
        "progressions": [
            {
                "chords": list(p),
                "count": n,
                "share": round(n / progression_total, 3),
                "songs": len(progression_songs[p]),
            }
            for p, n in progressions.most_common(6)
        ],
        "transitions": {
            a: {b: round(n / sum(after.values()), 3) for b, n in after.most_common()}
            for a, after in sorted(transitions.items())
        },
    }

    # Drums: songs with the usual bar length, weighed by their bars with drums.
    grooves = [s["groove"] for s in songs if s.get("groove")]
    steps = Counter(g["steps"] for g in grooves).most_common(1)[0][0] if grooves else 16
    alike = [g for g in grooves if g["steps"] == steps]
    weight = sum(g["bars"] for g in alike)
    lanes = {
        lane: [
            round(sum(g["lanes"][lane][i] * g["bars"] for g in alike) / weight, 3) if weight else 0.0
            for i in range(steps)
        ]
        for lane in LANES
    }
    drums = {
        "songs": len(grooves),
        "steps": steps,
        "lanes": lanes,
        "swing": round(median([g["swing"] for g in alike]) or 0.5, 3),
    }

    # Parts.
    parts: dict[str, Any] = {}
    names = sorted({p for s in songs for p in (s.get("parts") or {})})
    for part in names:
        present = [s["parts"][part] for s in songs if (s.get("parts") or {}).get(part, {}).get("present")]
        if not present:
            continue
        entry: dict[str, Any] = {
            "songs": len(present),
            "notesPerBar": round(median([p["notesPerBar"] for p in present]) or 0.0, 2),
        }
        if part != "drums":
            entry.update(
                {
                    "low": round(median([p["low"] for p in present]) or 0),
                    "median": round(median([p["median"] for p in present]) or 0),
                    "high": round(median([p["high"] for p in present]) or 0),
                    "voices": round(median([p["voices"] for p in present]) or 1.0, 2),
                }
            )
        parts[part] = entry

    # Sections.
    lengths = Counter(sec["bars"] for s in songs for sec in s.get("sections") or [])
    with_sections = [s for s in songs if s.get("sections")]
    sections = {
        "lengths": [{"bars": b, "count": n} for b, n in lengths.most_common()],
        "perSong": median([float(len(s["sections"])) for s in with_sections]),
        "songBars": median([float(s["bars"]) for s in songs if s.get("bars")]),
        "forms": ["".join(sec["label"] for sec in s["sections"]) for s in with_sections],
    }

    # Loudness.
    def values(key: str) -> list[float]:
        return [float(s["loudness"][key]) for s in songs if (s.get("loudness") or {}).get(key) is not None]

    lufs = values("lufs")
    loudness = {
        "lufs": median(lufs),
        "lufsRange": [min(lufs), max(lufs)] if lufs else None,
        "truePeak": max(values("truePeak")) if values("truePeak") else None,
        "crest": median(values("crest")),
        "contrast": median(values("contrast")),
    }

    summary: dict[str, Any] = {
        "kind": "spinus.style-summary",
        "version": SUMMARY_VERSION,
        "songs": count,
        "names": [str(s.get("name", "")) for s in songs],
        "tempo": {"median": round(median(tempos) or 0.0, 1), "min": round(min(tempos), 1), "max": round(max(tempos), 1)}
        if tempos
        else None,
        "beatsPerBar": {str(k): n for k, n in meters.most_common()},
        "keys": [{"name": k, "count": n} for k, n in keys.most_common()],
        "modes": {m: round(n / sum(modes.values()), 3) for m, n in modes.most_common()},
        "chords": chords_summary,
        "drums": drums,
        "parts": parts,
        "sections": sections,
        "loudness": loudness,
        "perSong": songs,
    }
    summary["plain"] = describe(summary)
    return summary


def per_bar(count: float) -> str:
    """4.48 -> "4.5", 26.49 -> "26": as precise as it is worth."""
    return f"{count:.0f}" if count >= 10 else f"{round(count, 1):g}"


def steps_text(probabilities: list[float], low: float, high: float) -> str:
    return ", ".join(str(i + 1) for i, p in enumerate(probabilities) if low <= p < high)


PART_NAMES = {
    "bass": "Bass",
    "vocals": "Vocals",
    "other": "Other instruments",
    "piano": "Piano",
    "guitar": "Guitar",
    "drums": "Drums",
}


def describe(summary: dict[str, Any]) -> list[str]:
    """The summary in plain words, a sentence or two per topic."""
    lines = []
    count = summary["songs"]
    lines.append(f"{count} song{'s' if count != 1 else ''}: {', '.join(summary['names'])}.")
    tempo = summary.get("tempo")
    if tempo:
        meter = max(summary["beatsPerBar"].items(), key=lambda m: m[1])[0] if summary["beatsPerBar"] else "4"
        span = (
            f"{tempo['min']:.0f} to {tempo['max']:.0f} BPM, typically {tempo['median']:.0f} BPM"
            if tempo["max"] - tempo["min"] >= 1
            else f"{tempo['median']:.0f} BPM"
        )
        lines.append(f"Tempo: {span}; {meter} beats per bar.")
    else:
        lines.append("Tempo: no steady beat found.")
    if summary["keys"]:
        mode, share = max(summary["modes"].items(), key=lambda m: m[1])
        keys = ", ".join(f"{k['name']} ({k['count']})" if k["count"] > 1 else k["name"] for k in summary["keys"][:4])
        if share >= 1:
            lines.append(f"Key: all {mode}: {keys}.")
        elif share > 0.5:
            lines.append(f"Key: mostly {mode} ({round(share * 100)} % of the songs): {keys}.")
        else:
            lines.append(f"Key: major and minor alike: {keys}.")
    chords = summary["chords"]
    if chords["common"]:
        every = chords["barsPerChord"] or 1.0
        rate = "a new chord every bar" if every < 1.5 else f"a new chord about every {every:.0f} bars"
        common = ", ".join(c["roman"] for c in chords["common"][:5])
        lines.append(f"Chords: {rate}; most used (from the key's first note): {common}.")
        if chords["progressions"]:
            top = chords["progressions"][0]
            in_songs = f"in {top['songs']} songs" if top["songs"] > 1 else "in one song"
            lines.append(f"Favourite progression: {' - '.join(top['chords'])} ({top['count']} times, {in_songs}).")
    else:
        lines.append("Chords: none found (no bass or chord parts heard).")
    drums = summary["drums"]
    if drums["songs"]:
        words = []
        for lane, label in (("kick", "kick"), ("snare", "snare or clap"), ("hats", "hi-hats")):
            probabilities = drums["lanes"][lane]
            often = steps_text(probabilities, 0.5, 2.0)
            # Below that, only the few likeliest steps: a long list of 20-40 % steps says nothing.
            likely = sorted(
                (i for i, p in enumerate(probabilities) if 0.25 <= p < 0.5), key=lambda i: -probabilities[i]
            )
            sometimes = ", ".join(str(i + 1) for i in sorted(likely[:3]))
            if often:
                words.append(f"{label} on steps {often}" + (f" (sometimes also {sometimes})" if sometimes else ""))
            elif sometimes:
                words.append(f"{label} with no steady place (most often on {sometimes})")
        swing = drums["swing"]
        feel = "straight" if swing < 0.54 else "lightly swung" if swing < 0.6 else "swung"
        lines.append(f"Drums ({drums['steps']} steps a bar): {'; '.join(words) or 'sparse'}; {feel}.")
    else:
        lines.append("Drums: none heard.")
    for part, entry in summary["parts"].items():
        name = PART_NAMES.get(part, part.replace("_", " ").capitalize())
        songs = "every song" if entry["songs"] == count else f"{entry['songs']} of {count} songs"
        if part == "drums":
            continue
        voices = "chords" if entry["voices"] >= 1.8 else "single notes"
        lines.append(
            f"{name}: in {songs}, {note_name(entry['low'])} to {note_name(entry['high'])}, "
            f"about {per_bar(entry['notesPerBar'])} notes a bar, mostly {voices}."
        )
    sections = summary["sections"]
    if sections["lengths"]:
        usual = sections["lengths"][0]["bars"]
        others = [str(s["bars"]) for s in sections["lengths"][1:3]]
        also = f" (also {' and '.join(others)})" if others else ""
        lines.append(
            f"Sections: usually {usual} bars long{also}; a song is about {sections['songBars']:.0f} "
            f"bars in {sections['perSong']:.0f} sections."
        )
    loudness = summary["loudness"]
    if loudness["lufs"] is not None:
        span = loudness["lufsRange"]
        text = f"Loudness: about {loudness['lufs']:.0f} LUFS"
        if span and span[1] - span[0] >= 1:
            text += f" ({span[0]:.0f} to {span[1]:.0f})"
        if loudness["truePeak"] is not None:
            text += f", peaks up to {loudness['truePeak']:.1f} dBTP"
        if loudness["crest"] is not None:
            text += "; dense and compressed" if loudness["crest"] < 10 else "; open, with room for the peaks"
        if loudness["contrast"] is not None:
            text += f"; the quietest section is {loudness['contrast']:.0f} dB below the loudest"
        lines.append(text + ".")
    return lines
