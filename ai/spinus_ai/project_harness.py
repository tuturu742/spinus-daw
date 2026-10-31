"""The project harness: the music harness's tools working on the open project (DESIGN.md §12.11).

The Track creator's harness (harness.py) edits a song plan of its own; this one reads the project Spinus sends
(the assistant's context: project.json, the devices that can be added, each device's parameters with the values
they show) and turns every edit into Spinus commands. A working copy of the project follows the edits, so a later
tool sees an earlier one's channels, notes and clips; what an edit makes before Spinus has made it is named by a
reference to its step ("$3.channel"), which Spinus fills in when it performs the steps.

Two callers: the assistant collects the steps into one proposal the user applies (AGENTS rule 3); the live
interface for outside agents performs each tool call's steps at once, as one undo step (AGENTS rule 3's
exception). Nothing here touches Spinus itself."""

from __future__ import annotations

import copy
import json
import math
import re
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any, cast

from . import keynames
from .gateway import Tool

TICKS_PER_BEAT = 960
TICKS_PER_STEP = 240
MOST_ISSUES = 24
SCALES = {"major": (0, 2, 4, 5, 7, 9, 11), "minor": (0, 2, 3, 5, 7, 8, 10)}
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
FLATS = {"Db": "C#", "Eb": "D#", "Gb": "F#", "Ab": "G#", "Bb": "A#"}

# What a part plays, from words in its name or its instrument (generic words, no makers).
ROLE_WORDS: list[tuple[str, tuple[str, ...]]] = [
    ("drums", ("drum", "kick", "snare", "hat", "clap", "perc", "cymbal", "crash", "tom", "beat", "kit", "shaker")),
    ("bass", ("bass", "sub", "808")),
    ("guitar", ("guitar", "gtr")),
    ("arp", ("arp",)),
    ("texture", ("texture", "rain", "crackle", "vinyl", "ambience", "field")),
    ("pad", ("pad", "string", "choir", "atmos", "drone", "texture")),
    ("keys", ("key", "piano", "organ", "rhodes", "chord", "stab", "pluck", "bell")),
    ("lead", ("lead", "melody", "hook", "vocal", "vox", "solo", "flute", "brass")),
    ("fx", ("fx", "riser", "impact", "sweep", "noise", "downlifter", "swell", "boom")),
]  # fmt: skip
SUSTAINED = ("pad", "keys", "guitar", "lead", "bass", "arp")

# The Drum Machine's pads (every factory kit uses the Spinus Synth Kit's layout) and General MIDI drums, by name.
PADS = {
    "kick": 60, "snare": 61, "clap": 62, "closed hat": 63, "hat": 63, "hihat": 63, "hi-hat": 63, "open hat": 64,
    "low tom": 65, "tom": 66, "mid tom": 66, "high tom": 67, "rim": 68, "cowbell": 69, "crash": 70, "shaker": 71,
    "kick 2": 72, "snare 2": 73, "ride": 74, "perc": 75,
}  # fmt: skip
GM = {
    "kick": 36, "snare": 38, "clap": 39, "closed hat": 42, "hat": 42, "hihat": 42, "hi-hat": 42, "open hat": 46,
    "low tom": 45, "tom": 47, "mid tom": 47, "high tom": 50, "rim": 37, "cowbell": 56, "crash": 49, "shaker": 70,
    "kick 2": 35, "snare 2": 40, "ride": 51, "perc": 60,
}  # fmt: skip

# "Make it pluckier": which settings move, by words in their names, and how (a factor on the value as the device
# shows it, or a value to set). Generic for any instrument whose settings are named this way.
SHAPES: dict[str, list[tuple[tuple[str, ...], str, float]]] = {
    "pluckier": [(("decay",), "scale", 0.45), (("sustain",), "scale", 0.25), (("attack",), "scale", 0.2),
                 (("release",), "scale", 0.6)],
    "punchier": [(("attack",), "scale", 0.2), (("decay",), "scale", 0.7)],
    "softer": [(("attack",), "add_ms", 25.0), (("cutoff", "freq"), "scale", 0.8)],
    "brighter": [(("cutoff", "freq", "bright", "tone"), "scale", 1.6)],
    "darker": [(("cutoff", "freq", "bright", "tone"), "scale", 0.6)],
    "warmer": [(("cutoff", "freq", "tone"), "scale", 0.8), (("reso", "q"), "scale", 0.7)],
    "longer": [(("release",), "scale", 2.0), (("decay",), "scale", 1.6)],
    "shorter": [(("release",), "scale", 0.5), (("decay",), "scale", 0.6)],
    "wider": [(("width", "spread", "stereo"), "scale", 1.5)],
    "narrower": [(("width", "spread", "stereo"), "scale", 0.6)],
}  # fmt: skip

NUMBER = re.compile(r"^\s*([+-]?(?:\d+(?:\.\d*)?|\.\d+))\s*([a-zA-Z%:]*\d*)\s*$")


class Mistake(ValueError):
    """A tool was called wrongly; the text says how to call it right."""


def scale_of(key: dict[str, Any]) -> set[int]:
    """The key's pitch classes (major or minor; another scale counts as major)."""
    return {(int(key["tonic"]) + i) % 12 for i in SCALES.get(str(key.get("scale")), SCALES["major"])}


def note_name(pitch: int) -> str:
    """FL's naming: MIDI 60 is C5."""
    return f"{NOTE_NAMES[pitch % 12]}{pitch // 12}"


def pitch_of(word: str) -> int | None:
    """A MIDI number from "60", "C5", "D#4", "Eb3" (FL's octaves: C5 = 60)."""
    if re.fullmatch(r"\d{1,3}", word):
        value = int(word)
        return value if 0 <= value <= 127 else None
    m = re.fullmatch(r"([A-Ga-g])([#b]?)(-?\d)", word)
    if not m:
        return None
    name = m.group(1).upper() + m.group(2)
    name = FLATS.get(name, name)
    if name not in NOTE_NAMES:  # Cb, Fb, E#, B#
        base = NOTE_NAMES.index(m.group(1).upper())
        offset = 1 if m.group(2) == "#" else -1
        value = (base + offset) % 12 + 12 * int(m.group(3))
    else:
        value = NOTE_NAMES.index(name) + 12 * int(m.group(3))
    return value if 0 <= value <= 127 else None


def db_to_volume(db: float) -> float:
    """A volume automation value (0..1) for a level in dB (Spinus's fader law: dB = 20 ln(v) + 6)."""
    return max(0.0, min(1.0, math.exp((db - 6.0) / 20.0)))


def pan_value(text: str) -> float:
    """ "30% L", "20% R", "C" as 0..1 (0.5 is the centre)."""
    t = text.strip().upper()
    if t in ("C", "CENTER", "CENTRE", "0", "0%"):
        return 0.5
    m = re.fullmatch(r"(\d+(?:\.\d+)?)\s*%?\s*([LR])", t)
    if not m:
        raise Mistake(f"read pan as '30% L', '20% R' or 'C', not '{text}'")
    amount = min(100.0, float(m.group(1))) / 100.0
    return 0.5 - amount / 2 if m.group(2) == "L" else 0.5 + amount / 2


@dataclass
class Placed:
    """A note as it sounds in the song: song ticks, from which clip and pattern."""

    start: int
    length: int
    pitch: int
    velocity: float
    clip: Any
    pattern: Any


def schema(properties: dict[str, Any], required: list[str]) -> dict[str, Any]:
    return {"type": "object", "properties": properties, "required": required}


PART = {"type": "string", "description": "The part (channel) by its name, as song_overview lists it"}
BARS = {"type": "string", "description": "Bars, from 1: '9' or '9-16', or a section's name ('Chorus', 'Verse 2')"}
NOTES = {
    "type": "string",
    "description": "'step pitch length [velocity]; ...' with steps of a sixteenth from the first bar's start "
    "(16 a bar in 4/4), pitch as a MIDI number or a name (C5 = 60), length in steps, velocity 1-127 "
    "(100 if left out). Drums: the drum's name for pitch (kick, snare, clap, hat, open hat, low/mid/high tom, rim, "
    "crash, ride, shaker, perc).",
}
SETTINGS = {
    "type": "object",
    "description": 'Settings by name, with the value as the device shows it: {"Cutoff": "1.2 kHz", '
    '"Decay": "180 ms", "Mix": "30%"}',
}

