"""The style of a few reference songs, for the Track creator: what the songs share, as plain numbers.

Spinus analyzes each song with the Analyzer's steps (tempo and beats, key, loudness, stems, the stems'
notes and drum hits) and gives the results here. `analyze_song` turns one song's results into its
*features* (kept by Spinus per song, so a song added again is not analyzed again); `summarize` combines
the features of all the songs into the *style summary*. The summary is the input of the Track creator's
later steps (choosing sounds, writing a new song with rules), so it holds explicit numbers: the drums'
probability per sixteenth step, how often each chord progression comes, section lengths in bars, and each
part's register and note density. It also says it all in plain words (`plain`).

Everything here is measured with NumPy: no model, nothing downloaded.

- **Key**: several independent estimates vote (estimate_key), with a confidence per song.
- **Chords**, one per bar: the harmonic parts' notes (bass, piano, guitar, "other"), weighed by how long
  and loud they sound, short ones left out, plus the harmonic stems' chroma, compared with chord templates
  (major, minor, diminished, suspended, sevenths); the bass note held longest favours its root. Named as
  Roman numerals from the song's key (in A minor, A minor is `i`, C major `bIII`).
- **Drum groove** (drum_grid): onsets in the drum stem's kick, snare and hats bands on every sixteenth,
  on beats moved onto the drums, with the recognised hits as a second opinion; groove_of, from the hits
  alone, when there is no stem. Swing: where the off-beat sixteenths fall between two eighths (0.5 is
  straight, 0.67 a triplet feel). Unlike grooves are grouped, not averaged.
- **Sections**: the mix's chroma and loudness per bar; a checkerboard kernel along the bars'
  self-similarity finds where the music changes (Foote's novelty), on the song's phrase grid
  (phrase_boundaries); sections that sound alike get the same letter (A, B, A, ...).
- **Progressions**: 4-chord loops counted whichever chord they start on; shared only when two songs play
  one at least SHARED_TIMES times.
"""

from __future__ import annotations

import itertools
import math
import statistics
from collections import Counter
from typing import Any

import numpy as np

FEATURES_VERSION = 4  # change when what analyze_song measures changes (Spinus then analyzes songs again)
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


SHORTEST_NOTE = 0.12  # seconds: shorter notes (passing notes, transcription specks) say little about the chord


def chords_of(
    song: dict[str, Any], bars: list[float], heard: tuple[np.ndarray, np.ndarray] | None = None
) -> list[dict[str, Any]]:
    """One chord per bar from the harmonic parts' notes: [{bar, root, quality, name, roman}] (bars with
    no harmony are left out). Notes count by length and loudness, those under SHORTEST_NOTE not at all; the
    bass note heard longest in the bar (when it is most of the bass) favours chords on that root. `heard`
    (chroma frames and their times, from the harmonic stems' audio) is added as a second opinion, so a bar
    the transcription got wrong is pulled back toward what sounds."""
    parts = song.get("parts") or {}
    notes = [
        (n, stem)
        for stem, found in parts.items()
        if stem in HARMONIC
        for n in found or []
        if float(n["end"]) - float(n["start"]) >= SHORTEST_NOTE
    ]
    if not notes or len(bars) < 2:
        return []
    tonic = int((song.get("key") or {}).get("tonic", 0))
    length = bars[-1] - bars[-2]
    edges = [*bars, bars[-1] + length]
    found = []
    for index in range(len(edges) - 1):
        start, end = edges[index], edges[index + 1]
        profile = np.zeros(12)
        bass = np.zeros(12)
        for note, stem in notes:
            overlap = min(float(note["end"]), end) - max(float(note["start"]), start)
            if overlap <= 0:
                continue
            pitch = int(note["pitch"])
            weight = overlap * max(0.1, float(note.get("velocity", 0.8)))
            profile[pitch % 12] += weight
            if stem == "bass":
                bass[pitch % 12] += weight
        if heard is not None and profile.sum() > 0:
            frames, times = heard
            inside = (times >= start) & (times < end)
            if inside.any():
                sound = frames[inside].mean(axis=0)
                profile = profile / profile.sum() + 0.5 * sound / max(float(sound.sum()), 1e-9)
        root_heard = int(np.argmax(bass)) if bass.sum() > 0 and bass.max() >= 0.4 * bass.sum() else None
        chord = best_chord(profile, root_heard)
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


