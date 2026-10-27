"""The Track creator's second way in: a song described in words ("a sunny, upbeat indie pop song at 118 BPM in G
major, with acoustic guitar") read into a style summary that the song writer (spinus_ai.songwriter) and the
sound choice take as they take the summary of reference songs.

Offline by default: a table of about seventy genres and their other names, each with a feel (the usual chord
loops, drum groove, parts, section lengths, loudness and sound palette) and its usual tempo and mode, and the
words of the description read with rules: a genre's names, a tempo ("72 BPM"), a key ("D minor", "F#m"),
mood, pace and energy words (slowburn, brooding, euphoric, laid-back...), the instruments it names (piano,
strings, whistle...: the sounds to look for), what to leave out ("no drums") and how long ("short", "long").
When no genre is named, a neutral style leans on the moods, and the page says the genre was not recognized.
What was understood is said in plain words, so the user can see what Spinus took.

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
STEPS = 16
NEUTRAL = "neutral"


def lane(*steps: int, rest: float = 0.0, also: dict[int, float] | None = None) -> list[float]:
    """A drum lane of 16 steps: the given steps always (0.95), `also` sometimes, the rest at `rest`."""
    values = [rest] * STEPS
    for s in steps:
        values[s] = 0.95
    for s, p in (also or {}).items():
        values[s] = p
    return values


Loops = list[tuple[str, ...]]

# Chord loops (Roman numerals, as spinus_ai.style writes them), likelier first.
POP_MAJ: Loops = [("I", "V", "vi", "IV"), ("vi", "IV", "I", "V"), ("I", "IV", "vi", "V")]
POP_MIN: Loops = [("i", "bVI", "bIII", "bVII"), ("i", "bVII", "bVI", "bVII"), ("i", "iv", "bVI", "bVII")]
DARK_MIN: Loops = [("i", "bVI", "bVII", "i"), ("i", "iv", "bVI", "v"), ("i", "bIII", "bVII", "bVI")]
ROCK_MAJ: Loops = [("I", "bVII", "IV", "I"), ("I", "V", "IV", "IV"), ("I", "IV", "V", "IV")]
ROCK_MIN: Loops = [("i", "bVII", "bVI", "bVII"), ("i", "bIII", "bVII", "iv")]
SOUL_MAJ: Loops = [("Imaj7", "vi7", "ii7", "V7"), ("IVmaj7", "iii7", "ii7", "Imaj7"), ("ii7", "V7", "Imaj7", "IVmaj7")]
SOUL_MIN: Loops = [("i7", "iv7", "bVImaj7", "v7"), ("i7", "bVII", "bVImaj7", "v7")]
JAZZ_MAJ: Loops = [("ii7", "V7", "Imaj7", "vi7"), ("Imaj7", "vi7", "ii7", "V7"), ("iii7", "vi7", "ii7", "V7")]
JAZZ_MIN: Loops = [("i7", "iv7", "v7", "i7"), ("iv7", "bVII7", "bIIImaj7", "bVImaj7")]
FOLK_MAJ: Loops = [("I", "IV", "I", "V"), ("I", "V", "vi", "IV"), ("I", "IV", "V", "I")]
FOLK_MIN: Loops = [("i", "bVII", "bVI", "bVII"), ("i", "iv", "i", "V")]
MODAL_MIN: Loops = [("i", "i", "bVII", "i"), ("i", "bII", "i", "bVII")]
MODAL_MAJ: Loops = [("I", "I", "bVII", "I")]
REGGAE_MAJ: Loops = [("I", "IV", "I", "V"), ("I", "vi", "IV", "V")]
REGGAE_MIN: Loops = [("i", "iv", "i", "bVII"), ("i", "bVII", "i", "bVII")]
LATIN_MIN: Loops = [("i", "bVI", "bVII", "i"), ("i", "iv", "V", "i"), ("i", "bVII", "bVI", "V")]
GAME_MAJ: Loops = [("I", "IV", "V", "I"), ("vi", "IV", "V", "I"), ("I", "vi", "IV", "V")]
FILM_MIN: Loops = [("i", "bVI", "bIII", "bVII"), ("i", "bVI", "iv", "V"), ("i", "bVII", "bVI", "bVII")]
FILM_MAJ: Loops = [("I", "iii", "IV", "I"), ("I", "V", "vi", "IV"), ("IV", "I", "V", "vi")]
FUNK_MAJ: Loops = [("I7", "IV7", "I7", "V7"), ("ii7", "V7", "ii7", "V7")]
FUNK_MIN: Loops = [("i7", "iv7", "i7", "bVII"), ("i7", "i7", "iv7", "bVII")]

# Drum lanes.
FOUR = lane(0, 4, 8, 12)
BACKBEAT = lane(4, 12)
EIGHTHS = lane(0, 2, 4, 6, 8, 10, 12, 14, rest=0.1)
SIXTEENTHS = lane(*range(16))
OFFBEATS = lane(2, 6, 10, 14, rest=0.2)
NONE = lane()


@dataclass
class Feel:
    """How a family of genres usually plays: chords, groove, parts, form, loudness and sound palette."""

    loops: dict[str, Loops]
    bars_per_chord: float
    kick: list[float]
    snare: list[float]
    hats: list[float]
    swing: float
    bass: tuple[int, int, int, float]  # low, middle, high, notes a bar
    other: tuple[int, int, int, float, float]  # low, middle, high, notes a bar, voices
    vocals: tuple[int, int, int, float]
    drum_hits: float
    lengths: dict[int, int]  # section bars -> how often
    sections: float
    song_bars: float
    lufs: float
    palette: str  # in words, for the page
    sounds: dict[str, str] = field(default_factory=dict)  # layer -> a word to look for in a sound's name
    drums: bool = True
    guitar_keys: bool = False  # keys guitars like (E minor, G major...)


FEELS: dict[str, Feel] = {
    "pop": Feel({"major": POP_MAJ, "minor": POP_MIN}, 1.0, lane(0, 8, also={6: 0.3, 10: 0.4}), BACKBEAT, EIGHTHS,
                0.5, (31, 40, 50, 6.0), (52, 62, 74, 6.0, 3.0), (57, 66, 76, 4.0), 13.0, {8: 8, 4: 2, 16: 2}, 8, 104,
                -8.0, "piano, synths and a clear lead"),
    "indiepop": Feel({"major": POP_MAJ + FOLK_MAJ, "minor": POP_MIN}, 1.0, lane(0, 8, also={6: 0.3, 10: 0.4}),
                     lane(4, 12, also={13: 0.2}), EIGHTHS, 0.52, (31, 40, 50, 5.0), (48, 62, 74, 8.0, 3.0),
                     (57, 67, 79, 4.0), 13.0, {8: 8, 4: 2, 16: 2}, 8, 100, -9.0,
                     "guitars, bright keys and a catchy lead", {"chords": "guitar"}, guitar_keys=True),
    "synth": Feel({"major": POP_MAJ, "minor": POP_MIN}, 1.0, lane(0, 8, also={10: 0.3}), BACKBEAT, EIGHTHS, 0.5,
                  (28, 38, 50, 8.0), (52, 62, 76, 6.0, 3.0), (55, 64, 74, 3.0), 13.0, {8: 6, 16: 4, 4: 2}, 8, 112,
                  -9.0, "synths: synth bass, pads and a bright lead", {"chords": "synth", "pad": "pad"}),
    "coldsynth": Feel({"minor": DARK_MIN + POP_MIN, "major": POP_MAJ}, 1.0, lane(0, 8, also={6: 0.3, 10: 0.3}),
                      BACKBEAT, lane(*range(0, 16, 2), rest=0.3), 0.5, (28, 36, 48, 8.0), (52, 64, 76, 4.0, 2.5),
                      (52, 60, 70, 2.5), 14.0, {8: 6, 16: 4}, 8, 112, -9.0,
                      "a cold synth palette: a driving synth bass, cold pads and a thin lead",
                      {"bass": "synth", "pad": "pad", "chords": "synth"}),
    "dream": Feel({"major": SOUL_MAJ + POP_MAJ, "minor": POP_MIN}, 2.0, lane(0, 8, also={10: 0.3}), BACKBEAT,
                  lane(*range(0, 16, 2), rest=0.2), 0.5, (31, 40, 50, 4.0), (52, 64, 79, 6.0, 3.5), (57, 65, 74, 2.5),
                  10.0, {8: 6, 16: 4}, 8, 112, -11.0, "washed-out guitars and big pads", {"pad": "pad"},
                  guitar_keys=True),
    "rock": Feel({"major": ROCK_MAJ, "minor": ROCK_MIN}, 1.0, lane(0, 8, also={6: 0.35, 10: 0.5}), BACKBEAT, EIGHTHS,
                 0.5, (28, 40, 52, 8.0), (48, 60, 72, 8.0, 2.5), (55, 66, 76, 3.5), 14.0, {8: 8, 16: 3}, 8, 112, -8.0,
                 "guitars, bass and drums", {"chords": "guitar"}, guitar_keys=True),
    "punk": Feel({"major": ROCK_MAJ + FOLK_MAJ, "minor": ROCK_MIN}, 1.0, lane(0, 8, also={6: 0.5, 10: 0.4}),
                 BACKBEAT, EIGHTHS, 0.5, (28, 40, 52, 8.0), (48, 60, 72, 16.0, 2.0), (55, 66, 76, 4.0), 16.0,
                 {8: 8, 4: 3}, 8, 80, -7.0, "loud guitars", {"chords": "guitar"}, guitar_keys=True),
    "metal": Feel({"minor": DARK_MIN, "major": ROCK_MAJ}, 1.0, lane(0, 2, 8, 10, rest=0.2, also={4: 0.4, 12: 0.4}),
                  BACKBEAT, EIGHTHS, 0.5, (28, 38, 50, 8.0), (40, 52, 64, 12.0, 2.0), (52, 62, 72, 2.0), 18.0,
                  {8: 6, 16: 4}, 8, 128, -8.0, "distorted guitars", {"chords": "guitar"}, guitar_keys=True),
    "blast": Feel({"minor": [*DARK_MIN, ("i", "bII", "i", "bVII")], "major": ROCK_MAJ}, 2.0, SIXTEENTHS,
                  lane(2, 6, 10, 14), lane(*range(0, 16, 2), rest=0.3), 0.5, (31, 40, 52, 8.0),
                  (52, 64, 79, 16.0, 2.0), (55, 62, 72, 2.0), 30.0, {16: 6, 8: 4}, 7, 160, -9.0,
                  "tremolo guitars and blast beats", {"chords": "guitar"}, guitar_keys=True),
    "dsbm": Feel({"minor": DARK_MIN, "major": POP_MAJ}, 2.0, lane(0, 8, also={10: 0.4}), lane(4, 12),
                 lane(*range(0, 16, 2), rest=0.2), 0.5, (33, 40, 52, 4.0), (52, 64, 76, 16.0, 2.2), (55, 62, 72, 1.5),
                 12.0, {16: 6, 8: 3, 32: 1}, 7, 176, -10.0, "tremolo guitars, a sad piano or pads",
                 {"chords": "guitar", "pad": "pad"}, guitar_keys=True),
    "four": Feel({"minor": POP_MIN + MODAL_MIN, "major": POP_MAJ + SOUL_MAJ}, 1.0, FOUR, BACKBEAT, OFFBEATS, 0.5,
                 (31, 40, 50, 8.0), (55, 64, 76, 4.0, 3.0), (57, 66, 76, 3.0), 14.0, {16: 6, 8: 4, 32: 2}, 8, 160,
                 -8.0, "synths, stabs and a piano", {"chords": "synth"}),
    "disco": Feel({"major": SOUL_MAJ + POP_MAJ, "minor": SOUL_MIN}, 1.0, FOUR, BACKBEAT, lane(2, 6, 10, 14, rest=0.4),
                  0.5, (28, 38, 50, 8.0), (55, 64, 76, 6.0, 3.0), (57, 66, 76, 4.0), 16.0, {8: 6, 16: 4}, 8, 128,
                  -8.0, "strings, a funky bass and electric piano", {"pad": "string", "chords": "piano"}),
    "techno": Feel({"minor": MODAL_MIN, "major": MODAL_MAJ}, 4.0, FOUR, lane(4, 12, rest=0.05), SIXTEENTHS, 0.5,
                   (28, 36, 46, 8.0), (52, 60, 72, 4.0, 1.5), (57, 64, 72, 1.0), 20.0, {16: 6, 32: 4, 8: 2}, 7, 192,
                   -8.0, "dark synths and a rolling bass", {"bass": "synth"}),
    "trance": Feel({"minor": POP_MIN, "major": POP_MAJ}, 2.0, FOUR, BACKBEAT, OFFBEATS, 0.5, (28, 38, 50, 16.0),
                   (55, 67, 79, 8.0, 3.0), (57, 66, 76, 2.0), 16.0, {16: 6, 32: 4, 8: 2}, 8, 224, -7.0,
                   "supersaw chords, a rolling bass and plucks", {"arp": "pluck", "chords": "saw"}),
    "hard": Feel({"minor": POP_MIN, "major": POP_MAJ}, 2.0, FOUR, lane(4, 12, rest=0.0), OFFBEATS, 0.5,
                 (24, 33, 45, 4.0), (55, 67, 79, 8.0, 3.0), (57, 66, 76, 2.0), 14.0, {16: 6, 32: 2}, 8, 192, -6.0,
                 "distorted kicks and big leads", {"lead": "lead"}),
    "breaks": Feel({"minor": [*POP_MIN, ("i", "i", "bVI", "bVI")], "major": POP_MAJ}, 2.0,
                   lane(0, 10, also={7: 0.3}), lane(4, 12, also={15: 0.3}), lane(*range(0, 16, 2), rest=0.35), 0.5,
                   (24, 36, 48, 4.0), (55, 64, 76, 4.0, 3.0), (57, 66, 76, 2.5), 16.0, {16: 6, 32: 3, 8: 2}, 7, 176,
                   -8.0, "a deep bass, pads and chopped breaks", {"pad": "pad"}),
    "garage": Feel({"major": SOUL_MAJ, "minor": SOUL_MIN + POP_MIN}, 1.0, lane(0, also={7: 0.4, 10: 0.6}),
                   BACKBEAT, lane(*range(0, 16, 2), rest=0.3), 0.62, (28, 38, 48, 6.0), (55, 64, 76, 4.0, 3.5),
                   (57, 66, 76, 4.0), 13.0, {16: 6, 8: 4}, 8, 128, -8.0, "organ stabs and a sub bass"),
    "halftime": Feel({"minor": POP_MIN + DARK_MIN, "major": POP_MAJ}, 2.0, lane(0, also={10: 0.4, 3: 0.2}), lane(8),
                     lane(*range(0, 16, 2), rest=0.3), 0.5, (24, 33, 45, 4.0), (55, 64, 76, 4.0, 3.0),
                     (57, 66, 76, 2.0), 12.0, {16: 6, 8: 4}, 7, 128, -7.0, "a heavy bass and wide synths"),
    "trap": Feel({"minor": POP_MIN + DARK_MIN, "major": POP_MAJ}, 2.0, lane(0, also={3: 0.5, 7: 0.4, 10: 0.6}),
                 lane(8), SIXTEENTHS, 0.5, (24, 33, 45, 3.0), (55, 67, 79, 4.0, 1.5), (55, 62, 72, 4.0), 20.0,
                 {8: 6, 16: 4}, 7, 96, -8.0, "an 808 bass, bells and dark keys", {"bass": "808", "arp": "bell"}),
    "boombap": Feel({"minor": SOUL_MIN + DARK_MIN, "major": SOUL_MAJ}, 2.0, lane(0, 10, also={7: 0.5, 3: 0.2}),
                    BACKBEAT, EIGHTHS, 0.6, (28, 38, 48, 3.0), (52, 63, 74, 4.0, 3.0), (55, 62, 70, 6.0), 10.0,
                    {8: 6, 16: 4}, 7, 96, -9.0, "dusty keys, a round bass and a sampled feel", {"chords": "piano"}),
    "lofi": Feel({"minor": SOUL_MIN, "major": SOUL_MAJ}, 1.0, lane(0, 10, also={7: 0.4}), BACKBEAT,
                 lane(*range(0, 16, 2), rest=0.15), 0.62, (31, 40, 50, 3.0), (52, 63, 74, 6.0, 3.5), (60, 67, 76, 2.5),
                 10.0, {8: 8, 4: 2}, 7, 64, -12.0, "a warm electric piano and soft drums", {"chords": "piano"}),
    "vapor": Feel({"major": SOUL_MAJ, "minor": SOUL_MIN}, 2.0, lane(0, 8, also={10: 0.3}), BACKBEAT,
                  lane(*range(0, 16, 2), rest=0.2), 0.55, (31, 40, 50, 4.0), (52, 64, 76, 4.0, 4.0), (57, 66, 76, 2.0),
                  10.0, {8: 6, 16: 4}, 7, 96, -11.0, "hazy electric piano, saxophone and pads",
                  {"chords": "piano", "pad": "pad"}),
    "onedrop": Feel({"major": REGGAE_MAJ, "minor": REGGAE_MIN}, 2.0, lane(8), lane(8),
                    lane(*range(0, 16, 2), rest=0.1), 0.58, (31, 40, 50, 5.0), (55, 64, 74, 8.0, 3.0),
                    (57, 66, 76, 3.0), 9.0, {8: 6, 16: 4}, 8, 112, -10.0, "an organ skank, a deep bass and a guitar",
                    {"chords": "organ"}),
    "ska": Feel({"major": REGGAE_MAJ + ROCK_MAJ, "minor": REGGAE_MIN}, 1.0, lane(0, 8), BACKBEAT, EIGHTHS, 0.5,
                (31, 40, 50, 8.0), (55, 64, 74, 8.0, 3.0), (57, 66, 76, 4.0), 14.0, {8: 8, 16: 2}, 8, 96, -8.0,
                "offbeat guitar, horns and a walking bass", {"counter": "brass"}),
    "funk": Feel({"major": FUNK_MAJ, "minor": FUNK_MIN}, 2.0, lane(0, 10, also={3: 0.5, 6: 0.4}),
                 lane(4, 12, also={7: 0.3, 9: 0.3}), SIXTEENTHS, 0.55, (28, 40, 52, 10.0), (55, 64, 74, 8.0, 3.0),
                 (57, 66, 76, 4.0), 18.0, {8: 6, 16: 4}, 8, 112, -9.0, "a slap bass, clavinet and horns",
                 {"counter": "brass", "chords": "clav"}),
    "soul": Feel({"major": SOUL_MAJ, "minor": SOUL_MIN}, 1.0, lane(0, 8, also={10: 0.4}), BACKBEAT, EIGHTHS, 0.58,
                 (31, 40, 50, 5.0), (52, 62, 72, 5.0, 3.5), (57, 66, 77, 4.0), 11.0, {8: 8, 16: 2}, 8, 96, -10.0,
                 "electric piano, organ and a warm bass", {"chords": "piano"}),
    "jazz": Feel({"major": JAZZ_MAJ, "minor": JAZZ_MIN}, 1.0, lane(also={0: 0.3, 8: 0.2}),
                 lane(also={6: 0.2, 14: 0.2}, rest=0.05), lane(0, 4, 8, 12, also={6: 0.6, 14: 0.6}), 0.66,
                 (31, 40, 52, 4.0), (52, 62, 72, 3.0, 3.5), (60, 68, 79, 3.0), 8.0, {8: 8}, 8, 96, -14.0,
                 "piano, upright bass and brushes", {"chords": "piano"}),
    "bossa": Feel({"major": JAZZ_MAJ, "minor": JAZZ_MIN}, 2.0, lane(0, 6, 8, 14), lane(0, 3, 6, 10, 12, rest=0.0),
                  EIGHTHS, 0.5, (31, 40, 50, 3.0), (52, 62, 72, 6.0, 3.5), (57, 65, 74, 3.0), 10.0, {8: 8}, 8, 96,
                  -14.0, "nylon guitar and soft percussion", {"chords": "guitar"}),
    "latin": Feel({"minor": LATIN_MIN, "major": FOLK_MAJ}, 1.0, lane(0, 8, also={6: 0.4}), lane(3, 6, 10, 12),
                  EIGHTHS, 0.5, (31, 40, 50, 6.0), (52, 64, 76, 8.0, 3.0), (57, 66, 76, 4.0), 14.0, {8: 8, 16: 2}, 8,
                  112, -9.0, "piano montunos, brass and percussion", {"chords": "piano", "counter": "brass"}),
    "dembow": Feel({"minor": LATIN_MIN + POP_MIN, "major": POP_MAJ}, 1.0, FOUR, lane(3, 6, 11, 14), EIGHTHS, 0.5,
                   (24, 33, 45, 4.0), (55, 64, 76, 4.0, 3.0), (57, 64, 74, 5.0), 14.0, {8: 8, 16: 2}, 8, 96, -8.0,
                   "a dembow beat, a sub bass and plucks", {"arp": "pluck"}),
    "afro": Feel({"major": POP_MAJ + SOUL_MAJ, "minor": POP_MIN}, 1.0, lane(0, 6, 10), BACKBEAT, SIXTEENTHS, 0.55,
                 (31, 40, 50, 5.0), (55, 64, 76, 6.0, 3.0), (57, 66, 76, 4.0), 18.0, {8: 8, 16: 2}, 8, 112, -9.0,
                 "log drums, plucked guitars and shakers", {"chords": "guitar"}),
    "folk": Feel({"major": FOLK_MAJ, "minor": FOLK_MIN}, 1.0, lane(0, 8, rest=0.0), lane(4, 12, rest=0.0),
                 lane(*range(0, 16, 2), rest=0.0), 0.5, (31, 40, 50, 2.0), (48, 60, 72, 8.0, 3.0), (55, 64, 74, 3.0),
                 8.0, {8: 8, 16: 2}, 8, 96, -12.0, "acoustic guitar, a little piano and fiddle",
                 {"chords": "guitar"}, guitar_keys=True),
    "film": Feel({"minor": FILM_MIN, "major": FILM_MAJ}, 2.0, lane(also={0: 0.5}), lane(also={8: 0.3}), NONE, 0.5,
                 (28, 36, 48, 1.5), (48, 60, 79, 4.0, 4.0), (60, 67, 79, 2.0), 4.0, {8: 4, 16: 4}, 6, 96, -14.0,
                 "strings, brass, choir and timpani", {"pad": "string", "counter": "brass", "chords": "string"}),
    "chip": Feel({"major": GAME_MAJ, "minor": POP_MIN}, 1.0, lane(0, 8, also={10: 0.4}), BACKBEAT, EIGHTHS, 0.5,
                 (36, 45, 57, 8.0), (60, 67, 79, 8.0, 1.0), (67, 74, 84, 5.0), 12.0, {8: 8, 4: 2}, 8, 96, -9.0,
                 "square-wave chip sounds", {"chords": "square", "lead": "square", "bass": "square"}),
    "ambient": Feel({"minor": [*POP_MIN, ("i", "iv", "i", "bVI")], "major": [("I", "IV", "vi", "V"), *SOUL_MAJ]},
                    4.0, NONE, NONE, NONE, 0.5, (28, 38, 48, 1.0), (52, 64, 79, 2.0, 4.0), (60, 69, 79, 1.0), 1.0,
                    {16: 6, 32: 2}, 5, 128, -16.0, "long pads, bells and drones", {"pad": "pad", "arp": "bell"},
                    drums=False),
}  # fmt: skip


@dataclass
class Genre:
    """A genre: its name, the words it is known by, its feel and its usual tempo and mode."""

    id: str
    name: str  # as the user reads it ("depressive black metal")
    words: tuple[str, ...]  # the names it is known by in a description
    feel: str
    tempo: float
    tempo_range: tuple[float, float]
    minor: float  # how often minor (the rest major)

    @property
    def style(self) -> Feel:
        return FEELS[self.feel]

    @property
    def drums(self) -> bool:
        return self.style.drums


G = Genre
GENRES: tuple[Genre, ...] = (
    # Pop and its neighbours
    G("pop", "pop", ("pop", "dance pop", "dance-pop", "mainstream"), "pop", 116, (96, 128), 0.4),
    G("indiepop", "indie pop", ("indie pop", "indie-pop", "indie", "twee", "bedroom pop", "sunshine pop"), "indiepop",
      118, (100, 132), 0.3),
    G("kpop", "K-pop", ("k-pop", "kpop", "korean pop"), "pop", 120, (100, 140), 0.45),
    G("jpop", "J-pop", ("j-pop", "jpop", "japanese pop", "anime", "anisong"), "pop", 132, (110, 160), 0.4),
    G("citypop", "city pop", ("city pop", "city-pop"), "soul", 108, (95, 120), 0.25),
    G("hyperpop", "hyperpop", ("hyperpop", "hyper pop", "bubblegum bass", "pc music"), "pop", 160, (140, 180), 0.35),
    G("synthpop", "synthpop", ("synthpop", "synth-pop", "synth pop", "electropop", "electro-pop", "80s pop"), "synth",
      118, (100, 132), 0.5),
    G("newwave", "new wave", ("new wave", "new-wave"), "synth", 132, (115, 150), 0.5),
    G("synthwave", "synthwave", ("synthwave", "retrowave", "outrun", "80s synth"), "synth", 108, (84, 120), 0.7),
    G("darksynth", "darksynth", ("darksynth", "dark synth", "dark synthwave"), "coldsynth", 112, (95, 130), 0.95),
    G("darkwave", "darkwave", ("darkwave", "dark wave", "minimal wave", "ebm", "electronic body music"), "coldsynth",
      112, (95, 130), 0.9),
    G("coldwave", "coldwave", ("coldwave", "cold wave"), "coldsynth", 120, (100, 140), 0.9),
    G("postpunk", "post-punk", ("post-punk", "post punk", "postpunk", "gothic rock", "goth rock", "goth"), "coldsynth",
      138, (115, 160), 0.85),
    G("dreampop", "dream pop", ("dream pop", "dream-pop", "dreampop", "ethereal wave"), "dream", 96, (80, 115), 0.5),
    G("shoegaze", "shoegaze", ("shoegaze", "shoegazing", "nu gaze", "blackgaze"), "dream", 112, (90, 130), 0.55),
    G("chillwave", "chillwave", ("chillwave", "glo-fi"), "vapor", 100, (85, 115), 0.4),
    G("vaporwave", "vaporwave", ("vaporwave", "vapor wave", "future funk"), "vapor", 84, (70, 110), 0.35),
    # Rock and metal
    G("rock", "rock", ("rock", "classic rock", "hard rock", "rock and roll", "rock'n'roll", "alternative",
                       "alternative rock", "alt rock", "grunge", "garage rock", "stoner rock", "blues rock"), "rock",
      126, (95, 160), 0.45),
    G("indierock", "indie rock", ("indie rock", "indie-rock", "math rock", "post-rock", "post rock"), "rock", 128,
      (100, 150), 0.45),
    G("punk", "punk", ("punk", "punk rock", "hardcore punk", "skate punk"), "punk", 180, (150, 210), 0.35),
    G("poppunk", "pop punk", ("pop punk", "pop-punk", "easycore"), "punk", 172, (150, 200), 0.25),
    G("emo", "emo", ("emo", "midwest emo", "screamo"), "punk", 150, (120, 180), 0.4),
    G("metal", "metal", ("metal", "heavy metal", "nu metal", "nu-metal", "metalcore", "djent", "groove metal",
                         "sludge"), "metal", 120, (90, 160), 0.9),
    G("thrash", "thrash metal", ("thrash", "thrash metal", "speed metal", "death metal", "deathcore", "grindcore"),
      "metal", 180, (150, 220), 0.95),
    G("doom", "doom metal", ("doom", "doom metal", "funeral doom", "drone metal"), "dsbm", 64, (50, 80), 0.95),
    G("powermetal", "power metal", ("power metal", "symphonic metal"), "metal", 160, (130, 190), 0.6),
    G("blackmetal", "black metal", ("black metal", "blackened", "blast beats", "atmospheric black metal"), "blast", 170,
      (140, 200), 0.95),
    G("dsbm", "depressive black metal", ("depressive suicidal black metal", "depressive black metal", "dsbm"), "dsbm",
      76, (62, 92), 0.95),
    # Dance
    G("edm", "EDM", ("edm", "big room", "electro house", "festival", "dance music", "club"), "four", 128, (124, 132),
      0.6),
    G("house", "house", ("house", "deep house", "tech house", "progressive house", "afro house", "house music"),
      "four", 124, (118, 128), 0.6),
    G("disco", "disco", ("disco", "funky house"), "disco", 118, (110, 125), 0.35),
    G("nudisco", "nu-disco", ("nu-disco", "nu disco", "nudisco", "french house"), "disco", 120, (112, 126), 0.4),
    G("techno", "techno", ("techno", "industrial techno", "minimal techno", "acid techno", "acid"),
      "techno", 132, (125, 145), 0.85),
    G("trance", "trance", ("trance", "uplifting trance", "progressive trance", "psytrance", "psy trance", "goa"),
      "trance", 138, (130, 148), 0.75),
    G("hardstyle", "hardstyle", ("hardstyle", "hard dance", "gabber", "hardcore techno", "rawstyle"), "hard", 150,
      (145, 160), 0.7),
    G("dnb", "drum and bass", ("drum and bass", "drum & bass", "drum n bass", "dnb", "d&b", "liquid", "neurofunk"),
      "breaks", 174, (165, 178), 0.8),
    G("jungle", "jungle", ("jungle", "ragga jungle"), "breaks", 165, (155, 175), 0.8),
    G("breakbeat", "breakbeat", ("breakbeat", "breaks", "big beat", "nu skool breaks"), "breaks", 130,
      (120, 140), 0.65),
    G("garage", "UK garage", ("uk garage", "garage", "2-step", "2 step", "two step", "speed garage"), "garage", 132,
      (128, 138), 0.6),
    G("dubstep", "dubstep", ("dubstep", "brostep", "riddim"), "halftime", 140, (138, 150), 0.85),
    G("futurebass", "future bass", ("future bass", "melodic bass"), "halftime", 150, (130, 160), 0.35),
    # Hip hop and its neighbours
    G("hiphop", "hip hop", ("hip hop", "hip-hop", "hiphop", "rap", "boom bap", "boom-bap", "old school"), "boombap",
      90, (80, 100), 0.7),
    G("trap", "trap", ("trap beat", "trap", "drill", "uk drill", "rage"), "trap", 140, (130, 160), 0.85),
    G("phonk", "phonk", ("phonk", "drift phonk", "memphis"), "trap", 130, (120, 150), 0.95),
    G("lofi", "lo-fi hip hop", ("lo-fi hip hop", "lofi hip hop", "lo-fi", "lofi", "lo fi", "chillhop", "jazzhop"),
      "lofi", 80, (70, 92), 0.45),
    G("triphop", "trip-hop", ("trip-hop", "trip hop", "triphop"), "boombap", 84, (70, 100), 0.85),
    G("downtempo", "downtempo", ("downtempo", "chillout", "chill-out", "lounge", "chill out"), "vapor", 95, (80, 110),
      0.5),
    # Caribbean, Latin and African
    G("reggae", "reggae", ("reggae", "roots reggae", "lovers rock", "dancehall"), "onedrop", 76, (66, 90), 0.4),
    G("dub", "dub", ("dub", "dub reggae", "steppers"), "onedrop", 72, (62, 85), 0.7),
    G("ska", "ska", ("ska", "rocksteady", "two tone"), "ska", 160, (130, 180), 0.35),
    G("reggaeton", "reggaeton", ("reggaeton", "reguetón", "dembow", "latin trap"), "dembow", 94, (88, 100), 0.7),
    G("latin", "latin", ("latin", "salsa", "cumbia", "merengue", "bachata", "mambo", "son cubano"), "latin", 110,
      (90, 180), 0.6),
    G("bossa", "bossa nova", ("bossa nova", "bossa", "samba", "brazilian"), "bossa", 128, (110, 140), 0.45),
    G("afrobeats", "afrobeats", ("afrobeats", "afrobeat", "afro-beat", "afropop", "amapiano", "highlife"), "afro",
      108, (98, 116), 0.4),
    # Soul, funk, jazz
    G("funk", "funk", ("funk", "p-funk", "boogie"), "funk", 108, (95, 120), 0.45),
    G("soul", "soul", ("soul", "motown", "neo soul", "neo-soul", "northern soul"), "soul", 92, (70, 110), 0.4),
    G("rnb", "R&B", ("r&b", "rnb", "r and b", "rhythm and blues", "contemporary r&b", "slow jam"), "soul", 84,
      (65, 100), 0.5),
    G("gospel", "gospel", ("gospel", "spiritual", "church"), "soul", 84, (65, 120), 0.15),
    G("jazz", "jazz", ("jazz", "swing", "bebop", "smooth jazz", "jazz fusion", "big band", "cool jazz"), "jazz", 120,
      (70, 200), 0.4),
    G("blues", "blues", ("blues", "delta blues", "chicago blues"), "soul", 88, (60, 120), 0.4),
    # Acoustic and roots
    G("folk", "folk", ("folk", "indie folk", "singer-songwriter", "singer songwriter", "acoustic", "americana",
                       "celtic"), "folk", 100, (80, 125), 0.35),
    G("country", "country", ("country", "country pop", "western", "honky tonk"), "folk", 112, (90, 130), 0.2),
    G("bluegrass", "bluegrass", ("bluegrass", "hillbilly"), "folk", 140, (120, 170), 0.2),
    # Scores and games
    G("cinematic", "cinematic", ("cinematic", "orchestral", "film score", "movie score", "soundtrack", "trailer",
                                 "score", "symphonic", "classical"), "film", 90, (60, 130), 0.65),
    G("chiptune", "chiptune", ("chiptune", "chip tune", "8-bit", "8bit", "8 bit", "video game", "videogame",
                               "game music", "retro game", "nes", "gameboy"), "chip", 150, (110, 180), 0.35),
    G("ambient", "ambient", ("ambient", "drone", "soundscape", "meditation", "new age", "dark ambient"), "ambient", 70,
      (55, 90), 0.5),
)  # fmt: skip
BY_ID = {g.id: g for g in GENRES}
NEUTRAL_GENRE = Genre(NEUTRAL, "a neutral style", (), "pop", 100, (60, 170), 0.5)

# Mood, pace and energy words: what each moves. pace: the tempo's factor; lean: toward minor (+) or major (-);
# energy: busier (+) or calmer (-). Longest first where one contains another.
MOODS: dict[str, tuple[float, float, float]] = {
    "slow-burning": (0.88, 0.3, -0.5), "slow burning": (0.88, 0.3, -0.5), "slowburn": (0.88, 0.3, -0.5),
    "slow-burn": (0.88, 0.3, -0.5), "slow burn": (0.88, 0.3, -0.5), "slowly": (0.85, 0.0, -0.3),
    "slow": (0.85, 0.0, 0.0), "slower": (0.85, 0.0, 0.0), "sluggish": (0.8, 0.0, -0.3), "crawling": (0.75, 0.3, -0.5),
    "ballad": (0.8, 0.0, -0.5), "fast": (1.15, 0.0, 0.3), "faster": (1.15, 0.0, 0.3), "quick": (1.1, 0.0, 0.3),
    "uptempo": (1.12, 0.0, 0.5), "up-tempo": (1.12, 0.0, 0.5), "rapid": (1.15, 0.0, 0.5),
    "frantic": (1.2, 0.3, 1.0), "driving": (1.05, 0.0, 1.0), "danceable": (1.05, 0.0, 0.5),
    "dark": (1.0, 1.0, 0.0), "sad": (0.95, 1.0, -0.3), "melancholic": (0.95, 1.0, -0.3),
    "melancholy": (0.95, 1.0, -0.3), "brooding": (0.92, 1.0, -0.3), "gloomy": (0.92, 1.0, -0.3),
    "depressing": (0.9, 1.0, -0.3), "grim": (1.0, 1.0, 0.0), "cold": (1.0, 0.6, 0.0), "icy": (1.0, 0.6, 0.0),
    "bleak": (0.95, 1.0, -0.3), "somber": (0.92, 1.0, -0.3), "sombre": (0.92, 1.0, -0.3),
    "haunting": (0.95, 1.0, -0.3), "eerie": (0.95, 1.0, -0.3), "lonely": (0.92, 1.0, -0.5),
    "mournful": (0.88, 1.0, -0.5), "moody": (0.97, 0.7, 0.0), "ominous": (0.95, 1.0, 0.0),
    "desperate": (1.0, 1.0, 0.5), "nostalgic": (0.97, 0.3, -0.3), "bittersweet": (0.97, 0.3, -0.3),
    "mysterious": (0.95, 0.7, -0.3), "tense": (1.0, 0.7, 0.5),
    "happy": (1.03, -1.0, 0.3), "bright": (1.0, -1.0, 0.0), "uplifting": (1.03, -1.0, 0.5),
    "joyful": (1.03, -1.0, 0.5), "cheerful": (1.03, -1.0, 0.3), "sunny": (1.0, -1.0, 0.3),
    "upbeat": (1.06, -1.0, 0.5), "hopeful": (1.0, -0.7, 0.0), "euphoric": (1.04, -1.0, 1.0),
    "warm": (0.98, -0.5, -0.3), "playful": (1.03, -1.0, 0.3), "lighthearted": (1.03, -1.0, 0.0),
    "light-hearted": (1.03, -1.0, 0.0), "fun": (1.03, -1.0, 0.3), "feel-good": (1.03, -1.0, 0.3),
    "summery": (1.0, -1.0, 0.0), "romantic": (0.95, -0.3, -0.3), "triumphant": (1.0, -1.0, 1.0),
    "catchy": (1.0, -0.3, 0.3), "anthemic": (1.0, -0.5, 1.0), "epic": (1.0, 0.3, 1.0),
    "calm": (0.92, 0.0, -1.0), "chill": (0.92, 0.0, -1.0), "chilled": (0.92, 0.0, -1.0),
    "laid-back": (0.9, 0.0, -1.0), "laid back": (0.9, 0.0, -1.0), "laidback": (0.9, 0.0, -1.0),
    "mellow": (0.92, 0.0, -1.0), "relaxed": (0.9, 0.0, -1.0), "relaxing": (0.9, 0.0, -1.0),
    "soft": (0.95, 0.0, -0.7), "gentle": (0.93, 0.0, -0.8), "dreamy": (0.95, 0.0, -0.7),
    "peaceful": (0.9, -0.3, -1.0), "quiet": (0.95, 0.0, -0.8), "hazy": (0.93, 0.0, -0.7), "lazy": (0.88, 0.0, -1.0),
    "smooth": (0.97, 0.0, -0.5), "intimate": (0.93, 0.0, -0.8), "sparse": (0.97, 0.0, -0.8),
    "minimal": (1.0, 0.0, -0.5), "atmospheric": (0.95, 0.3, -0.5),
    "energetic": (1.05, 0.0, 1.0), "aggressive": (1.05, 0.5, 1.0), "intense": (1.03, 0.3, 1.0),
    "heavy": (1.0, 0.5, 1.0), "powerful": (1.0, 0.0, 1.0), "angry": (1.05, 0.7, 1.0), "furious": (1.1, 0.7, 1.0),
    "raw": (1.0, 0.3, 0.5), "punchy": (1.0, 0.0, 0.7), "groovy": (1.0, 0.0, 0.3), "funky": (1.0, -0.3, 0.5),
    "busy": (1.0, 0.0, 0.7), "wild": (1.08, 0.0, 1.0), "hypnotic": (1.0, 0.5, 0.0), "bouncy": (1.03, -0.5, 0.5),
}  # fmt: skip
SHORT = ("short", "brief", "quick sketch", "a sketch")
LONG = ("long", "extended", "epic")

# Instruments a description may name: the layer whose sound should be one, and the word to look for in the
# sound's name (a preset called "Grand Piano"). Longer names first.
INSTRUMENTS: tuple[tuple[str, str, str], ...] = (
    ("electric piano", "chords", "piano"), ("rhodes", "chords", "rhodes"), ("wurlitzer", "chords", "wurli"),
    ("piano", "chords", "piano"), ("organ", "chords", "organ"), ("clavinet", "chords", "clav"),
    ("acoustic guitar", "chords", "guitar"), ("electric guitar", "chords", "guitar"), ("guitars", "chords", "guitar"),
    ("guitar", "chords", "guitar"), ("ukulele", "chords", "uke"), ("banjo", "chords", "banjo"),
    ("mandolin", "arp", "mandolin"), ("harp", "arp", "harp"), ("keys", "chords", "keys"),
    ("strings", "pad", "string"), ("violin", "lead", "violin"), ("fiddle", "lead", "fiddle"),
    ("cello", "counter", "cello"), ("choir", "pad", "choir"), ("voices", "pad", "choir"), ("pads", "pad", "pad"),
    ("pad", "pad", "pad"), ("bells", "arp", "bell"), ("bell", "arp", "bell"), ("glockenspiel", "arp", "glock"),
    ("marimba", "arp", "marimba"), ("vibraphone", "arp", "vibra"), ("music box", "arp", "box"),
    ("arpeggios", "arp", "arp"), ("arpeggio", "arp", "arp"), ("plucks", "arp", "pluck"), ("pluck", "arp", "pluck"),
    ("whistled", "lead", "whistle"), ("whistling", "lead", "whistle"), ("whistle", "lead", "whistle"),
    ("flute", "lead", "flute"), ("recorder", "lead", "flute"), ("saxophone", "lead", "sax"), ("sax", "lead", "sax"),
    ("trumpet", "lead", "trumpet"), ("horns", "counter", "brass"), ("brass", "counter", "brass"),
    ("synth lead", "lead", "lead"), ("lead synth", "lead", "lead"), ("supersaw", "chords", "saw"),
    ("808s", "bass", "808"), ("808", "bass", "808"), ("sub bass", "bass", "sub"), ("reese", "bass", "reese"),
    ("slap bass", "bass", "slap"), ("upright bass", "bass", "upright"), ("bass guitar", "bass", "bass"),
    ("synth bass", "bass", "synth"),
)  # fmt: skip
WITHOUT = {"drums": ("no drums", "without drums", "drumless", "no beat", "without a beat", "beatless", "no percussion"),
           "lead": ("no melody", "without melody", "without a melody", "no lead", "instrumental bed")}  # fmt: skip

KEY = re.compile(
    r"\b([a-g])\s?(#|♯|sharp|b(?=\s?(?:maj|min|m\b|major|minor))|♭|flat)?\s?"
    r"(major|minor|maj\b|min\b|m\b)",
    re.IGNORECASE,
)
TEMPO = re.compile(r"\b(\d{2,3}(?:\.\d+)?)\s?(?:bpm|beats per minute|beats a minute)\b", re.IGNORECASE)
TEMPO_BEFORE = re.compile(r"\b(?:tempo|bpm)(?:\s+of|\s*[:=])?\s+(\d{2,3})\b", re.IGNORECASE)

# Keys a song of no named key is written in (tonic, how often): the usual ones, guitars' for guitar genres.
MINOR_KEYS = ((9, 3), (4, 2), (2, 2), (0, 1), (7, 1))
MAJOR_KEYS = ((0, 3), (7, 2), (2, 2), (5, 1), (9, 1))
GUITAR_MINOR = ((4, 3), (9, 2), (2, 2), (11, 1))
GUITAR_MAJOR = ((7, 3), (2, 2), (9, 2), (4, 1))


@dataclass
class Reading:
    """What a description says, in the summary's terms (the rules' reading, or the AI's checked)."""

    genre: str = NEUTRAL
    tempo: float | None = None  # said in BPM
    tonic: int | None = None
    scale: str | None = None  # "minor" or "major", as said
    moods: list[str] = field(default_factory=list)  # the words understood, as written
    pace: float = 1.0  # tempo factor
    minor_lean: float = 0.0  # -1 bright .. +1 dark
    energy: float = 0.0  # -1 calm .. +1 busy
    length: float = 1.0  # song length factor
    sounds: dict[str, str] = field(default_factory=dict)  # layer -> a word to look for in a sound's name
    instruments: list[str] = field(default_factory=list)  # as written
    without: list[str] = field(default_factory=list)  # parts to leave out

    @property
    def genre_named(self) -> bool:
        return self.genre in BY_ID