TOOLS = [
    Tool(
        "song_overview",
        "The open project: tempo, key, sections (from the Playlist's markers, with their bars), the parts (channels: "
        "role, sound, insert, where they play), the mixer (effects, sends, sidechains). Call it first.",
        schema({}, []),
    ),
    Tool(
        "read_part",
        "A part's notes over some bars, in the notation write_part takes (steps from the first bar's start).",
        schema({"part": PART, "bars": BARS}, ["part", "bars"]),
    ),
    Tool(
        "sound_settings",
        "A part's instrument (or one of its insert's effects) and its settings with the values they show now.",
        schema(
            {"part": PART, "effect": {"type": "string", "description": "An effect on the part's insert, by name"}},
            ["part"],
        ),
    ),
    Tool(
        "write_part",
        "Write a part's notes over some bars (replacing what it played there unless replace is false). "
        "loop_bars repeats the notes every that many bars over the range.",
        schema(
            {
                "part": PART,
                "bars": BARS,
                "notes": NOTES,
                "loop_bars": {"type": "integer"},
                "replace": {"type": "boolean"},
            },
            ["part", "bars", "notes"],
        ),
    ),
    Tool(
        "motif",
        "Remember a riff or phrase (notation as write_part's, bars long) to place again later with place_motif.",
        schema({"name": {"type": "string"}, "notes": NOTES, "bars": {"type": "integer"}}, ["name", "notes", "bars"]),
    ),
    Tool(
        "place_motif",
        "Play a motif in a part from a bar, repeated, moved up or down (semitones) and varied: 'rhythm' (pushed "
        "off the beat), 'invert', 'octave-up', 'octave-down', 'displace' (later by a step).",
        schema(
            {
                "part": PART,
                "motif": {"type": "string"},
                "at_bar": {"type": "integer"},
                "times": {"type": "integer"},
                "transpose": {"type": "integer"},
                "vary": {"type": "string", "enum": ["", "rhythm", "invert", "octave-up", "octave-down", "displace"]},
            },
            ["part", "motif", "at_bar"],
        ),
    ),
    Tool(
        "add_part",
        "Add a part: a channel with an instrument found by words (e.g. '3x Osc', 'Drum Machine', 'Sampler', or an "
        "installed plugin's name), an optional preset, its family (Drums, Bass, Synths, Leads, Guitars, Keys, "
        "Orchestra, FX) and colour.",
        schema(
            {
                "name": {"type": "string"},
                "instrument": {"type": "string"},
                "preset": {"type": "string"},
                "family": {"type": "string"},
                "colour": {"type": "string", "description": "#rrggbb"},
            },
            ["name", "instrument"],
        ),
    ),
    Tool(
        "insert_bars",
        "Make room for a new section: everything from after_bar on moves later by `bars` bars (clips, automation, "
        "markers), and a marker names the new section. copy_from fills it with a section's (or bars') clips.",
        schema(
            {
                "after_bar": {"type": "integer"},
                "bars": {"type": "integer"},
                "name": {"type": "string"},
                "copy_from": {"type": "string", "description": "A section's name or bars to copy into it"},
            },
            ["after_bar", "bars", "name"],
        ),
    ),
    Tool(
        "copy_bars",
        "Copy what plays in some bars (all parts, or only `parts`) to start at another bar.",
        schema(
            {"bars": BARS, "to_bar": {"type": "integer"}, "parts": {"type": "array", "items": {"type": "string"}}},
            ["bars", "to_bar"],
        ),
    ),
    Tool(
        "automate",
        "Automate over some bars: a part's 'volume' or 'pan', the master's 'volume', or one of a part's settings "
        "(its instrument's, or an effect's as 'Effect: Setting'). points: [{bar, value}] with bar from 1 "
        "(fractions allowed) and the value as shown: '-12 dB', '30% L', '1.2 kHz'.",
        schema(
            {
                "part": {"type": "string", "description": "A part's name, or 'master'"},
                "what": {"type": "string"},
                "points": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "properties": {"bar": {"type": "number"}, "value": {"type": "string"}},
                        "required": ["bar", "value"],
                    },
                },
            },
            ["part", "what", "points"],
        ),
    ),
    Tool(
        "add_effect",
        "Add an effect (found by words: 'EQ', 'Compressor', 'Limiter', 'Filter', 'Ducker', a reverb or delay "
        "you have) to a part's insert, a bus, or 'master', with settings.",
        schema({"to": {"type": "string"}, "effect": {"type": "string"}, "settings": SETTINGS}, ["to", "effect"]),
    ),
    Tool(
        "send",
        "Send a part to a bus by name ('Reverb', 'Delay', or any insert); a missing Reverb or Delay bus is made "
        "with an effect of that kind you have.",
        schema(
            {"part": PART, "to": {"type": "string"}, "level": {"type": "string", "description": "'-12 dB'"}},
            ["part", "to", "level"],
        ),
    ),
    Tool(
        "duck",
        "Sidechain ducking: the part dips when another part (usually the kick) plays.",
        schema(
            {"part": PART, "source": PART, "amount": {"type": "string", "description": "'50%'"}},
            ["part", "source"],
        ),
    ),
    Tool(
        "bus",
        "Route parts' inserts into a new bus (e.g. 'Drums' for all drum parts).",
        schema(
            {"name": {"type": "string"}, "parts": {"type": "array", "items": {"type": "string"}}}, ["name", "parts"]
        ),
    ),
    Tool(
        "set_sound",
        "Change a part's sound: another instrument found by words (as add_part; its notes, mix and look stay, "
        "automation of the old instrument's settings goes), a preset of its instrument and/or settings by name, as "
        "the device shows them.",
        schema(
            {"part": PART, "instrument": {"type": "string"}, "preset": {"type": "string"}, "settings": SETTINGS},
            ["part"],
        ),
    ),
    Tool(
        "shape_sound",
        "Change a part's sound in words: pluckier, punchier, softer, brighter, darker, warmer, longer, shorter, "
        "wider, narrower. Moves the matching settings of its instrument; says what changed.",
        schema(
            {
                "part": PART,
                "words": {"type": "array", "items": {"type": "string", "enum": sorted(SHAPES)}},
                "amount": {"type": "string", "enum": ["a little", "clearly", "a lot"]},
            },
            ["part", "words"],
        ),
    ),
    Tool(
        "check",
        "Spinus's checks of the song (as it is with your edits so far): notes outside the key, parts in one "
        "register masking each other, too much in the low end, empty or unused parts, sections off 4-bar phrases, "
        "the master's limiter, and loudness per section when the song was rendered. Fix what matters.",
        schema({"bars": BARS}, []),
    ),
]
TOOL_NAMES = [t.name for t in TOOLS]


