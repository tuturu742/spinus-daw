"""The Track creator's second way in: a song described in words ("a slow, dark depressive black metal song in
D minor, 72 BPM, with a piano") read into a style summary that the song writer (spinus_ai.songwriter) and the
sound choice take as they take the summary of reference songs.

Offline by default: a small table of genres (their usual tempo, modes, chord loops, drum groove, parts,
section lengths and loudness) and the words of the description read with rules: a genre's names, a tempo
("72 BPM"), a key ("D minor", "F#m"), mood and pace words (slow, fast, dark, happy, calm, energetic...), the
instruments it names (piano, strings, guitar...: the sounds to look for), what to leave out ("no drums") and
how long ("short", "long"). What was understood is said in plain words, so the user can see what Spinus took.

With an AI connection, when the user asks, the AI reads the words instead, answering only through the
read_description tool with the same fields (a genre from the table, a tempo, a key, moods...); its answer is
checked like the rules' reading, and the summary is built from it the same way. When the AI is not reachable
or answers something unusable, the rules' reading stands, with a note saying so.

Pure Python: nothing downloaded, no model.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Any

import httpx

from .assistant import NoConnection, Say, complete_with_fallback
from .connections import ConnectionStore
from .gateway import Message, Request, Text, Tool
from .routing import Routing
from .style import SUMMARY_VERSION
from .usage import UsageLog

NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
FLATS = {"Db": 1, "Eb": 3, "Gb": 6, "Ab": 8, "Bb": 10}
STEPS = 16


def lane(*steps: int, rest: float = 0.0, also: dict[int, float] | None = None) -> list[float]:
    """A drum lane of 16 steps: the given steps always (0.95), `also` sometimes, the rest at `rest`."""
    values = [rest] * STEPS
    for s in steps:
        values[s] = 0.95
    for s, p in (also or {}).items():
        values[s] = p
    return values


@dataclass
class Genre:
    """What a genre's songs usually have, in the style summary's own terms."""

    id: str
    name: str  # as the user reads it ("depressive black metal")
    words: tuple[str, ...]  # the names it is known by in a description, longest first
    tempo: float
    tempo_range: tuple[float, float]
    minor: float  # how often minor (the rest major)
    loops: dict[str, list[tuple[str, ...]]]  # per mode, chord loops (Roman numerals), likelier first
    bars_per_chord: float
    kick: list[float]
    snare: list[float]
    hats: list[float]
    swing: float
    bass: tuple[int, int, int, float]  # low, middle, high, notes a bar
    other: tuple[int, int, int, float, float]  # low, middle, high, notes a bar, voices
    vocals: tuple[int, int, int, float]
    drum_hits: float  # drum notes a bar
    lengths: dict[int, int]  # section bars -> how often
    sections: float  # sections a song
    song_bars: float
    lufs: float
    drums: bool = True


FOUR = lane(0, 4, 8, 12)
BACKBEAT = lane(4, 12)
EIGHTHS = lane(0, 2, 4, 6, 8, 10, 12, 14, rest=0.1)
SIXTEENTHS = lane(*range(16), rest=0.0)

GENRES: tuple[Genre, ...] = (
    Genre(
        "dsbm", "depressive black metal", ("depressive suicidal black metal", "depressive black metal", "dsbm"),
        76, (62, 92), 0.95,
        {"minor": [("i", "bVI", "bVII", "i"), ("i", "iv", "bVI", "v"), ("i", "bIII", "bVII", "bVI")],
         "major": [("I", "vi", "IV", "V")]},
        2.0, lane(0, 8, also={10: 0.4}), lane(4, 12), lane(*range(0, 16, 2), rest=0.2), 0.5,
        (33, 40, 52, 4.0), (52, 64, 76, 16.0, 2.2), (55, 62, 72, 1.5), 12.0,
        {16: 6, 8: 3, 32: 1}, 7, 176, -10.0,
    ),
    Genre(
        "blackmetal", "black metal", ("black metal", "blackened", "blast beats"),
        170, (140, 200), 0.95,
        {"minor": [("i", "bVI", "bVII", "i"), ("i", "bII", "i", "bVII"), ("i", "iv", "bVI", "bVII")],
         "major": [("I", "bVII", "IV", "I")]},
        2.0, SIXTEENTHS, lane(2, 6, 10, 14), lane(*range(0, 16, 2), rest=0.3), 0.5,
        (31, 40, 52, 8.0), (52, 64, 79, 16.0, 2.0), (55, 62, 72, 2.0), 30.0,
        {16: 6, 8: 4}, 7, 160, -9.0,
    ),
    Genre(
        "synthwave", "synthwave", ("synthwave", "retrowave", "outrun", "80s synth", "synth pop", "synthpop"),
        108, (84, 120), 0.7,
        {"minor": [("i", "bVI", "bIII", "bVII"), ("i", "bVII", "bVI", "bVII"), ("i", "iv", "bVI", "bVII")],
         "major": [("I", "V", "vi", "IV"), ("vi", "IV", "I", "V")]},
        1.0, lane(0, 8, also={10: 0.3}), BACKBEAT, EIGHTHS, 0.5,
        (28, 38, 50, 8.0), (52, 62, 76, 6.0, 3.0), (55, 66, 76, 3.0), 13.0,
        {8: 6, 16: 4, 4: 2}, 8, 112, -9.0,
    ),
    Genre(
        "lofi", "lo-fi hip hop", ("lo-fi hip hop", "lofi hip hop", "lo-fi", "lofi", "chillhop"),
        80, (70, 92), 0.45,
        {"minor": [("i7", "iv7", "bVImaj7", "v7"), ("ii7", "v7", "i7", "i7")],
         "major": [("Imaj7", "vi7", "ii7", "V7"), ("IVmaj7", "iii7", "ii7", "Imaj7")]},
        1.0, lane(0, 10, also={7: 0.4}), BACKBEAT, lane(*range(0, 16, 2), rest=0.15), 0.62,
        (31, 40, 50, 3.0), (52, 63, 74, 6.0, 3.5), (60, 67, 76, 2.5), 10.0,
        {8: 8, 4: 2}, 7, 64, -12.0,
    ),
    Genre(
        "house", "house", ("deep house", "house music", "house"),
        124, (118, 128), 0.6,
        {"minor": [("i", "bVII", "bVI", "bVII"), ("i", "iv", "i", "v")],
         "major": [("vi", "IV", "I", "V"), ("ii7", "V7", "Imaj7", "vi7")]},
        1.0, FOUR, BACKBEAT, lane(2, 6, 10, 14, rest=0.2), 0.5,
        (31, 40, 50, 8.0), (55, 64, 76, 4.0, 3.0), (57, 66, 76, 3.0), 14.0,
        {16: 6, 8: 4, 32: 2}, 8, 160, -8.0,
    ),
    Genre(
        "techno", "techno", ("techno", "industrial techno", "minimal techno"),
        132, (125, 145), 0.85,
        {"minor": [("i", "i", "bVII", "i"), ("i", "bII", "i", "bVII")], "major": [("I", "I", "bVII", "I")]},
        4.0, FOUR, lane(4, 12, rest=0.05), SIXTEENTHS, 0.5,
        (28, 36, 46, 8.0), (52, 60, 72, 4.0, 1.5), (57, 64, 72, 1.0), 20.0,
        {16: 6, 32: 4, 8: 2}, 7, 192, -8.0,
    ),
    Genre(
        "trap", "trap", ("trap beat", "trap", "drill"),
        140, (130, 160), 0.85,
        {"minor": [("i", "bVI", "bIII", "bVII"), ("i", "iv", "bVI", "v")], "major": [("vi", "IV", "I", "V")]},
        2.0, lane(0, also={3: 0.5, 7: 0.4, 10: 0.6}), lane(8), lane(*range(16), rest=0.0), 0.5,
        (24, 33, 45, 3.0), (55, 67, 79, 4.0, 1.5), (55, 62, 72, 4.0), 20.0,
        {8: 6, 16: 4}, 7, 96, -8.0,
    ),
    Genre(
        "ambient", "ambient", ("ambient", "drone", "soundscape", "meditation"),
        70, (55, 90), 0.5,
        {"minor": [("i", "bVI", "bIII", "bVII"), ("i", "iv", "i", "bVI")],
         "major": [("I", "IV", "vi", "V"), ("Imaj7", "IVmaj7", "Imaj7", "vi7")]},
        4.0, lane(0, rest=0.0), lane(rest=0.0), lane(rest=0.0), 0.5,
        (28, 38, 48, 1.0), (52, 64, 79, 2.0, 4.0), (60, 69, 79, 1.0), 1.0,
        {16: 6, 32: 2}, 5, 128, -16.0, False,
    ),
    Genre(
        "pop", "pop", ("pop song", "pop"),
        116, (96, 128), 0.4,
        {"minor": [("i", "bVI", "bIII", "bVII"), ("vi", "IV", "I", "V")],
         "major": [("I", "V", "vi", "IV"), ("vi", "IV", "I", "V"), ("I", "IV", "vi", "V")]},
        1.0, lane(0, 8, also={6: 0.3, 10: 0.4}), BACKBEAT, EIGHTHS, 0.5,
        (31, 40, 50, 6.0), (52, 62, 74, 6.0, 3.0), (57, 66, 76, 4.0), 13.0,
        {8: 8, 4: 2, 16: 2}, 8, 104, -8.0,
    ),
    Genre(
        "rock", "rock", ("hard rock", "punk rock", "alternative rock", "indie rock", "rock"),
        130, (100, 160), 0.45,
        {"minor": [("i", "bVII", "bVI", "bVII"), ("i", "bIII", "bVII", "iv")],
         "major": [("I", "bVII", "IV", "I"), ("I", "V", "IV", "IV"), ("I", "IV", "V", "IV")]},
        1.0, lane(0, 8, also={6: 0.35, 10: 0.5}), BACKBEAT, EIGHTHS, 0.5,
        (28, 40, 52, 8.0), (48, 60, 72, 8.0, 2.5), (55, 66, 76, 3.5), 14.0,
        {8: 8, 16: 3}, 8, 112, -8.0,
    ),
    Genre(
        "dnb", "drum and bass", ("drum and bass", "drum & bass", "drum n bass", "dnb", "d&b", "jungle", "liquid"),
        174, (165, 178), 0.8,
        {"minor": [("i", "bVI", "bIII", "bVII"), ("i", "iv", "bVI", "bVII"), ("i", "i", "bVI", "bVI")],
         "major": [("vi", "IV", "I", "V")]},
        2.0, lane(0, 10, also={7: 0.3}), lane(4, 12, also={15: 0.3}), lane(*range(0, 16, 2), rest=0.35), 0.5,
        (24, 36, 48, 4.0), (55, 64, 76, 4.0, 3.0), (57, 66, 76, 2.5), 16.0,
        {16: 6, 32: 3, 8: 2}, 7, 176, -8.0,
    ),
)  # fmt: skip
BY_ID = {g.id: g for g in GENRES}
DEFAULT = "pop"

# Mood and pace words: what they move. Tempo by a factor, minor's share, busier (+) or calmer (-).
SLOW = ("slow", "slower", "downtempo", "sluggish", "crawling", "ballad")
FAST = ("fast", "faster", "quick", "uptempo", "up-tempo", "rapid", "frantic")
DARK = ("dark", "sad", "melancholic", "melancholy", "depressing", "depressive", "gloomy", "grim", "cold", "bleak",
        "somber", "sombre", "haunting", "eerie", "lonely", "mournful", "moody", "ominous", "desperate")  # fmt: skip
BRIGHT = ("happy", "bright", "uplifting", "joyful", "cheerful", "sunny", "hopeful", "euphoric", "warm", "playful")
CALM = ("calm", "chill", "chilled", "mellow", "relaxed", "relaxing", "soft", "gentle", "dreamy", "peaceful", "quiet")
BUSY = ("energetic", "aggressive", "intense", "driving", "heavy", "powerful", "angry", "furious", "raw", "punchy")
SHORT = ("short", "brief", "quick sketch")
LONG = ("long", "epic", "extended")

# Instruments a description may name: the layer whose sound should be one, and the word to look for in the
# sound's name (a preset called "Grand Piano"). Longest first.
INSTRUMENTS: tuple[tuple[str, str, str], ...] = (
    ("electric piano", "chords", "piano"), ("rhodes", "chords", "rhodes"), ("piano", "chords", "piano"),
    ("organ", "chords", "organ"), ("acoustic guitar", "chords", "guitar"), ("guitar", "chords", "guitar"),
    ("keys", "chords", "keys"), ("strings", "pad", "string"), ("violin", "lead", "violin"),
    ("cello", "counter", "cello"), ("choir", "pad", "choir"), ("pad", "pad", "pad"), ("bells", "arp", "bell"),
    ("bell", "arp", "bell"), ("arpeggio", "arp", "arp"), ("pluck", "arp", "pluck"), ("flute", "lead", "flute"),
    ("saxophone", "lead", "sax"), ("sax", "lead", "sax"), ("trumpet", "lead", "trumpet"),
    ("brass", "counter", "brass"), ("synth lead", "lead", "lead"), ("808", "bass", "808"),
    ("sub bass", "bass", "sub"), ("reese", "bass", "reese"), ("bass guitar", "bass", "bass"),
)  # fmt: skip
WITHOUT = {"drums": ("no drums", "without drums", "drumless", "no beat", "without a beat", "beatless"),
           "lead": ("no melody", "without melody", "without a melody", "no lead", "instrumental bed")}  # fmt: skip

KEY = re.compile(
    r"\b([a-g])\s?(#|♯|sharp|b(?=\s?(?:maj|min|m\b|major|minor))|♭|flat)?\s?"
    r"(major|minor|maj\b|min\b|m\b)",
    re.IGNORECASE,
)
TEMPO = re.compile(r"\b(\d{2,3}(?:\.\d+)?)\s?(?:bpm|beats per minute|beats a minute)\b", re.IGNORECASE)
TEMPO_BEFORE = re.compile(r"\b(?:tempo|bpm)(?:\s+of|\s*[:=])?\s+(\d{2,3})\b", re.IGNORECASE)


@dataclass
class Reading:
    """What a description says, in the summary's terms (the rules' reading, or the AI's checked)."""

    genre: str = DEFAULT
    genre_named: bool = False  # the description named it (else the default)
    tempo: float | None = None  # said in BPM
    tonic: int | None = None
    scale: str | None = None  # "minor" or "major", said or from moods
    moods: list[str] = field(default_factory=list)  # the words understood, as written
    pace: float = 1.0  # tempo factor from slow/fast
    minor_lean: float = 0.0  # -1 bright .. +1 dark
    energy: float = 0.0  # -1 calm .. +1 busy
    length: float = 1.0  # song length factor
    sounds: dict[str, str] = field(default_factory=dict)  # layer -> a word to look for in a sound's name
    instruments: list[str] = field(default_factory=list)  # as written
    without: list[str] = field(default_factory=list)  # parts to leave out