def genre_of(reading: Reading) -> Genre:
    return BY_ID.get(reading.genre, NEUTRAL_GENRE)


def found(text: str, word: str) -> re.Match[str] | None:
    return re.search(rf"(?<![a-z0-9]){re.escape(word)}(?![a-z0-9])", text)


def words_in(text: str, words: tuple[str, ...]) -> list[str]:
    return [w for w in words if found(text, w)]


def read(text: str) -> Reading:
    """The rules' reading of a description."""
    lower = " ".join(text.lower().replace("\u2013", "-").split())
    reading = Reading()

    # The genre: the longest name found (the earliest on a tie), so "indie pop" wins over "pop" and "indie".
    best: tuple[int, int, Genre, str] | None = None
    for g in GENRES:
        for w in g.words:
            if (m := found(lower, w)) and (
                best is None or len(w) > best[0] or (len(w) == best[0] and m.start() < best[1])
            ):
                best = (len(w), m.start(), g, w)
    named = ""
    if best is not None:
        reading.genre, named = best[2].id, best[3]

    tempo = TEMPO.search(lower) or TEMPO_BEFORE.search(lower)
    if tempo and 40 <= float(tempo.group(1)) <= 240:
        reading.tempo = float(tempo.group(1))

    for match in KEY.finditer(text):
        letter, accidental, scale = match.group(1).upper(), (match.group(2) or "").lower(), match.group(3).lower()
        tonic = NAMES.index(letter)
        if accidental in ("#", "♯", "sharp"):
            tonic = (tonic + 1) % 12
        elif accidental in ("b", "♭", "flat"):
            tonic = (tonic - 1) % 12
        reading.tonic = tonic
        reading.scale = "major" if scale in ("major", "maj") else "minor"
        break

    # Moods: each word once, a longer one taking its words ("slow burn" is not also "slow"); the genre's own
    # name's words ("depressive" black metal) are not moods.
    taken: list[tuple[int, int]] = []
    hits = []
    for word in sorted(MOODS, key=len, reverse=True):
        for m in re.finditer(rf"(?<![a-z0-9]){re.escape(word)}(?![a-z0-9])", lower):
            if any(a < m.end() and m.start() < b for a, b in taken) or word in named.split():
                continue
            if named and best is not None and best[1] <= m.start() < best[1] + len(named):
                continue
            taken.append((m.start(), m.end()))
            hits.append((m.start(), word))
            break
    pace, lean, energy = 1.0, 0.0, 0.0
    for _, word in sorted(hits):
        p, mood_lean, e = MOODS[word]
        pace *= p
        lean += mood_lean
        energy += e
        reading.moods.append(word)
    reading.pace = max(0.7, min(1.3, pace))
    reading.minor_lean = max(-1.0, min(1.0, lean))
    reading.energy = max(-1.0, min(1.0, energy))
    reading.length = 0.7 if words_in(lower, SHORT) else 1.35 if words_in(lower, LONG) else 1.0

    used: set[str] = set()
    for word, layer, look_for in INSTRUMENTS:
        if found(lower, word) and layer not in reading.sounds and not any(word in u for u in used):
            reading.sounds[layer] = look_for
            reading.instruments.append(word)
            used.add(word)
    for part, phrases in WITHOUT.items():
        if words_in(lower, phrases):
            reading.without.append(part)
    if not genre_of(reading).drums and "drums" not in reading.without:
        reading.without.append("drums")
    return reading