DRUM_BANDS = {"kick": (40.0, 120.0), "snare": (1500.0, 5000.0), "hats": (7000.0, 11000.0)}


def band_onsets(audio: np.ndarray, rate: int) -> tuple[dict[str, np.ndarray], float]:
    """How sharply each drum band gets louder, frame by frame (spectral flux of the log spectrum, per
    band: kick 40-120 Hz, snare 1.5-5 kHz, hats 7-11 kHz), and the frames' spacing in seconds."""
    mono = audio.mean(axis=0) if audio.ndim == 2 else audio
    size = 1 << max(8, round(math.log2(0.046 * rate)))  # about 46 ms
    hop = size // 8
    padded = np.pad(mono, (size // 2, size // 2))
    frames = np.lib.stride_tricks.sliding_window_view(padded, size)[::hop]
    magnitude = np.log1p(100 * np.abs(np.fft.rfft(frames * np.hanning(size), axis=1)))
    freqs = np.fft.rfftfreq(size, 1 / rate)
    rise = np.maximum(np.diff(magnitude, axis=0, prepend=magnitude[:1]), 0)
    bands = {}
    for name, (low, high) in DRUM_BANDS.items():
        inside = (freqs >= low) & (freqs < min(high, rate / 2))
        bands[name] = rise[:, inside].mean(axis=1) if inside.any() else np.zeros(len(rise))
    return bands, hop / rate


def loud_split(values: np.ndarray, busy: bool = False) -> tuple[float, float]:
    """A song's own line between "a hit" and "not a hit" for one band: three groups of its values (log)
    by k-means (none, bleed, hits); returns (the line between the top two, the hits' centre)."""
    logs = np.log(values + 1e-6)
    centres = np.percentile(logs, [10, 50, 95])
    for _ in range(30):
        nearest = np.argmin(np.abs(logs[:, None] - centres[None]), axis=1)
        centres = np.array([logs[nearest == k].mean() if (nearest == k).any() else centres[k] for k in range(3)])
    centres.sort()
    # `busy` (the hats, often on every sixteenth): when the top two groups are close (under a factor of
    # two), both are hits, and the line is below them both. Not for the kick and snare bands, where the middle
    # group is the other drum's bleed.
    if busy and centres[2] - centres[1] < math.log(2.0):
        return float((centres[0] + centres[1]) / 2), float(centres[2])
    return float((centres[1] + centres[2]) / 2), float(centres[2])


def aligned_beats(total: np.ndarray, step: float, beats: np.ndarray) -> np.ndarray:
    """The beats moved onto the drums: where the drums' onsets peak within 50 ms of each beat, smoothed over
    nine beats. Beat times and onsets can be some 10-30 ms apart (one finds the beat, the other the attack),
    which is a large part of a sixteenth at a fast tempo."""
    reach = int(0.05 / step)
    loud = np.percentile(total, 75)
    offsets = []
    for beat in beats:
        i = int(beat / step)
        part = total[max(0, i - reach) : i + reach + 1]
        offsets.append((int(np.argmax(part)) - min(reach, i)) * step if len(part) and part.max() > loud else np.nan)
    found = np.array(offsets)
    smooth = np.zeros(len(found))
    for k in range(len(found)):
        near = found[max(0, k - 4) : k + 5]
        smooth[k] = float(np.nanmedian(near)) if np.isfinite(near).any() else 0.0
    moved: np.ndarray = beats + smooth
    return moved


def drum_grid(audio: np.ndarray, rate: int, song: dict[str, Any]) -> dict[str, Any] | None:
    """The groove from the drum stem itself, not from guessed drum kinds: for every sixteenth of every bar,
    whether the kick band, the snare band and the hats band have an onset there (each against the song's own
    loudness of that band), on beats moved onto the drums' onsets. A step whose low end outweighs its snare
    band (for this song) counts as kick, the other way as snare, and both when both are strong. When the
    snares fall on beats 1 and 3 rather than 2 and 4, the bar is taken to start a beat later (Beat This!
    sometimes places the downbeat a beat off). Swing: where between two eighths the off-beat hits (hats or
    snare band) fall, when the song plays sixteenths at all."""
    beats = np.array([float(b) for b in song.get("beats") or []])
    bars = bar_starts(song)
    if len(beats) < 8 or not bars:
        return None
    per_bar = max(1, int(song.get("beatsPerBar") or 4))
    steps = per_bar * 4
    bands, step = band_onsets(audio, rate)
    total = np.sum([v / max(float(np.percentile(v, 99)), 1e-9) for v in bands.values()], axis=0)
    moved = aligned_beats(total, step, beats)
    first = int(np.argmin(np.abs(beats - bars[0])))
    values: dict[str, list[float]] = {name: [] for name in bands}
    places = []
    windows = []  # each step's time span: half a sixteenth either side
    for b in range(first, len(moved) - 1):
        length = moved[b + 1] - moved[b]
        reach = max(1, int(length / 8 / step))
        for q in range(4):
            windows.append((moved[b] + length * (q - 0.5) / 4, moved[b] + length * (q + 0.5) / 4))
            i = int((moved[b] + length * q / 4) / step)
            for name, onsets in bands.items():
                part = onsets[max(0, i - reach) : i + reach + 1]
                values[name].append(float(part.max()) if len(part) else 0.0)
            places.append((b - first) * 4 + q)
    if not places:
        return None
    where = np.array(places)
    v = {name: np.array(found) for name, found in values.items()}
    kick_line, kick_hits = loud_split(v["kick"])
    snare_line, _ = loud_split(v["snare"])
    hats_line, _ = loud_split(v["hats"], busy=True)
    low, mid = np.log(v["kick"] + 1e-6), np.log(v["snare"] + 1e-6)
    kick_on, snare_on = low > kick_line, mid > snare_line
    balance = low - mid
    either = kick_on | snare_on
    lower, upper = np.percentile(balance[either], [40, 60]) if either.any() else (0.0, 0.0)
    hit = {
        "kick": kick_on & ((balance >= lower) | (low >= kick_hits)),
        "snare": snare_on & (balance <= upper),
        "hats": np.log(v["hats"] + 1e-6) > hats_line,
    }
    # The recognised hits (spinus_ai.drums) are a second opinion on kick and snare, on the same moved grid:
    # each method alone misreads some real kits (the bands hear a snare's body as kick, the recogniser hears
    # snares as kicks or toms), so a step's chance is the mean of both. The hats come from the band only: the
    # recogniser took many real hats for cymbals.
    recognised: dict[str, np.ndarray] = {}
    hits = sorted((float(h["time"]), str(h.get("kind", ""))) for h in song.get("drums") or [])
    if hits:
        times = np.array([t for t, _ in hits])
        for lane in ("kick", "snare"):
            kinds = LANES[lane]
            heard = np.zeros(len(windows), dtype=bool)
            for k, (start, end) in enumerate(windows):
                a, b2 = np.searchsorted(times, [start, end])
                heard[k] = any(hits[j][1] in kinds for j in range(a, b2))
            recognised[lane] = heard
    any_hit = hit["kick"] | hit["snare"] | hit["hats"]
    active = sorted({int(p) // steps for p in where[any_hit]})
    if not active:
        return None
    in_active = np.isin(where // steps, active)

    def lane_of(on: np.ndarray) -> list[float]:
        return [float(on[in_active & (where % steps == s)].mean()) for s in range(steps)]

    lanes: dict[str, list[float]] = {}
    for name, on in hit.items():
        chances = lane_of(on)
        if name in recognised:
            chances = [(a + b) / 2 for a, b in zip(chances, lane_of(recognised[name]), strict=True)]
        lanes[name] = [round(p, 3) for p in chances]
    shifted = False
    if per_bar == 4:
        snare = lanes["snare"]
        odd, even = snare[0] + snare[8], snare[4] + snare[12]
        if odd > 1.5 * even and odd >= 0.8:
            lanes = {name: lane[4:] + lane[:4] for name, lane in lanes.items()}
            shifted = True
    # Swing: the off-beat sixteenths' place between two eighths, if they are played in a fifth of the eighths.
    offbeat = bands["hats"] + bands["snare"]
    line = float(np.percentile(offbeat, 85))
    shares, eighths = [], 0
    for b in range(first, len(moved) - 1):
        for half in (0, 1):
            start = moved[b] + (moved[b + 1] - moved[b]) * half / 2
            length = (moved[b + 1] - moved[b]) / 2
            i0, i1 = int((start + 0.4 * length) / step), int((start + 0.8 * length) / step)
            eighths += 1
            part = offbeat[i0:i1]
            if len(part) and part.max() > line:
                shares.append(((i0 + int(np.argmax(part))) * step - start) / length)
    played = len(shares) / max(1, eighths)
    swing = round(float(np.median(shares)), 3) if played >= 0.2 else 0.5
    return {
        "steps": steps,
        "bars": len(active),
        "lanes": lanes,
        "swing": swing,
        "sixteenths": round(played, 2),
        "offsetMs": round(float(np.median(moved - beats)) * 1000, 1),
        "barShifted": shifted,
        "from": "drum stem and recognised hits" if recognised else "drum stem",
    }


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


def phrase_boundaries(novelty: np.ndarray, phrase: int) -> list[int]:
    """Where sections start (bars), on the song's phrase grid: songs change every 4 or 8 bars, so a change
    found a bar early or late belongs on the nearest phrase line. The grid's offset is the one the strongest
    changes fall on (a song may start with a pickup). Sections shorter than a phrase join the next one, and
    one longer than 8 phrases is split where it changes most."""
    count = len(novelty)
    if count < 2 * phrase or novelty.max() <= 1e-9:
        return [0]
    near = np.array([novelty[max(0, i - 1) : i + 2].max() for i in range(count)])
    offset = max(range(phrase), key=lambda o: float(sum(novelty[i] for i in range(o, count, phrase) if i > 0)))
    grid = [i for i in range(offset, count, phrase) if 0 < i < count]
    threshold = float(novelty.mean() + 0.5 * novelty.std())
    boundaries = [0] + [i for i in grid if near[i] >= threshold]
    # Too short (under two phrases in a song of a dozen phrases or more, else under one): joined to the next
    # (the last to the one before). Real songs' 4-bar "sections" were mostly fills and transitions.
    shortest = 2 * phrase if count >= 12 * phrase else phrase
    merged = [0]
    for start in boundaries[1:]:
        if start - merged[-1] >= shortest and count - start >= shortest:
            merged.append(start)
    # Too long: split at the strongest change inside, two phrases from either end.
    changed = True
    while changed:
        changed = False
        for start, end in itertools.pairwise([*merged, count]):
            if end - start > 8 * phrase:
                inside = [i for i in grid if start + 2 * phrase <= i <= end - 2 * phrase]
                if inside:
                    merged.append(max(inside, key=lambda i: near[i]))
                    merged.sort()
                    changed = True
                    break
    return merged


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
    boundaries = phrase_boundaries(novelty, 4 if count >= 16 else 2)
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


# Temperley's (Kostka-Payne) key profiles, a second opinion beside Krumhansl-Kessler's.
TEMPERLEY_MAJOR = np.array([5.0, 2.0, 3.5, 2.0, 4.5, 4.0, 2.0, 4.5, 2.0, 3.5, 1.5, 4.0])
TEMPERLEY_MINOR = np.array([5.0, 2.0, 3.5, 4.5, 2.0, 4.0, 2.0, 4.5, 3.5, 2.0, 1.5, 4.0])


def key_name(index: int) -> str:
    """0-11: C major..B major, 12-23: C minor..B minor."""
    return f"{NAMES[index % 12]} {'major' if index < 12 else 'minor'}"


def key_scores(profile: np.ndarray) -> list[np.ndarray]:
    """How well a pitch-class profile fits each of the 24 keys, with both profiles (correlations)."""
    from .analysis import MAJOR, MINOR

    out = []
    for major, minor in ((MAJOR, MINOR), (TEMPERLEY_MAJOR, TEMPERLEY_MINOR)):
        scores = np.zeros(24)
        if profile.std() > 0:
            for t in range(12):
                scores[t] = np.corrcoef(profile, np.roll(major, t))[0, 1]
                scores[12 + t] = np.corrcoef(profile, np.roll(minor, t))[0, 1]
        out.append(scores)
    return out


def note_profile(notes: list[dict[str, Any]]) -> np.ndarray:
    profile = np.zeros(12)
    for n in notes:
        length = float(n["end"]) - float(n["start"])
        if length >= SHORTEST_NOTE:
            profile[int(n["pitch"]) % 12] += length * max(0.1, float(n.get("velocity", 0.8)))
    return profile


def estimate_key(song: dict[str, Any], mix: np.ndarray | None, stems: np.ndarray | None) -> dict[str, Any] | None:
    """The key, by several independent estimates that vote: the key the Analyzer found (the mix's chroma),
    the mix's chroma, the harmonic stems' chroma (bass and the rest: no drums or voice) and the harmonic
    notes, each with Krumhansl-Kessler's and Temperley's profiles. Their z-scored fits are added; between
    keys sharing their notes or a fifth apart that fit almost as well, the one whose tonic the bass plays
    most wins (the relative major and minor have the same notes). Confidence: how many estimates agree
    ("high": two thirds, "medium": two fifths, "low" below), with the others' keys as alternatives."""
    parts = song.get("parts") or {}
    profiles = {"mix": mix, "stems": stems, "notes": note_profile([n for s in HARMONIC for n in parts.get(s) or []])}
    total = np.zeros(24)
    votes: dict[str, str] = {}
    for label, profile in profiles.items():
        if profile is None or float(np.sum(profile)) <= 0:
            continue
        for kind, scores in zip(("KK", "Temperley"), key_scores(np.asarray(profile, dtype=float)), strict=True):
            if scores.std() > 0:
                total += (scores - scores.mean()) / scores.std()
                votes[f"{label} ({kind})"] = key_name(int(np.argmax(scores)))
    given = song.get("key") or {}
    if given:
        found = int(given.get("tonic", 0)) % 12 + (12 if given.get("scale") == "minor" else 0)
        votes["Analyzer"] = key_name(found)
        total[found] += 1.0  # one more opinion (it is the mix's chroma with Krumhansl-Kessler's profiles)
    if not votes:
        return None
    best = int(np.argmax(total))
    # The tonic's evidence: how long the bass plays each note, the notes it plays where phrases start (every
    # fourth bar) counting three times: a loop such as Am F C G is in A minor because it starts on A.
    bass_notes = parts.get("bass") or []
    bass = note_profile(bass_notes)
    for start in bar_starts(song)[::4]:
        for n in bass_notes:
            if float(n["start"]) - 0.1 <= start < float(n["end"]):
                bass[int(n["pitch"]) % 12] += 2.0 * (float(n["end"]) - float(n["start"]))
    # A key most of the estimates (with the audio, four or more) name stands; the bass decides only between
    # close keys when they are split, or with only the notes to go on.
    named = Counter(votes.values()).most_common(1)[0]
    if len(votes) >= 4 and named[1] / len(votes) > 0.5:
        best = next(i for i in range(24) if key_name(i) == named[0])
    elif bass.sum() > 0:
        tonic = best % 12
        relative = (tonic + 9) % 12 + 12 if best < 12 else (tonic + 3) % 12
        mode = 12 if best >= 12 else 0
        near = [best, relative, (tonic + 7) % 12 + mode, (tonic + 5) % 12 + mode]
        # The relative key shares every note, so profiles barely tell them apart: a wider margin for it.
        margins = (0.0, 0.3, 0.15, 0.15)
        close = [k for k, m in zip(near, margins, strict=True) if total[k] >= total[best] - m * abs(total[best])]
        # Another key wins only if the bass clearly plays its tonic more (a fifth more).
        stronger = [k for k in close if bass[k % 12] > 1.2 * bass[best % 12]]
        if stronger:
            best = max(stronger, key=lambda k: (bass[k % 12], total[k]))
    agree = sum(1 for v in votes.values() if v == key_name(best)) / len(votes)
    return {
        "tonic": best % 12,
        "scale": "minor" if best >= 12 else "major",
        "name": key_name(best),
        "confidence": "high" if agree >= 2 / 3 else "medium" if agree >= 0.4 else "low",
        "agreement": round(agree, 2),
        "alternatives": sorted({v for v in votes.values() if v != key_name(best)}),
        "votes": votes,
    }


def tempo_doubt(tempo: float | None, groove: dict[str, Any] | None) -> str | None:
    """Whether the beat found may be twice or half the felt one: "half" when a fast song (140 BPM and up)
    has its snare on every beat (the felt backbeat is then every other one), "double" when a slow one (85
    and under) has it on the off-beat eighths."""
    if not tempo or groove is None or groove.get("steps") != 16:
        return None
    snare = groove["lanes"]["snare"]
    if tempo >= 140 and min(snare[0], snare[4], snare[8], snare[12]) >= 0.5:
        return "half"
    if tempo <= 85 and min(snare[2], snare[6], snare[10], snare[14]) >= 0.5 and max(snare[4], snare[12]) < 0.3:
        return "double"
    return None


def analyze_song(
    song: dict[str, Any],
    audio: np.ndarray | None = None,
    rate: int = 0,
    stems: dict[str, tuple[np.ndarray, int]] | None = None,
) -> dict[str, Any]:
    """One song's features from the Analyzer's results: {name, tempo, tempoDoubt, beatsPerBar, key (with
    its confidence), keyFound, bars, seconds, chords, groove, parts, sections, loudness}. `song`: {name,
    tempo, beatsPerBar, firstDownbeat, beats, downbeats (seconds), key {tonic, scale, name}, loudness (an
    ai.v1.mix.analyze insert: loudness, truePeak, crest), parts {stem: [{start, end, pitch, velocity}]
    (seconds)}, drums [{time, kind}]}. `audio` (the mix, [channels, samples]) gives the sections and a key
    estimate; `stems` (name: (audio, rate)) the drums' grid from the drum stem and the harmonic stems'
    chroma for the key and the chords."""
    from .analysis import chroma as chroma_of

    stems = stems or {}
    bars = bar_starts(song)
    seconds = float(audio.shape[-1] / rate) if audio is not None and rate > 0 else float(song.get("seconds") or 0.0)
    loudness = song.get("loudness") or {}
    sections = sections_of(audio, rate, bars) if audio is not None and rate > 0 else []
    levels = [s["level"] for s in sections]
    mix_profile = chroma_of(audio, rate) if audio is not None and rate > 0 else None
    harmonic = [stems[name] for name in HARMONIC if name in stems]
    stem_profile = sum((chroma_of(a, r) for a, r in harmonic), np.zeros(12)) if harmonic else None
    heard = None
    if harmonic:
        frames = [frames_of(a, r) for a, r in harmonic]
        shortest = min(len(f[0]) for f in frames)
        heard = (np.sum([f[0][:shortest] for f in frames], axis=0), frames[0][2][:shortest])
    key = estimate_key(song, mix_profile, stem_profile)
    keyed = {**song, "key": key} if key else song
    groove = drum_grid(stems["drums"][0], stems["drums"][1], song) if "drums" in stems else None
    if groove is None:
        groove = groove_of(song)
    tempo = float(song["tempo"]) if song.get("tempo") else None
    given = song.get("key") or {}
    return {
        "version": FEATURES_VERSION,
        "name": str(song.get("name", "")),
        "tempo": tempo,
        "tempoDoubt": tempo_doubt(tempo, groove),
        "beatsPerBar": int(song.get("beatsPerBar") or 4),
        "key": key,
        "keyFound": str(given.get("name", "")) if given else None,
        "bars": len(bars),
        "seconds": round(seconds, 2),
        "chords": chords_of(keyed, bars, heard),
        "groove": groove,
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


SHARED_TIMES = 3


def supporting(times: Counter[int]) -> int:
    """How many songs play a loop at least SHARED_TIMES times."""
    return sum(1 for n in times.values() if n >= SHARED_TIMES)


def rotation_key(window: tuple[str, ...]) -> tuple[str, ...]:
    """A chord loop whichever chord it is heard from (i bVI bIII bVII and bVI bIII bVII i are one loop)."""
    return min(window[i:] + window[:i] for i in range(len(window)))


def groove_groups(grooves: list[dict[str, Any]], alike: float = 0.8) -> list[list[int]]:
    """Grooves grouped by likeness (the cosine of their kick, snare and hats steps): each joins the first
    group whose first groove it resembles; the biggest groups first."""

    def vector(g: dict[str, Any]) -> np.ndarray:
        return np.concatenate([np.asarray(g["lanes"][lane], dtype=float) for lane in LANES])

    groups: list[list[int]] = []
    for i, g in enumerate(grooves):
        v = vector(g)
        for members in groups:
            w = vector(grooves[members[0]])
            if float(v @ w) / max(float(np.linalg.norm(v) * np.linalg.norm(w)), 1e-9) >= alike:
                members.append(i)
                break
        else:
            groups.append([i])
    return sorted(groups, key=lambda m: (-len(m), m[0]))


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
    progressions: Counter[tuple[str, ...]] = Counter()  # by loop (rotation_key)
    progression_songs: dict[tuple[str, ...], Counter[int]] = {}  # loop -> times in each song
    rotations: dict[tuple[str, ...], Counter[tuple[str, ...]]] = {}  # how each loop was heard
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
            loop = rotation_key(window)
            progressions[loop] += 1
            progression_songs.setdefault(loop, Counter())[index] += 1
            rotations.setdefault(loop, Counter())[window] += 1
    chord_total = sum(roman_counts.values())
    progression_total = sum(progressions.values())
    chords_summary = {
        "barsPerChord": round(median(bars_per_chord) or 0.0, 2) if bars_per_chord else None,
        "common": [{"roman": r, "share": round(n / chord_total, 3)} for r, n in roman_counts.most_common(8)],
        "qualities": {q: round(n / chord_total, 3) for q, n in quality_counts.most_common()},
        "progressions": [
            {
                "chords": list(rotations[p].most_common(1)[0][0]),  # as it was heard most often
                "count": n,
                "share": round(n / progression_total, 3),
                "songs": len(progression_songs[p]),
                # Shared: heard at least SHARED_TIMES times in each of two songs or more, not by chance.
                "shared": supporting(progression_songs[p]) >= 2,
            }
            for p, n in sorted(
                progressions.items(), key=lambda item: (-supporting(progression_songs[item[0]]), -item[1])
            )[:6]
        ],
        "transitions": {
            a: {b: round(n / sum(after.values()), 3) for b, n in after.most_common()}
            for a, after in sorted(transitions.items())
        },
    }

    # Drums: songs with the usual bar length, grouped by how alike their grooves are (a half-time song and a
    # four-on-the-floor one are not averaged into a groove neither plays); the biggest group is the main one.
    grooves = [(s, s["groove"]) for s in songs if s.get("groove")]
    steps = Counter(g["steps"] for _, g in grooves).most_common(1)[0][0] if grooves else 16
    alike = [(s, g) for s, g in grooves if g["steps"] == steps]
    groups = groove_groups([g for _, g in alike])
    found = []
    for members in groups:
        weight = sum(alike[i][1]["bars"] for i in members)
        found.append(
            {
                "songs": [str(alike[i][0].get("name", "")) for i in members],
                "lanes": {
                    lane: [
                        round(sum(alike[i][1]["lanes"][lane][k] * alike[i][1]["bars"] for i in members) / weight, 3)
                        if weight
                        else 0.0
                        for k in range(steps)
                    ]
                    for lane in LANES
                },
                "swing": round(median([alike[i][1]["swing"] for i in members]) or 0.5, 3),
            }
        )
    drums = {
        "songs": len(grooves),
        "steps": steps,
        "grooves": found,
        "lanes": found[0]["lanes"] if found else {lane: [0.0] * steps for lane in LANES},
        "swing": round(median([g["swing"] for _, g in alike]) or 0.5, 3),
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
        "tempoDoubts": {str(s.get("name", "")): s["tempoDoubt"] for s in songs if s.get("tempoDoubt")},
        "keyConfidence": {
            str(s.get("name", "")): s["key"].get("confidence")
            for s in songs
            if s.get("key") and "confidence" in s["key"]
        },
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


def plural(count: int, word: str) -> str:
    return f"{count} {word}{'s' if count != 1 else ''}"


def feel_of(swing: float) -> str:
    """Within about 0.06 of straight is measuring noise on real songs."""
    return "straight" if swing < 0.56 else "lightly swung" if swing < 0.62 else "swung"


def groove_words(groove: dict[str, Any]) -> str:
    words = []
    for lane, label in (("kick", "kick"), ("snare", "snare or clap"), ("hats", "hi-hats")):
        probabilities = groove["lanes"][lane]
        often = steps_text(probabilities, 0.5, 2.0)
        # Below that, only the few likeliest steps: a long list of 20-40 % steps says nothing.
        likely = sorted((i for i, p in enumerate(probabilities) if 0.25 <= p < 0.5), key=lambda i: -probabilities[i])
        sometimes = ", ".join(str(i + 1) for i in sorted(likely[:3]))
        if often:
            words.append(f"{label} on steps {often}" + (f" (sometimes also {sometimes})" if sometimes else ""))
        elif sometimes:
            words.append(f"{label} with no steady place (most often on {sometimes})")
    return f"{'; '.join(words) or 'sparse'}; {feel_of(groove['swing'])}"


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
        doubts = summary.get("tempoDoubts") or {}
        halves = [n for n, d in doubts.items() if d == "half"]
        doubles = [n for n, d in doubts.items() if d == "double"]
        note = ""
        if halves:
            note += f" {plural(len(halves), 'song')} may be felt at half that speed ({', '.join(halves)})."
        if doubles:
            note += f" {plural(len(doubles), 'song')} may be felt at twice that speed ({', '.join(doubles)})."
        lines.append(f"Tempo: {span}; {meter} beats per bar.{note}")
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
        unsure = [n for n, c in (summary.get("keyConfidence") or {}).items() if c == "low"]
        if unsure:
            lines.append(f"Not sure of the key of: {', '.join(unsure)} (the estimates disagree).")
    chords = summary["chords"]
    if chords["common"]:
        every = chords["barsPerChord"] or 1.0
        rate = "a new chord every bar" if every < 1.5 else f"a new chord about every {every:.0f} bars"
        common = ", ".join(c["roman"] for c in chords["common"][:5])
        lines.append(f"Chords: {rate}; most used (from the key's first note): {common}.")
        if chords["progressions"]:
            top = chords["progressions"][0]
            if top.get("shared"):
                lines.append(f"Shared progression: {' - '.join(top['chords'])} (in {top['songs']} songs).")
            else:
                lines.append(
                    f"No progression is shared by two songs; the one repeated most is {' - '.join(top['chords'])} "
                    f"({top['count']} times, in one song)."
                )
    else:
        lines.append("Chords: none found (no bass or chord parts heard).")
    drums = summary["drums"]
    if drums["songs"]:
        groups = drums.get("grooves") or [{"songs": [], "lanes": drums["lanes"], "swing": drums["swing"]}]
        main = groups[0]
        if len(groups) == 1 or len(main["songs"]) == drums["songs"]:
            lines.append(f"Drums ({drums['steps']} steps a bar): {groove_words(main)}.")
        elif len(main["songs"]) >= 2:
            lines.append(
                f"Main groove ({len(main['songs'])} of {drums['songs']} songs, {drums['steps']} steps a bar): "
                f"{groove_words(main)}."
            )
            for other in groups[1:3]:
                lines.append(f"Another groove ({', '.join(other['songs'])}): {groove_words(other)}.")
        else:
            lines.append(
                f"Drums: every song has a groove of its own ({drums['steps']} steps a bar); "
                f"for example {', '.join(main['songs'])}: {groove_words(main)}."
            )
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
