"""The Track creator's song writer: a new song in the style of the reference songs, from their style summary.

Offline by default: rules and seeded chance turn the summary's explicit numbers (spinus_ai.style.summarize)
into a song, with no model and no AI connection. When the user asks and a connection is set up, an LLM may
vary the plan (the sections' order and lengths, the chord loops, the melody's shape) through the
refine_song tool; what it answers is checked against the style, and the rules then write every note, so
nothing it says can step outside the style's limits. Without a connection, or when its answer is not
usable, the rules' song stands.

What is written, from the summary:

- **Tempo and key**: the songs' median tempo moved by up to 3 % (inside their range); the key of one of
  the songs of a mode drawn by the modes' shares, its tonic sometimes moved a step.
- **Sections**: a form of intro, verses, choruses, a bridge and an outro, about as many sections and bars
  as the songs have, each section's length drawn from the songs' section lengths, on 4-bar phrases. Alike
  sections share one pattern (so a chorus is the same chorus each time).
- **Chords**: a loop of four chords per section, drawn from the chord-to-chord transitions of the songs in
  that mode (Markov chain, the likelier loops more often), from their chord vocabulary only (chords heard
  in at least 2 % of their bars). A loop a reference song plays as its own (three times or more) is used at
  most once, so the song is a new combination, not a copy of one song. A chord lasts one bar, or two when
  the songs change chords every two bars.
- **Drums**: each step of the groove's kick, snare and hats lanes: always where the songs play it at least
  60 % of the time, by chance where less (fewer in quiet sections); a second bar variant every fourth bar;
  a fill (snare, toms) at the end of each section and a crash at the start of the next.
- **Bass**: the chords' roots (sometimes the fifth or the octave, and a step leading into the next chord)
  in the bass part's register, on the steps the kick plays most, as many notes a bar as the bass part.
- **Chords part** (the songs' "other" instruments): the chords voiced close to the part's middle note, each
  voicing the nearest to the one before, as many voices as the part plays (or broken into single notes when
  it plays one at a time), with the part's notes a bar.
- **Lead** (the vocals' tune): two-bar phrases in the vocals' register and density, rhythm repeated with
  variation (A A' B A''), chord notes on the strong beats and when held, notes of the key between, mostly
  steps with few leaps, in a contour per section (rise, fall, arch, wave). Only the style is used: no
  melody of the reference songs is reproduced (they are not even read: the summary has no melodies).

**The parts** (song["layers"], their notes in song["layerParts"]): the user's list, each part with a role
(ROLES), written exactly: "drums", "bass", "chords", "lead" take the four parts above; "pad" holds the chords,
"arp" plays them one note after another, "counter" is a slower line under the lead, "sub" holds the roots low
in the choruses; "riser" (a held note over the two bars before each chorus) and "impact" (a kick and a crash
where each chorus and bridge starts) are effects placed on the Playlist (song["fx"]). Two parts of one role
get complementary material: a second chords part breaks the chords into an arpeggio an octave up, a third
plays off-beat stabs; a second lead harmonises the first a third below; a second bass, pad, arp or
counter-melody plays an octave higher with a rhythm of its own; a second drums part plays percussion. The
sections set who plays: thin verses, full choruses, a bridge that breaks down and builds again. Without a
list, the plan's "size" proposes one: "sketch" the four parts, "full" about ten (with a sub bass when the bass
sits high enough to leave room for one). Parts of the four roles' first instances follow those parts' seeds,
the others their own (seeds["layers"]), so redoing a part redoes what is made from it.

Each part and each section has its own seed (song["seeds"]), so redoing one part (a new seed for it) or
one section keeps everything else as it was; the same seeds always write the same song.

Notes are in ticks (960 per beat) from the start of their section's pattern; drum notes name a drum
("kick", "snare", "hat", "openHat", "crash", "tomLow", "tomMid", "tomHigh"), which Spinus maps to the
channel's instrument. Pure Python: no NumPy, nothing downloaded.
"""

from __future__ import annotations

import hashlib
import itertools
import json
import math
import random
import re
from collections import Counter
from dataclasses import dataclass, field
from typing import Any

import httpx

from .assistant import NoConnection, Say, answered_by, complete_with_fallback
from .connections import ConnectionStore
from .gateway import Message, Request, Text, Tool
from .keynames import key_name, tonic_of
from .routing import Routing
from .usage import UsageLog

SONG_VERSION = 1
TICKS_PER_BEAT = 960
TICKS_PER_STEP = TICKS_PER_BEAT // 4  # a sixteenth

NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
SCALES = {"major": (0, 2, 4, 5, 7, 9, 11), "minor": (0, 2, 3, 5, 7, 8, 10)}
# As spinus_ai.style.QUALITIES (a test checks they agree): intervals from the root.
QUALITIES: dict[str, tuple[int, ...]] = {
    "maj": (0, 4, 7),
    "min": (0, 3, 7),
    "dim": (0, 3, 6),
    "sus4": (0, 5, 7),
    "7": (0, 4, 7, 10),
    "maj7": (0, 4, 7, 11),
    "m7": (0, 3, 7, 10),
}
QUALITY_SUFFIX = {"maj": "", "min": "m", "dim": "dim", "sus4": "sus4", "7": "7", "maj7": "maj7", "m7": "m7"}

PARTS = ("drums", "bass", "chords", "lead")
# The summary's part (a stem) each part of the song is written from.
SUMMARY_PART = {"drums": "drums", "bass": "bass", "chords": "other", "lead": "vocals"}
PART_WORDS = {"drums": "Drums", "bass": "Bass", "chords": "Chords", "lead": "Lead"}
DRUMS = ("kick", "snare", "hat", "openHat", "crash", "tomLow", "tomMid", "tomHigh")

KINDS = ("intro", "verse", "chorus", "bridge", "outro")
KIND_WORDS = {"intro": "Intro", "verse": "Verse", "chorus": "Chorus", "bridge": "Bridge", "outro": "Outro"}
ENERGY = {"intro": 0.45, "verse": 0.7, "chorus": 1.0, "bridge": 0.6, "outro": 0.5}
CONTOURS = ("rise", "fall", "arch", "wave", "valley")
# Song forms by how many sections they have; the plan takes the one nearest the songs' count.
FORMS: tuple[tuple[str, ...], ...] = (
    ("intro", "verse", "chorus", "verse", "chorus", "outro"),
    ("intro", "verse", "chorus", "verse", "chorus", "chorus", "outro"),
    ("intro", "verse", "chorus", "verse", "chorus", "bridge", "chorus", "outro"),
    ("intro", "verse", "verse", "chorus", "verse", "chorus", "bridge", "chorus", "outro"),
    ("intro", "verse", "chorus", "verse", "chorus", "bridge", "chorus", "chorus", "outro"),
    ("intro", "verse", "verse", "chorus", "verse", "verse", "chorus", "bridge", "chorus", "outro"),
)

SIZES = ("sketch", "full")
# The roles a part can play: its default name, family (the Channel Rack group), the part of the four it is made
# from ("fx": an effect), the role of the sound it wants (as the sound index's roles) and whether it plays drums.
ROLES: dict[str, tuple[str, str, str, str, bool]] = {
    "drums": ("Drums", "Drums", "drums", "drums", True),
    "bass": ("Bass", "Bass", "bass", "bass", False),
    "sub": ("Sub Bass", "Bass", "bass", "bass", False),
    "chords": ("Chords", "Synths", "chords", "keys", False),
    "pad": ("Pad", "Synths", "chords", "pad", False),
    "arp": ("Arp", "Synths", "chords", "pluck", False),
    "lead": ("Lead", "Leads", "lead", "lead", False),
    "counter": ("Counter-melody", "Leads", "lead", "lead", False),
    "riser": ("Riser", "FX", "fx", "fx", False),
    "impact": ("Impacts", "FX", "fx", "drums", True),
    "rhythm-guitar": ("Rhythm guitar", "Guitars", "chords", "keys", False),
    "lead-guitar": ("Lead guitar", "Guitars", "lead", "lead", False),
    "acoustic-guitar": ("Acoustic guitar", "Guitars", "chords", "pluck", False),
    "clean-guitar": ("Clean guitar", "Guitars", "chords", "pluck", False),
    "piano": ("Piano", "Keys", "chords", "keys", False),
    "organ": ("Organ", "Keys", "chords", "keys", False),
    "strings": ("Strings", "Orchestra", "chords", "pad", False),
    "brass": ("Brass", "Orchestra", "chords", "lead", False),
    "woodwinds": ("Woodwinds", "Orchestra", "lead", "lead", False),
    "choir": ("Choir", "Orchestra", "chords", "pad", False),
    "percussion": ("Percussion", "Drums", "drums", "drums", True),
}
SKETCH = ("drums", "bass", "chords", "lead")
FULL = ("drums", "bass", "sub", "chords", "pad", "arp", "lead", "counter", "riser", "impact")
MOST_PARTS = 16
SUB_RANGE = (24, 40)  # a sub bass's notes
SUB_ROOM = 40  # the bass's middle note from which a sub bass fits below it

SHARED_TIMES = 3  # as style.SHARED_TIMES: a loop a song plays this often is that song's own
LEAST_SHARE = 0.02  # chords heard in fewer of the songs' bars are left out of the vocabulary
MOST_BARS = 256
SECTION_BARS = (4, 32)


# ---------------------------------------------------------------------------------------------------
# Small helpers


def mix_seed(*parts: object) -> int:
    """A seed from several values, the same on every computer and every run (Python's hash() is not)."""
    digest = hashlib.sha256(repr(parts).encode("utf-8")).digest()
    return int.from_bytes(digest[:4], "big") & 0x7FFFFFFF


def rng_for(*parts: object) -> random.Random:
    return random.Random(mix_seed(*parts))


def weighted(rng: random.Random, options: dict[Any, float]) -> Any:
    items = [(k, w) for k, w in options.items() if w > 0]
    if not items:
        raise ValueError("nothing to choose from")
    total = sum(w for _, w in items)
    pick = rng.random() * total
    for k, w in items:
        pick -= w
        if pick <= 0:
            return k
    return items[-1][0]


def rotation_key(window: tuple[str, ...]) -> tuple[str, ...]:
    """A chord loop whichever chord it is heard from (as style.rotation_key)."""
    return min(window[i:] + window[:i] for i in range(len(window)))


def note_name(midi: int) -> str:
    return f"{NAMES[midi % 12]}{midi // 12 - 1}"


def key_of_name(name: str) -> tuple[int, str] | None:
    parts = name.split()
    if len(parts) != 2 or (tonic := tonic_of(parts[0])) is None or parts[1] not in SCALES:
        return None
    return tonic, parts[1]


ROMAN = re.compile(r"^(b?)(VII|VI|IV|V|III|II|I|vii|vi|iv|v|iii|ii|i)(°|sus4|maj7|7)?$")
DEGREE = {"I": 0, "II": 2, "III": 4, "IV": 5, "V": 7, "VI": 9, "VII": 11}


def parse_roman(text: str) -> tuple[int, str] | None:
    """A Roman numeral as style.roman writes it ("bIII", "vi°", "Vsus4", "ii7") -> (semitones above the
    tonic, quality); None for anything else."""
    match = ROMAN.match(text)
    if not match:
        return None
    flat, numeral, suffix = match.groups()
    offset = (DEGREE[numeral.upper()] - (1 if flat else 0)) % 12
    lower = numeral.islower()
    if suffix == "°":
        return (offset, "dim") if lower else None
    if suffix == "sus4":
        return (offset, "sus4") if not lower else None
    if suffix == "maj7":
        return (offset, "maj7") if not lower else None
    if suffix == "7":
        return offset, "m7" if lower else "7"
    return offset, "min" if lower else "maj"


def chord_tones(roman: str, tonic: int) -> list[int]:
    parsed = parse_roman(roman)
    if parsed is None:
        return []
    offset, quality = parsed
    root = (tonic + offset) % 12
    return [(root + i) % 12 for i in QUALITIES[quality]]


def chord_label(roman: str, tonic: int) -> str:
    parsed = parse_roman(roman)
    if parsed is None:
        return roman
    return NAMES[(tonic + parsed[0]) % 12] + QUALITY_SUFFIX[parsed[1]]


def scale_of(key: dict[str, Any]) -> set[int]:
    return {(int(key["tonic"]) + i) % 12 for i in SCALES.get(str(key["scale"]), SCALES["major"])}


def allowed_pitch_classes(chord: list[int], scale: set[int]) -> set[int]:
    """The notes that fit over a chord: its own notes, and the key's notes that do not rub a semitone against
    one of its notes from outside the key (the raised seventh of a major V in a minor key)."""
    outside = [p for p in chord if p not in scale]
    fits = {p for p in scale if not any((p - q) % 12 in (1, 11) for q in outside)}
    return set(chord) | fits


# ---------------------------------------------------------------------------------------------------
# The style, read from the summary with defaults for what it lacks


@dataclass
class PartStyle:
    low: int
    median: int
    high: int
    per_bar: float
    voices: float = 1.0
    present: bool = True


DEFAULT_PARTS = {
    "bass": PartStyle(28, 38, 50, 6.0, 1.0, False),
    "other": PartStyle(48, 60, 72, 8.0, 2.5, False),
    "vocals": PartStyle(55, 64, 76, 3.0, 1.0, False),
}
DEFAULT_LANES = {
    "kick": [0.9 if s % 8 == 0 else 0.1 if s % 4 == 2 else 0.0 for s in range(16)],
    "snare": [0.9 if s in (4, 12) else 0.05 for s in range(16)],
    "hats": [0.85 if s % 2 == 0 else 0.15 for s in range(16)],
}