def words_in(text: str, words: tuple[str, ...]) -> list[str]:
    return [w for w in words if re.search(rf"(?<![a-z0-9]){re.escape(w)}(?![a-z0-9])", text)]


def read(text: str) -> Reading:
    """The rules' reading of a description."""
    lower = " ".join(text.lower().split())
    reading = Reading()
    best: tuple[int, int, Genre] | None = None
    for g in GENRES:
        for w in g.words:
            found = re.search(rf"(?<![a-z0-9]){re.escape(w)}(?![a-z0-9])", lower)
            if found and (best is None or len(w) > best[0] or (len(w) == best[0] and found.start() < best[1])):
                best = (len(w), found.start(), g)
    named = ""
    if best is not None:
        reading.genre, reading.genre_named = best[2].id, True
        named = next(w for w in best[2].words if len(w) == best[0] and w in lower)

    tempo = TEMPO.search(lower) or TEMPO_BEFORE.search(lower)
    if tempo:
        bpm = float(tempo.group(1))
        if 40 <= bpm <= 240:
            reading.tempo = bpm

    for match in KEY.finditer(text):
        letter, accidental, scale = match.group(1).upper(), (match.group(2) or "").lower(), match.group(3).lower()
        # "a minor" at the start of a phrase ("a minor key") and "a m..." are taken as A only with a scale word.
        tonic = NAMES.index(letter)
        if accidental in ("#", "♯", "sharp"):
            tonic = (tonic + 1) % 12
        elif accidental in ("b", "♭", "flat"):
            tonic = (tonic - 1) % 12
        reading.tonic = tonic
        reading.scale = "major" if scale in ("major", "maj") else "minor"
        break

    slow, fast = words_in(lower, SLOW), words_in(lower, FAST)
    dark, bright = words_in(lower, DARK), words_in(lower, BRIGHT)
    calm, busy = words_in(lower, CALM), words_in(lower, BUSY)
    reading.pace = 0.85 if slow and not fast else 1.15 if fast and not slow else 1.0
    reading.minor_lean = (len(dark) > 0) - (len(bright) > 0)
    reading.energy = (len(busy) > 0) - (len(calm) > 0)
    reading.length = 0.7 if words_in(lower, SHORT) else 1.35 if words_in(lower, LONG) else 1.0
    # Words of the genre's own name ("depressive" black metal) are not moods of their own.
    reading.moods = [w for w in (*slow, *fast, *dark, *bright, *calm, *busy) if w not in named.split()]
    taken: set[str] = set()
    for word, layer, look_for in INSTRUMENTS:
        if words_in(lower, (word,)) and layer not in reading.sounds and not any(word in t for t in taken):
            reading.sounds[layer] = look_for
            reading.instruments.append(word)
            taken.add(word)
    for part, phrases in WITHOUT.items():
        if words_in(lower, phrases):
            reading.without.append(part)
    if not BY_ID[reading.genre].drums and "drums" not in reading.without:
        reading.without.append("drums")
    return reading