class ProjectHarness:
    """The tools on a working copy of the open project; `steps` are the Spinus commands its edits made."""

    def __init__(self, context: dict[str, Any]) -> None:
        self.project: dict[str, Any] = copy.deepcopy(context.get("project") or {})
        self.devices: list[dict[str, Any]] = [d for d in context.get("devices") or [] if isinstance(d, dict)]
        self.device_names: dict[str, str] = {str(k): str(v) for k, v in (context.get("deviceNames") or {}).items()}
        # Device ID -> [{id, name, text}] with the value each shows now (Spinus sends them).
        self.parameters: dict[str, list[dict[str, Any]]] = {
            str(k): [p for p in v if isinstance(p, dict)]
            for k, v in (context.get("deviceParameters") or {}).items()
            if isinstance(v, list)
        }
        self.measured: dict[str, Any] = dict(context.get("measured") or {})
        self.steps: list[dict[str, Any]] = []
        self.changes: list[str] = []  # in words, one per edit
        self.motifs: dict[str, dict[str, Any]] = {}
        self.project.setdefault("channels", [])
        self.project.setdefault("patterns", [])
        self.project.setdefault("playlist", {}).setdefault("clips", [])
        self.project["playlist"].setdefault("lanes", [])
        self.project["playlist"].setdefault("markers", [])
        self.project.setdefault("mixer", {}).setdefault("inserts", [])

    # ------------------------------------------------------------------------------------------------
    # The song's shape

    @property
    def settings(self) -> dict[str, Any]:
        return dict(self.project.get("settings") or {})

    @property
    def bar_ticks(self) -> int:
        signature = self.settings.get("timeSignature") or {}
        num, den = int(signature.get("numerator", 4) or 4), int(signature.get("denominator", 4) or 4)
        return max(1, num * 4 * TICKS_PER_BEAT // max(1, den))

    @property
    def steps_per_bar(self) -> int:
        return max(1, self.bar_ticks // TICKS_PER_STEP)

    def clips(self) -> list[dict[str, Any]]:
        return list(self.project["playlist"]["clips"])

    def song_end(self) -> int:
        end = max((int(c.get("start", 0)) + int(c.get("length", 0)) for c in self.clips()), default=0)
        bars = max(1, math.ceil(end / self.bar_ticks))
        return bars * self.bar_ticks

    def sections(self) -> list[dict[str, Any]]:
        """The sections between the Playlist's markers: name, first tick, end tick, bars (1-based)."""
        markers = sorted(
            (m for m in self.project["playlist"]["markers"] if isinstance(m, dict)), key=lambda m: int(m.get("at", 0))
        )
        end = self.song_end()
        if not markers or int(markers[0].get("at", 0)) > 0:
            markers = [{"at": 0, "name": "Start" if markers else "Song"}, *markers]
        out = []
        for i, m in enumerate(markers):
            start = int(m.get("at", 0))
            stop = int(markers[i + 1].get("at", 0)) if i + 1 < len(markers) else max(end, start + self.bar_ticks)
            if stop <= start:
                continue
            out.append(
                {
                    "name": str(m.get("name") or f"Section {i + 1}"),
                    "start": start,
                    "end": stop,
                    "bars": f"{start // self.bar_ticks + 1}-{math.ceil(stop / self.bar_ticks)}",
                }
            )
        return out

    def bar_range(self, text: Any) -> tuple[int, int]:
        """Song ticks [start, end) for '9', '9-16' or a section's name."""
        said = str(text or "").strip()
        m = re.fullmatch(r"(\d+)\s*(?:-\s*(\d+))?", said)
        if m:
            first, last = int(m.group(1)), int(m.group(2) or m.group(1))
            if first < 1 or last < first or last > 2000:
                raise Mistake(f"bars '{said}' is not a range like '9' or '9-16'")
            return (first - 1) * self.bar_ticks, last * self.bar_ticks
        wanted = said.lower()
        for s in self.sections():
            if s["name"].lower() == wanted:
                return int(s["start"]), int(s["end"])
        names = ", ".join(s["name"] for s in self.sections())
        raise Mistake(f"'{said}' is neither bars ('9-16') nor a section ({names})")

    def bar_of(self, tick: int) -> str:
        return f"bar {tick // self.bar_ticks + 1} step {(tick % self.bar_ticks) // TICKS_PER_STEP}"

    # ------------------------------------------------------------------------------------------------
    # Parts, devices, inserts

    def channels(self) -> list[dict[str, Any]]:
        return list(self.project["channels"])

    def part(self, said: Any) -> dict[str, Any]:
        text = str(said or "").strip()
        for c in self.channels():
            if str(c.get("name", "")).lower() == text.lower() or str(c.get("id")) == text:
                return c
        for c in self.channels():
            if text.lower() and text.lower() in str(c.get("name", "")).lower():
                return c
        raise Mistake(f"there is no part '{text}' (parts: {', '.join(str(c.get('name')) for c in self.channels())})")

    def device_name(self, device: dict[str, Any] | None) -> str:
        if not device:
            return "none"
        name = self.device_names.get(str(device.get("id")))
        if name:
            return name
        if device.get("kind") == "plugin":
            return str((device.get("plugin") or {}).get("name", "plugin"))
        found = next(
            (
                d.get("name")
                for d in self.devices
                if (d.get("device") or {}).get("stockType") == device.get("stockType") and device.get("stockType")
            ),
            None,
        )
        return str(found or device.get("stockType") or "device")

    def role(self, channel: dict[str, Any]) -> str:
        instrument = channel.get("instrument") or {}
        if instrument.get("stockType") in ("spinus.drum-machine", "spinus.beat-slicer"):
            return "drums"
        words = f"{channel.get('name', '')} {channel.get('group', '')} {self.device_name(instrument)}".lower()
        for role, keys in ROLE_WORDS:
            if any(k in words for k in keys):
                return role
        return "audio" if channel.get("audioTrack") else "synth"

    def inserts(self) -> list[dict[str, Any]]:
        mixer = self.project["mixer"]
        return ([mixer["master"]] if "master" in mixer else []) + list(mixer["inserts"])

    def insert(self, iid: Any) -> dict[str, Any] | None:
        return next((i for i in self.inserts() if i.get("id") == iid), None)

    def insert_named(self, said: str) -> dict[str, Any] | None:
        if said.strip().lower() == "master":
            master = self.project["mixer"].get("master")
            return cast(dict[str, Any], master) if isinstance(master, dict) else None
        return next((i for i in self.inserts() if str(i.get("name", "")).lower() == said.strip().lower()), None)

    def find_device(self, words: str, kind: str) -> dict[str, Any]:
        """The device object for words in a device's name ('3x Osc', 'Limiter', a plugin's name)."""
        wanted = [w for w in re.split(r"\s+", words.lower().strip()) if w]
        if not wanted:
            raise Mistake(f"which {kind}?")
        matches = []
        for d in self.devices:
            if kind in ("instrument", "effect") and d.get("type") != kind:
                continue
            text = f"{d.get('name', '')} {d.get('maker', '')} {(d.get('device') or {}).get('stockType', '')}".lower()
            if all(w in text for w in wanted):
                matches.append(d)
        usable = [d for d in matches if d.get("available", True)]
        if not usable:
            if matches:
                raise Mistake(f"{matches[0].get('name')} is not downloaded here (Options > Get free plugins)")
            names = ", ".join(str(d.get("name")) for d in self.devices if d.get("type") == kind)[:600]
            raise Mistake(f"no {kind} matches '{words}' (some there are: {names})")
        usable.sort(key=lambda d: (str(d.get("name", "")).lower() != words.lower().strip(), len(str(d.get("name")))))
        return copy.deepcopy(usable[0].get("device") or {})

    # ------------------------------------------------------------------------------------------------
    # Steps

    def step(self, command: str, parameters: dict[str, Any], change: str = "") -> int:
        """Adds a command; returns its number (for "$<n>.<field>" references to what it makes)."""
        self.steps.append({"command": command, "parameters": parameters})
        if change:
            self.changes.append(change)
        return len(self.steps)

    def take_proposal(self, summary: str) -> dict[str, Any] | None:
        """The steps so far as one proposal ({summary, steps}), and a fresh start; None when nothing changed."""
        if not self.steps:
            return None
        proposal = {"summary": summary.strip() or "; ".join(self.changes)[:300], "steps": self.steps}
        self.steps, self.changes = [], []
        return proposal

    # ------------------------------------------------------------------------------------------------
    # Notes

    def placed(self, channel_id: Any, start: int = 0, end: int | None = None) -> list[Placed]:
        """A channel's notes as they play in the song (pattern clips; patterns don't loop) in [start, end)."""
        stop = end if end is not None else 1 << 40
        patterns = {p.get("id"): p for p in self.project["patterns"]}
        out: list[Placed] = []
        for clip in self.clips():
            if clip.get("type") != "pattern" or clip.get("muted"):
                continue
            pattern = patterns.get(clip.get("pattern"))
            if pattern is None:
                continue
            c_start, c_len, offset = int(clip.get("start", 0)), int(clip.get("length", 0)), int(clip.get("offset", 0))
            if c_start >= stop or c_start + c_len <= start:
                continue
            for entry in pattern.get("channels", []):
                if entry.get("channel") != channel_id:
                    continue
                for n in entry.get("notes", []):
                    at = int(n.get("start", 0))
                    if not offset <= at < offset + c_len:
                        continue
                    song = c_start + at - offset
                    if start <= song < stop:
                        out.append(
                            Placed(
                                song,
                                int(n.get("length", TICKS_PER_STEP)),
                                int(n.get("pitch", 60)),
                                float(n.get("velocity", 0.8)),
                                clip.get("id"),
                                pattern.get("id"),
                            )
                        )
        out.sort(key=lambda p: (p.start, p.pitch))
        return out

    def drum_map(self, channel: dict[str, Any]) -> dict[str, int]:
        return PADS if (channel.get("instrument") or {}).get("stockType") == "spinus.drum-machine" else GM

    def notation(self, channel: dict[str, Any], notes: list[Placed], origin: int) -> str:
        names = (
            {v: k for k, v in reversed(list(self.drum_map(channel).items()))} if self.role(channel) == "drums" else {}
        )
        out = []
        for n in notes:
            step = (n.start - origin) / TICKS_PER_STEP
            pitch = names.get(n.pitch) or str(n.pitch)
            length = max(n.length / TICKS_PER_STEP, 0.25)
            out.append(f"{step:g} {pitch} {length:g} {round(n.velocity * 127)}")
        return "; ".join(out)

    def parse(self, channel: dict[str, Any], text: str, span: int, loop_ticks: int) -> list[dict[str, Any]]:
        """Notes (ticks from the range's start) from the notation, the loop repeated over `span` ticks."""
        drums = self.role(channel) == "drums"
        mapping = self.drum_map(channel)
        loop: list[dict[str, Any]] = []
        bad: list[str] = []
        for raw in re.split(r"[;\n]+", text or ""):
            fields = raw.replace(",", " ").split()
            if not fields:
                continue
            # "open hat" / "low tom": join a two-word drum name.
            if drums and len(fields) >= 4 and f"{fields[1]} {fields[2]}".lower() in mapping:
                fields = [fields[0], f"{fields[1]} {fields[2]}", *fields[3:]]
            if len(fields) not in (3, 4):
                bad.append(raw.strip())
                continue
            try:
                step, length = float(fields[0]), float(fields[2])
                velocity = float(fields[3]) if len(fields) == 4 else 100.0
            except ValueError:
                bad.append(raw.strip())
                continue
            word = fields[1].lower()
            pitch = mapping.get(word) if word in mapping else pitch_of(fields[1])
            if pitch is None or not all(math.isfinite(v) for v in (step, length, velocity)) or step < 0 or length <= 0:
                bad.append(raw.strip())
                continue
            start = round(step * TICKS_PER_STEP)
            if start >= loop_ticks:
                bad.append(raw.strip())
                continue
            loop.append(
                {
                    "start": start,
                    "length": max(30, min(round(length * TICKS_PER_STEP), loop_ticks - start)),
                    "pitch": int(pitch),
                    "velocity": round(max(0.05, min(1.0, velocity / 127.0)), 3),
                }
            )
        if bad:
            raise Mistake(
                f"{len(bad)} notes could not be read, e.g. '{bad[0]}': write 'step pitch length velocity; ...'"
                + (" with the drum's name as pitch" if drums else "")
            )
        out = []
        for repeat in range(max(1, math.ceil(span / max(1, loop_ticks)))):
            for n in loop:
                at = repeat * loop_ticks + n["start"]
                if at < span:
                    out.append({**n, "start": at, "length": min(n["length"], span - at)})
        return out

    def lane_for(self, name: str, start: int, end: int) -> Any:
        """A lane named after the part with nothing in [start, end), else a new one."""
        lanes = self.project["playlist"]["lanes"]
        for wanted in (name, f"{name} 2", f"{name} 3"):
            lane = next((x for x in lanes if str(x.get("name", "")).lower() == wanted.lower()), None)
            if lane is None:
                number = self.step("lane.add", {"name": wanted})
                lane = {"id": f"${number}.lane", "name": wanted}
                lanes.append(lane)
                return lane["id"]
            busy = any(
                c.get("lane") == lane.get("id")
                and int(c.get("start", 0)) < end
                and int(c.get("start", 0)) + int(c.get("length", 0)) > start
                for c in self.clips()
            )
            if not busy:
                return lane.get("id")
        number = self.step("lane.add", {"name": f"{name} 4"})
        lanes.append({"id": f"${number}.lane", "name": f"{name} 4"})
        return f"${number}.lane"

    def clear(self, channel: dict[str, Any], start: int, end: int) -> int:
        """Takes the channel's notes in [start, end) out of the song: from the pattern itself when only that clip
        plays it, else from a copy of it the clip plays from now on. Returns how many notes went."""
        cid = channel.get("id")
        notes = self.placed(cid, start, end)
        clips_using: dict[Any, int] = {}
        for c in self.clips():
            if c.get("type") == "pattern":
                clips_using[c.get("pattern")] = clips_using.get(c.get("pattern"), 0) + 1
        patterns = {p.get("id"): p for p in self.project["patterns"]}
        done = 0
        for clip_id in dict.fromkeys(n.clip for n in notes):
            clip = next(c for c in self.clips() if c.get("id") == clip_id)
            pattern = patterns[clip.get("pattern")]
            c_start, offset = int(clip.get("start", 0)), int(clip.get("offset", 0))
            if clips_using.get(pattern.get("id"), 0) > 1:
                number = self.step("clip.makeUnique", {"clip": clip_id})
                twin = copy.deepcopy(pattern)
                twin["id"] = f"${number}.pattern"
                self.project["patterns"].append(twin)
                clips_using[pattern.get("id")] -= 1
                clip["pattern"] = twin["id"]
                pattern = twin
            entry = next((e for e in pattern.get("channels", []) if e.get("channel") == cid), None)
            if entry is None:
                continue
            kept = []
            for n in entry.get("notes", []):
                song = c_start + int(n.get("start", 0)) - offset
                if start <= song < end:
                    done += 1
                else:
                    kept.append(n)
            entry["notes"] = kept
            self.set_notes(pattern, cid, kept)
        return done

    def set_notes(self, pattern: dict[str, Any], channel_id: Any, notes: list[dict[str, Any]]) -> None:
        """pattern.setNotes, keeping the song's shape: Spinus fits a pattern's length to its notes, and the clips that
        play all of it with it; their lengths are put back."""
        old = int(pattern.get("length") or 0)
        following = [
            c
            for c in self.clips()
            if c.get("type") == "pattern"
            and c.get("pattern") == pattern.get("id")
            and int(c.get("offset", 0)) == 0
            and int(c.get("length", 0)) == old
        ]
        self.step("pattern.setNotes", {"pattern": pattern["id"], "channel": channel_id, "notes": notes})
        if old:
            self.step("pattern.setLength", {"pattern": pattern["id"], "length": old})
            for clip in following:
                self.step("clip.set", {"clip": clip.get("id"), "length": old})

    def put(self, channel: dict[str, Any], start: int, notes: list[dict[str, Any]], span: int, name: str) -> None:
        """A new pattern of these notes (ticks from `start`) for the channel, played from `start` on its lane."""
        number = self.step("pattern.add", {"name": name, "length": span})
        pid = f"${number}.pattern"
        self.step("pattern.setNotes", {"pattern": pid, "channel": channel["id"], "notes": notes})
        self.step("pattern.setLength", {"pattern": pid, "length": span})
        self.project["patterns"].append(
            {"id": pid, "name": name, "length": span, "channels": [{"channel": channel["id"], "notes": notes}]}
        )
        lane = self.lane_for(str(channel.get("name", "Part")), start, start + span)
        clip_number = self.step("clip.addPattern", {"lane": lane, "pattern": pid, "start": start, "length": span})
        self.project["playlist"]["clips"].append(
            {
                "id": f"${clip_number}.clip",
                "lane": lane,
                "start": start,
                "length": span,
                "type": "pattern",
                "pattern": pid,
                "offset": 0,
            }
        )

    # ------------------------------------------------------------------------------------------------
    # The tools

    def call(self, name: str, arguments: dict[str, Any]) -> tuple[str, bool]:
        """The tool's answer, and whether it is an error (a Mistake's text tells the model how to call it)."""
        method: Callable[..., str] | None = getattr(self, f"tool_{name}", None)
        if name not in TOOL_NAMES or method is None:
            return f"Unknown tool {name}.", True
        before = (len(self.steps), copy.deepcopy(self.project))
        try:
            return method(**{k: v for k, v in (arguments or {}).items() if v is not None}), False
        except Mistake as mistake:
            self.steps = self.steps[: before[0]]
            self.project = before[1]
            return f"Not done: {mistake}", True
        except (TypeError, KeyError, ValueError) as error:
            self.steps = self.steps[: before[0]]
            self.project = before[1]
            return f"Bad arguments for {name}: {error}", True

    def tool_song_overview(self) -> str:
        settings = self.settings
        key = settings.get("key") or {}
        key_name = keynames.key_name(int(key["tonic"]), str(key.get("scale", "major"))) if "tonic" in key else "not set"
        parts = []
        for c in self.channels():
            notes = self.placed(c.get("id"))
            bars = sorted({n.start // self.bar_ticks + 1 for n in notes})
            insert = self.insert(c.get("targetInsert"))
            parts.append(
                {
                    "name": c.get("name"),
                    "role": self.role(c),
                    "sound": self.device_name(c.get("instrument")),
                    "insert": insert.get("name") if insert else None,
                    "volumeDb": c.get("volumeDb", 0),
                    "plays": spans(bars) if bars else ("audio clips" if c.get("audioTrack") else "nothing"),
                    "notes": len(notes),
                }
            )
        inserts = []
        for i in self.inserts():
            target = self.insert(i.get("output"))
            inserts.append(
                {
                    "name": i.get("name") or "Insert",
                    "effects": [self.device_name(e.get("device")) for e in i.get("effects", [])],
                    "volumeDb": i.get("volumeDb", 0),
                    "output": target.get("name") if target else ("master" if i.get("output") else None),
                    "sends": [
                        f"{(self.insert(s.get('target')) or {}).get('name', '?')} {s.get('levelDb', 0)} dB"
                        for s in i.get("sends", [])
                    ],
                    "duckedBy": [(self.insert(s) or {}).get("name", "?") for s in i.get("sidechainSources", [])],
                }
            )
        sig = settings.get("timeSignature") or {}
        return json.dumps(
            {
                "tempoBpm": settings.get("tempoBpm"),
                "timeSignature": f"{sig.get('numerator', 4)}/{sig.get('denominator', 4)}",
                "key": key_name,
                "bars": self.song_end() // self.bar_ticks,
                "stepsPerBar": self.steps_per_bar,
                "sections": [{"name": s["name"], "bars": s["bars"]} for s in self.sections()],
                "parts": parts,
                "mixer": inserts,
            }
        )

    def tool_read_part(self, part: str, bars: str) -> str:
        channel = self.part(part)
        start, end = self.bar_range(bars)
        notes = self.placed(channel.get("id"), start, end)
        first, last = start // self.bar_ticks + 1, end // self.bar_ticks
        head = f"{channel.get('name')}, bars {first}-{last} ({self.steps_per_bar} steps a bar)"
        if not notes:
            return f"{head}: no notes."
        return f"{head}: {self.notation(channel, notes, start)}"

    def device_of(self, part: str, effect: str = "") -> dict[str, Any]:
        channel = self.part(part)
        if not effect:
            device = channel.get("instrument")
            if not device:
                raise Mistake(f"{channel.get('name')} has no instrument")
            return dict(device)
        insert = self.insert(channel.get("targetInsert")) or {}
        for slot in insert.get("effects", []):
            device = slot.get("device") or {}
            if effect.lower() in self.device_name(device).lower():
                return dict(device)
        raise Mistake(f"{channel.get('name')}'s insert has no effect '{effect}'")

    def tool_sound_settings(self, part: str, effect: str = "") -> str:
        device = self.device_of(part, effect)
        listed = self.parameters.get(str(device.get("id")), [])
        if not listed:
            stored = device.get("parameters") or {}
            return json.dumps({"device": self.device_name(device), "settings": stored or "not known here"})
        return json.dumps(
            {
                "device": self.device_name(device),
                "settings": {str(p.get("name") or p.get("id")): p.get("text", "") for p in listed[:80]},
            }
        )

    def tool_write_part(self, part: str, bars: str, notes: str, loop_bars: int = 0, replace: bool = True) -> str:
        channel = self.part(part)
        if channel.get("audioTrack"):
            raise Mistake(f"{channel.get('name')} is an audio track: it plays audio clips, not notes")
        start, end = self.bar_range(bars)
        span = end - start
        loop = int(loop_bars) * self.bar_ticks if loop_bars and int(loop_bars) > 0 else span
        parsed = self.parse(channel, notes, span, min(loop, span))
        removed = self.clear(channel, start, end) if replace else 0
        first, last = start // self.bar_ticks + 1, end // self.bar_ticks
        if parsed:
            self.put(channel, start, parsed, span, f"{channel.get('name')} {first}-{last}")
        self.changes.append(f"{channel.get('name')}: {len(parsed)} notes in bars {first}-{last}")
        return (
            f"{channel.get('name')}: {len(parsed)} notes in bars {first}-{last}"
            + (f", {removed} old ones taken out" if removed else "")
            + "."
        )

    def tool_motif(self, name: str, notes: str, bars: int) -> str:
        if not re.fullmatch(r"[A-Za-z0-9 _-]{1,30}", name or ""):
            raise Mistake("a motif's name is a short word")
        if not 1 <= int(bars) <= 16:
            raise Mistake("a motif is 1 to 16 bars long")
        self.motifs[name] = {"notes": notes, "bars": int(bars)}
        return f"Motif '{name}' ({bars} bars) remembered."

    def tool_place_motif(
        self, part: str, motif: str, at_bar: int, times: int = 1, transpose: int = 0, vary: str = ""
    ) -> str:
        source = self.motifs.get(motif)
        if source is None:
            raise Mistake(f"there is no motif '{motif}' (motifs: {', '.join(self.motifs) or 'none yet'})")
        channel = self.part(part)
        bars = int(source["bars"])
        span = bars * self.bar_ticks
        notes = self.parse(channel, str(source["notes"]), span, span)
        drums = self.role(channel) == "drums"
        first_pitch = notes[0]["pitch"] if notes else 60
        for i, n in enumerate(notes):
            if not drums:
                n["pitch"] = int(n["pitch"]) + int(transpose)
                if vary == "invert":
                    n["pitch"] = 2 * (first_pitch + int(transpose)) - n["pitch"]
                elif vary == "octave-up":
                    n["pitch"] += 12
                elif vary == "octave-down":
                    n["pitch"] -= 12
                n["pitch"] = max(0, min(127, n["pitch"]))
            if vary == "rhythm" and i % 2 == 1:
                n["start"] = min(span - TICKS_PER_STEP, n["start"] + TICKS_PER_STEP)
            elif vary == "displace":
                n["start"] = (n["start"] + TICKS_PER_STEP) % span
        notes.sort(key=lambda n: n["start"])
        text = "; ".join(
            f"{n['start'] / TICKS_PER_STEP:g} {n['pitch']} {n['length'] / TICKS_PER_STEP:g} "
            f"{round(n['velocity'] * 127)}"
            for n in notes
        )
        repeats = max(1, min(32, int(times)))
        return self.tool_write_part(
            part, f"{int(at_bar)}-{int(at_bar) + bars * repeats - 1}", text, loop_bars=bars, replace=True
        )

    def tool_add_part(self, name: str, instrument: str, preset: str = "", family: str = "", colour: str = "") -> str:
        if any(str(c.get("name", "")).lower() == name.strip().lower() for c in self.channels()):
            raise Mistake(f"there is already a part named '{name}'")
        device = self.find_device(instrument, "instrument")
        number = self.step("channel.add", {"name": name, "instrument": device}, f"added the part {name}")
        made = {
            "id": f"${number}.channel",
            "name": name,
            "instrument": {**device, "id": f"${number}.device"},
            "targetInsert": f"${number}.insert",
            "volumeDb": 0,
            "group": family,
        }
        self.project["channels"].append(made)
        self.project["mixer"]["inserts"].append(
            {"id": f"${number}.insert", "name": name, "effects": [], "sends": [], "sidechainSources": []}
        )
        if preset:
            self.step("device.loadPreset", {"device": f"${number}.device", "preset": preset})
        look: dict[str, Any] = {}
        if family:
            look["group"] = family.strip()
        if colour:
            if not re.fullmatch(r"#[0-9a-fA-F]{6}", colour):
                raise Mistake("a colour is '#rrggbb'")
            look["colour"] = "#ff" + colour[1:].lower()
        if look:
            self.step("channel.set", {"channel": f"${number}.channel", **look})
        return f"Added {name} ({self.device_name(device)}{', ' + preset if preset else ''})."

    def tool_insert_bars(self, after_bar: int, bars: int, name: str, copy_from: str = "") -> str:
        if not 0 <= int(after_bar) <= self.song_end() // self.bar_ticks:
            raise Mistake(f"after_bar is 0 to {self.song_end() // self.bar_ticks}")
        if not 1 <= int(bars) <= 64:
            raise Mistake("insert 1 to 64 bars")
        at, shift = int(after_bar) * self.bar_ticks, int(bars) * self.bar_ticks
        source = self.bar_range(copy_from) if copy_from else None
        moved = 0
        # Clips that cross the point are split there first; the later part moves with the rest.
        for clip in sorted(self.clips(), key=lambda c: int(c.get("start", 0))):
            c_start, c_len = int(clip.get("start", 0)), int(clip.get("length", 0))
            if c_start < at < c_start + c_len:
                number = self.step("clip.split", {"clip": clip.get("id"), "at": at})
                clip["length"] = at - c_start
                tail = {**clip, "id": f"${number}.clip", "start": at, "length": c_start + c_len - at}
                if clip.get("type") == "pattern":
                    tail["offset"] = int(clip.get("offset", 0)) + at - c_start
                self.project["playlist"]["clips"].append(tail)
        for clip in sorted(self.clips(), key=lambda c: -int(c.get("start", 0))):
            if int(clip.get("start", 0)) >= at:
                clip["start"] = int(clip["start"]) + shift
                self.step("clip.set", {"clip": clip.get("id"), "start": clip["start"]})
                moved += 1
        for marker in self.project["playlist"]["markers"]:
            if int(marker.get("at", 0)) >= at:
                marker["at"] = int(marker["at"]) + shift
                self.step("marker.set", {"marker": marker.get("id"), "at": marker["at"]})
        number = self.step("marker.add", {"at": at, "name": name})
        self.project["playlist"]["markers"].append({"id": f"${number}.marker", "at": at, "name": name})
        self.changes.append(f"{bars} bars of {name} after bar {after_bar}")
        copied = ""
        if source is not None:
            # The source moved too if it lay after the point.
            s_start, s_end = source
            if s_start >= at:
                s_start, s_end = s_start + shift, s_end + shift
            span = min(s_end - s_start, shift)
            copied = " " + self.copy_range(s_start, s_start + span, at, None)
        return f"Made room for {name}: bars {after_bar + 1}-{after_bar + bars}; {moved} clips moved later.{copied}"

    def copy_range(self, start: int, end: int, to: int, parts: list[str] | None) -> str:
        if parts:
            done = []
            for said in parts:
                channel = self.part(said)
                notes = self.placed(channel.get("id"), start, end)
                if notes:
                    text = self.notation(channel, notes, start)
                    self.clear(channel, to, to + end - start)
                    parsed = self.parse(channel, text, end - start, end - start)
                    self.put(channel, to, parsed, end - start, f"{channel.get('name')} copy")
                    done.append(str(channel.get("name")))
            return f"Copied {', '.join(done) or 'nothing'} to bar {to // self.bar_ticks + 1}."
        count = 0
        for clip in list(self.clips()):
            c_start, c_len = int(clip.get("start", 0)), int(clip.get("length", 0))
            if c_start >= end or c_start + c_len <= start:
                continue
            cut_start, cut_end = max(c_start, start), min(c_start + c_len, end)
            new_start = to + cut_start - start
            copy_clip = {k: v for k, v in clip.items() if k != "id"}
            copy_clip["start"], copy_clip["length"] = new_start, cut_end - cut_start
            if clip.get("type") == "pattern":
                copy_clip["offset"] = int(clip.get("offset", 0)) + cut_start - c_start
                number = self.step(
                    "clip.addPattern",
                    {
                        "lane": clip.get("lane"),
                        "pattern": clip.get("pattern"),
                        "start": new_start,
                        "length": cut_end - cut_start,
                        "offset": copy_clip["offset"],
                    },
                )
            else:
                number = self.step("clip.add", {"clip": copy.deepcopy(copy_clip)})
            copy_clip["id"] = f"${number}.clip"
            self.project["playlist"]["clips"].append(copy_clip)
            count += 1
        return f"Copied {count} clips to bar {to // self.bar_ticks + 1}."

    def tool_copy_bars(self, bars: str, to_bar: int, parts: list[str] | None = None) -> str:
        start, end = self.bar_range(bars)
        if int(to_bar) < 1:
            raise Mistake("to_bar is from 1")
        text = self.copy_range(start, end, (int(to_bar) - 1) * self.bar_ticks, parts)
        self.changes.append(text)
        return text

    def target_for(self, part: str, what: str) -> tuple[dict[str, Any], str, Callable[[str], dict[str, Any]]]:
        """An automation target, its lane's name, and how a value's text becomes a point."""
        what_l = what.strip().lower()
        if part.strip().lower() == "master":
            master = self.project["mixer"].get("master") or {"id": 1}
            if what_l != "volume":
                raise Mistake("the master's automation is 'volume'")
            return (
                {"kind": "insertVolume", "insert": master.get("id", 1)},
                "Master volume",
                lambda t: {"value": round(db_to_volume(read_db(t)), 4)},
            )
        channel = self.part(part)
        name = str(channel.get("name"))
        if what_l == "volume":
            return (
                {"kind": "channelVolume", "channel": channel["id"]},
                f"{name} volume",
                lambda t: {"value": round(db_to_volume(read_db(t)), 4)},
            )
        if what_l == "pan":
            return {"kind": "channelPan", "channel": channel["id"]}, f"{name} pan", lambda t: {"value": pan_value(t)}
        effect, _, setting = what.partition(":") if ":" in what else ("", "", what)
        device = self.device_of(part, effect.strip())
        parameter = self.parameter_named(device, setting.strip())
        return (
            {"kind": "deviceParameter", "device": device.get("id"), "parameterId": parameter},
            f"{name} {setting.strip()}",
            lambda t: {"text": t},
        )

    def parameter_named(self, device: dict[str, Any], said: str) -> str:
        listed = self.parameters.get(str(device.get("id")), [])
        wanted = said.lower()
        for p in listed:
            if str(p.get("name", "")).lower() == wanted or str(p.get("id", "")).lower() == wanted:
                return str(p.get("id"))
        for p in listed:
            if wanted and wanted in str(p.get("name", "")).lower():
                return str(p.get("id"))
        stored = device.get("parameters") or {}
        if said in stored:
            return said
        names = ", ".join(str(p.get("name")) for p in listed[:40]) or ", ".join(stored)
        raise Mistake(f"{self.device_name(device)} has no setting '{said}' (some: {names})")

    def tool_automate(self, part: str, what: str, points: list[dict[str, Any]]) -> str:
        if not isinstance(points, list) or len(points) < 2:
            raise Mistake("automation needs at least two points: [{bar, value}, ...]")
        target, lane_name, point_of = self.target_for(part, what)
        bars = sorted(float(p["bar"]) for p in points)
        start = max(0, math.floor(bars[0] - 1)) * self.bar_ticks
        end = max(start + self.bar_ticks, math.ceil(bars[-1] - 1) * self.bar_ticks + 1)
        made = []
        for p in sorted(points, key=lambda q: float(q["bar"])):
            time = round((float(p["bar"]) - 1) * self.bar_ticks) - start
            made.append({"time": max(0, time), **point_of(str(p["value"]))})
        length = max(int(made[-1]["time"]) + 1, end - start)
        self.step(
            "clip.addAutomation",
            {"laneName": lane_name, "start": start, "length": length, "target": target, "points": made},
            f"{lane_name} automated over bars {bars[0]:g}-{bars[-1]:g}",
        )
        return f"Automated {lane_name} from bar {bars[0]:g} to {bars[-1]:g} ({len(made)} points)."

    def where(self, said: str) -> dict[str, Any]:
        """An insert by a part's name, an insert's name or 'master'."""
        found = self.insert_named(said)
        if found is not None:
            return found
        channel = self.part(said)
        insert = self.insert(channel.get("targetInsert"))
        if insert is None:
            raise Mistake(f"{channel.get('name')} has no mixer insert")
        return insert

    def tool_add_effect(self, to: str, effect: str, settings: dict[str, Any] | None = None) -> str:
        insert = self.where(to)
        device = self.find_device(effect, "effect")
        number = self.step(
            "insert.addEffect",
            {"insert": insert.get("id"), "device": device},
            f"{self.device_name(device)} on {insert.get('name') or 'the insert'}",
        )
        insert.setdefault("effects", []).append({"device": {**device, "id": f"${number}.device"}})
        if settings:
            self.step("device.setParameters", {"device": f"${number}.device", "text": text_settings(settings)})
        return f"Added {self.device_name(device)} to {insert.get('name') or 'the insert'}."

    def tool_send(self, part: str, to: str, level: str) -> str:
        insert = self.where(part)
        target = self.insert_named(to)
        if target is None:
            kind = to.strip().lower()
            words = {"reverb": ("reverb", "hall", "room", "plate"), "delay": ("delay", "echo")}.get(kind)
            if words is None:
                raise Mistake(f"there is no bus '{to}' (make one with bus, or send to 'Reverb' or 'Delay')")
            device = None
            for w in words:
                try:
                    device = self.find_device(w, "effect")
                    break
                except Mistake:
                    continue
            if device is None:
                raise Mistake(f"no {kind} effect is installed (get one in Options > Get free plugins)")
            number = self.step("insert.add", {"name": to.strip().title()}, f"a {to.strip().title()} bus")
            target = {"id": f"${number}.insert", "name": to.strip().title(), "effects": [], "sends": []}
            self.project["mixer"]["inserts"].append(target)
            self.step("insert.addEffect", {"insert": target["id"], "device": device})
        self.step(
            "insert.setSend",
            {"insert": insert.get("id"), "target": target.get("id"), "levelDb": read_db(level)},
            f"{insert.get('name')} sent to {target.get('name')}",
        )
        insert.setdefault("sends", []).append({"target": target.get("id"), "levelDb": read_db(level)})
        return f"{insert.get('name')} goes to {target.get('name')} at {read_db(level):g} dB."

    def tool_duck(self, part: str, source: str, amount: str = "50%") -> str:
        insert, from_insert = self.where(part), self.where(source)
        if insert.get("id") == from_insert.get("id"):
            raise Mistake("a part can't duck under itself")
        has = any((e.get("device") or {}).get("stockType") == "spinus.ducker" for e in insert.get("effects", []))
        if not has:
            ducker = (
                self.find_device("ducker", "effect")
                if self.devices
                else {"kind": "stock", "stockType": "spinus.ducker"}
            )
            number = self.step("insert.addEffect", {"insert": insert.get("id"), "device": ducker})
            self.step("device.setParameters", {"device": f"${number}.device", "text": {"Amount": amount}})
            insert.setdefault("effects", []).append({"device": {**ducker, "id": f"${number}.device"}})
        self.step(
            "insert.setSidechain",
            {"insert": insert.get("id"), "source": from_insert.get("id")},
            f"{insert.get('name')} ducks under {from_insert.get('name')}",
        )
        insert["sidechainSources"] = [from_insert.get("id")]
        return f"{insert.get('name')} now dips when {from_insert.get('name')} plays ({amount})."

    def tool_bus(self, name: str, parts: list[str]) -> str:
        inserts = [self.where(p).get("id") for p in parts]
        if not inserts:
            raise Mistake("a bus needs parts")
        number = self.step("insert.addBus", {"name": name, "inserts": inserts}, f"a {name} bus")
        self.project["mixer"]["inserts"].append({"id": f"${number}.insert", "name": name, "effects": [], "sends": []})
        for i in self.inserts():
            if i.get("id") in inserts:
                i["output"] = f"${number}.insert"
        return f"Made the bus {name} for {len(inserts)} parts."

    def tool_set_sound(
        self, part: str, preset: str = "", settings: dict[str, Any] | None = None, instrument: str = ""
    ) -> str:
        channel = self.part(part)
        if not instrument and not preset and not settings:
            raise Mistake("give an instrument, a preset, settings, or some of them")
        if instrument:
            new = self.find_device(instrument, "instrument")
            number = self.step(
                "channel.replaceInstrument",
                {"channel": channel.get("id"), "instrument": new, "name": channel.get("name")},
                f"{channel.get('name')} now plays {self.device_name(new)}",
            )
            old_id = (channel.get("instrument") or {}).get("id")
            channel["instrument"] = {**new, "id": f"${number}.device"}
            # Automation of the old instrument's settings goes with it (as Spinus's command does).
            self.project["playlist"]["clips"] = [
                c
                for c in self.clips()
                if not (
                    old_id is not None
                    and c.get("type") == "automation"
                    and (c.get("target") or {}).get("kind") == "deviceParameter"
                    and (c.get("target") or {}).get("device") == old_id
                )
            ]
        device = channel.get("instrument") or {}
        if not device:
            raise Mistake(f"{channel.get('name')} has no instrument")
        if not preset and not settings:
            return f"{channel.get('name')}: now {self.device_name(device)}"
        if preset:
            self.step("device.loadPreset", {"device": device.get("id"), "preset": preset})
        if settings:
            self.step("device.setParameters", {"device": device.get("id"), "text": text_settings(settings)})
        self.changes.append(f"{channel.get('name')}'s sound changed")
        return f"{channel.get('name')}: " + ", ".join(
            [f"now {self.device_name(device)}"] * bool(instrument)
            + [f"preset {preset}"] * bool(preset)
            + [f"{k} {v}" for k, v in (settings or {}).items()]
        )

    def tool_shape_sound(self, part: str, words: list[str], amount: str = "clearly") -> str:
        channel = self.part(part)
        device = channel.get("instrument") or {}
        listed = self.parameters.get(str(device.get("id")), [])
        if not listed:
            raise Mistake(
                f"Spinus didn't send {self.device_name(device)}'s settings: use set_sound with settings by name"
            )
        strength = {"a little": 0.5, "clearly": 1.0, "a lot": 1.6}.get(amount, 1.0)
        changed: dict[str, str] = {}
        for word in words:
            for keys, how, value in SHAPES.get(word, []):
                for p in listed:
                    pname = str(p.get("name", "")).lower()
                    if not any(k in pname for k in keys) or str(p.get("name")) in changed:
                        continue
                    new = moved_text(str(p.get("text", "")), how, value, strength)
                    if new is not None:
                        changed[str(p.get("name"))] = new
        if not changed:
            names = ", ".join(str(p.get("name")) for p in listed[:30])
            raise Mistake(
                f"none of {self.device_name(device)}'s settings match {', '.join(words)}; set them with set_sound "
                f"(its settings: {names})"
            )
        self.step(
            "device.setParameters",
            {"device": device.get("id"), "text": changed},
            f"{channel.get('name')} {', '.join(words)}",
        )
        return f"{channel.get('name')} ({', '.join(words)}): " + ", ".join(f"{k} -> {v}" for k, v in changed.items())

    def tool_check(self, bars: str = "") -> str:
        lines = self.issues(bars)
        if not lines:
            return "No issues found."
        more = len(lines) - MOST_ISSUES
        return "\n".join(lines[:MOST_ISSUES] + ([f"... and {more} more"] if more > 0 else []))

    # ------------------------------------------------------------------------------------------------
    # The checks

    def issues(self, bars: str = "") -> list[str]:
        start, end = self.bar_range(bars) if bars else (0, self.song_end())
        out: list[str] = []
        key = self.settings.get("key") or {}
        scale = (
            scale_of({"tonic": int(key["tonic"]), "scale": str(key.get("scale", "major"))}) if "tonic" in key else None
        )
        sections = [s for s in self.sections() if s["end"] > start and s["start"] < end] or [
            {"name": "the song", "start": start, "end": end, "bars": ""}
        ]
        unused = [
            str(c.get("name"))
            for c in self.channels()
            if not c.get("audioTrack") and not self.placed(c.get("id")) and self.role(c) != "fx"
        ]
        if unused and not bars:
            out.append(f"parts that never play: {', '.join(unused)} (write them or remove them)")
        for s in sections:
            ranges: dict[str, tuple[int, int, int, float]] = {}  # part -> low, median, high, average length (steps)
            pitched = {
                str(c.get("name")): (c, self.placed(c.get("id"), s["start"], s["end"]))
                for c in self.channels()
                if self.role(c) not in ("drums", "fx", "texture")
            }
            every = [n for _, notes in pitched.values() for n in notes]
            # A section may change key on purpose (a lift): check its notes against the key they fit best when
            # that fits clearly better than the song's.
            here = scale
            if scale is not None and every:
                best_tonic, best_scale, best_out = section_key(every)
                song_out = sum(n.pitch % 12 not in scale for n in every) / len(every)
                if song_out > 0.12 and best_out < song_out / 3:
                    here = scale_of({"tonic": best_tonic, "scale": best_scale})
                    out.append(
                        f"{s['name']}: plays in {keynames.key_name(best_tonic, best_scale)}, not the song's key "
                        "(fine for a key change on purpose)"
                    )
            for name, (_c, notes) in pitched.items():
                if not notes:
                    continue
                if here is not None:
                    outside = [n for n in notes if n.pitch % 12 not in here]
                    if len(outside) > max(2, len(notes) // 8):
                        out.append(
                            f"{name}, {s['name']}: {len(outside)} notes outside the key, e.g. "
                            + ", ".join(f"{note_name(n.pitch)} at {self.bar_of(n.start)}" for n in outside[:3])
                        )
                pitches = sorted(n.pitch for n in notes)
                average = sum(n.length for n in notes) / len(notes) / TICKS_PER_STEP
                ranges[name] = (
                    pitches[len(pitches) // 4],
                    pitches[len(pitches) // 2],
                    pitches[3 * len(pitches) // 4],
                    average,
                )
                per_bar = len(notes) / max(1, (s["end"] - s["start"]) / self.bar_ticks)
                if per_bar > 48:
                    out.append(f"{name}, {s['name']}: {per_bar:.0f} notes a bar (too busy to hear; thin it)")
            bass_like = [n for n, r in ranges.items() if r[1] < 48]
            # A sub under a bass is a layer, not mud; two or more other low parts are.
            muddy = [n for n in bass_like if "sub" not in n.lower()]
            if len(muddy) > 1:
                out.append(f"{s['name']}: {', '.join(muddy)} all play low (muddy low end; keep one there)")
            names = sorted(n for n in ranges if n not in bass_like)
            for i, a in enumerate(names):
                for b in names[i + 1 :]:
                    la, ma, ha, sa = ranges[a]
                    lb, mb, hb, sb = ranges[b]
                    overlap = min(ha, hb) - max(la, lb)
                    narrow = max(1, min(ha - la, hb - lb))
                    # Masking: both hold notes (a beat or longer on average) in the same range.
                    if sa >= 4 and sb >= 4 and (abs(ma - mb) <= 2 or overlap >= 0.7 * narrow):
                        out.append(
                            f"{a} and {b}, {s['name']}: both held in the same register (around {note_name(ma)}), so "
                            "they mask each other: move one an octave, thin one out, or cut one with EQ"
                        )
            bar_count = (s["end"] - s["start"]) // self.bar_ticks
            if s["name"] != "the song" and bar_count % 4 and bar_count > 1:
                out.append(f"{s['name']}: {bar_count} bars, off the 4-bar phrases")
        master = self.project["mixer"].get("master") or {}
        if not bars and not any(
            "limit" in self.device_name(e.get("device")).lower() for e in master.get("effects", [])
        ):
            out.append("master: no limiter (add_effect to master, 'Limiter', with a ceiling of '-1 dB')")
        out += self.measured_issues()
        return out

    def measured_issues(self) -> list[str]:
        """From a render Spinus made (the live interface's render tool): peaks and loudness per section."""
        out = []
        mix = self.measured.get("mix") or {}
        if isinstance(mix.get("truePeakDb"), int | float) and mix["truePeakDb"] > -1.0:
            out.append(f"the mix peaks at {mix['truePeakDb']:.1f} dBTP: lower the limiter's ceiling to -1 dB")
        sections = [s for s in self.measured.get("sections") or [] if isinstance(s.get("lufs"), int | float)]
        if len(sections) >= 3:
            loud = [s["lufs"] for s in sections]
            if max(loud) - min(loud) < 2.0:
                out.append(
                    "every section is about as loud ("
                    + ", ".join(f"{s['name']} {s['lufs']:.1f}" for s in sections)
                    + " LUFS): thin the verses or build the choruses"
                )
        for stem in self.measured.get("silentParts") or []:
            out.append(f"{stem}: has notes but made no sound in the render (its sound may still be loading or muted)")
        return out


def section_key(notes: list[Placed]) -> tuple[int, str, float]:
    """The major or minor key the notes fit best (weighted by length), and the share of notes outside it."""
    best = (0, "major", 1.0)
    for tonic in range(12):
        for scale in ("major", "minor"):
            pcs = scale_of({"tonic": tonic, "scale": scale})
            total = sum(n.length for n in notes) or 1
            outside = sum(n.length for n in notes if n.pitch % 12 not in pcs) / total
            if outside < best[2] - 1e-9:
                best = (tonic, scale, outside)
    return best


def spans(bars: list[int]) -> str:
    """[1, 2, 3, 5] -> '1-3, 5'."""
    out: list[str] = []
    start = prev = bars[0]
    for b in bars[1:]:
        if b == prev + 1:
            prev = b
            continue
        out.append(f"{start}-{prev}" if prev != start else f"{start}")
        start = prev = b
    out.append(f"{start}-{prev}" if prev != start else f"{start}")
    return ", ".join(out)


def read_db(text: Any) -> float:
    m = re.fullmatch(r"\s*([+-]?\d+(?:\.\d+)?)\s*(?:dB)?\s*", str(text), re.IGNORECASE)
    if not m:
        raise Mistake(f"read '{text}' as a level like '-12 dB'")
    return float(m.group(1))


def text_settings(settings: dict[str, Any]) -> dict[str, str]:
    if not isinstance(settings, dict) or not settings:
        raise Mistake('settings are {"Name": "value as shown"}')
    return {str(k): str(v) for k, v in settings.items()}


def moved_text(text: str, how: str, value: float, strength: float) -> str | None:
    """A setting's shown value moved ("180 ms" scaled by 0.45 -> "81 ms"); None when it can't be read."""
    m = NUMBER.match(text.replace("\u2212", "-"))
    if not m:
        return None
    number, unit = float(m.group(1)), m.group(2)
    if how == "scale":
        factor = value**strength
        if unit.lower() in ("db",):
            return None
        new = number * factor if number else (5.0 if factor > 1 and unit.lower() in ("ms",) else number)
    elif how == "add_ms":
        if unit.lower() not in ("ms", "s"):
            return None
        new = number + (value * strength / (1000.0 if unit.lower() == "s" else 1.0))
    else:
        return None
    if unit == "%":
        new = max(0.0, min(100.0, new))
    rounded = round(new, 2) if abs(new) < 10 else round(new, 1) if abs(new) < 100 else round(new)
    return f"{rounded:g} {unit}".strip()