@dataclass
class Vocabulary:
    """The chords of the songs in one mode: how often each is heard, what follows it, their own loops."""

    counts: Counter[str] = field(default_factory=Counter)
    transitions: dict[str, Counter[str]] = field(default_factory=dict)
    own_loops: set[tuple[str, ...]] = field(default_factory=set)  # loops a song plays SHARED_TIMES times
    bars_per_chord: float = 1.0

    def allowed(self) -> list[str]:
        total = sum(self.counts.values())
        return [c for c, n in self.counts.most_common() if total and n / total >= LEAST_SHARE and n >= 2]

    def share(self, roman: str) -> float:
        total = sum(self.counts.values())
        return self.counts[roman] / total if total else 0.0

    def transition(self, a: str, b: str) -> float:
        after = self.transitions.get(a)
        if not after:
            return 0.0
        return after[b] / max(1, sum(after.values()))


@dataclass
class Style:
    tempo: float
    tempo_low: float
    tempo_high: float
    beats_per_bar: int
    steps: int
    keys: list[tuple[int, str, int]]  # tonic, scale, songs
    modes: dict[str, float]
    lanes: dict[str, list[float]]
    swing: float
    parts: dict[str, PartStyle]
    lengths: list[tuple[int, int]]  # section bars (on 4-bar phrases), how many
    sections_per_song: float
    song_bars: float
    vocabularies: dict[str, Vocabulary]
    has_drums: bool
    without: frozenset[str] = frozenset()  # parts the song leaves out (a described song: "no drums")
    double_kick: bool = False  # the drums play a double kick in the choruses (metal)
    key_fixed: bool = False  # a key the user wrote: not moved a step
    guitars: tuple[str, ...] = ()  # the guitar roles the reference songs have (guitar_roles)

    @staticmethod
    def from_summary(summary: dict[str, Any]) -> Style:
        if not isinstance(summary, dict) or summary.get("kind") != "spinus.style-summary":
            raise ValueError("This is not a style summary.")
        tempo = summary.get("tempo") or {}
        median = float(tempo.get("median") or 120.0)
        meters = summary.get("beatsPerBar") or {"4": 1}
        beats = int(max(meters.items(), key=lambda kv: kv[1])[0]) if meters else 4
        beats = beats if beats in (2, 3, 4, 5, 6, 7) else 4
        steps = beats * 4

        keys = []
        for k in summary.get("keys") or []:
            parsed = key_of_name(str(k.get("name", "")))
            if parsed:
                keys.append((parsed[0], parsed[1], int(k.get("count", 1))))
        modes = {str(m): float(v) for m, v in (summary.get("modes") or {}).items() if m in SCALES}

        drums = summary.get("drums") or {}
        found = drums.get("lanes") or {}
        has_drums = bool(drums.get("songs")) and all(isinstance(found.get(lane), list) for lane in DEFAULT_LANES)
        lanes = {}
        for lane, usual in DEFAULT_LANES.items():
            values = [float(v) for v in found.get(lane) or []] if has_drums else []
            lanes[lane] = resample(values, steps) if values else resample(usual, steps)

        parts = {}
        found_parts = summary.get("parts") or {}
        for name, default in DEFAULT_PARTS.items():
            p = found_parts.get(name)
            if name == "other" and not (isinstance(p, dict) and p.get("median")):
                # A separation with guitar and piano stems leaves little in "other" for a guitar band: the chords
                # are then the guitar's (or the piano's), whichever more songs have.
                harmonic: list[dict[str, Any]] = [
                    h
                    for h in (found_parts.get(n) for n in ("guitar", "piano"))
                    if isinstance(h, dict) and h.get("median")
                ]
                p = max(harmonic, key=lambda h: int(h.get("songs") or 0)) if harmonic else None
            if isinstance(p, dict) and p.get("median"):
                low, mid, high = int(p.get("low") or default.low), int(p["median"]), int(p.get("high") or default.high)
                # At least an octave to move in, around the middle.
                if high - low < 12:
                    low, high = min(low, mid - 6), max(high, mid + 6)
                parts[name] = PartStyle(
                    low,
                    mid,
                    high,
                    max(0.5, float(p.get("notesPerBar") or default.per_bar)),
                    float(p.get("voices") or 1.0),
                )
            else:
                parts[name] = default

        sections = summary.get("sections") or {}
        lengths: Counter[int] = Counter()
        for entry in sections.get("lengths") or []:
            bars = int(entry.get("bars", 0))
            if bars > 0:
                lengths[phrase_bars(bars)] += int(entry.get("count", 1))
        if not lengths:
            lengths.update({8: 3, 16: 2})

        return Style(
            tempo=median,
            tempo_low=float(tempo.get("min") or median),
            tempo_high=float(tempo.get("max") or median),
            beats_per_bar=beats,
            steps=steps,
            keys=keys or [(9, "minor", 1)],
            modes=modes or {"minor": 1.0},
            lanes=lanes,
            swing=float(drums.get("swing") or 0.5),
            parts=parts,
            lengths=sorted(lengths.items()),
            sections_per_song=float(sections.get("perSong") or 8),
            song_bars=float(sections.get("songBars") or 96),
            vocabularies=vocabularies(summary),
            has_drums=has_drums,
            without=frozenset(str(p) for p in summary.get("without") or [] if p in PARTS),
            double_kick=bool(drums.get("doubleKick")),
            key_fixed=bool(summary.get("keyFixed")),
            guitars=guitar_roles(summary),
        )

    def vocabulary(self, mode: str) -> Vocabulary:
        return self.vocabularies.get(mode) or self.vocabularies.get("any") or Vocabulary()