def tempo_of(reading: Reading) -> float:
    """The tempo said, or the genre's moved by the pace (gently when a genre was named: its own tempo counts
    more than a mood word)."""
    if reading.tempo:
        return reading.tempo
    genre = genre_of(reading)
    low, high = genre.tempo_range
    pace = 1.0 + (reading.pace - 1.0) * (0.6 if reading.genre_named else 1.0)
    return float(round(min(max(genre.tempo * pace, low * 0.9), high * 1.1)))


def modes_of(reading: Reading) -> dict[str, float]:
    if reading.scale is not None:
        return {reading.scale: 1.0}
    minor = min(1.0, max(0.0, genre_of(reading).minor + 0.4 * reading.minor_lean))
    return {m: round(v, 3) for m, v in (("minor", minor), ("major", 1.0 - minor)) if v > 0}


def understood(reading: Reading, tempo: float, modes: dict[str, float]) -> str:
    """'Understood: 72 BPM, D minor, slow, dark; genre: depressive black metal'."""
    said = [f"{tempo:g} BPM" if reading.tempo else f"about {tempo:g} BPM"]
    if reading.tonic is not None and reading.scale is not None:
        said.append(f"{NAMES[reading.tonic]} {reading.scale}")
    else:
        mode = max(modes, key=lambda m: modes[m])
        said.append(f"mostly {mode}" if modes[mode] < 0.85 else mode)
    said += reading.moods
    genre = genre_of(reading)
    text = "Understood: " + ", ".join(said) + "; genre: "
    text += genre.name if reading.genre_named else "not recognized (using a neutral style)"
    if reading.instruments:
        text += "; sounds: " + ", ".join(reading.instruments)
    if reading.without:
        text += "; without " + " and ".join("a lead" if p == "lead" else p for p in reading.without)
    return text