def understood(reading: Reading, tempo: float, key: str) -> str:
    """'Understood: 72 BPM, D minor, slow, dark; genre: depressive black metal'."""
    genre = BY_ID[reading.genre]
    said = [f"{tempo:g} BPM", key, *reading.moods]
    text = "Understood: " + ", ".join(said) + "; genre: " + genre.name
    if not reading.genre_named:
        text += " (no genre named: Spinus's default)"
    if reading.instruments:
        text += "; sounds: " + ", ".join(reading.instruments)
    if reading.without:
        text += "; without " + " and ".join("a lead" if p == "lead" else p for p in reading.without)
    return text


def to_summary(reading: Reading, text: str, read_by: str = "") -> dict[str, Any]:
    """A style summary (as spinus_ai.style.summarize makes one) from a reading: the genre's numbers, moved by what
    the description says."""
    genre = BY_ID[reading.genre]
    low, high = genre.tempo_range
    tempo = reading.tempo or round(min(max(genre.tempo * reading.pace, low * 0.9), high * 1.1))
    if reading.scale is not None:
        modes = {reading.scale: 1.0}
    else:
        minor = min(1.0, max(0.0, genre.minor + 0.35 * reading.minor_lean))
        modes = {m: round(v, 3) for m, v in (("minor", minor), ("major", 1.0 - minor)) if v > 0}
    if reading.tonic is not None and reading.scale is not None:
        tonic, scale = reading.tonic, reading.scale
    else:
        scale = max(modes, key=lambda m: modes[m])
        tonic = 9 if scale == "minor" else 0  # A minor, C major: moved by the plan's own chance
        if genre.id in ("dsbm", "blackmetal", "rock"):
            tonic = 4 if scale == "minor" else 7  # E minor, G major: guitars' keys
    key = f"{NAMES[tonic]} {scale}"
    keys = [{"name": key, "count": 3}]

    busy = 1.0 + 0.3 * reading.energy
    chords = chords_of(genre, list(modes))
    bass_low, bass_mid, bass_high, bass_per = genre.bass
    o_low, o_mid, o_high, o_per, o_voices = genre.other
    v_low, v_mid, v_high, v_per = genre.vocals

    def part(low: int, mid: int, high: int, per: float, voices: float = 1.0, present: bool = True) -> dict[str, Any]:
        return {
            "songs": 3 if present else 0,
            "notesPerBar": round(per * busy, 2),
            "low": low,
            "median": mid,
            "high": high,
            "voices": voices,
        }

    drums_on = "drums" not in reading.without
    summary: dict[str, Any] = {
        "kind": "spinus.style-summary",
        "version": SUMMARY_VERSION,
        "songs": 0,
        "names": [],
        "tempo": {"median": float(tempo), "min": float(tempo), "max": float(tempo)},
        "beatsPerBar": {"4": 3},
        "keys": keys,
        "modes": modes,
        "chords": chords,
        "drums": {
            "songs": 3 if drums_on else 0,
            "steps": STEPS,
            "lanes": {
                "kick": scaled(genre.kick, busy),
                "snare": scaled(genre.snare, busy),
                "hats": scaled(genre.hats, busy),
            },
            "swing": genre.swing,
        },
        "parts": {
            "bass": part(bass_low, bass_mid, bass_high, bass_per),
            "drums": {"songs": 3 if drums_on else 0, "notesPerBar": round(genre.drum_hits * busy, 1)},
            "other": part(o_low, o_mid, o_high, o_per, o_voices),
            "vocals": part(v_low, v_mid, v_high, v_per, present="lead" not in reading.without),
        },
        "sections": {
            "lengths": [{"bars": b, "count": n} for b, n in genre.lengths.items()],
            "perSong": genre.sections,
            "songBars": round(genre.song_bars * reading.length),
            "forms": [],
        },
        "loudness": {"lufs": genre.lufs},
        "perSong": [],
        "without": list(reading.without),
        "described": {
            "text": text,
            "genre": genre.id,
            "genreName": genre.name,
            "understood": understood(reading, float(tempo), key),
            "sounds": dict(reading.sounds),
            "readBy": read_by,
        },
    }
    summary["plain"] = plain(summary, genre)
    return summary