def resample(values: list[float], steps: int) -> list[float]:
    if len(values) == steps:
        return list(values)
    if not values:
        return [0.0] * steps
    return [values[min(len(values) - 1, i * len(values) // steps)] for i in range(steps)]


def phrase_bars(bars: int) -> int:
    """A section's length on the 4-bar phrase grid, within SECTION_BARS."""
    return max(SECTION_BARS[0], min(SECTION_BARS[1], 4 * round(bars / 4)))


def collapse(chords: list[dict[str, Any]]) -> list[str]:
    """Chords with repeats joined (I I V V -> I V)."""
    sequence: list[str] = []
    for chord in chords:
        if not sequence or sequence[-1] != chord.get("roman"):
            sequence.append(str(chord.get("roman")))
    return sequence


def vocabularies(summary: dict[str, Any]) -> dict[str, Vocabulary]:
    """The chord vocabulary per mode, from each song's chords (perSong: Roman numerals from its own key, so
    songs of one mode agree), and "any" from all songs; from the summary's own lists when perSong has none."""
    found: dict[str, Vocabulary] = {}
    songs = [s for s in summary.get("perSong") or [] if isinstance(s, dict) and s.get("chords")]
    for song in songs:
        mode = str((song.get("key") or {}).get("scale", ""))
        sequence: list[str] = []
        for chord in song["chords"]:
            roman = str(chord.get("roman", ""))
            if parse_roman(roman) is None:
                continue
            for target in {mode, "any"}:
                found.setdefault(target, Vocabulary()).counts[roman] += 1
            if not sequence or sequence[-1] != roman:
                sequence.append(roman)
        loops: Counter[tuple[str, ...]] = Counter()
        for i in range(len(sequence) - 3):
            loops[rotation_key(tuple(sequence[i : i + 4]))] += 1
        for target in {mode, "any"}:
            vocab = found.setdefault(target, Vocabulary())
            for a, b in itertools.pairwise(sequence):
                vocab.transitions.setdefault(a, Counter())[b] += 1
            vocab.own_loops.update(loop for loop, n in loops.items() if n >= SHARED_TIMES)
    # The median bars per chord of each mode's songs.
    for target, vocab in found.items():
        values = sorted(
            len(song["chords"]) / max(1, len(collapse(song["chords"])))
            for song in songs
            if target in (str((song.get("key") or {}).get("scale", "")), "any")
        )
        if values:
            vocab.bars_per_chord = values[len(values) // 2]
    if not found:
        chords = summary.get("chords") or {}
        vocab = Vocabulary()
        for entry in chords.get("common") or []:
            if parse_roman(str(entry.get("roman", ""))) is not None:
                vocab.counts[str(entry["roman"])] = max(2, round(1000 * float(entry.get("share", 0))))
        for a, after in (chords.get("transitions") or {}).items():
            vocab.transitions[str(a)] = Counter({str(b): max(1, round(100 * float(p))) for b, p in after.items()})
        for entry in chords.get("progressions") or []:
            if int(entry.get("count", 0)) >= SHARED_TIMES:
                vocab.own_loops.add(rotation_key(tuple(str(c) for c in entry.get("chords", []))))
        vocab.bars_per_chord = float(chords.get("barsPerChord") or 1.0)
        if vocab.counts:
            found["any"] = vocab
    return found


# ---------------------------------------------------------------------------------------------------
# The plan: tempo, key, sections


def guitar_roles(summary: dict[str, Any]) -> tuple[str, ...]:
    """The guitar parts the reference songs call for (style.summarize's `guitars`): a rhythm guitar when at
    least half the songs have distorted guitars, a clean guitar when half have clean ones, and a lead guitar when
    half of those with guitars play a lead line. None when fewer than half the songs have guitars, or when no
    song was measured with a guitar stem."""
    guitars = summary.get("guitars") if isinstance(summary, dict) else None
    if not isinstance(guitars, dict):
        return ()
    songs, count = int(guitars.get("songs") or 0), int(summary.get("songs") or 0)
    if not songs or songs * 2 < count:
        return ()
    roles = []
    if int(guitars.get("distorted") or 0) * 2 >= songs:
        roles.append("rhythm-guitar")
    if int(guitars.get("clean") or 0) * 2 >= songs:
        roles.append("clean-guitar")
    if int(guitars.get("lead") or 0) * 2 >= songs:
        roles.append("lead-guitar")
    return tuple(roles)


def default_parts(style: Style, size: str) -> list[dict[str, str]]:
    """The parts a size proposes: the sketch's four, or the full arrangement's (a sub bass only when there is
    room for one). With guitars in the reference songs, the guitars take the chords' place (and a lead guitar
    joins the lead)."""
    roles = list(SKETCH if size == "sketch" else tuple(r for r in FULL if r != "sub" or has_sub(style)))
    if style.guitars:
        chords = [g for g in style.guitars if g != "lead-guitar"]
        at = roles.index("chords")
        roles[at : at + 1] = chords or ["chords"]
        if "lead-guitar" in style.guitars:
            roles.insert(roles.index("lead") + 1, "lead-guitar")
    parts: list[dict[str, str]] = []
    for r in roles:
        taken = sum(1 for p in parts if p["role"] == r)
        parts.append({"id": r if not taken else f"{r}-{taken + 1}", "name": ROLES[r][0], "role": r})
    return parts


def check_parts(parts: Any) -> list[dict[str, str]]:
    """The user's list of parts, checked: 1 to MOST_PARTS, each with a known role and an id of its own."""
    if not isinstance(parts, list) or not 1 <= len(parts) <= MOST_PARTS:
        raise ValueError(f"The song needs 1 to {MOST_PARTS} parts.")
    checked: list[dict[str, str]] = []
    for p in parts:
        if not isinstance(p, dict) or p.get("role") not in ROLES:
            raise ValueError(f"A part's role is one of {', '.join(ROLES)}.")
        pid, name = str(p.get("id") or "").strip(), str(p.get("name") or "").strip()
        if not pid or len(pid) > 40 or any(c["id"] == pid for c in checked):
            raise ValueError("Each part needs an id of its own.")
        checked.append({"id": pid, "name": (name or ROLES[str(p["role"])][0])[:60], "role": str(p["role"])})
    return checked


def make_plan(
    style: Style, seed: int, size: str = "sketch", parts: list[dict[str, str]] | None = None
) -> dict[str, Any]:
    if size not in SIZES:
        raise ValueError(f"The arrangement is {' or '.join(SIZES)}, not '{size}'.")
    chosen = check_parts(parts) if parts is not None else default_parts(style, size)
    rng = rng_for(seed, "plan")
    # Tempo: the median moved by up to 3 %, inside the songs' range.
    low = max(style.tempo_low, style.tempo * 0.97)
    high = min(style.tempo_high, style.tempo * 1.03)
    tempo = round(rng.uniform(min(low, high), max(low, high)))

    mode = weighted(rng, style.modes)
    keys = [k for k in style.keys if k[1] == mode] or [(k[0], mode, k[2]) for k in style.keys]
    tonic = weighted(rng, {k[0]: float(k[2]) for k in keys})
    if rng.random() < 0.35 and not style.key_fixed:  # a little variation: a step up or down
        tonic = (tonic + rng.choice((-2, -1, 1, 2))) % 12

    count = max(5, min(10, round(style.sections_per_song)))
    best = min(abs(len(f) - count) for f in FORMS)
    form = rng.choice([f for f in FORMS if abs(len(f) - count) == best])
    target_bars = max(32.0, min(float(MOST_BARS - 16), style.song_bars * rng.uniform(0.85, 1.0)))

    pool = dict(style.lengths)
    lengths = sorted(pool)

    def draw(kind: str) -> int:
        middle = lengths[len(lengths) // 2]
        if kind in ("intro", "outro"):
            choices = {b: float(n) for b, n in pool.items() if b <= max(8, middle // 2)}
        elif kind == "bridge":
            choices = {b: float(n) for b, n in pool.items() if b <= max(8, middle)}
        else:
            choices = {b: float(n) for b, n in pool.items() if b >= 8}
        return int(weighted(rng, choices or {8: 1.0}))

    bars = {kind: draw(kind) for kind in KINDS if kind in form}
    template = form

    def joined() -> tuple[str, ...]:
        """Two long verses in a row would be one very long verse: one is kept."""
        return tuple(k for i, k in enumerate(template) if i == 0 or k != template[i - 1] or bars[k] < 16)

    # Toward the songs' length: lengthen or shorten the verse and chorus by the next length the songs have.
    for _ in range(12):
        form = joined()
        total = sum(bars[k] for k in form)
        if abs(total - target_bars) <= 8:
            break
        grow = total < target_bars
        moved = False
        for kind in sorted(("chorus", "verse"), key=lambda k: -form.count(k)):
            now = bars[kind]
            options = [b for b in lengths if (b > now if grow else now > b >= 8)]
            if options:
                bars[kind] = min(options) if grow else max(options)
                moved = True
                break
        if not moved:
            break
    form = joined()

    contour = {
        "intro": "rise",
        "verse": rng.choice(("arch", "wave", "rise")),
        "chorus": rng.choice(("arch", "fall", "wave")),
        "bridge": rng.choice(("valley", "rise", "wave")),
        "outro": "fall",
    }
    sections = [
        {"id": kind, "kind": kind, "bars": bars[kind], "contour": contour[kind]} for kind in KINDS if kind in form
    ]
    return {
        "tempo": tempo,
        "key": {"tonic": tonic, "scale": mode, "name": key_name(tonic, mode)},
        "beatsPerBar": style.beats_per_bar,
        "sections": sections,
        "order": list(form),
        "size": size,
        "parts": chosen,
    }


def new_seeds(seed: int, plan: dict[str, Any]) -> dict[str, Any]:
    return {
        "song": seed,
        "parts": {part: mix_seed(seed, "part", part) for part in PARTS},
        "layers": {p["id"]: mix_seed(seed, "layer", p["id"]) for p in plan.get("parts", [])},
        "sections": {s["id"]: mix_seed(seed, "section", s["id"]) for s in plan["sections"]},
    }


# ---------------------------------------------------------------------------------------------------
# Chords


def make_loop(
    vocab: Vocabulary, rng: random.Random, kind: str, tonic_chord: str, used: set[tuple[str, ...]], length: int = 4
) -> list[str]:
    """A loop of chords from the vocabulary: likely transitions, coming back round to its first chord. A loop a
    reference song plays as its own is less likely, and never twice in a song (`used`)."""
    allowed = vocab.allowed() or [tonic_chord]
    if len(allowed) == 1:
        return [allowed[0]] * length
    counts = {c: float(vocab.counts[c]) or 1.0 for c in allowed}

    def follow(chord: str) -> dict[str, float]:
        after = {
            c: float(n) for c, n in (vocab.transitions.get(chord) or Counter()).items() if c in counts and c != chord
        }
        return after or {c: w for c, w in counts.items() if c != chord}

    candidates: dict[tuple[str, ...], float] = {}
    for _ in range(120):
        starts = dict(counts)
        if kind == "bridge" and len(starts) > 1:
            starts.pop(tonic_chord, None)  # a bridge leaves home
        elif tonic_chord in starts:
            starts[tonic_chord] *= 2.0  # the other sections often start at home
        sequence = [weighted(rng, starts)]
        while len(sequence) < length:
            options = follow(sequence[-1])
            if rng.random() < 0.12:  # now and then a less travelled way, for new combinations
                options = {c: w for c, w in counts.items() if c != sequence[-1]}
            sequence.append(weighted(rng, options))
        loop = tuple(sequence)
        score = sum(math.log(vocab.transition(a, b) + 0.02) for a, b in zip(loop, loop[1:] + loop[:1], strict=True))
        score += sum(math.log(vocab.share(c) + 0.01) for c in loop) * 0.25
        if len(set(loop)) < min(3, len(allowed)):
            score -= 1.5
        key = rotation_key(loop)
        if key in used:
            continue
        if key in vocab.own_loops:
            score -= 2.5
        candidates[loop] = score
    if not candidates:
        return list(rng.choice(sorted(used))) if used else [tonic_chord] * length
    best = sorted(candidates.items(), key=lambda kv: -kv[1])[:8]
    top = best[0][1]
    return list(weighted(rng, {loop: math.exp(score - top) for loop, score in best}))


def section_chords(
    style: Style, plan: dict[str, Any], section: dict[str, Any], seeds: dict[str, Any], used: set[tuple[str, ...]]
) -> list[dict[str, Any]]:
    """One chord per bar of the section: {bar, roman, name, tones}."""
    key = plan["key"]
    vocab = style.vocabulary(str(key["scale"]))
    tonic_chord = "i" if key["scale"] == "minor" else "I"
    given = section.get("progression")
    if isinstance(given, list) and given:
        loop = [str(c) for c in given]
    else:
        rng = rng_for(seeds["sections"][section["id"]], "chords")
        loop = make_loop(vocab, rng, str(section["kind"]), tonic_chord, used)
    own = rotation_key(tuple(loop))
    if own in vocab.own_loops:
        used.update(vocab.own_loops)  # one reference loop at most: no other section takes one
    used.add(own)
    hold = 2 if vocab.bars_per_chord >= 1.75 and int(section["bars"]) >= 8 else 1
    chords: list[dict[str, Any]] = []
    for bar in range(int(section["bars"])):
        roman = loop[(bar // hold) % len(loop)]
        chords.append({"bar": bar, "roman": roman})
    # The song ends at home.
    allowed = vocab.allowed()
    if section["kind"] == "outro" and (tonic_chord in allowed or not allowed):
        for c in chords[-hold:]:
            c["roman"] = tonic_chord
    for c in chords:
        c["name"] = chord_label(c["roman"], int(key["tonic"]))
        c["tones"] = chord_tones(c["roman"], int(key["tonic"]))
    return chords


# ---------------------------------------------------------------------------------------------------
# The parts


@dataclass
class Context:
    """What a part's writer needs for one section."""

    style: Style
    plan: dict[str, Any]
    section: dict[str, Any]
    chords: list[dict[str, Any]]
    rng: random.Random
    last: bool  # the song's last section
    playing_share: dict[str, float]  # each part's bars played over the song's bars

    @property
    def bars(self) -> int:
        return int(self.section["bars"])

    @property
    def steps(self) -> int:
        return self.style.steps

    @property
    def energy(self) -> float:
        return float(self.section.get("energy", ENERGY.get(str(self.section["kind"]), 0.7)))

    @property
    def scale(self) -> set[int]:
        return scale_of(self.plan["key"])

    def at(self, bar: int, step: float) -> int:
        """Ticks from the pattern's start, on the grid (the swing is the pattern's: pattern_swing)."""
        return round((bar * self.steps + step) * TICKS_PER_STEP)


def pattern_swing(swing: float) -> float:
    """The songs' swing (where the off-beat sixteenth falls in its eighth: 0.5 straight, 0.67 a triplet feel) as
    a Spinus pattern's swing (0 to 1: the off-beat sixteenths later by up to half a sixteenth). Under 0.56 is
    measuring noise on real songs (style.feel_of): straight."""
    return 0.0 if swing < 0.56 else round(min(1.0, 4.0 * swing - 2.0), 3)


def plays(part: str, kind: str, bar: int, bars: int) -> bool:
    """Which parts play in which sections: the intro builds up, the lead sings in the verses, choruses and
    bridge, the outro thins out."""
    if kind == "intro":
        return part == "chords" or (part in ("drums", "bass") and bar >= bars // 2)
    if kind == "outro":
        return part != "lead"
    if kind == "bridge" and part == "drums":
        return bar >= bars // 2
    return True


def write_drums(ctx: Context) -> list[dict[str, Any]]:
    rng, steps, lanes = ctx.rng, ctx.steps, ctx.style.lanes
    drum_of = {"kick": "kick", "snare": "snare", "hats": "hat"}
    energy = ctx.energy

    # The lanes are averages over songs and sections, so a step the songs play in some sections reads 30 %:
    # the likeliest steps (as many as the lane's hits a bar, strong beats first on a tie) always play, the
    # others by chance, so that a bar has about the lane's hits.
    core: dict[str, set[int]] = {}
    chance: dict[str, float] = {}
    for lane, probabilities in lanes.items():
        expected = sum(probabilities)
        chosen = {s for s in range(steps) if probabilities[s] >= 0.6}
        # Each section its own choice among the likely steps (the likelier, and strong beats, more often), so
        # that over the song the steps are played about as often as in the songs.
        pool = {
            s: (p * (1.25 if s % 4 == 0 else 1.1 if s % 2 == 0 else 1.0)) ** 2
            for s, p in enumerate(probabilities)
            if p >= 0.15 and s not in chosen
        }
        while len(chosen) < round(0.85 * expected) and pool:
            pick = weighted(rng, pool)
            chosen.add(pick)
            pool.pop(pick)
        core[lane] = chosen
        rest = sum(p for s, p in enumerate(probabilities) if s not in chosen)
        chance[lane] = min(1.5, max(0.0, expected - len(chosen)) / rest) if rest > 0 else 0.0

    def bar_variant() -> dict[str, list[bool]]:
        hits = {}
        for lane, probabilities in lanes.items():
            row = []
            for s, p in enumerate(probabilities):
                if s in core[lane]:
                    row.append(True)
                elif p < 0.05:
                    row.append(False)
                else:
                    row.append(rng.random() < p * chance[lane] * (0.55 + 0.45 * energy))
            hits[lane] = row
        return hits

    base, variant = bar_variant(), bar_variant()
    notes: list[dict[str, Any]] = []
    kind = str(ctx.section["kind"])
    fills = ("roll", "toms", "short")
    fill = rng.choice(fills if energy >= 0.6 else ("short", "roll"))
    for bar in range(ctx.bars):
        if not plays("drums", kind, bar, ctx.bars):
            continue
        pattern = variant if bar % 4 == 3 else base
        end = bar == ctx.bars - 1
        if end and ctx.last:  # the ending: one last hit
            notes.append(drum_note(ctx, bar, 0, "kick", 0.95))
            notes.append(drum_note(ctx, bar, 0, "crash", 0.9))
            continue
        fill_from = steps // 2 if end and fill != "short" else steps * 3 // 4 if end else steps
        for lane, row in pattern.items():
            for step, on in enumerate(row):
                if not on or (step >= fill_from and lane != "kick"):
                    continue
                if step >= fill_from and step % 4 != 0:
                    continue
                p = lanes[lane][step]
                strong = step % 4 == 0
                velocity = 0.5 + 0.35 * p + (0.08 if strong else 0.0) + rng.uniform(-0.04, 0.04)
                if lane == "hats":
                    velocity -= 0.12
                notes.append(drum_note(ctx, bar, step, drum_of[lane], velocity * (0.8 + 0.2 * energy)))
        if ctx.style.double_kick and kind == "chorus" and not end:
            # Metal's double kick: every sixteenth (every eighth when it is very fast), the off-beats softer.
            every = 1 if float(ctx.plan["tempo"]) <= 170 else 2
            taken = {n["start"] for n in notes if n["drum"] == "kick"}
            for step in range(0, steps, every):
                if ctx.at(bar, step) not in taken:
                    notes.append(drum_note(ctx, bar, step, "kick", 0.62 if step % 4 else 0.85))
        if bar == 0 and kind != "intro" and plays("drums", kind, 0, ctx.bars):
            notes.append(drum_note(ctx, bar, 0, "crash", 0.8))
        if end:
            notes += fill_notes(ctx, bar, fill_from, fill)
    return notes


def fill_notes(ctx: Context, bar: int, start: int, fill: str) -> list[dict[str, Any]]:
    steps = ctx.steps
    hits: list[tuple[int, str]] = []
    if fill == "roll":
        hits = [(s, "snare") for s in range(start, steps) if s % 2 == 0 or s >= steps - 4]
    elif fill == "toms":
        span = steps - start
        for i, s in enumerate(range(start, steps)):
            drum = ("tomHigh", "tomMid", "tomLow", "snare")[min(3, 4 * i // max(1, span))]
            if s % 2 == 0 or i >= span - 2:
                hits.append((s, drum))
    else:
        hits = [(s, "snare") for s in range(start, steps) if s % 2 == 0] + [(steps - 1, "snare")]
    notes = []
    for i, (step, drum) in enumerate(sorted(set(hits))):
        velocity = 0.55 + 0.4 * i / max(1, len(hits) - 1)  # louder toward the next section
        notes.append(drum_note(ctx, bar, step, drum, velocity))
    return notes


def drum_note(ctx: Context, bar: int, step: int, drum: str, velocity: float) -> dict[str, Any]:
    return {
        "start": ctx.at(bar, step),
        "length": TICKS_PER_STEP // 2,
        "drum": drum,
        "velocity": round(min(1.0, max(0.1, velocity)), 3),
    }


def density(ctx: Context, part: str, scale: float = 1.0) -> float:
    """Notes a bar while the part plays, so that over the whole song it comes to the songs' notes a bar."""
    style = ctx.style.parts[SUMMARY_PART[part]]
    share = max(0.4, ctx.playing_share.get(part, 1.0))
    return style.per_bar / share * scale


def nearest_octave(pitch_class: int, centre: float, low: int, high: int) -> int:
    best = None
    for octave in range(0, 11):
        p = 12 * octave + pitch_class
        if low <= p <= high and (best is None or abs(p - centre) < abs(best - centre)):
            best = p
    if best is None:  # no such note in the range: the nearest outside it
        best = min((12 * o + pitch_class for o in range(11)), key=lambda p: abs(p - centre))
    return best


def rhythm(rng: random.Random, steps: int, count: int, weights: list[float]) -> list[int]:
    """`count` steps of a bar, the first always, the others drawn by weight without repeats."""
    count = max(1, min(steps, count))
    chosen = {0}
    pool = {s: w for s, w in enumerate(weights) if s != 0 and w > 0}
    while len(chosen) < count and pool:
        s = weighted(rng, pool)
        chosen.add(s)
        pool.pop(s)
    return sorted(chosen)


def write_bass(ctx: Context) -> list[dict[str, Any]]:
    rng, steps = ctx.rng, ctx.steps
    style = ctx.style.parts["bass"]
    kick = ctx.style.lanes["kick"]
    per_bar = density(ctx, "bass", 0.75 + 0.3 * ctx.energy)
    weights = [0.15 + 2.0 * kick[s] + (0.6 if s % 4 == 0 else 0.3 if s % 2 == 0 else 0.0) for s in range(steps)]

    def cell() -> list[int]:
        count = max(1, round(per_bar + rng.uniform(-0.5, 0.5)))
        return rhythm(rng, steps, count, weights)

    cells = [cell(), cell()]
    tonic = int(ctx.plan["key"]["tonic"])
    notes: list[dict[str, Any]] = []
    kind = str(ctx.section["kind"])
    previous = style.median
    for bar in range(ctx.bars):
        if not plays("bass", kind, bar, ctx.bars):
            continue
        chord = ctx.chords[bar]
        tones = chord["tones"] or [tonic]
        root = tones[0]
        onsets = cells[1] if bar % 4 == 3 else cells[0]
        if ctx.last and bar == ctx.bars - 1:
            onsets = [0]
        next_root = ctx.chords[bar + 1]["tones"][0] if bar + 1 < ctx.bars and ctx.chords[bar + 1]["tones"] else root
        for i, step in enumerate(onsets):
            end = onsets[i + 1] if i + 1 < len(onsets) else steps
            pitch = nearest_octave(root, (style.median + previous) / 2, style.low, style.high)
            roll = rng.random()
            if i > 0 and roll < 0.18 and len(tones) > 2:
                fifth = nearest_octave(tones[2], pitch + 3, style.low, style.high)
                pitch = fifth if style.low <= fifth <= style.high else pitch
            elif i > 0 and roll < 0.3 and pitch + 12 <= style.high:
                pitch += 12
            elif i == len(onsets) - 1 and i > 0 and next_root != root and roll < 0.55:
                # A step of the key leading into the next chord's root.
                target = nearest_octave(next_root, pitch, style.low, style.high)
                direction = 1 if target < pitch else -1
                lead = target + direction
                while lead % 12 not in ctx.scale and abs(lead - target) < 3:
                    lead += direction
                if style.low <= lead <= style.high and lead % 12 in allowed_pitch_classes(tones, ctx.scale):
                    pitch = lead
            previous = pitch
            length = (end - step) * TICKS_PER_STEP
            length = round(length * (0.6 if per_bar > 8 else 0.9))
            velocity = 0.72 + (0.12 if step % 4 == 0 else 0.0) + rng.uniform(-0.05, 0.05)
            notes.append(
                {
                    "start": ctx.at(bar, step),
                    "length": max(60, length),
                    "pitch": pitch,
                    "velocity": round(min(1.0, velocity * (0.85 + 0.15 * ctx.energy)), 3),
                }
            )
    return notes


def voicing_options(tones: list[int], voices: int, low: int, high: int) -> list[list[int]]:
    """Close voicings of the chord with `voices` notes inside low..high (the root and fifth doubled when more
    voices than notes are wanted)."""
    wanted = list(tones)
    while len(wanted) < voices:
        wanted.append(tones[len(wanted) % len(tones)])
    wanted = wanted[:voices] if voices < len(tones) else wanted
    if voices < len(tones):  # leave out the fifth first
        wanted = [tones[0], tones[1], *([tones[3]] if len(tones) > 3 and voices > 2 else [tones[2]])[: voices - 2]]
    options = []
    for bass in range(low, high + 1):
        if bass % 12 not in wanted:
            continue
        chord = [bass]
        rest = list(wanted)
        rest.remove(bass % 12)
        for pc in sorted(rest, key=lambda p: (p - bass) % 12 or 12):  # a doubled root on top
            note = chord[-1] + ((pc - chord[-1]) % 12 or 12)
            chord.append(note)
        if chord[-1] <= high:
            options.append(chord)
    return options


def write_chords(ctx: Context) -> list[dict[str, Any]]:
    rng, steps = ctx.rng, ctx.steps
    style = ctx.style.parts["other"]
    per_bar = density(ctx, "chords", 0.75 + 0.3 * ctx.energy)
    notes: list[dict[str, Any]] = []
    kind = str(ctx.section["kind"])
    hats = ctx.style.lanes["hats"]
    if style.voices >= 1.3:
        voices = max(2, min(4, round(style.voices + 0.5)))
        onsets_per_bar = max(1, min(8, round(per_bar / voices)))
        patterns = {
            1: [0],
            2: [0, steps // 2],
            3: [0, steps * 3 // 8, steps * 3 // 4],
            4: list(range(0, steps, steps // 4)),
        }
        if onsets_per_bar in patterns:
            cells = [patterns[onsets_per_bar]] * 2
        else:
            eighths = [s for s in range(0, steps, 2)]
            weights = [0.0] * steps
            for s in eighths:
                weights[s] = 0.3 + hats[s]
            cells = [rhythm(rng, steps, onsets_per_bar, weights), rhythm(rng, steps, onsets_per_bar, weights)]
        previous: list[int] | None = None
        for bar in range(ctx.bars):
            if not plays("chords", kind, bar, ctx.bars):
                continue
            tones = ctx.chords[bar]["tones"]
            if not tones:
                continue
            options = voicing_options(tones, voices, style.low, style.high) or voicing_options(
                tones, voices, style.low - 12, style.high + 12
            )
            if not options:
                continue

            def cost(v: list[int], before: list[int] | None = previous) -> float:
                centre = sum(v) / len(v)
                moved = sum(min(abs(a - b) for b in before) for a in v) if before else 0.0
                return abs(centre - style.median) * 0.6 + moved * 0.5 + rng.random() * 0.3

            chord = min(options, key=cost)
            previous = chord
            onsets = cells[1] if bar % 4 == 3 else cells[0]
            if ctx.last and bar == ctx.bars - 1:
                onsets = [0]
            for i, step in enumerate(onsets):
                end = onsets[i + 1] if i + 1 < len(onsets) else steps
                length = round((end - step) * TICKS_PER_STEP * (0.95 if len(onsets) <= 2 else 0.8))
                velocity = 0.6 + (0.1 if step == 0 else 0.0) + rng.uniform(-0.04, 0.04)
                for pitch in chord:
                    notes.append(
                        {
                            "start": ctx.at(bar, step),
                            "length": max(60, length),
                            "pitch": pitch,
                            "velocity": round(velocity * (0.85 + 0.15 * ctx.energy), 3),
                        }
                    )
        return notes

    # One note at a time: the chord broken into a line.
    onsets_per_bar = max(1, min(16, round(per_bar)))
    weights = [0.3 + hats[s] + (0.5 if s % 4 == 0 else 0.0) for s in range(steps)]
    cells = [rhythm(rng, steps, onsets_per_bar, weights), rhythm(rng, steps, onsets_per_bar, weights)]
    shape = rng.choice(("up", "down", "updown"))
    for bar in range(ctx.bars):
        if not plays("chords", kind, bar, ctx.bars):
            continue
        tones = ctx.chords[bar]["tones"]
        if not tones:
            continue
        ladder = sorted({p for p in range(style.low, style.high + 1) if p % 12 in tones}, key=lambda p: p)
        ladder = [p for p in ladder if abs(p - style.median) <= 9] or ladder
        if shape == "down":
            ladder = ladder[::-1]
        elif shape == "updown":
            ladder = ladder + ladder[-2:0:-1]
        onsets = cells[1] if bar % 4 == 3 else cells[0]
        for i, step in enumerate(onsets):
            end = onsets[i + 1] if i + 1 < len(onsets) else steps
            notes.append(
                {
                    "start": ctx.at(bar, step),
                    "length": max(60, round((end - step) * TICKS_PER_STEP * 0.9)),
                    "pitch": ladder[i % len(ladder)],
                    "velocity": round(0.65 + rng.uniform(-0.05, 0.05), 3),
                }
            )
    return notes


def contour_at(contour: str, x: float) -> float:
    """-1..1 along a phrase (x 0..1)."""
    if contour == "rise":
        return -0.6 + 1.2 * x
    if contour == "fall":
        return 0.6 - 1.2 * x
    if contour == "arch":
        return 1.0 - 4.0 * (x - 0.5) ** 2 - 0.3
    if contour == "valley":
        return 4.0 * (x - 0.5) ** 2 - 0.7
    if contour == "wave":
        return 0.6 * math.sin(2 * math.pi * x)
    return 0.0


def write_lead(ctx: Context) -> list[dict[str, Any]]:
    rng, steps = ctx.rng, ctx.steps
    style = ctx.style.parts["vocals"]
    kind = str(ctx.section["kind"])
    scale = ctx.scale
    lift = {"chorus": 1.15, "verse": 0.9, "bridge": 0.8}.get(kind, 0.8)
    per_bar = density(ctx, "lead", lift)
    phrase = 4 if per_bar < 1.5 else 2  # bars
    centre = style.median + {"chorus": 3, "verse": -1, "bridge": 1}.get(kind, 0)
    centre = max(style.low + 4, min(style.high - 4, centre))
    span = max(3.0, min(7.0, (style.high - style.low) / 3))
    contour = str(ctx.section.get("contour") or "arch")
    weights = [
        1.0 if s % steps == 0 else 0.75 if s % 4 == 0 else 0.45 if s % 2 == 0 else 0.15 for s in range(phrase * steps)
    ]
    # Breathe: the end of each phrase is a rest.
    for s in range(phrase * steps - 4, phrase * steps):
        weights[s] = 0.0

    def phrase_rhythm() -> list[int]:
        count = max(1, round(per_bar * phrase + rng.uniform(-1, 1)))
        return rhythm(rng, phrase * steps, count, weights)

    rhythms = {"A": phrase_rhythm(), "B": phrase_rhythm()}
    plan = ("A", "A", "B", "A")
    notes: list[dict[str, Any]] = []
    previous = round(centre)
    for start in range(0, ctx.bars, phrase):
        if not plays("lead", kind, start, ctx.bars):
            continue
        letter = plan[(start // phrase) % len(plan)]
        onsets = list(rhythms[letter])
        if letter == "A" and (start // phrase) % len(plan) == 1 and len(onsets) > 2 and rng.random() < 0.6:
            # A': the same rhythm with one note moved, so it answers rather than repeats.
            i = rng.randrange(1, len(onsets))
            moved = onsets[i] + rng.choice((-2, 2))
            if 0 < moved < phrase * steps - 4 and moved not in onsets:
                onsets[i] = moved
                onsets.sort()
        onsets = [s for s in onsets if start + s // steps < ctx.bars]
        for i, s in enumerate(onsets):
            bar, step = start + s // steps, s % steps
            end = onsets[i + 1] if i + 1 < len(onsets) else min(phrase * steps, s + steps // 2 + 4)
            held = end - s >= 4
            chord = ctx.chords[bar]["tones"] or [int(ctx.plan["key"]["tonic"])]
            strong = step % (steps // 2) == 0 or held or i == len(onsets) - 1
            fits = set(chord) if strong else allowed_pitch_classes(chord, scale) & (scale | set(chord))
            target = centre + span * contour_at(contour, s / (phrase * steps))
            candidates = [p for p in range(style.low, style.high + 1) if p % 12 in fits]
            if not candidates:
                continue

            def cost(p: int, target: float = target, before: int = previous) -> float:
                leap = abs(p - before)
                return abs(p - target) * 0.5 + (leap if leap > 4 else leap * 0.3) + rng.random() * 1.2

            pitch = min(candidates, key=cost)
            previous = pitch
            length = round((end - s) * TICKS_PER_STEP * 0.92)
            velocity = 0.7 + (0.1 if step % 4 == 0 else 0.0) + rng.uniform(-0.05, 0.05)
            notes.append(
                {
                    "start": ctx.at(bar, step),
                    "length": max(60, length),
                    "pitch": pitch,
                    "velocity": round(min(1.0, velocity), 3),
                }
            )
    return notes


WRITERS = {"drums": write_drums, "bass": write_bass, "chords": write_chords, "lead": write_lead}


# ---------------------------------------------------------------------------------------------------
# The full arrangement's layers


def layer_plays(layer: str, kind: str, bar: int, bars: int, instance: int = 0) -> bool:
    """Which extra layers play in which sections: the choruses have them all, the verses none (they stay thin),
    the bridge breaks down to the pad and builds again with the arpeggio, the intro and outro have the pad. A
    second part of the four roles (another chords part, a harmony) plays in the choruses and the bridge."""
    if instance > 0 and layer in ("drums", "bass", "chords", "lead"):
        return kind == "chorus" or (kind == "bridge" and bar >= bars // 2)
    if layer == "pad":
        return kind in ("intro", "chorus", "bridge", "outro")
    if layer == "arp":
        return kind == "chorus" or (kind == "bridge" and bar >= bars // 2)
    if layer in ("counter", "sub", "brass"):
        return kind == "chorus"
    if layer == "lead-guitar":
        return kind in ("chorus", "bridge") or (kind == "intro" and bar >= bars // 2)
    if layer in ("organ", "choir"):
        return kind in ("chorus", "bridge", "outro")
    if layer == "strings":
        return kind in ("intro", "chorus", "bridge", "outro")
    if layer == "woodwinds":
        return kind == "chorus" or (kind == "verse" and bar >= bars // 2)
    if layer == "percussion":
        return kind in ("verse", "chorus", "bridge")
    return True


def chord_spans(ctx: Context) -> list[tuple[int, int]]:
    """The section's bars as (first bar, bars) runs of one chord."""
    spans: list[tuple[int, int]] = []
    for bar, chord in enumerate(ctx.chords):
        if spans and ctx.chords[spans[-1][0]]["roman"] == chord["roman"]:
            spans[-1] = (spans[-1][0], spans[-1][1] + 1)
        else:
            spans.append((bar, 1))
    return spans


def pad_register(style: Style) -> tuple[int, int, int]:
    other = style.parts["other"]
    middle = max(55, min(67, other.median + 3))
    return middle - 9, middle, middle + 12


def write_pad(ctx: Context, octave: int = 0) -> list[dict[str, Any]]:
    """The chords held: a voicing of 3 or 4 notes for each run of one chord, moving as little as it can."""
    low, middle, high = (n + 12 * octave for n in pad_register(ctx.style))
    kind = str(ctx.section["kind"])
    notes: list[dict[str, Any]] = []
    previous: list[int] | None = None
    voices = 4 if ctx.energy >= 0.9 else 3
    for first, length in chord_spans(ctx):
        playing = [b for b in range(first, first + length) if layer_plays("pad", kind, b, ctx.bars)]
        tones = ctx.chords[first]["tones"]
        if not playing or not tones:
            continue
        options = voicing_options(tones, voices, low, high) or voicing_options(tones, voices, low - 12, high + 12)
        if not options:
            continue

        def cost(v: list[int], before: list[int] | None = previous) -> float:
            moved = sum(min(abs(a - b) for b in before) for a in v) if before else 0.0
            return abs(sum(v) / len(v) - middle) * 0.5 + moved * 0.6 + ctx.rng.random() * 0.2

        chord = min(options, key=cost)
        previous = chord
        start, bars = playing[0], len(playing)
        for pitch in chord:
            notes.append(
                {
                    "start": ctx.at(start, 0),
                    "length": round(bars * ctx.steps * TICKS_PER_STEP * 0.98),
                    "pitch": pitch,
                    "velocity": round(0.45 + 0.15 * ctx.energy, 3),
                }
            )
    return notes


def write_arp(ctx: Context, octave: int = 0, slower: bool = False, plays: str = "arp") -> list[dict[str, Any]]:
    """The chord's notes one after another, up and down over about an octave and a half above the chords: in
    sixteenths when the songs' other instruments are busy (and the tempo leaves room), else in eighths;
    `slower` halves that (a second arpeggio, or chords broken into one)."""
    other = ctx.style.parts["other"]
    every = (1 if other.per_bar >= 8 and ctx.plan["tempo"] < 140 else 2) * (2 if slower else 1)
    low = max(52, min(72, other.median + 5)) + 12 * octave
    shape = ctx.rng.choice(("up", "updown", "down"))
    kind = str(ctx.section["kind"])
    notes: list[dict[str, Any]] = []
    for bar in range(ctx.bars):
        tones = ctx.chords[bar]["tones"]
        if not tones or not layer_plays(plays, kind, bar, ctx.bars, 1 if plays != "arp" else 0):
            continue
        ladder = sorted(p for p in range(low, low + 19) if p % 12 in tones)
        if shape == "down":
            ladder = ladder[::-1]
        elif shape == "updown":
            ladder = ladder + ladder[-2:0:-1]
        for i, step in enumerate(range(0, ctx.steps, every)):
            notes.append(
                {
                    "start": ctx.at(bar, step),
                    "length": round(every * TICKS_PER_STEP * 0.8),
                    "pitch": ladder[i % len(ladder)],
                    "velocity": round((0.62 if step % 4 == 0 else 0.5) + ctx.rng.uniform(-0.03, 0.03), 3),
                }
            )
    return notes


def write_counter(
    ctx: Context, lead: list[dict[str, Any]], octave: int = 0, plays: str = "counter"
) -> list[dict[str, Any]]:
    """A slower line under the lead: a held chord note each half bar (the chord's third first, then its fifth), a
    fifth or so below the lead's middle, moving as little as it can and never rubbing a semitone (or doubling
    the note) against the lead note sounding with it."""
    vocals = ctx.style.parts["vocals"]
    centre = max(52, vocals.median - 7) + 12 * octave
    half = ctx.steps // 2 * TICKS_PER_STEP
    kind = str(ctx.section["kind"])

    def lead_at(at: int) -> list[int]:
        return [int(n["pitch"]) for n in lead if int(n["start"]) <= at < int(n["start"]) + int(n["length"])]

    notes: list[dict[str, Any]] = []
    previous = centre
    for slot in range(ctx.bars * 2):
        bar = slot // 2
        tones = ctx.chords[bar]["tones"]
        if not tones or not layer_plays(plays, kind, bar, ctx.bars):
            continue
        at = slot * half
        above = lead_at(at)
        wanted = [tones[1], tones[2]] if len(tones) > 2 else list(tones)
        candidates = [
            p
            for p in range(centre - 7, centre + 8)
            if p % 12 in wanted and not any(abs(p - q) % 12 in (0, 1, 11) for q in above)
        ]
        if not candidates:
            continue
        before = previous
        pitch = min(candidates, key=lambda p: abs(p - before) + 2.0 * ctx.rng.random())
        previous = pitch
        notes.append(
            {
                "start": at,
                "length": round(half * 0.95),
                "pitch": pitch,
                "velocity": round(0.5 + ctx.rng.uniform(-0.04, 0.04), 3),
            }
        )
    return notes


def write_sub(ctx: Context) -> list[dict[str, Any]]:
    """The chords' roots held low, under the bass, for each run of one chord."""
    kind = str(ctx.section["kind"])
    notes: list[dict[str, Any]] = []
    for first, length in chord_spans(ctx):
        playing = [b for b in range(first, first + length) if layer_plays("sub", kind, b, ctx.bars)]
        tones = ctx.chords[first]["tones"]
        if not playing or not tones:
            continue
        pitch = nearest_octave(tones[0], 31, *SUB_RANGE)
        notes.append(
            {
                "start": ctx.at(playing[0], 0),
                "length": round(len(playing) * ctx.steps * TICKS_PER_STEP * 0.95),
                "pitch": pitch,
                "velocity": 0.8,
            }
        )
    return notes


def has_sub(style: Style) -> bool:
    """A sub bass when the bass sits high enough (a plucked or mid bass) to leave room under it."""
    return style.parts["bass"].median >= SUB_ROOM


def write_stabs(ctx: Context) -> list[dict[str, Any]]:
    """Short chords on the off-beat eighths (a third chords part), an octave over the chords part."""
    low, middle, high = (n + 12 for n in pad_register(ctx.style))
    kind = str(ctx.section["kind"])
    notes: list[dict[str, Any]] = []
    for bar in range(ctx.bars):
        tones = ctx.chords[bar]["tones"]
        if not tones or not layer_plays("chords", kind, bar, ctx.bars, 1):
            continue
        options = voicing_options(tones, 3, low, high) or voicing_options(tones, 3, low - 12, high)
        if not options:
            continue
        chord = min(options, key=lambda v: abs(sum(v) / len(v) - middle))
        for step in range(2, ctx.steps, 4):
            for pitch in chord:
                notes.append({"start": ctx.at(bar, step), "length": TICKS_PER_STEP, "pitch": pitch, "velocity": 0.55})
    return notes


def write_harmony(ctx: Context, lead: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """A second lead: the first a third (or so) below, on the chord's or the key's notes, in the choruses and
    the bridge's second half."""
    kind = str(ctx.section["kind"])
    bar_ticks = ctx.steps * TICKS_PER_STEP
    notes: list[dict[str, Any]] = []
    for n in lead:
        bar = min(ctx.bars - 1, int(n["start"]) // bar_ticks)
        if not layer_plays("lead", kind, bar, ctx.bars, 1):
            continue
        tones = ctx.chords[bar]["tones"]
        fits = allowed_pitch_classes(tones, ctx.scale) & (ctx.scale | set(tones)) if tones else ctx.scale
        below = [p for p in range(int(n["pitch"]) - 5, int(n["pitch"]) - 2) if p % 12 in fits]
        if below:
            notes.append({**n, "pitch": max(below), "velocity": round(float(n["velocity"]) * 0.85, 3)})
    return notes


def write_percussion(ctx: Context) -> list[dict[str, Any]]:
    """A second drums part: hats on the off-beat sixteenths (some by chance) and a low tom at each bar's end."""
    kind = str(ctx.section["kind"])
    notes: list[dict[str, Any]] = []
    for bar in range(ctx.bars):
        if not layer_plays("drums", kind, bar, ctx.bars, 1):
            continue
        for step in range(1, ctx.steps, 2):
            if step % 4 == 3 or ctx.rng.random() < 0.5:
                notes.append(drum_note(ctx, bar, step, "hat", 0.35 + 0.2 * ctx.rng.random()))
        if bar % 2 == 1:
            notes.append(drum_note(ctx, bar, ctx.steps - 2, "tomLow", 0.6))
    return notes


def shifted(notes: list[dict[str, Any]], semitones: int) -> list[dict[str, Any]]:
    return [{**n, "pitch": min(127, int(n["pitch"]) + semitones)} for n in notes]


# The instruments' own ways of playing (the roles beyond the four parts).

GUITAR = (40, 76)  # a guitar's notes, low E to the twelfth fret of the high strings


def moved(voicing: list[int], before: list[int] | None) -> float:
    """How far a voicing's notes move from the one before (0 for the first)."""
    return float(sum(min(abs(a - b) for b in before) for a in voicing)) if before else 0.0


def is_heavy(style: Style) -> bool:
    """Rock and metal's chugging: a busy kick or busy other instruments (power chords on every sixteenth)."""
    return style.double_kick or sum(style.lanes["kick"]) >= 5.5 or style.parts["other"].per_bar >= 12


def guitar_voicing(tones: list[int], low: int = 40) -> list[int]:
    """A chord as a guitar plays it open: the root low, then the fifth, the octave, the third, the fifth and the
    octave again, up to the high strings."""
    root = nearest_octave(tones[0], low + 5, low, low + 11)
    third = tones[1] if len(tones) > 1 else tones[0]
    fifth = tones[2] if len(tones) > 2 else tones[0] + 7
    shape = [root, root + (fifth - tones[0]) % 12, root + 12, root + 12 + (third - tones[0]) % 12]
    shape += [shape[1] + 12, root + 24]
    return [p for p in shape if p <= GUITAR[1]]


def guitar_note(ctx: Context, bar: int, step: float, length: float, pitch: int, velocity: float) -> dict[str, Any]:
    return {
        "start": ctx.at(bar, step),
        "length": max(40, round(length * TICKS_PER_STEP)),
        "pitch": pitch,
        "velocity": round(min(1.0, max(0.1, velocity)), 3),
    }


def write_rhythm_guitar(ctx: Context, octave: int = 0) -> list[dict[str, Any]]:
    """Power chords (root, fifth, octave). Heavy styles chug palm-muted sixteenths (or gallops: an eighth and
    two sixteenths) on the root, the power chord ringing on the strong beats and where the kick hits; others
    strike the power chord on the eighths. The intro's first half lets one ring each bar."""
    heavy = is_heavy(ctx.style)
    kick = ctx.style.lanes["kick"]
    gallop = heavy and ctx.rng.random() < 0.4
    kind = str(ctx.section["kind"])
    notes: list[dict[str, Any]] = []
    for bar in range(ctx.bars):
        tones = ctx.chords[bar]["tones"]
        if not tones:
            continue
        root = nearest_octave(tones[0], 45 + 12 * octave, 40 + 12 * octave, 52 + 12 * octave)
        power = [root, root + 7, root + 12]
        if (kind == "intro" and bar < ctx.bars // 2) or (ctx.last and bar == ctx.bars - 1):
            notes += [guitar_note(ctx, bar, 0, ctx.steps * 0.95, p, 0.8) for p in power]
            continue
        if heavy:
            steps = [s for s in range(ctx.steps) if not gallop or s % 4 != 1]
            for s in steps:
                accent = s % 8 == 0 or kick[s % len(kick)] >= 0.6
                if accent:
                    notes += [guitar_note(ctx, bar, s, 1.8, p, 0.9) for p in power]
                else:  # palm-muted: the root alone, short and softer
                    notes.append(guitar_note(ctx, bar, s, 0.55, root, 0.62 + ctx.rng.uniform(-0.04, 0.04)))
        else:
            for s in range(0, ctx.steps, 2):
                velocity = 0.82 if s % 4 == 0 else 0.68
                notes += [guitar_note(ctx, bar, s, 1.8, p, velocity) for p in power]
    return notes


# Down and up strokes of a strum on the eighths: D D U U D U.
STRUM = ((0, True, 0.75), (4, True, 0.62), (6, False, 0.48), (10, False, 0.48), (12, True, 0.62), (14, False, 0.5))


def write_acoustic_guitar(ctx: Context, octave: int = 0) -> list[dict[str, Any]]:
    """Strummed open chords: D D U U D U, a down stroke from the low string up (a few ticks apart), an up
    stroke from the high strings down on four of them; each stroke rings until the next."""
    kind = str(ctx.section["kind"])
    notes: list[dict[str, Any]] = []
    for bar in range(ctx.bars):
        tones = ctx.chords[bar]["tones"]
        if not tones:
            continue
        shape = guitar_voicing(tones, 40 + 12 * octave)
        strokes = STRUM if kind not in ("intro", "outro") else ((0, True, 0.7), (8, True, 0.6))
        for i, (step, down, velocity) in enumerate(strokes):
            end = strokes[i + 1][0] if i + 1 < len(strokes) else ctx.steps
            strings = shape if down else shape[::-1][:4]
            for k, pitch in enumerate(strings):
                note = guitar_note(
                    ctx, bar, step, end - step, pitch, velocity - 0.03 * k + ctx.rng.uniform(-0.03, 0.03)
                )
                note["start"] += 12 * k  # the strum's spread
                notes.append(note)
    return notes


def write_clean_guitar(ctx: Context, octave: int = 0) -> list[dict[str, Any]]:
    """A fingerpicked chord: the open shape's strings one at a time on the eighths (bass, then the inner and
    high strings), each ringing."""
    order = (0, 3, 2, 4, 1, 3, 2, 4)
    notes: list[dict[str, Any]] = []
    for bar in range(ctx.bars):
        tones = ctx.chords[bar]["tones"]
        if not tones:
            continue
        shape = guitar_voicing(tones, 45 + 12 * octave)
        for i, step in enumerate(range(0, ctx.steps, 2)):
            pitch = shape[order[i % len(order)] % len(shape)]
            notes.append(
                guitar_note(ctx, bar, step, 5, pitch, (0.68 if i == 0 else 0.55) + ctx.rng.uniform(-0.03, 0.03))
            )
    return notes


PENTATONIC = {"minor": (0, 3, 5, 7, 10), "major": (0, 2, 4, 7, 9)}


def write_lead_guitar(ctx: Context, octave: int = 0) -> list[dict[str, Any]]:
    """A riff on the key's pentatonic scale: a one-bar motif (sixteenths in heavy styles, eighths otherwise)
    repeated, its strong beats moved onto the chord's notes; busier in the bridge (a solo)."""
    key = ctx.plan["key"]
    tonic, scale = int(key["tonic"]), str(key["scale"])
    ladder = [
        p
        for p in range(55 + 12 * octave, 80 + 12 * octave)
        if (p - tonic) % 12 in PENTATONIC.get(scale, PENTATONIC["minor"])
    ]
    heavy = is_heavy(ctx.style)
    kind = str(ctx.section["kind"])
    every = 1 if heavy or kind == "bridge" else 2
    count = ctx.rng.randint(5, 8) if every == 2 else ctx.rng.randint(7, 11)
    onsets = rhythm(ctx.rng, ctx.steps, count, [1.0 if s % every == 0 else 0.0 for s in range(ctx.steps)])
    walk = [ctx.rng.choice((-2, -1, 1, 2)) for _ in onsets]
    notes: list[dict[str, Any]] = []
    for bar in range(ctx.bars):
        tones = ctx.chords[bar]["tones"]
        if not tones or not layer_plays("lead-guitar", kind, bar, ctx.bars):
            continue
        index = len(ladder) // 2
        for i, step in enumerate(onsets):
            index = max(0, min(len(ladder) - 1, index + walk[i]))
            pitch = ladder[index]
            if step % 4 == 0:  # strong beats on the chord
                pitch = min((p for p in ladder if p % 12 in tones), key=lambda p: abs(p - pitch), default=pitch)
            end = onsets[i + 1] if i + 1 < len(onsets) else ctx.steps
            notes.append(guitar_note(ctx, bar, step, (end - step) * 0.9, pitch, 0.78 + (0.1 if step % 4 == 0 else 0.0)))
    return notes


def write_piano(ctx: Context, octave: int = 0) -> list[dict[str, Any]]:
    """Two hands: the left the chord's root in octaves, the right a voiced chord near middle C. Verses break it
    into eighths (root-fifth-third-fifth in the right hand); choruses strike it on the beats."""
    kind = str(ctx.section["kind"])
    notes: list[dict[str, Any]] = []
    previous: list[int] | None = None
    for bar in range(ctx.bars):
        tones = ctx.chords[bar]["tones"]
        if not tones:
            continue
        root = nearest_octave(tones[0], 40 + 12 * octave, 33 + 12 * octave, 48 + 12 * octave)
        options = voicing_options(tones, 3, 55 + 12 * octave, 76 + 12 * octave) or [[60, 64, 67]]
        chord = min(options, key=lambda v: moved(v, previous) + abs(sum(v) / 3 - 64 - 12 * octave))
        previous = chord
        if kind == "chorus":
            for step in range(0, ctx.steps, 4):
                notes.append(guitar_note(ctx, bar, step, 3.6, root, 0.7))
                notes.append(guitar_note(ctx, bar, step, 3.6, root + 12, 0.6))
                notes += [guitar_note(ctx, bar, step, 3.6, p, 0.66 if step else 0.75) for p in chord]
        else:
            notes.append(guitar_note(ctx, bar, 0, ctx.steps * 0.95, root, 0.62))
            broken = (chord[0], chord[2], chord[1], chord[2])
            for i, step in enumerate(range(0, ctx.steps, 2)):
                notes.append(guitar_note(ctx, bar, step, 1.9, broken[i % 4], 0.55 + (0.08 if step % 8 == 0 else 0.0)))
    return notes


def held_voicing(
    ctx: Context, layer: str, low: int, high: int, voices: int, velocity: float, bass: int | None = None
) -> list[dict[str, Any]]:
    """A voicing held for each run of one chord (organ, strings, choir), with a low root under it when `bass`."""
    kind = str(ctx.section["kind"])
    notes: list[dict[str, Any]] = []
    previous: list[int] | None = None
    for first, length in chord_spans(ctx):
        playing = [b for b in range(first, first + length) if layer_plays(layer, kind, b, ctx.bars)]
        tones = ctx.chords[first]["tones"]
        if not playing or not tones:
            continue
        options = voicing_options(tones, voices, low, high) or voicing_options(tones, voices, low - 12, high + 12)
        if not options:
            continue
        chord = min(options, key=lambda v: moved(v, previous) + ctx.rng.random() * 0.2)
        previous = chord
        held = len(playing) * ctx.steps * 0.97
        if bass is not None:
            notes.append(
                guitar_note(ctx, playing[0], 0, held, nearest_octave(tones[0], bass, bass - 6, bass + 6), velocity)
            )
        notes += [guitar_note(ctx, playing[0], 0, held, p, velocity * (0.8 + 0.2 * ctx.energy)) for p in chord]
    return notes


def write_organ(ctx: Context, octave: int = 0) -> list[dict[str, Any]]:
    """Held four-note chords over the root, as a drawbar organ pads a band."""
    return held_voicing(ctx, "organ", 53 + 12 * octave, 74 + 12 * octave, 4, 0.6, 40 + 12 * octave)


def write_strings(ctx: Context, octave: int = 0) -> list[dict[str, Any]]:
    """Sustained, widely spread voicings: the cellos' root low, violins and violas above."""
    return held_voicing(ctx, "strings", 58 + 12 * octave, 79 + 12 * octave, 3, 0.55, 45 + 12 * octave)


def write_choir(ctx: Context, octave: int = 0) -> list[dict[str, Any]]:
    """Held three-part chords in the voices' range."""
    return held_voicing(ctx, "choir", 52 + 12 * octave, 74 + 12 * octave, 3, 0.5)


def write_brass(ctx: Context, octave: int = 0) -> list[dict[str, Any]]:
    """Stabs: a short three-note chord on the bar's first beat and where the snare hits, in the choruses."""
    snare = ctx.style.lanes["snare"]
    kind = str(ctx.section["kind"])
    notes: list[dict[str, Any]] = []
    for bar in range(ctx.bars):
        tones = ctx.chords[bar]["tones"]
        if not tones or not layer_plays("brass", kind, bar, ctx.bars):
            continue
        options = voicing_options(tones, 3, 55 + 12 * octave, 72 + 12 * octave) or [[60, 64, 67]]
        chord = min(options, key=lambda v: abs(sum(v) / 3 - 63 - 12 * octave))
        for step in [0] + [s for s in range(2, ctx.steps) if snare[s % len(snare)] >= 0.6 and s % 4]:
            notes += [guitar_note(ctx, bar, step, 1.5, p, 0.82) for p in chord]
        if bar % 2 == 1:
            notes += [guitar_note(ctx, bar, ctx.steps - 2, 2, p, 0.75) for p in chord]
    return notes


def write_percussion_role(ctx: Context, octave: int = 0) -> list[dict[str, Any]]:
    """Hand percussion: a shaker on the sixteenths (accented on the off-beats), congas on the "and"s and a clap
    with the backbeat."""
    kind = str(ctx.section["kind"])
    notes: list[dict[str, Any]] = []
    for bar in range(ctx.bars):
        if not layer_plays("percussion", kind, bar, ctx.bars):
            continue
        for step in range(ctx.steps):
            notes.append(drum_note(ctx, bar, step, "hat", 0.42 if step % 2 else 0.3))
        for step in (3, 7, 11):
            notes.append(drum_note(ctx, bar, step, "tomHigh", 0.6))
        notes.append(drum_note(ctx, bar, 14, "tomMid", 0.65))
        if ctx.energy >= 0.9:
            notes += [drum_note(ctx, bar, s, "snare", 0.5) for s in (4, 12)]
    return notes


INSTRUMENT_WRITERS = {
    "rhythm-guitar": write_rhythm_guitar,
    "acoustic-guitar": write_acoustic_guitar,
    "clean-guitar": write_clean_guitar,
    "lead-guitar": write_lead_guitar,
    "piano": write_piano,
    "organ": write_organ,
    "strings": write_strings,
    "choir": write_choir,
    "brass": write_brass,
    "percussion": write_percussion_role,
}


def make_layers(
    style: Style,
    plan: dict[str, Any],
    seeds: dict[str, Any],
    sections: list[dict[str, Any]],
    parts: dict[str, dict[str, list[dict[str, Any]]]],
) -> tuple[list[dict[str, Any]], dict[str, dict[str, list[dict[str, Any]]]], dict[str, Any]]:
    """The plan's parts (the user's list) from the song's four parts: each part's notes per section by its role
    and how many of that role came before it, and the effects with where they play (bars from the song's
    start)."""
    shares = playing_shares(plan)
    last_id = plan["order"][-1]
    wanted = plan.get("parts") or default_parts(style, str(plan.get("size", "sketch")))
    instances: dict[str, int] = {}
    notes: dict[str, dict[str, list[dict[str, Any]]]] = {}
    layers: list[dict[str, Any]] = []
    for part in wanted:
        role = part["role"]
        k = instances.get(role, 0)
        instances[role] = k + 1
        base = ROLES[role][2]
        layer = {
            "id": part["id"],
            "name": part["name"],
            "role": role,
            "family": ROLES[role][1],
            "part": base,
            "soundRole": ROLES[role][3],
            "drums": ROLES[role][4],
            "instance": k,
        }
        register = layer_register(style, role, k)
        if register:
            layer["register"] = register
        layers.append(layer)
        if base in style.without or (role == "impact" and "drums" in style.without):
            continue
        by_section: dict[str, list[dict[str, Any]]] = {}
        for section in sections:
            sid = section["id"]
            if role in ("riser", "impact"):
                break
            if k == 0 and role in PARTS:
                by_section[sid] = parts[role].get(sid, [])
                continue
            # The first pad, arp... follows its part's seed (redoing the chords redoes them); others their own.
            if k == 0 or base == "fx":
                rng = rng_for(seeds["parts"].get(base, seeds["song"]), seeds["sections"][sid], role, sid)
            else:
                own = seeds.get("layers", {}).get(part["id"], mix_seed(seeds["song"], "layer", part["id"]))
                rng = rng_for(own, seeds["sections"][sid], part["id"], sid)
            ctx = Context(style, plan, section, section["chords"], rng, sid == last_id, shares)
            octave = 1 if k % 2 == 1 else 0
            if role == "drums":
                by_section[sid] = write_percussion(ctx)
            elif role == "bass":
                by_section[sid] = [
                    n
                    for n in shifted(write_bass(ctx), 12)
                    if layer_plays(
                        "bass", str(section["kind"]), int(n["start"]) // (ctx.steps * TICKS_PER_STEP), ctx.bars, 1
                    )
                ]
            elif role == "chords":
                by_section[sid] = write_arp(ctx, 1, True, "chords") if k == 1 else write_stabs(ctx)
            elif role == "lead":
                by_section[sid] = write_harmony(ctx, parts["lead"].get(sid, []))
            elif role == "pad":
                by_section[sid] = write_pad(ctx, octave)
            elif role == "arp":
                by_section[sid] = write_arp(ctx, octave, k > 0)
            elif role == "counter":
                by_section[sid] = write_counter(ctx, parts["lead"].get(sid, []), octave)
            elif role == "sub":
                by_section[sid] = write_sub(ctx)
            elif role in INSTRUMENT_WRITERS:
                by_section[sid] = INSTRUMENT_WRITERS[role](ctx, octave)
            elif role == "woodwinds":  # a line over the lead's, an octave above the counter-melody
                by_section[sid] = write_counter(ctx, parts["lead"].get(sid, []), 1, "woodwinds")
        if any(by_section.values()):
            notes[part["id"]] = by_section

    # The effects: a riser over the two bars before each chorus, an impact where a chorus or the bridge starts.
    by_id = {s["id"]: s for s in sections}
    bar_ticks = style.steps * TICKS_PER_STEP
    risers, impacts = [], []
    for i, entry in enumerate(plan_arrangement(plan, by_id)):
        kind = str(by_id[entry["section"]]["kind"])
        if kind == "chorus" and entry["bar"] >= 2:
            risers.append(entry["bar"] - 2)
        if kind in ("chorus", "bridge") and i > 0:
            impacts.append(entry["bar"])
    tonic = int(plan["key"]["tonic"])
    fx: dict[str, Any] = {}
    for layer in layers:
        if layer["role"] == "riser" and risers:
            fx[layer["id"]] = {
                "bars": 2,
                "at": risers,
                "notes": [{"start": 0, "length": 2 * bar_ticks - TICKS_PER_STEP, "pitch": 60 + tonic, "velocity": 0.8}],
            }
        elif layer["role"] == "impact" and impacts and "drums" not in style.without:
            fx[layer["id"]] = {
                "bars": 1,
                "at": impacts,
                "notes": [
                    {"start": 0, "length": TICKS_PER_STEP // 2, "drum": "kick", "velocity": 1.0},
                    {"start": 0, "length": TICKS_PER_STEP // 2, "drum": "crash", "velocity": 1.0},
                ],
            }
    layers = [layer for layer in layers if layer["id"] in notes or layer["id"] in fx]
    return layers, notes, fx


def layer_register(style: Style, role: str, instance: int = 0) -> list[int] | None:
    """The notes a part of a role plays (low, middle, high), for choosing its sound."""
    up = 12 if instance % 2 == 1 and role not in ("lead", "sub") else 0
    if role == "pad":
        return [n + up for n in pad_register(style)]
    if role == "arp" or (role == "chords" and instance == 1):
        low = max(52, min(72, style.parts["other"].median + 5)) + (12 if instance else 0)
        return [low, low + 9, low + 18]
    if role == "counter":
        centre = max(52, style.parts["vocals"].median - 7) + up
        return [centre - 7, centre, centre + 7]
    if role == "sub":
        return [SUB_RANGE[0], 31, SUB_RANGE[1]]
    if role == "riser":
        return [60, 66, 71]
    fixed = {
        "rhythm-guitar": [40, 50, 64], "lead-guitar": [55, 67, 79], "acoustic-guitar": [40, 55, 76],
        "clean-guitar": [45, 57, 76], "piano": [33, 60, 76], "organ": [40, 62, 74], "strings": [45, 65, 79],
        "brass": [55, 63, 72], "woodwinds": [60, 72, 84], "choir": [52, 62, 74],
    }  # fmt: skip
    if role in fixed:
        return [n + up for n in fixed[role]]
    found = {"bass": "bass", "chords": "other", "lead": "vocals"}.get(role)
    if found:
        p = style.parts[found]
        return [p.low + up, p.median + up, p.high + up]
    return None


def plan_arrangement(plan: dict[str, Any], by_id: dict[str, dict[str, Any]]) -> list[dict[str, Any]]:
    """The sections in order with the bar each starts at."""
    arrangement = []
    bar = 0
    for sid in plan["order"]:
        arrangement.append({"section": sid, "bar": bar})
        bar += int(by_id[sid]["bars"])
    return arrangement


# ---------------------------------------------------------------------------------------------------
# The song


def playing_shares(plan: dict[str, Any]) -> dict[str, float]:
    by_id = {s["id"]: s for s in plan["sections"]}
    total = sum(int(by_id[i]["bars"]) for i in plan["order"])
    shares = {}
    for part in PARTS:
        played = sum(
            sum(
                1
                for bar in range(int(by_id[i]["bars"]))
                if plays(part, str(by_id[i]["kind"]), bar, int(by_id[i]["bars"]))
            )
            for i in plan["order"]
        )
        shares[part] = played / max(1, total)
    return shares


def build(
    style: Style,
    plan: dict[str, Any],
    seeds: dict[str, Any],
    keep: dict[str, Any] | None = None,
    redo_part: str | None = None,
    redo_section: str | None = None,
) -> dict[str, Any]:
    """The song from its plan and seeds. With `keep` (an earlier song) and a part or section to redo, every
    other part and section is taken as it was."""
    used: set[tuple[str, ...]] = set()
    shares = playing_shares(plan)
    sections = []
    patterns: dict[str, dict[str, list[dict[str, Any]]]] = {}
    last_id = plan["order"][-1]
    kept_sections = {s["id"]: s for s in (keep or {}).get("sections", [])}
    for section in plan["sections"]:
        sid = section["id"]
        kept = kept_sections.get(sid)
        if kept is not None and redo_section != sid:
            chords = kept["chords"]
            used.add(rotation_key(tuple(kept["loop"])))
            if rotation_key(tuple(kept["loop"])) in style.vocabulary(str(plan["key"]["scale"])).own_loops:
                used.update(style.vocabulary(str(plan["key"]["scale"])).own_loops)
        else:
            chords = []
        sections.append((section, chords))
    # Sections being written anew take their chords after the kept ones (which they must not copy).
    for index, (section, chords) in enumerate(sections):
        if not chords:
            sections[index] = (section, section_chords(style, plan, section, seeds, used))

    out_sections = []
    for section, chords in sections:
        sid = section["id"]
        loop: list[str] = []
        for c in chords:
            if not loop or loop[-1] != c["roman"]:
                loop.append(c["roman"])
        hold = 2 if len(chords) >= 2 and chords[0]["roman"] == chords[1]["roman"] else 1
        loop_chords = [c["roman"] for c in chords[::hold]][:4]
        parts = {}
        for part in PARTS:
            kept_notes = ((keep or {}).get("parts") or {}).get(part, {}).get(sid)
            if keep is not None and kept_notes is not None and redo_part != part and redo_section != sid:
                parts[part] = kept_notes
                continue
            if part in style.without:
                parts[part] = []
                continue
            rng = rng_for(seeds["parts"][part], seeds["sections"][sid], part, sid)
            ctx = Context(
                style, plan, {**section, "energy": ENERGY[str(section["kind"])]}, chords, rng, sid == last_id, shares
            )
            parts[part] = WRITERS[part](ctx)
        for part in PARTS:
            patterns.setdefault(part, {})[sid] = parts[part]
        out_sections.append(
            {
                "id": sid,
                "kind": section["kind"],
                "name": KIND_WORDS[str(section["kind"])],
                "bars": int(section["bars"]),
                "contour": section.get("contour", "arch"),
                "energy": ENERGY[str(section["kind"])],
                "loop": loop_chords,
                "chords": chords,
                **({"progression": section["progression"]} if section.get("progression") else {}),
            }
        )

    by_id = {s["id"]: s for s in out_sections}
    arrangement = plan_arrangement(plan, by_id)
    bar = sum(int(by_id[sid]["bars"]) for sid in plan["order"])
    tempo = float(plan["tempo"])
    song: dict[str, Any] = {
        "kind": "spinus.song",
        "version": SONG_VERSION,
        "seeds": seeds,
        "plan": plan,
        "tempo": tempo,
        "key": plan["key"],
        "beatsPerBar": style.beats_per_bar,
        "stepsPerBar": style.steps,
        "barTicks": style.beats_per_bar * TICKS_PER_BEAT,
        "swing": round(style.swing, 3),
        "patternSwing": pattern_swing(style.swing),
        "sections": out_sections,
        "arrangement": arrangement,
        "bars": bar,
        "seconds": round(bar * style.beats_per_bar * 60.0 / tempo, 1),
        "parts": patterns,
        "refinedBy": "",
        "note": "",
        "size": str(plan.get("size", "sketch")),
    }
    song["layers"], song["layerParts"], song["fx"] = make_layers(style, plan, seeds, out_sections, patterns)
    return song


def write(
    summary: dict[str, Any], seed: int, size: str = "sketch", parts: list[dict[str, str]] | None = None
) -> dict[str, Any]:
    """A new song in the summary's style, the same for the same seed. Rules only. `parts`: the user's list
    ({id, name, role}); without one, `size` proposes it: "sketch" (four parts) or "full" (about ten)."""
    style = Style.from_summary(summary)
    plan = make_plan(style, seed, size, parts)
    song = build(style, plan, new_seeds(seed, plan))
    return finish(song, summary, style)


def redo(
    song: dict[str, Any], summary: dict[str, Any], seed: int, part: str | None = None, section: str | None = None
) -> dict[str, Any]:
    """The song with one part (in every section) or one section (every part, and its chords) written anew from
    `seed`; the rest as it was."""
    check_song(song)
    style = Style.from_summary(summary)
    plan = json.loads(json.dumps(song["plan"]))
    seeds = json.loads(json.dumps(song["seeds"]))
    if part is not None:
        # A part of the list: the first of the four roles is that part (and what is made from it); another
        # gets a seed of its own.
        layer = next((p for p in plan.get("parts", []) if p["id"] == part), None)
        if (
            layer is not None
            and layer["role"] in PARTS
            and part == next(p["id"] for p in plan["parts"] if p["role"] == layer["role"])
        ):
            part = layer["role"]
        elif layer is not None:
            seeds.setdefault("layers", {})[part] = mix_seed(seed, "layer", part)
            again = build(style, plan, seeds, keep=song)
            again["refinedBy"] = song.get("refinedBy", "")
            return finish(again, summary, style)
        if part not in PARTS:
            raise ValueError(f"There is no part '{part}' to write again.")
        seeds["parts"][part] = mix_seed(seed, "part", part)
    if section is not None:
        found = [s for s in plan["sections"] if s["id"] == section]
        if not found:
            raise ValueError(f"There is no section '{section}' to write again.")
        found[0].pop("progression", None)  # new chords too
        seeds["sections"][section] = mix_seed(seed, "section", section)
    if part is None and section is None:
        raise ValueError("Say which part or section to write again.")
    again = build(style, plan, seeds, keep=song, redo_part=part, redo_section=section)
    again["refinedBy"] = song.get("refinedBy", "")
    return finish(again, summary, style)


def check_song(song: Any) -> None:
    if not isinstance(song, dict) or song.get("kind") != "spinus.song" or song.get("version") != SONG_VERSION:
        raise ValueError("This is not a song the Track creator wrote (or one of another version).")


def finish(song: dict[str, Any], summary: dict[str, Any], style: Style) -> dict[str, Any]:
    song["checks"] = compare(song, summary, style)
    song["plain"] = describe(song)
    return song


# ---------------------------------------------------------------------------------------------------
# Comparing with the style, in numbers


def cosine(a: list[float], b: list[float]) -> float:
    dot = sum(x * y for x, y in zip(a, b, strict=False))
    norm = math.sqrt(sum(x * x for x in a)) * math.sqrt(sum(y * y for y in b))
    return dot / norm if norm > 0 else (1.0 if not any(a) and not any(b) else 0.0)


def compare(song: dict[str, Any], summary: dict[str, Any], style: Style | None = None) -> dict[str, Any]:
    """How the song stands to the style, in numbers: tempo, key, chords against the vocabulary, the drums'
    steps against the groove lanes, each part's notes a bar and register, section lengths, and how many notes
    fit the key and the chords."""
    style = style or Style.from_summary(summary)
    by_id = {s["id"]: s for s in song["sections"]}
    steps, bar_ticks = int(song["stepsPerBar"]), int(song["barTicks"])
    total_bars = int(song["bars"])
    key = song["key"]
    scale = scale_of(key)
    vocab = style.vocabulary(str(key["scale"]))
    allowed = set(vocab.allowed())

    # Drums: how often each step of each lane is hit in the bars the drums play, fills and endings left out.
    lane_of = {"kick": "kick", "snare": "snare", "hat": "hats"}
    counts = {lane: [0] * steps for lane in style.lanes}
    groove_bars = 0
    drum_hits = 0
    for entry in song["arrangement"]:
        section = by_id[entry["section"]]
        notes = song["parts"]["drums"].get(entry["section"], [])
        drum_hits += len(notes)
        bars_with = {round(n["start"] / TICKS_PER_STEP) // steps for n in notes}
        for bar in bars_with:
            if bar == int(section["bars"]) - 1:
                continue
            groove_bars += 1
            for n in notes:
                s = round(n["start"] / TICKS_PER_STEP)
                if s // steps == bar and n["drum"] in lane_of:
                    counts[lane_of[n["drum"]]][s % steps] += 1
    lanes = {lane: [round(c / max(1, groove_bars), 3) for c in row] for lane, row in counts.items()}
    groove = {lane: round(cosine(lanes[lane], style.lanes[lane]), 3) for lane in style.lanes}
    # For scale: how alike each reference song's own steps are to those lanes (their average), the median.
    reference: dict[str, float] = {}
    for lane in style.lanes:
        alike = sorted(
            cosine(resample([float(v) for v in s["groove"]["lanes"][lane]], steps), style.lanes[lane])
            for s in summary.get("perSong") or []
            if isinstance(s, dict) and isinstance(s.get("groove"), dict) and lane in (s["groove"].get("lanes") or {})
        )
        if alike:
            reference[lane] = round(alike[len(alike) // 2], 3)

    # Pitched parts over the whole song, as the summary measures the stems.
    parts = {}
    in_key = fits_chord = strong_on_chord = strong_total = pitched = 0
    for part in ("bass", "chords", "lead"):
        pitches: list[int] = []
        for entry in song["arrangement"]:
            section = by_id[entry["section"]]
            for n in song["parts"][part].get(entry["section"], []):
                pitches.append(int(n["pitch"]))
                bar = min(int(section["bars"]) - 1, int(n["start"]) // bar_ticks)
                tones = set(section["chords"][bar]["tones"])
                pitched += 1
                in_key += 1 if n["pitch"] % 12 in scale else 0
                fits_chord += 1 if n["pitch"] % 12 in allowed_pitch_classes(list(tones), scale) else 0
                if part == "lead" and (int(n["start"]) % bar_ticks) % (bar_ticks // 2) == 0:
                    strong_total += 1
                    strong_on_chord += 1 if n["pitch"] % 12 in tones else 0
        pitches.sort()
        target = style.parts[SUMMARY_PART[part]]
        parts[part] = {
            "notesPerBar": round(len(pitches) / max(1, total_bars), 2),
            "styleNotesPerBar": target.per_bar,
            "low": pitches[0] if pitches else None,
            "median": pitches[len(pitches) // 2] if pitches else None,
            "high": pitches[-1] if pitches else None,
            "styleRegister": [target.low, target.median, target.high],
            "insideRegister": round(
                sum(1 for p in pitches if target.low <= p <= target.high) / max(1, len(pitches)), 3
            ),
        }
    parts["drums"] = {
        "notesPerBar": round(drum_hits / max(1, total_bars), 2),
        "styleNotesPerBar": float(((summary.get("parts") or {}).get("drums") or {}).get("notesPerBar") or 0),
    }

    used = Counter(c["roman"] for e in song["arrangement"] for c in by_id[e["section"]]["chords"])
    loops = [tuple(s["loop"]) for s in song["sections"]]
    tempo = summary.get("tempo") or {}
    lengths = sorted({b for b, _ in style.lengths})
    return {
        "tempo": {"song": song["tempo"], "style": tempo.get("median"), "range": [tempo.get("min"), tempo.get("max")]},
        "key": {
            "song": key["name"],
            "styleKeys": [k.get("name") for k in summary.get("keys") or []],
            "styleModes": summary.get("modes"),
        },
        "chords": {
            "used": dict(used.most_common()),
            "inVocabulary": round(sum(n for c, n in used.items() if c in allowed) / max(1, sum(used.values())), 3),
            "vocabulary": sorted(allowed, key=lambda c: -vocab.counts[c]),
            "loops": [list(loop) for loop in loops],
            "loopsOfOneReference": sum(1 for loop in loops if rotation_key(loop) in vocab.own_loops),
        },
        "groove": groove,
        "referenceGroove": reference,
        "lanes": lanes,
        "parts": parts,
        "notesInKey": round(in_key / max(1, pitched), 3),
        "notesFitChords": round(fits_chord / max(1, pitched), 3),
        "leadStrongBeatsOnChordNotes": round(strong_on_chord / max(1, strong_total), 3),
        "sections": {
            "song": [
                {"name": by_id[e["section"]]["name"], "bars": by_id[e["section"]]["bars"]} for e in song["arrangement"]
            ],
            "onPhrases": all(int(s["bars"]) % 4 == 0 for s in song["sections"]),
            "styleLengths": lengths,
            "fromStyleLengths": all(int(s["bars"]) in lengths for s in song["sections"]),
            "bars": total_bars,
            "styleBars": (summary.get("sections") or {}).get("songBars"),
            "count": len(song["arrangement"]),
            "styleCount": (summary.get("sections") or {}).get("perSong"),
        },
    }


def describe(song: dict[str, Any]) -> list[str]:
    """What will be written, in plain words."""
    minutes, seconds = divmod(round(float(song["seconds"])), 60)
    by_id = {s["id"]: s for s in song["sections"]}
    lines = [
        f"A new song of {song['bars']} bars ({minutes}:{seconds:02d}) at {song['tempo']:g} BPM in "
        f"{song['key']['name']}, {song['beatsPerBar']} beats a bar.",
        "Sections: "
        + ", ".join(f"{by_id[e['section']]['name']} {by_id[e['section']]['bars']}" for e in song["arrangement"])
        + " (bars).",
    ]
    for s in song["sections"]:
        names: list[str] = []
        for c in s["chords"]:
            if not names or names[-1] != c["name"]:
                names.append(c["name"])
        loop = " - ".join(s["loop"])
        lines.append(f"{s['name']} chords: {loop} ({' '.join(names[:4])}).")
    checks = song.get("checks") or {}
    parts = checks.get("parts") or {}
    if parts:
        words = []
        for part in ("bass", "chords", "lead"):
            p = parts[part]
            if p.get("low") is not None:
                words.append(
                    f"{PART_WORDS[part].lower()} {note_name(p['low'])}-{note_name(p['high'])}, "
                    f"{p['notesPerBar']:g} notes a bar (the songs' {p['styleNotesPerBar']:g})"
                )
        lines.append("Parts: " + "; ".join(words) + ".")
    groove = checks.get("groove") or {}
    if groove:
        lines.append(
            "Drums from the songs' groove (alike: "
            + ", ".join(f"{lane} {round(100 * v)} %" for lane, v in groove.items())
            + "), a fill at the end of each section."
        )
    if song.get("layers"):
        lines.append(
            f"{len(song['layers'])} parts, a channel each: "
            + ", ".join(str(layer["name"]) for layer in song["layers"])
            + ". Thin verses, full choruses."
        )
    if song.get("refinedBy"):
        lines.append(f"Varied by the AI ({song['refinedBy']}); every note written by the rules.")
    if song.get("note"):
        lines.append(str(song["note"]))
    return lines


# ---------------------------------------------------------------------------------------------------
# The AI's variation of the plan (optional)

SYSTEM = """You help Spinus, a digital audio workstation, write a new song in the style of some reference \
songs. Spinus's rules have planned the song; you may vary the plan so it is more interesting while staying \
in the style. You answer only by calling the refine_song tool once. You may change: the order and number of \
sections (intro, verse, chorus, bridge, outro; each kind keeps one length), their lengths in bars (multiples \
of 4, from 4 to 32), each section's four-chord loop (as Roman numerals, only from the style's chord \
vocabulary given), and the melody's shape in each section (rise, fall, arch, wave or valley). Leave out what \
you would keep. Spinus writes every note itself from the plan; never copy a reference song."""

REFINE_SONG = Tool(
    "refine_song",
    "Vary the song's plan: sections, chord loops, melody shapes. Leave out what stays.",
    {
        "type": "object",
        "properties": {
            "sections": {
                "type": "array",
                "description": "The song's sections in order",
                "items": {
                    "type": "object",
                    "properties": {
                        "kind": {"type": "string", "enum": list(KINDS)},
                        "bars": {"type": "integer", "description": "A multiple of 4, 4 to 32"},
                    },
                    "required": ["kind", "bars"],
                },
            },
            "loops": {
                "type": "object",
                "description": "Section kind -> its chord loop (2 to 8 Roman numerals from the vocabulary)",
                "additionalProperties": {"type": "array", "items": {"type": "string"}},
            },
            "contours": {
                "type": "object",
                "description": "Section kind -> the melody's shape",
                "additionalProperties": {"type": "string", "enum": list(CONTOURS)},
            },
            "reason": {"type": "string", "description": "In one sentence, what you changed and why"},
        },
    },
)


def describe_for_ai(style: Style, song: dict[str, Any]) -> str:
    vocab = style.vocabulary(str(song["key"]["scale"]))
    return json.dumps(
        {
            "style": {
                "tempo": style.tempo,
                "sectionLengths": [{"bars": b, "count": n} for b, n in style.lengths],
                "sectionsPerSong": style.sections_per_song,
                "songBars": style.song_bars,
                "chordVocabulary": {c: round(vocab.share(c), 3) for c in vocab.allowed()},
                "commonTransitions": {
                    a: dict(after.most_common(4)) for a, after in vocab.transitions.items() if a in vocab.allowed()
                },
            },
            "plan": {
                "tempo": song["tempo"],
                "key": song["key"]["name"],
                "sections": [
                    {"kind": song_section(song, e["section"])["kind"], "bars": song_section(song, e["section"])["bars"]}
                    for e in song["arrangement"]
                ],
                "loops": {s["kind"]: s["loop"] for s in song["sections"]},
                "contours": {s["kind"]: s["contour"] for s in song["sections"]},
            },
        },
        indent=1,
    )


def song_section(song: dict[str, Any], sid: str) -> dict[str, Any]:
    return next(s for s in song["sections"] if s["id"] == sid)


def check_refinement(answer: Any, style: Style, song: dict[str, Any]) -> tuple[dict[str, Any] | None, list[str]]:
    """The plan with the AI's changes that pass the style's limits, and what was taken in words; (None, [])
    when nothing usable came."""
    if not isinstance(answer, dict):
        return None, []
    plan = json.loads(json.dumps(song["plan"]))
    vocab = style.vocabulary(str(plan["key"]["scale"]))
    allowed = set(vocab.allowed())
    taken: list[str] = []

    sections = answer.get("sections")
    if isinstance(sections, list) and 4 <= len(sections) <= 12:
        order: list[str] = []
        bars: dict[str, int] = {}
        ok = True
        for s in sections:
            if not isinstance(s, dict) or s.get("kind") not in KINDS or not isinstance(s.get("bars"), int):
                ok = False
                break
            kind, length = str(s["kind"]), int(s["bars"])
            if length % 4 or not SECTION_BARS[0] <= length <= SECTION_BARS[1]:
                ok = False
                break
            bars.setdefault(kind, length)  # one length per kind (one pattern)
            order.append(kind)
        total = sum(bars[k] for k in order) if ok else 0
        if ok and ("verse" in order or "chorus" in order) and 16 <= total <= MOST_BARS:
            old = {s["id"]: s for s in plan["sections"]}
            plan["sections"] = [
                {**old.get(kind, {"id": kind, "kind": kind, "contour": "arch"}), "bars": bars[kind]}
                for kind in KINDS
                if kind in bars
            ]
            plan["order"] = order
            taken.append("the sections: " + ", ".join(f"{KIND_WORDS[k]} {bars[k]}" for k in order))

    loops = answer.get("loops")
    if isinstance(loops, dict):
        for kind, loop in loops.items():
            section = next((s for s in plan["sections"] if s["kind"] == kind), None)
            if section is None or not isinstance(loop, list) or not 2 <= len(loop) <= 8:
                continue
            romans = [str(c) for c in loop]
            if all(c in allowed for c in romans) and len(set(romans)) >= 2:
                section["progression"] = romans
                taken.append(f"the {kind}'s chords {' - '.join(romans)}")
    # Chords from one reference loop at most once (as the rules do).
    own = [
        s for s in plan["sections"] if s.get("progression") and rotation_key(tuple(s["progression"])) in vocab.own_loops
    ]
    for s in own[1:]:
        s.pop("progression")

    contours = answer.get("contours")
    if isinstance(contours, dict):
        for kind, shape in contours.items():
            section = next((s for s in plan["sections"] if s["kind"] == kind), None)
            if section is not None and shape in CONTOURS:
                section["contour"] = shape
                taken.append(f"{'an' if shape[0] in 'aeiou' else 'a'} {shape} melody in the {kind}")
    if not taken:
        return None, []
    return plan, taken


async def refine(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routing: Routing,
    usage_log: UsageLog,
    summary: dict[str, Any],
    song: dict[str, Any],
    say: Say,
) -> dict[str, Any]:
    """The rules' song as the AI varied its plan, written again by the rules; NoConnection without one. When
    the answer is not usable, the rules' song with a note saying so."""
    routes = routing.resolve("patterns")
    if not routes:
        raise NoConnection("No AI connection is set up.")
    style = Style.from_summary(summary)

    async def no_text(_: str) -> None:
        pass

    await say({"kind": "status", "text": "The AI is varying the plan..."})
    request = Request(
        routes[0].model, SYSTEM, [Message("user", [Text(describe_for_ai(style, song))])], [REFINE_SONG], 3000
    )
    failed: list[str] = []
    completion, index = await complete_with_fallback(client, connections, routes, 0, request, no_text, say, failed)
    usage_log.add(routes[index].connection.id, "patterns", completion.usage)
    for use in completion.tool_uses:
        if use.name != "refine_song":
            continue
        plan, taken = check_refinement(use.input, style, song)
        if plan is None:
            break
        # Sections new to the plan get seeds of their own.
        seeds = json.loads(json.dumps(song["seeds"]))
        for s in plan["sections"]:
            seeds["sections"].setdefault(s["id"], mix_seed(seeds["song"], "section", s["id"]))
        varied = build(style, plan, seeds)
        varied["refinedBy"] = answered_by(completion.model, failed)
        varied["note"] = "The AI changed " + "; ".join(taken) + "."
        return finish(varied, summary, style)
    song = dict(song)
    song["note"] = "The AI's answer was not usable; the rules' song stands."
    return finish(song, summary, style)