def to_summary(reading: Reading, text: str, read_by: str = "") -> dict[str, Any]:
    """A style summary (as spinus_ai.style.summarize makes one) from a reading: the genre's numbers, moved by what
    the description says."""
    genre = genre_of(reading)
    feel = genre.style
    tempo = tempo_of(reading)
    modes = modes_of(reading)
    if reading.tonic is not None and reading.scale is not None:
        keys = [{"name": f"{NAMES[reading.tonic]} {reading.scale}", "count": 3}]
    else:
        keys = []
        for mode in modes:
            usual = (
                (GUITAR_MINOR if mode == "minor" else GUITAR_MAJOR)
                if feel.guitar_keys
                else (MINOR_KEYS if mode == "minor" else MAJOR_KEYS)
            )
            keys += [{"name": f"{NAMES[t]} {mode}", "count": n} for t, n in usual]

    busy = 1.0 + 0.3 * reading.energy
    bass_low, bass_mid, bass_high, bass_per = feel.bass
    o_low, o_mid, o_high, o_per, o_voices = feel.other
    v_low, v_mid, v_high, v_per = feel.vocals

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
    sounds = {**feel.sounds, **reading.sounds}  # what the description names first, then the genre's palette
    summary: dict[str, Any] = {
        "kind": "spinus.style-summary",
        "version": SUMMARY_VERSION,
        "songs": 0,
        "names": [],
        "tempo": {"median": tempo, "min": tempo, "max": tempo},
        "beatsPerBar": {"4": 3},
        "keys": keys,
        "modes": modes,
        "chords": chords_of(feel, list(modes)),
        "drums": {
            "songs": 3 if drums_on else 0,
            "steps": STEPS,
            "lanes": {
                "kick": scaled(feel.kick, busy),
                "snare": scaled(feel.snare, busy),
                "hats": scaled(feel.hats, busy),
            },
            "swing": feel.swing,
        },
        "parts": {
            "bass": part(bass_low, bass_mid, bass_high, bass_per),
            "drums": {"songs": 3 if drums_on else 0, "notesPerBar": round(feel.drum_hits * busy, 1)},
            "other": part(o_low, o_mid, o_high, o_per, o_voices),
            "vocals": part(v_low, v_mid, v_high, v_per, present="lead" not in reading.without),
        },
        "sections": {
            "lengths": [{"bars": b, "count": n} for b, n in feel.lengths.items()],
            "perSong": feel.sections,
            "songBars": round(feel.song_bars * reading.length),
            "forms": [],
        },
        "loudness": {"lufs": feel.lufs},
        "perSong": [],
        "without": list(reading.without),
        "described": {
            "text": text,
            "genre": genre.id,
            "genreName": genre.name if reading.genre_named else "",
            "understood": understood(reading, tempo, modes),
            "sounds": sounds,
            "named": dict(reading.sounds),
            "palette": feel.palette,
            "readBy": read_by,
        },
    }
    summary["plain"] = plain(summary, genre, reading)
    return summary