def scaled(values: list[float], busy: float) -> list[float]:
    """A lane busier or calmer: the steps it plays always stay, the sometimes ones move."""
    return [round(v if v >= 0.9 else min(0.85, v * busy), 3) for v in values]


def chords_of(genre: Genre, modes: list[str]) -> dict[str, Any]:
    """The genre's loops as the summary's chord lists (what the song writer reads when there are no songs)."""
    common: dict[str, float] = {}
    transitions: dict[str, dict[str, float]] = {}
    progressions = []
    for mode in modes:
        for rank, loop in enumerate(genre.loops.get(mode) or []):
            weight = 1.0 / (rank + 1)
            for chord in loop:
                common[chord] = common.get(chord, 0.0) + weight
            for a, b in zip(loop, loop[1:] + loop[:1], strict=True):
                if a != b:
                    after = transitions.setdefault(a, {})
                    after[b] = after.get(b, 0.0) + weight
            progressions.append({"chords": list(loop), "count": 2})
    total = sum(common.values()) or 1.0
    return {
        "common": [{"roman": c, "share": round(w / total, 3)} for c, w in sorted(common.items(), key=lambda x: -x[1])],
        "transitions": {
            a: {b: round(w / sum(after.values()), 3) for b, w in after.items()} for a, after in transitions.items()
        },
        "progressions": progressions,
        "barsPerChord": genre.bars_per_chord,
    }


def plain(summary: dict[str, Any], genre: Genre) -> list[str]:
    described = summary["described"]
    lines = [described["understood"] + "."]
    lines.append(
        f"As {genre.name} usually is: around {summary['tempo']['median']:g} BPM, chords such as "
        + ", ".join(" - ".join(p["chords"]) for p in summary["chords"]["progressions"][:2])
        + f", sections of {', '.join(str(b) for b in genre.lengths)} bars, about "
        + f"{summary['sections']['songBars']} bars in all."
    )
    if summary["without"]:
        lines.append("Left out: " + ", ".join(summary["without"]) + ".")
    if described["sounds"]:
        lines.append(
            "Sounds to look for in your sound index: "
            + ", ".join(f"{layer}: {word}" for layer, word in described["sounds"].items())
            + "."
        )
    return lines


def describe(text: str) -> dict[str, Any]:
    """The rules' summary of a description."""
    return to_summary(read(text), text)


# ---------------------------------------------------------------------------------------------------
# The AI's reading (optional)

SYSTEM = """You help Spinus, a digital audio workstation, read a user's description of a song they want \
written. Answer only by calling the read_description tool once, with what the description says or clearly \
implies. Choose the genre from the list given (the closest one). Give a tempo only when the description says \
one or the genre and mood make one clear. Spinus writes the notes itself from your reading."""