def scaled(values: list[float], busy: float) -> list[float]:
    """A lane busier or calmer: the steps it plays always stay, the sometimes ones move."""
    return [round(v if v >= 0.9 else min(0.85, v * busy), 3) for v in values]


def chords_of(feel: Feel, modes: list[str]) -> dict[str, Any]:
    """The loops as the summary's chord lists (what the song writer reads when there are no songs)."""
    common: dict[str, float] = {}
    transitions: dict[str, dict[str, float]] = {}
    progressions = []
    for mode in modes:
        for rank, loop in enumerate(
            feel.loops.get(mode) or feel.loops.get("minor" if mode == "major" else "major") or []
        ):
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
        "barsPerChord": feel.bars_per_chord,
    }


def plain(summary: dict[str, Any], genre: Genre, reading: Reading) -> list[str]:
    described = summary["described"]
    as_usual = f"As {genre.name} usually is" if reading.genre_named else "A neutral style, leaning on the moods"
    lines = [described["understood"] + "."]
    lines.append(
        f"{as_usual}: around {summary['tempo']['median']:g} BPM, chords such as "
        + ", ".join(" - ".join(p["chords"]) for p in summary["chords"]["progressions"][:2])
        + f", sections of {', '.join(str(b) for b in genre.style.lengths)} bars, about "
        + f"{summary['sections']['songBars']} bars in all; sounds: {described['palette']}."
    )
    if summary["without"]:
        lines.append("Left out: " + ", ".join(summary["without"]) + ".")
    if described["sounds"]:
        lines.append(
            "Sounds to look for in your sound index (for the parts you don't choose yourself): "
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
    reading = Reading(genre=str(answer["genre"]))
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
    if not genre_of(reading).drums and "drums" not in reading.without:
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