READ_DESCRIPTION = Tool(
    "read_description",
    "What the description asks for, in Spinus's terms.",
    {
        "type": "object",
        "properties": {
            "genre": {"type": "string", "enum": [g.id for g in GENRES], "description": "The closest genre"},
            "tempo": {"type": "number", "description": "BPM, 40 to 240"},
            "key": {"type": "string", "description": "e.g. 'D minor' or 'F# major'"},
            "moods": {"type": "array", "items": {"type": "string"}, "description": "Mood and pace words"},
            "pace": {"type": "string", "enum": ["slow", "medium", "fast"]},
            "mood": {"type": "string", "enum": ["dark", "neutral", "bright"]},
            "energy": {"type": "string", "enum": ["calm", "medium", "intense"]},
            "length": {"type": "string", "enum": ["short", "medium", "long"]},
            "instruments": {
                "type": "object",
                "description": "Layer (chords, pad, arp, lead, counter, bass) -> an instrument name to look for",
                "additionalProperties": {"type": "string"},
            },
            "without": {"type": "array", "items": {"type": "string", "enum": ["drums", "lead"]}},
        },
        "required": ["genre"],
    },
)
LAYERS = ("chords", "pad", "arp", "lead", "counter", "bass")


def check_reading(answer: Any) -> Reading | None:
    """The AI's answer as a reading, keeping only what is within limits; None when there is no usable genre."""
    if not isinstance(answer, dict) or answer.get("genre") not in BY_ID:
        return None
    reading = Reading(genre=str(answer["genre"]), genre_named=True)
    tempo = answer.get("tempo")
    if isinstance(tempo, int | float) and 40 <= tempo <= 240:
        reading.tempo = float(round(tempo))
    key = answer.get("key")
    if isinstance(key, str):
        found = read(key)
        reading.tonic, reading.scale = found.tonic, found.scale
    reading.pace = {"slow": 0.85, "fast": 1.15}.get(str(answer.get("pace")), 1.0)
    reading.minor_lean = {"dark": 1.0, "bright": -1.0}.get(str(answer.get("mood")), 0.0)
    reading.energy = {"calm": -1.0, "intense": 1.0}.get(str(answer.get("energy")), 0.0)
    reading.length = {"short": 0.7, "long": 1.35}.get(str(answer.get("length")), 1.0)
    moods = answer.get("moods")
    if isinstance(moods, list):
        reading.moods = [str(m)[:24] for m in moods if isinstance(m, str)][:6]
    instruments = answer.get("instruments")
    if isinstance(instruments, dict):
        for layer, word in instruments.items():
            if layer in LAYERS and isinstance(word, str) and 0 < len(word.strip()) <= 24:
                reading.sounds[layer] = word.strip().lower()
                reading.instruments.append(word.strip().lower())
    without = answer.get("without")
    if isinstance(without, list):
        reading.without = [p for p in ("drums", "lead") if p in without]
    if not BY_ID[reading.genre].drums and "drums" not in reading.without:
        reading.without.append("drums")
    return reading


async def read_with_ai(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routing: Routing,
    usage_log: UsageLog,
    text: str,
    say: Say,
) -> dict[str, Any]:
    """The AI's reading as a summary; NoConnection without one. When the answer is not usable, the rules'
    reading with a note saying so."""
    routes = routing.resolve("patterns")
    if not routes:
        raise NoConnection("No AI connection is set up.")

    async def no_text(_: str) -> None:
        pass

    await say({"kind": "status", "text": "The AI is reading the description..."})
    genres = "; ".join(f"{g.id}: {g.name}" for g in GENRES)
    request = Request(
        routes[0].model,
        SYSTEM,
        [Message("user", [Text(f"Genres: {genres}\n\nThe description:\n{text[:2000]}")])],
        [READ_DESCRIPTION],
        1000,
    )
    completion, index = await complete_with_fallback(client, connections, routes, 0, request, no_text, say)
    usage_log.add(routes[index].connection.id, "patterns", completion.usage)
    for use in completion.tool_uses:
        if use.name == "read_description" and (reading := check_reading(use.input)) is not None:
            return to_summary(reading, text, completion.model)
    summary = describe(text)
    summary["described"]["note"] = "The AI's answer was not usable; Spinus's own reading stands."
    return summary
