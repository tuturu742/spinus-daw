"""The music harness: the Track creator's AI writes a song the way a coding agent writes code, with tools.

A song is a working copy kept here (Workspace): its form (sections, bars, chords, energy, tempo, key), every part's
notes per section, the sound picked for each part and named motifs. The model never sends the whole song: it reads
and edits the working copy through small tools, checks it like code ("tests and linter": short issue lists with
locations, as a compiler gives them), fixes what the checks find with patches of a few bars, and calls finish.
Re-using a motif or copying a section costs a line, not the notes again, so a song costs fewer tokens than writing
it out whole (spinus_ai.compose, the one-shot writer it is compared with: `python -m spinus_ai.harness eval`).

Tools (TOOLS; each answers in a few lines of text):

- read: brief (the style and the parts), form, read (a part's notes in a section, or some of its bars), list_sounds;
- write: set_form, set_chords, write (a part's notes in a section, a loop repeated), edit (only some bars), motif
  (a named phrase) and place (a motif into a part at a bar, transposed, repeated, fitted to the chords), copy
  (a section's notes to another), pick_sound, rules_write (Spinus's offline writer fills a part or a section);
- check: check (notes outside the key or rubbing on the chords, parts in the same register, notes a bar against the
  references', the drums' groove against theirs, repetition, empty parts, section lengths), measure (how loud each
  part played with its sound when Spinus last played it, and a balance from the notes' velocities);
- finish: the song is checked a last time and goes to the Track creator's page, where the user hears it and
  Applies it (one undo step; the Replace question as for every written song).

The loop is the assistant's (spinus_ai.assistant.tool_loop). On Anthropic the request's start (tools, system prompt,
brief) is cached between turns, and once a request's input passes CLEAR_AFTER tokens, the provider clears the oldest
tool calls and results on its side (context editing, CONTEXT_EDITS: the working copy here holds what they did, and
read shows it again). The history is never trimmed here: a model that reasons signs its reasoning against the
conversation as sent, and the provider refuses (or drops) that reasoning once earlier turns were edited; its own
clearing does not count as an edit. There is no token cap (an answer is never cut short on purpose): the cost is set
by the reasoning level and the rounds; the tokens used are shown live and per round after. "Stop now" keeps the best
checked version so far; a model that never calls finish is stopped after MOST_TURNS with the same.

The same tools are served outside Spinus for development and evaluation (`python -m spinus_ai.harness mcp`, an MCP
server over stdio), so a coding agent can compose through the identical harness from a saved brief.
"""

from __future__ import annotations

import argparse
import asyncio
import dataclasses
import json
import math
import re
import sys
import tempfile
from collections import Counter
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import httpx

from . import compose
from . import songwriter as sw
from .assistant import NoConnection, Say, answered_by, tool_loop
from .connections import ConnectionStore
from .gateway import Completion, Message, Request, Text, Tool, ToolUse
from .routing import Route, Routing
from .usage import UsageLog

MOST_TURNS = 150  # generous: a model that never calls finish is stopped here, with the best checked version
MOST_LINES = 24  # a tool's answer is cut to this many issue lines ("and N more")
# Anthropic's context editing: once a request's input passes CLEAR_AFTER tokens, the provider clears the oldest tool
# calls and their results (all but the last KEEP_CALLS), at least CLEAR_AT_LEAST tokens at a time so the cache is
# rewritten seldom (each clearing starts it again). The calls' arguments go too (clear_tool_inputs): the notes a write
# sent are in the working copy, and read shows them again. finish is kept.
CLEAR_AFTER = 30000
KEEP_CALLS = 8
CLEAR_AT_LEAST = 8000


def context_edits(clear_after: int) -> list[dict[str, Any]]:
    if clear_after <= 0:
        return []
    return [
        {
            "type": "clear_tool_uses_20250919",
            "trigger": {"type": "input_tokens", "value": clear_after},
            "keep": {"type": "tool_uses", "value": KEEP_CALLS},
            "clear_at_least": {"type": "input_tokens", "value": min(CLEAR_AT_LEAST, clear_after // 2)},
            "clear_tool_inputs": True,
            "exclude_tools": ["finish"],
        }
    ]


SYSTEM = """You are a songwriter and arranger composing inside Spinus, a digital audio workstation, through a \
music harness: tools that read and edit a working copy of the song, check it, and finish it. Work like a good \
programmer: look at the brief and form, write each part, run check, fix what it reports with small edits, and \
call finish when the song is good. Write an original song in the style of the brief; never copy a reference song.

Write idiomatically for each instrument: a bass that locks with the kick, chords voiced for the instrument \
(guitar strums and power chords, piano voicings, held pads), a singable lead with phrases and rests, drums with a \
groove, fills at section ends and a crash where a section starts. Thin verses, full choruses; parts come and go.

Notation: a note is "step pitch length velocity"; step is the sixteenth from the start of the section (or of the \
loop, motif or bar range you write), pitch a MIDI note (60 is middle C), length in sixteenths, velocity 1 to 127; \
notes are separated by semicolons. Drum parts write the drum's name (kick, snare, hat, openHat, crash, tomLow, \
tomMid, tomHigh) instead of the pitch. Chords are Roman numerals (i, iv, V7, bVI, Imaj7, Vsus4, vii°...).

Save tokens: write a loop once with loop_bars instead of every bar; reuse phrases with motif and place; copy a \
section that repeats; fix a few bars with edit, not the whole part; let rules_write fill a part you don't need to \
shape by hand. Riser and impact parts are placed by Spinus. Keep tool calls small; don't explain between them."""


def schema(properties: dict[str, Any], required: list[str]) -> dict[str, Any]:
    return {"type": "object", "properties": properties, "required": required, "additionalProperties": False}


PART = {"type": "string", "description": "The part's id, as the brief lists it"}
SECTION = {"type": "string", "description": "The section's id, as form lists it"}
NOTES = {"type": "string", "description": "'step pitch length velocity; ...' (drums: the drum's name for pitch)"}
BARS = {"type": "string", "description": "Bars of the section, from 1: '3' or '5-8'"}
SECTION_ITEM = schema(
    {
        "id": {"type": "string"},
        "kind": {"type": "string", "enum": list(sw.KINDS)},
        "bars": {"type": "integer"},
        "chords": {"type": "array", "items": {"type": "string"}},
        "barsPerChord": {"type": "integer", "enum": [1, 2]},
        "energy": {"type": "number"},
    },
    ["id", "kind", "bars", "chords"],
)

TOOLS = [
    Tool("brief", "The style, the key and tempo, and the parts with their roles, ranges and sounds.", schema({}, [])),
    Tool("form", "The sections: kind, bars, chords, energy; the order; which parts have notes where.", schema({}, [])),
    Tool(
        "read",
        "A part's notes in a section (or some of its bars), in the notation.",
        schema({"part": PART, "section": SECTION, "bars": BARS}, ["part", "section"]),
    ),
    Tool(
        "list_sounds",
        "The sounds a part may use, from the user's sound index (optionally those whose name has `query`).",
        schema({"part": PART, "query": {"type": "string"}}, ["part"]),
    ),
    Tool(
        "set_form",
        "Set the sections and their order (and the tempo and key). Notes of sections that go are dropped; a "
        "section that gets shorter keeps its first bars.",
        schema(
            {
                "sections": {"type": "array", "items": SECTION_ITEM},
                "order": {"type": "array", "items": {"type": "string"}},
                "tempo": {"type": "number"},
                "key": {"type": "string", "description": "'A minor', 'F# major'"},
            },
            ["sections", "order"],
        ),
    ),
    Tool(
        "set_chords",
        "Set one section's chord loop.",
        schema(
            {
                "section": SECTION,
                "chords": {"type": "array", "items": {"type": "string"}},
                "barsPerChord": {"type": "integer", "enum": [1, 2]},
            },
            ["section", "chords"],
        ),
    ),
    Tool(
        "write",
        "Write a part's notes for a whole section (replacing what it had): a loop of loop_bars bars repeated over "
        "the section (default: the section's length, no repeat). An empty string: the part rests there.",
        schema(
            {"part": PART, "section": SECTION, "notes": NOTES, "loop_bars": {"type": "integer"}},
            ["part", "section", "notes"],
        ),
    ),
    Tool(
        "edit",
        "Replace a part's notes in some bars of a section only (steps count from the first of those bars).",
        schema({"part": PART, "section": SECTION, "bars": BARS, "notes": NOTES}, ["part", "section", "bars", "notes"]),
    ),
    Tool(
        "motif",
        "Name a phrase (a riff, a hook, a fill) to place in parts later; steps from its start.",
        schema({"name": {"type": "string"}, "notes": NOTES, "bars": {"type": "integer"}}, ["name", "notes", "bars"]),
    ),
    Tool(
        "place",
        "Place a motif into a part at a bar of a section (replacing the notes in the bars it covers): transposed "
        "by semitones, repeated `times`, and with fit_chords each note moved to the nearest note that fits the "
        "chord under it.",
        schema(
            {
                "part": PART,
                "section": SECTION,
                "motif": {"type": "string"},
                "bar": {"type": "integer", "description": "From 1"},
                "transpose": {"type": "integer"},
                "times": {"type": "integer"},
                "fit_chords": {"type": "boolean"},
            },
            ["part", "section", "motif", "bar"],
        ),
    ),
    Tool(
        "copy",
        "Copy the notes of a section to another (repeated or cut to its length), for all parts or those named.",
        schema(
            {"from_section": SECTION, "to_section": SECTION, "parts": {"type": "array", "items": PART}},
            ["from_section", "to_section"],
        ),
    ),
    Tool(
        "pick_sound",
        "Choose a part's sound from list_sounds by its id.",
        schema({"part": PART, "sound": {"type": "string"}}, ["part", "sound"]),
    ),
    Tool(
        "rules_write",
        "Let Spinus's own rules write a part (in a section, or in all) from the form's chords.",
        schema({"part": PART, "section": SECTION}, ["part"]),
    ),
    Tool(
        "check",
        "Check the song (or one part, or one section): an issue list with locations, or OK.",
        schema({"part": PART, "section": SECTION}, []),
    ),
    Tool(
        "measure",
        "How loud each part played with its sound when Spinus last played them, and the balance its notes give.",
        schema({"section": SECTION}, []),
    ),
    Tool(
        "finish",
        "The song is done: Spinus checks it a last time and gives it to the user to hear and apply.",
        schema(
            {"summary": {"type": "string", "description": "In a sentence or two, the idea of the song"}}, ["summary"]
        ),
    ),
]
BASIC_TOOLS = list(TOOLS)

# ---------------------------------------------------------------------------------------------------
# How deep the AI may go: groups of tools, chosen on the Write a song page (owner: more tools, more cost)

# Group -> its name in words, and what it lets the AI do (said in the brief and the system prompt).
GROUPS: dict[str, tuple[str, str]] = {
    "basic": ("Basic", "the form, chords, every part's notes, motifs, sounds, check, measure and finish"),
    "arrangement": (
        "Arrangement",
        "parts and layers of its own, motif variations, drum fills, layers entering and leaving, the energy check",
    ),
    "transitions": ("Transitions and automation", "automation clips (filter sweeps, fades, sends), risers, impacts"),
    "mix": ("Effects and mix", "effects on each part's insert, sends to reverb and delay, sidechain ducking, buses"),
}
GROUP_ORDER = tuple(GROUPS)
# The page's presets.
DEPTHS: dict[str, tuple[str, ...]] = {
    "basic": ("basic",),
    "arrangement": ("basic", "arrangement"),
    "transitions": ("basic", "arrangement", "transitions"),
    "full": GROUP_ORDER,
}
DEFAULT_DEPTH = DEPTHS["arrangement"]  # owner: Basic + arrangement at first
TOOL_GROUP: dict[str, str] = {t.name: "basic" for t in BASIC_TOOLS}
GROUP_SYSTEM: dict[str, str] = {}  # what the system prompt adds for a group's tools


def check_groups(value: Any) -> tuple[str, ...]:
    """The groups allowed: a preset's name, a list, or "basic,arrangement"; Basic always, in GROUP_ORDER."""
    if value is None or value == "":
        return DEFAULT_DEPTH
    if isinstance(value, str):
        names = list(DEPTHS[value]) if value in DEPTHS else [v.strip() for v in value.split(",") if v.strip()]
    elif isinstance(value, list | tuple):
        names = [str(v) for v in value]
    else:
        raise ValueError("tools is a depth (basic, arrangement, transitions, full) or a list of groups.")
    unknown = [n for n in names if n not in GROUPS]
    if unknown:
        raise ValueError(f"There is no group of tools '{unknown[0]}' (the groups: {', '.join(GROUP_ORDER)}).")
    return tuple(g for g in GROUP_ORDER if g == "basic" or g in names)


def tools_for(groups: tuple[str, ...]) -> list[Tool]:
    """Only the allowed groups' tools are sent: the others' definitions cost no tokens."""
    return [t for t in TOOLS if TOOL_GROUP.get(t.name, "basic") in groups]


def system_for(groups: tuple[str, ...]) -> str:
    return SYSTEM + "".join("\n\n" + GROUP_SYSTEM[g] for g in groups if g in GROUP_SYSTEM)


def depth_words(groups: tuple[str, ...]) -> str:
    """ "Basic + arrangement", or "Full"."""
    if tuple(groups) == GROUP_ORDER:
        return "Full"
    return " + ".join(GROUPS[g][0] if i == 0 else GROUPS[g][0].lower() for i, g in enumerate(groups))


# ---------------------------------------------------------------------------------------------------
# Arrangement

FAMILIES = ("Drums", "Bass", "Synths", "Leads", "Guitars", "Keys", "Orchestra", "FX")
COLOUR = re.compile(r"^#[0-9a-fA-F]{6}$")
VARIATIONS = ("rhythm", "notes", "invert", "displace")
FILLS = ("snare", "toms", "build")
ARRANGEMENT_TOOLS = [
    Tool(
        "add_part",
        "Add a part of your own to the song (a layer: a pad, a second guitar, a percussion loop...), with its role. "
        "Spinus gives it a channel, coloured and grouped by family, and a sound of its role.",
        schema(
            {
                "id": {"type": "string", "description": "Short, lowercase, its own: 'pad2', 'shaker'"},
                "name": {"type": "string"},
                "role": {"type": "string", "enum": list(sw.ROLES)},
            },
            ["id", "role"],
        ),
    ),
    Tool("remove_part", "Remove a part you added (the user's parts stay).", schema({"part": PART}, ["part"])),
    Tool(
        "set_look",
        "A part's family (its Channel Rack group and mixer bus) and colour.",
        schema(
            {
                "part": PART,
                "family": {"type": "string", "description": f"One of {', '.join(FAMILIES)}, or a name of your own"},
                "colour": {"type": "string", "description": "'#rrggbb'"},
            },
            ["part"],
        ),
    ),
    Tool(
        "vary",
        "Make a new motif from one you named: rhythm (notes moved off and onto the beat), notes (some notes moved "
        "a step of the key), invert (upside down around its first note), displace (moved later by `amount` steps).",
        schema(
            {
                "name": {"type": "string", "description": "The new motif's name"},
                "motif": {"type": "string", "description": "The motif it varies"},
                "how": {"type": "string", "enum": list(VARIATIONS)},
                "amount": {"type": "integer", "description": "Steps for displace; how many notes for notes"},
            },
            ["name", "motif", "how"],
        ),
    ),
    Tool(
        "fill",
        "Drum fills at phrase ends in a section: the end of every `every` bars (and the section's last bar), with "
        "a crash where the next phrase starts.",
        schema(
            {
                "part": PART,
                "section": SECTION,
                "every": {"type": "integer", "description": "Phrase length in bars (4 or 8; the section's own)"},
                "kind": {"type": "string", "enum": list(FILLS)},
                "crash": {"type": "boolean"},
            },
            ["part", "section"],
        ),
    ),
    Tool(
        "play_only",
        "Where a part plays in a section: only the bars given ('1-4', '5-8'), 'none' (it rests), or 'all' (back as "
        "written). Layers entering and leaving.",
        schema({"part": PART, "section": SECTION, "bars": {"type": "string"}}, ["part", "section", "bars"]),
    ),
    Tool(
        "energy",
        "The song's energy section by section (how busy, how many parts, how loud the notes), against the reference "
        "songs' sections.",
        schema({}, []),
    ),
]
TOOLS += ARRANGEMENT_TOOLS
TOOL_GROUP.update({t.name: "arrangement" for t in ARRANGEMENT_TOOLS})
GROUP_SYSTEM["arrangement"] = """Arrangement tools: design the form and length yourself (set_form), add the \
parts and layers the song needs (add_part, with set_look for its family and colour), vary motifs instead of \
repeating them unchanged (vary, then place), put drum fills at phrase ends (fill), and let layers enter and \
leave so sections build and drop (play_only). Check the energy curve against the references (energy)."""

# ---------------------------------------------------------------------------------------------------
# Transitions and automation

AUTOMATABLE = ("volume", "pan", "filter")
SHAPES = ("linear", "ease-in", "ease-out")
TRANSITIONS = ("riser", "downlifter", "swell", "impact")
TRANSITIONS_TOOLS = [
    Tool(
        "automate",
        "A Playlist automation clip for a part: volume (dB), pan (-1 left to 1 right) or a filter's cutoff (Hz; "
        "Spinus adds a filter to the part's insert, low-pass or high-pass). Points are 'bar value' on the song's "
        "bars (1 is the first; 16.5 is half way through bar 16), separated by semicolons: '17 -30; 21 0' fades in "
        "over four bars.",
        schema(
            {
                "part": PART,
                "what": {"type": "string", "enum": list(AUTOMATABLE)},
                "points": {"type": "string", "description": "'bar value; bar value; ...'"},
                "filter": {"type": "string", "enum": ["low-pass", "high-pass"]},
                "shape": {"type": "string", "enum": list(SHAPES)},
            },
            ["part", "what", "points"],
        ),
    ),
    Tool(
        "transition",
        "Into or out of every play of a section: a riser (noise rising over the bars before it), a downlifter "
        "(falling away over the bars after it starts), a swell (a cymbal-like swell into its first beat) or an "
        "impact (a hit on its first beat: the kit's kick and crash, or with sample, the starter pack's).",
        schema(
            {
                "kind": {"type": "string", "enum": list(TRANSITIONS)},
                "section": SECTION,
                "bars": {"type": "integer", "description": "How long, 1 to 8 (a riser: 2 or 4; a swell: 1)"},
                "sample": {"type": "boolean", "description": "impact: the starter pack's crash and 808"},
            },
            ["kind", "section"],
        ),
    ),
    Tool("automation", "The song's automation clips and transitions so far.", schema({}, [])),
    Tool(
        "remove_automation",
        "Remove a part's automation clips (all, or of one kind).",
        schema({"part": PART, "what": {"type": "string", "enum": list(AUTOMATABLE)}}, ["part"]),
    ),
]
TOOLS += TRANSITIONS_TOOLS
TOOL_GROUP.update({t.name: "transitions" for t in TRANSITIONS_TOOLS})
GROUP_SYSTEM["transitions"] = """Transition and automation tools: lead into big sections and out of them \
(transition: risers before a chorus, an impact on its first beat, a downlifter after a drop, a swell into a \
new section), and shape parts over time with automation clips (automate: fades, filter sweeps opening a build, \
a pan moving). Keep them few and purposeful."""
# Spinus's own effects as automation reads them: the Spinus Filter's cutoff (src/dsp/filter/FilterProcessor.cpp:
# 20 Hz to 20 kHz, 1 kHz in the middle) and the faders (Tracktion's fader position, exp((dB - 6) / 20)).
CUTOFF = (20.0, 20000.0, 1000.0)
STARTER_SAMPLES = {"crash": "Drums/Cymbals/Crash Wide.flac", "boom": "Drums/808s/808 Long.flac"}


def normalised(what: str, value: float) -> float:
    """A volume (dB), pan (-1..1) or cutoff (Hz) as the automation's 0..1."""
    if what == "volume":
        return 0.0 if value <= -100 else min(1.0, math.exp((value - 6.0) / 20.0))
    if what == "pan":
        return (max(-1.0, min(1.0, value)) + 1.0) / 2.0
    low, high, centre = CUTOFF
    skew = math.log(0.5) / math.log((centre - low) / (high - low))
    return float(((max(low, min(high, value)) - low) / (high - low)) ** skew)


TOOL_NAMES = [t.name for t in TOOLS]


class Mistake(ValueError):
    """A tool call that cannot be done; its reason goes back to the model as the tool's error."""


def bar_range(text: Any, bars: int) -> tuple[int, int]:
    """'3' or '5-8' (from 1) -> first and last bar (from 0), inside the section."""
    match = re.fullmatch(r"\s*(\d+)\s*(?:-\s*(\d+))?\s*", str(text or ""))
    if not match:
        raise Mistake(f"bars is '3' or '5-8', not '{text}'")
    first, last = int(match.group(1)), int(match.group(2) or match.group(1))
    if not 1 <= first <= last <= bars:
        raise Mistake(f"the section has bars 1 to {bars}")
    return first - 1, last - 1


@dataclass
class Workspace:
    """The song being written: its form, every part's notes per section, the sounds picked, the motifs."""

    setup: compose.Setup
    seed: int
    size: str = "sketch"
    tempo: float = 120.0
    key: dict[str, Any] = field(default_factory=dict)
    sections: dict[str, dict[str, Any]] = field(default_factory=dict)  # id -> {id, kind, bars, chords, hold, energy}
    order: list[str] = field(default_factory=list)
    notes: dict[str, dict[str, list[dict[str, Any]]]] = field(default_factory=dict)  # part -> section -> notes
    sounds: dict[str, str] = field(default_factory=dict)
    motifs: dict[str, dict[str, Any]] = field(default_factory=dict)
    by_rules: set[tuple[str, str]] = field(default_factory=set)  # (part, section) the rules wrote
    levels: dict[str, Any] = field(default_factory=dict)  # part -> dB as the app measured it (None: silent)
    problems: dict[str, str] = field(default_factory=dict)
    summary: str = ""
    finished: bool = False
    calls: Counter[str] = field(default_factory=Counter)
    groups: tuple[str, ...] = GROUP_ORDER  # the groups of tools allowed (the page's depth)
    # The later groups' state: "added" (parts the AI added), "looks" (part -> family, colour), "stash" (part ->
    # section -> notes play_only took out).
    extra: dict[str, Any] = field(default_factory=dict)

    # --- making one -------------------------------------------------------------------------------

    @staticmethod
    def start(setup: compose.Setup, seed: int, size: str) -> Workspace:
        """A new song with the rules' form for the style (sections, lengths, chords) and no notes yet."""
        rules = sw.write(setup.summary, seed, size, setup.parts)
        ws = Workspace(setup, seed, size, float(rules["tempo"]), dict(rules["key"]))
        ws.setup = ws.own_setup()
        for s in rules["sections"]:
            hold = 2 if len(s["chords"]) >= 2 and s["chords"][0]["roman"] == s["chords"][1]["roman"] else 1
            loop = [c["roman"] for c in s["chords"][::hold]][: len(s["loop"])] or ["i"]
            ws.sections[s["id"]] = {
                "id": s["id"],
                "kind": s["kind"],
                "bars": int(s["bars"]),
                "chords": loop,
                "hold": hold,
                "energy": float(s["energy"]),
            }
        ws.order = [e["section"] for e in rules["arrangement"]]
        return ws

    @staticmethod
    def from_song(setup: compose.Setup, song: dict[str, Any]) -> Workspace:
        """The working copy of a song the harness wrote (to revise it)."""
        kept = (song.get("composed") or {}).get("workspace")
        if not isinstance(kept, dict):
            raise ValueError("This song was not written through the harness, so there is nothing to revise.")
        ws = Workspace(
            setup, int(song["seeds"]["song"]), str(song.get("size", "sketch")), float(song["tempo"]), dict(song["key"])
        )
        ws.sections = {s["id"]: dict(s) for s in kept["sections"]}
        ws.order = list(kept["order"])
        ws.notes = json.loads(json.dumps(kept["notes"]))
        ws.sounds = dict(kept.get("sounds") or {})
        ws.motifs = dict(kept.get("motifs") or {})
        ws.by_rules = {(p, s) for p, s in kept.get("byRules") or []}
        ws.extra = json.loads(json.dumps(kept.get("extra") or {}))
        ws.setup = ws.own_setup()
        for part in ws.extra.get("added", []):
            if not any(p["id"] == part["id"] for p in ws.setup.parts):
                ws.setup.parts.append(dict(part))
        return ws

    def export(self) -> dict[str, Any]:
        return {
            "sections": [self.sections[sid] for sid in self.sections],
            "order": self.order,
            "notes": self.notes,
            "sounds": self.sounds,
            "motifs": self.motifs,
            "byRules": sorted([list(x) for x in self.by_rules]),
            "extra": self.extra,
        }

    def own_setup(self) -> compose.Setup:
        """The setup with a parts list of this working copy's own (the AI may add parts)."""
        return dataclasses.replace(self.setup, parts=[dict(p) for p in self.setup.parts])

    def copy(self) -> Workspace:
        other = Workspace(self.own_setup(), self.seed, self.size, self.tempo, dict(self.key))
        other.groups = self.groups
        state = json.loads(json.dumps(self.export()))
        other.sections = {s["id"]: s for s in state["sections"]}
        other.order, other.notes, other.sounds, other.motifs = (
            state["order"],
            state["notes"],
            state["sounds"],
            state["motifs"],
        )
        other.by_rules = set(self.by_rules)
        other.extra = state["extra"]
        other.summary, other.finished = self.summary, self.finished
        return other

    # --- what the tools look at ---------------------------------------------------------------------

    @property
    def steps(self) -> int:
        return self.setup.style.steps

    def part(self, pid: Any) -> dict[str, str]:
        found = next((p for p in self.setup.written() if p["id"] == str(pid)), None)
        if found is None:
            names = ", ".join(p["id"] for p in self.setup.written())
            raise Mistake(f"there is no part '{pid}' (the parts: {names})")
        return found

    def section(self, sid: Any) -> dict[str, Any]:
        found = self.sections.get(str(sid or "").lower())
        if found is None:
            raise Mistake(f"there is no section '{sid}' (the sections: {', '.join(self.sections)})")
        return found

    def drums(self, pid: str) -> bool:
        return bool(sw.ROLES[self.part(pid)["role"]][4])

    def plan(self) -> dict[str, Any]:
        """The form as compose.check_plan gives it."""
        return {
            "tempo": self.tempo,
            "key": self.key,
            "sections": [self.sections[sid] for sid in self.sections if sid in self.order],
            "order": self.order,
        }

    def chord_at(self, sid: str, bar: int) -> list[int]:
        s = self.sections[sid]
        roman = s["chords"][(bar // int(s["hold"])) % len(s["chords"])]
        return sw.chord_tones(roman, int(self.key["tonic"]))

    def song(self) -> dict[str, Any]:
        """The song from the working copy, as songwriter.write gives songs (the rules fill parts with no notes)."""
        written = {p: by for p, by in self.notes.items() if any(by.values())}
        checked = compose.Checked(self.plan(), written, {}, dict(self.sounds), [], {})
        checked.filled = [p["id"] for p in self.setup.written() if p["id"] not in written]
        song = compose.build_song(checked, self.setup, self.seed, self.size)
        for layer in song["layers"]:
            look = (self.extra.get("looks") or {}).get(layer["id"]) or {}
            layer.update({k: v for k, v in look.items() if k in ("family", "colour")})
        # The transitions placed with the tools take the rules' place for their parts.
        for pid, fx in (self.extra.get("fx") or {}).items():
            if any(x["id"] == pid for x in song["layers"]) or any(p["id"] == pid for p in self.setup.parts):
                song["fx"][pid] = {"bars": fx["bars"], "at": list(fx["at"]), "notes": list(fx["notes"])}
                if not any(x["id"] == pid for x in song["layers"]):
                    part = next(p for p in self.setup.parts if p["id"] == pid)
                    role = part["role"]
                    song["layers"].append({"id": pid, "name": part["name"], "role": role, "family": sw.ROLES[role][1],
                                           "part": sw.ROLES[role][2], "soundRole": sw.ROLES[role][3],
                                           "drums": sw.ROLES[role][4], "instance": 0})  # fmt: skip
        production = self.production()
        if production["automation"] or production["samples"]:
            song["production"] = production
        return song

    # --- notation -----------------------------------------------------------------------------------

    def parse(self, pid: str, text: str, bars: int, loop_bars: int | None = None) -> tuple[list[dict[str, Any]], str]:
        """Notes for `bars` bars from the notation (a loop repeated), and what was fixed in words."""
        loop = max(1, min(bars, loop_bars or bars))
        parsed = compose.parse_notes(str(text or ""), self.drums(pid), self.part(pid)["role"], loop, bars, self.steps)
        if parsed.tokens and not parsed.notes:
            raise Mistake("no note could be read: write 'step pitch length velocity; ...'")
        fixed = []
        if parsed.bad:
            fixed.append(f"{parsed.bad} unreadable or past the loop, left out")
        if parsed.folded:
            low, high = compose.RANGES.get(self.part(pid)["role"], (24, 108))
            fixed.append(
                f"{parsed.folded} outside the range {sw.note_name(low)}-{sw.note_name(high)}, moved by octaves"
            )
        if parsed.snapped:
            fixed.append(f"{parsed.snapped} moved onto the sixteenth grid")
        return parsed.notes, ("; ".join(fixed))

    def notation(self, pid: str, notes: list[dict[str, Any]], offset: int = 0) -> str:
        drums = self.drums(pid)
        return "; ".join(
            f"{(int(n['start']) - offset) // sw.TICKS_PER_STEP} {n['drum'] if drums else n['pitch']} "
            f"{max(1, round(int(n['length']) / sw.TICKS_PER_STEP))} {round(float(n['velocity']) * 127)}"
            for n in notes
        )

    # --- the tools ----------------------------------------------------------------------------------

    def call(self, name: str, args: dict[str, Any]) -> str:
        if name not in TOOL_NAMES:
            raise Mistake(f"there is no tool '{name}'")
        if TOOL_GROUP.get(name, "basic") not in self.groups:
            group = GROUPS[TOOL_GROUP[name]][0]
            raise Mistake(f"'{name}' belongs to the {group} tools, which are not allowed for this song")
        self.calls[name] += 1
        result: str = getattr(self, "tool_" + name)(**args)
        return result

    def tool_brief(self) -> str:
        style = self.setup.style
        lines = [
            f"Tempo {self.tempo:g} BPM (the style: {style.tempo:g}, {style.tempo_low:g}-{style.tempo_high:g}); key "
            f"{self.key['name']}; {style.beats_per_bar} beats a bar, {self.steps} steps a bar; swing {style.swing:g}.",
        ]
        brief = compose.style_brief(self.setup)
        if brief.get("understood"):
            lines.append(f"Described: {brief.get('description') or ''} (understood: {brief['understood']}).")
        for mode, chords in brief["chordVocabulary"].items():
            if chords:
                lines.append(f"Chords in {mode}: " + ", ".join(f"{c} {round(100 * v)}%" for c, v in chords.items()))
        lines.append(
            "Section lengths: "
            + ", ".join(f"{b} bars x{n}" for b, n in style.lengths)
            + f"; about {style.sections_per_song:g} sections, {style.song_bars:g} bars a song."
        )
        for name, p in style.parts.items():
            lines.append(
                f"The references' {name}: {sw.note_name(p.low)}-{sw.note_name(p.high)}, around "
                f"{sw.note_name(p.median)}, {p.per_bar:g} notes a bar, {p.voices:g} at a time."
            )
        if style.has_drums:
            for lane, values in style.lanes.items():
                lines.append(f"Drum groove {lane} (chance a step is hit): " + " ".join(f"{v:.1f}" for v in values))
        if style.without:
            lines.append("Leave out: " + ", ".join(sorted(style.without)))
        for b in compose.part_brief(self.setup):
            words = f"Part {b['id']} ({b['name']}, role {b['role']}"
            words += (
                ", drums)" if b["drums"] else f", range {sw.note_name(b['range'][0])}-{sw.note_name(b['range'][1])})"
            )
            if b.get("currentSound"):
                words += f"; sound: {b['currentSound']}"
            if b.get("sounds"):
                words += f"; {len(b['sounds'])} sounds to choose from (list_sounds)"
            lines.append(words)
        return "\n".join(lines)

    def tool_form(self) -> str:
        lines = [
            f"Tempo {self.tempo:g}, key {self.key['name']}. Order: {' '.join(self.order)} "
            f"({sum(self.sections[s]['bars'] for s in self.order)} bars)."
        ]
        for sid, s in self.sections.items():
            has = [p for p, by in self.notes.items() if by.get(sid)]
            lines.append(
                f"{sid}: {s['kind']}, {s['bars']} bars, energy {s['energy']:g}, chords "
                f"{' '.join(s['chords'])} ({s['hold']} bar{'s' if s['hold'] > 1 else ''} each); notes: "
                f"{', '.join(has) or 'none yet'}"
            )
        return "\n".join(lines)

    def tool_read(self, part: str, section: str, bars: str = "") -> str:
        p, s = self.part(part), self.section(section)
        notes = self.notes.get(p["id"], {}).get(s["id"], [])
        bar_ticks = self.steps * sw.TICKS_PER_STEP
        if bars:
            first, last = bar_range(bars, int(s["bars"]))
            notes = [n for n in notes if first * bar_ticks <= int(n["start"]) < (last + 1) * bar_ticks]
            text = self.notation(p["id"], notes, first * bar_ticks)
            return f"{p['id']} in {s['id']} bars {first + 1}-{last + 1} (steps from bar {first + 1}): {text or 'rests'}"
        return f"{p['id']} in {s['id']}: {self.notation(p['id'], notes) or 'rests'}"

    def tool_list_sounds(self, part: str, query: str = "") -> str:
        p = self.part(part)
        found = [c for c in self.setup.candidates.get(p["id"], []) if query.lower() in c["name"].lower()]
        current = self.setup.current.get(p["id"])
        if not found:
            return (
                f"No sounds to choose for {p['id']}"
                + (f" with '{query}'" if query else "")
                + (f"; it plays {current}, chosen by the user." if current else ".")
            )
        picked = self.sounds.get(p["id"])
        return "\n".join(f"{c['id']}: {c['name']}" + (" (picked)" if c["id"] == picked else "") for c in found)

    def tool_set_form(
        self, sections: list[dict[str, Any]], order: list[str], tempo: float | None = None, key: str | None = None
    ) -> str:
        answer: dict[str, Any] = {
            "sections": sections,
            "order": order,
            "tempo": tempo or self.tempo,
            "key": key or self.key["name"],
        }
        try:
            plan = compose.check_plan(answer, self.setup)
        except compose.Unusable as why:
            raise Mistake(str(why)) from why
        old = self.sections
        self.tempo, self.key = plan["tempo"], plan["key"]
        self.sections = {s["id"]: s for s in plan["sections"]}
        self.order = plan["order"]
        bar_ticks = self.steps * sw.TICKS_PER_STEP
        for by in self.notes.values():
            for sid in list(by):
                if sid not in self.sections:
                    del by[sid]
                elif old.get(sid) and self.sections[sid]["bars"] < old[sid]["bars"]:
                    end = self.sections[sid]["bars"] * bar_ticks
                    by[sid] = [n for n in by[sid] if int(n["start"]) < end]
        return (
            f"Form set: {' '.join(self.order)}, {sum(self.sections[s]['bars'] for s in self.order)} bars, "
            f"{self.tempo:g} BPM, {self.key['name']}."
        )

    def tool_set_chords(self, section: str, chords: list[str], barsPerChord: int = 1) -> str:
        s = self.section(section)
        romans = [str(c).strip() for c in chords]
        wrong = [c for c in romans if sw.parse_roman(c) is None]
        if not 1 <= len(romans) <= 8 or wrong:
            raise Mistake(f"1 to 8 chords as Roman numerals ({', '.join(wrong)} not read)")
        s["chords"], s["hold"] = romans, 2 if barsPerChord == 2 else 1
        return f"{s['id']} chords: {' '.join(romans)}."

    def put(self, pid: str, sid: str, notes: list[dict[str, Any]]) -> None:
        self.notes.setdefault(pid, {})[sid] = sorted(
            notes, key=lambda n: (int(n["start"]), str(n.get("pitch", n.get("drum"))))
        )
        self.by_rules.discard((pid, sid))

    def tool_write(self, part: str, section: str, notes: str, loop_bars: int | None = None) -> str:
        p, s = self.part(part), self.section(section)
        written, fixed = self.parse(p["id"], notes, int(s["bars"]), loop_bars)
        self.put(p["id"], s["id"], written)
        return f"{p['id']} in {s['id']}: {len(written)} notes." + (f" Fixed: {fixed}." if fixed else "")

    def tool_edit(self, part: str, section: str, bars: str, notes: str) -> str:
        p, s = self.part(part), self.section(section)
        first, last = bar_range(bars, int(s["bars"]))
        bar_ticks = self.steps * sw.TICKS_PER_STEP
        written, fixed = self.parse(p["id"], notes, last - first + 1)
        offset = first * bar_ticks
        kept = [
            n
            for n in self.notes.get(p["id"], {}).get(s["id"], [])
            if not offset <= int(n["start"]) < (last + 1) * bar_ticks
        ]
        self.put(p["id"], s["id"], kept + [{**n, "start": int(n["start"]) + offset} for n in written])
        return f"{p['id']} in {s['id']} bars {first + 1}-{last + 1}: {len(written)} notes." + (
            f" Fixed: {fixed}." if fixed else ""
        )

    def tool_motif(self, name: str, notes: str, bars: int) -> str:
        if not re.fullmatch(r"[A-Za-z0-9 _-]{1,30}", name or ""):
            raise Mistake("a motif's name is a short word")
        if not 1 <= int(bars) <= 16:
            raise Mistake("a motif is 1 to 16 bars")
        self.motifs[name] = {"notes": str(notes), "bars": int(bars)}
        return f"Motif '{name}' ({bars} bar{'s' if bars > 1 else ''}) saved."

    def tool_place(
        self,
        part: str,
        section: str,
        motif: str,
        bar: int,
        transpose: int = 0,
        times: int = 1,
        fit_chords: bool = False,
    ) -> str:
        p, s = self.part(part), self.section(section)
        m = self.motifs.get(motif)
        if m is None:
            raise Mistake(f"there is no motif '{motif}' (motifs: {', '.join(self.motifs) or 'none yet'})")
        times = max(1, min(64, int(times)))
        first = int(bar) - 1
        length = int(m["bars"]) * times
        if not 0 <= first < int(s["bars"]):
            raise Mistake(f"the section has bars 1 to {s['bars']}")
        length = min(length, int(s["bars"]) - first)
        drums = self.drums(p["id"])
        notes, fixed = self.parse(p["id"], m["notes"], length, int(m["bars"]))
        bar_ticks = self.steps * sw.TICKS_PER_STEP
        low, high = compose.RANGES.get(p["role"], (24, 108))
        scale = sw.scale_of(self.key)
        moved = 0
        for n in notes:
            n["start"] = int(n["start"]) + first * bar_ticks
            if drums:
                continue
            pitch = int(n["pitch"]) + int(transpose)
            if fit_chords:
                fits = sw.allowed_pitch_classes(self.chord_at(s["id"], int(n["start"]) // bar_ticks), scale)
                if pitch % 12 not in fits:
                    pitch = min(
                        (pitch + d for d in (-1, 1, -2, 2) if (pitch + d) % 12 in fits),
                        default=pitch,
                        key=lambda x: abs(x - pitch),
                    )
                    moved += 1
            while pitch < low:
                pitch += 12
            while pitch > high:
                pitch -= 12
            n["pitch"] = pitch
        end = (first + length) * bar_ticks
        kept = [
            n for n in self.notes.get(p["id"], {}).get(s["id"], []) if not first * bar_ticks <= int(n["start"]) < end
        ]
        self.put(p["id"], s["id"], kept + notes)
        words = f"'{motif}' in {p['id']}, {s['id']} bars {first + 1}-{first + length}: {len(notes)} notes"
        if moved:
            words += f", {moved} moved to fit the chords"
        return words + "." + (f" Fixed: {fixed}." if fixed else "")

    def tool_copy(self, from_section: str, to_section: str, parts: list[str] | None = None) -> str:
        a, b = self.section(from_section), self.section(to_section)
        ids = [self.part(x)["id"] for x in parts] if parts else [p for p, by in self.notes.items() if by.get(a["id"])]
        bar_ticks = self.steps * sw.TICKS_PER_STEP
        span, end = int(a["bars"]) * bar_ticks, int(b["bars"]) * bar_ticks
        for pid in ids:
            source = self.notes.get(pid, {}).get(a["id"], [])
            out = []
            for k in range(math.ceil(end / span)):
                for n in source:
                    start = int(n["start"]) + k * span
                    if start < end:
                        out.append({**n, "start": start, "length": min(int(n["length"]), end - start)})
            self.put(pid, b["id"], out)
        return f"Copied {a['id']} to {b['id']} for {', '.join(ids) or 'no part (nothing there yet)'}."

    def tool_pick_sound(self, part: str, sound: str) -> str:
        p = self.part(part)
        if not any(c["id"] == sound for c in self.setup.candidates.get(p["id"], [])):
            raise Mistake(f"'{sound}' is not one of {p['id']}'s sounds (list_sounds)")
        self.sounds[p["id"]] = sound
        return f"{p['id']} plays {sound}."

    def tool_rules_write(self, part: str, section: str = "") -> str:
        p = self.part(part)
        ids = [self.section(section)["id"]] if section else list(self.sections)
        checked = compose.Checked(self.plan(), {}, {}, {}, [], {})
        rules = compose.build_song(checked, self.setup, self.seed, self.size)
        written = rules["layerParts"].get(p["id"], {})
        for sid in ids:
            self.notes.setdefault(p["id"], {})[sid] = list(written.get(sid, []))
            self.by_rules.add((p["id"], sid))
        count = sum(len(written.get(sid, [])) for sid in ids)
        return f"The rules wrote {p['id']} in {', '.join(ids)}: {count} notes."

    # --- transitions and automation -------------------------------------------------------------------

    def song_bars(self) -> int:
        return sum(int(self.sections[s]["bars"]) for s in self.order)

    def tool_automate(self, part: str, what: str, points: str, filter: str = "low-pass", shape: str = "linear") -> str:
        pid = self.part(part)["id"]
        if what not in AUTOMATABLE:
            raise Mistake(f"automate one of {', '.join(AUTOMATABLE)}")
        if shape not in SHAPES:
            raise Mistake(f"a shape is one of {', '.join(SHAPES)}")
        if filter not in ("low-pass", "high-pass"):
            raise Mistake("a filter is low-pass or high-pass")
        total = self.song_bars()
        read: list[tuple[float, float]] = []
        for raw in re.split(r"[;\n]+", str(points)):
            fields = raw.replace(",", " ").split()
            if not fields:
                continue
            try:
                bar, value = float(fields[0]), float(fields[1])
            except (ValueError, IndexError):
                raise Mistake(f"a point is 'bar value', not '{raw.strip()}'") from None
            if not (math.isfinite(bar) and math.isfinite(value)) or not 1 <= bar <= total + 1:
                raise Mistake(f"a point's bar is 1 to {total + 1} (the song's end)")
            read.append((bar - 1, value))
        if len(read) < 2:
            raise Mistake("an automation clip needs two points or more")
        read.sort()
        units = {"volume": (-100.0, 6.0, "dB"), "pan": (-1.0, 1.0, ""), "filter": (CUTOFF[0], CUTOFF[1], "Hz")}[what]
        if any(not units[0] <= v <= units[1] for _, v in read):
            raise Mistake(f"{what} values are {units[0]:g} to {units[1]:g} {units[2]}".rstrip())
        self.extra.setdefault("automation", []).append(
            {"part": pid, "what": what, "points": read, "filter": filter, "shape": shape}
        )
        return f"Automation: {pid} {what} from bar {read[0][0] + 1:g} to {read[-1][0] + 1:g} ({len(read)} points)."

    def fx_part(self, kind: str) -> str:
        """The part a transition plays on: the user's of its role, else one the AI adds for it."""
        role = "impact" if kind == "impact" else "riser"
        if kind in ("riser", "impact"):
            mine = next((p["id"] for p in self.setup.written() if p["role"] == role), None)
            if mine is None:
                mine = next((p["id"] for p in self.setup.parts if p["role"] == role), None)
            if mine is not None:
                return mine
        if not any(p["id"] == kind for p in self.setup.parts):
            part = {"id": kind, "name": kind.capitalize(), "role": role}
            self.setup.parts.append(part)
            self.extra.setdefault("added", []).append(part)
        return kind

    def tool_transition(self, kind: str, section: str, bars: int = 0, sample: bool = False) -> str:
        if kind not in TRANSITIONS:
            raise Mistake(f"a transition is one of {', '.join(TRANSITIONS)}")
        s = self.section(section)
        bars = int(bars) or {"riser": 2, "downlifter": 2, "swell": 1, "impact": 1}[kind]
        if not 1 <= bars <= 8:
            raise Mistake("a transition is 1 to 8 bars")
        starts, at = [], 0
        for sid in self.order:
            if sid == s["id"]:
                starts.append(at)
            at += int(self.sections[sid]["bars"])
        before = kind in ("riser", "swell")
        places = [b - bars if before else b for b in starts if (b - bars >= 0 if before else True)]
        if not places:
            raise Mistake(f"there are not {bars} bars before the {s['id']}: it starts the song")
        pid = self.fx_part(kind)
        bar_ticks = self.steps * sw.TICKS_PER_STEP
        if kind == "impact":
            notes = [
                {"start": 0, "length": sw.TICKS_PER_STEP // 2, "drum": d, "velocity": 1.0} for d in ("kick", "crash")
            ]
        else:
            pitch = 60 + int(self.key["tonic"])
            notes = [{"start": 0, "length": bars * bar_ticks - sw.TICKS_PER_STEP, "pitch": pitch, "velocity": 0.8}]
        fx = self.extra.setdefault("fx", {})
        entry = fx.setdefault(pid, {"bars": bars, "at": [], "notes": notes})
        if entry["bars"] != bars:
            raise Mistake(f"{pid}'s transitions are {entry['bars']} bars: use that length, or another kind")
        entry["at"] = sorted(set(entry["at"]) | set(places))

        # Each riser opens and rises, a downlifter closes and falls, a swell rises sharply into the beat.
        def ramp(what: str, a: float, b: float, at: int, shape: str) -> None:
            self.extra.setdefault("automation", []).append(
                {"part": pid, "what": what, "points": [(at, a), (at + bars, b)], "filter": "low-pass", "shape": shape}
            )

        for place in places:
            if kind == "riser":
                ramp("volume", -30.0, 0.0, place, "ease-in")
                ramp("filter", 300.0, 12000.0, place, "ease-in")
            elif kind == "downlifter":
                ramp("volume", 0.0, -40.0, place, "ease-out")
                ramp("filter", 12000.0, 200.0, place, "ease-out")
            elif kind == "swell":
                ramp("volume", -50.0, 0.0, place, "ease-in")
            elif sample:
                self.extra.setdefault("samples", []).extend(
                    {"file": STARTER_SAMPLES[k], "bar": place, "name": f"Impact {k}"} for k in ("crash", "boom")
                )
        where = ", ".join(f"bar {p + 1}" for p in places)
        return f"{kind.capitalize()} on {pid}, {bars} bar{'s' if bars > 1 else ''}, at {where}."

    def tool_automation(self) -> str:
        lines = []
        for a in self.extra.get("automation", []):
            values = ", ".join(f"bar {b + 1:g}: {v:g}" for b, v in a["points"])
            lines.append(
                f"{a['part']} {a['what']}" + (f" ({a['filter']})" if a["what"] == "filter" else "") + f": {values}"
            )
        for pid, fx in (self.extra.get("fx") or {}).items():
            lines.append(f"{pid}: {fx['bars']}-bar transition at bars " + ", ".join(str(b + 1) for b in fx["at"]))
        for smp in self.extra.get("samples", []):
            lines.append(f"sample {smp['file']} at bar {smp['bar'] + 1}")
        return "\n".join(lines) or "No automation or transitions yet."

    def tool_remove_automation(self, part: str, what: str = "") -> str:
        pid = self.part(part)["id"]
        kept = [
            a for a in self.extra.get("automation", []) if not (a["part"] == pid and (not what or a["what"] == what))
        ]
        gone = len(self.extra.get("automation", [])) - len(kept)
        self.extra["automation"] = kept
        if not what:
            (self.extra.get("fx") or {}).pop(pid, None)
        return f"Removed {gone} automation clip{'s' if gone != 1 else ''} of {pid}."

    def production(self) -> dict[str, Any]:
        """The automation clips and samples for Apply: values as the automation reads them (0 to 1)."""
        out: dict[str, Any] = {"automation": [], "samples": list(self.extra.get("samples", []))}
        for a in self.extra.get("automation", []):
            curve = {"linear": 0.0, "ease-in": 0.5, "ease-out": -0.5}[a.get("shape", "linear")]
            out["automation"].append({
                "part": a["part"], "what": a["what"], "filter": a.get("filter", "low-pass"),
                "points": [{"bar": round(b, 4), "value": round(normalised(a["what"], v), 4), "curve": curve}
                           for b, v in a["points"]],
            })  # fmt: skip
        return out

    # --- arrangement ----------------------------------------------------------------------------------

    def tool_add_part(self, id: str, role: str, name: str = "") -> str:
        pid = str(id).strip().lower()
        if not compose.SECTION_ID.match(pid):
            raise Mistake("a part's id is a short lowercase word")
        if any(p["id"] == pid for p in self.setup.parts):
            raise Mistake(f"there is a part '{pid}' already")
        if role not in sw.ROLES:
            raise Mistake(f"'{role}' is not a role (the roles: {', '.join(sw.ROLES)})")
        if len(self.setup.parts) >= sw.MOST_PARTS:
            raise Mistake(f"the song has {sw.MOST_PARTS} parts, the most it can have")
        part = {"id": pid, "name": (str(name).strip() or sw.ROLES[role][0])[:60], "role": role}
        self.setup.parts.append(part)
        self.extra.setdefault("added", []).append(part)
        return f"Part {pid} ({part['name']}, {role}) added, in the {sw.ROLES[role][1]} family; write its notes."

    def tool_remove_part(self, part: str) -> str:
        pid = self.part(part)["id"]
        if not any(p["id"] == pid for p in self.extra.get("added", [])):
            raise Mistake(f"{pid} is one of the user's parts: it stays (let it rest with play_only instead)")
        self.setup.parts = [p for p in self.setup.parts if p["id"] != pid]
        self.extra["added"] = [p for p in self.extra["added"] if p["id"] != pid]
        self.notes.pop(pid, None)
        self.sounds.pop(pid, None)
        (self.extra.get("looks") or {}).pop(pid, None)
        return f"Part {pid} removed."

    def tool_set_look(self, part: str, family: str = "", colour: str = "") -> str:
        pid = self.part(part)["id"]
        look = self.extra.setdefault("looks", {}).setdefault(pid, {})
        if family:
            if not re.fullmatch(r"[A-Za-z][A-Za-z0-9 &-]{0,19}", family):
                raise Mistake("a family is a short name (Drums, Bass, Synths...)")
            look["family"] = family
        if colour:
            if not COLOUR.match(colour):
                raise Mistake("a colour is '#rrggbb'")
            look["colour"] = colour.lower()
        if not family and not colour:
            raise Mistake("give a family, a colour or both")
        return f"{pid}: " + ", ".join(f"{k} {v}" for k, v in look.items()) + "."

    def tool_vary(self, name: str, motif: str, how: str, amount: int = 1) -> str:
        source = self.motifs.get(motif)
        if source is None:
            raise Mistake(f"there is no motif '{motif}' (motifs: {', '.join(self.motifs) or 'none yet'})")
        if how not in VARIATIONS:
            raise Mistake(f"vary is one of {', '.join(VARIATIONS)}")
        if not re.fullmatch(r"[A-Za-z0-9 _-]{1,30}", name or ""):
            raise Mistake("a motif's name is a short word")
        bars = int(source["bars"])
        span = bars * self.steps
        notes = [n.split() for n in re.split(r"[;\n]+", str(source["notes"])) if len(n.split()) in (3, 4)]
        if not notes:
            raise Mistake(f"'{motif}' has no notes to vary")
        amount = max(1, min(span - 1, int(amount)))
        scale = sorted(sw.scale_of(self.key))
        pitched = [n for n in notes if n[1].lstrip("-").replace(".", "", 1).isdigit()]
        first = float(pitched[0][1]) if pitched else 0.0
        out = []
        for i, n in enumerate(notes):
            step, pitch, rest = n[0], n[1], n[2:]
            if how == "displace":
                step = str((round(float(step)) + amount) % span)
            elif how == "rhythm" and i % 2 == 1:
                step = str(min(span - 1, round(float(step)) + 1))  # pushed a sixteenth: off the beat
            elif how == "invert" and n in pitched:
                pitch = str(round(2 * first - float(pitch)))
            elif how == "notes" and n in pitched and i % 2 == 1 and i // 2 < amount:
                p = round(float(pitch))
                ups = [q for q in range(p + 1, p + 4) if q % 12 in scale]
                pitch = str(ups[0] if ups else p)
            out.append(" ".join([step, pitch, *rest]))
        out.sort(key=lambda t: float(t.split()[0]))
        self.motifs[name] = {"notes": "; ".join(out), "bars": bars}
        return f"Motif '{name}': '{motif}' {how}" + (f" by {amount}" if how in ("displace", "notes") else "") + "."

    def tool_fill(self, part: str, section: str, every: int = 0, kind: str = "snare", crash: bool = True) -> str:
        p, s = self.part(part), self.section(section)
        if not self.drums(p["id"]):
            raise Mistake(f"{p['id']} plays no drums: fills are for drum parts")
        if kind not in FILLS:
            raise Mistake(f"a fill is one of {', '.join(FILLS)}")
        bars = int(s["bars"])
        every = int(every) if every else bars
        if every < 1:
            raise Mistake("every is a number of bars")
        bar_ticks = self.steps * sw.TICKS_PER_STEP
        ends = sorted({b for b in range(every - 1, bars, every)} | {bars - 1})
        notes = list(self.notes.get(p["id"], {}).get(s["id"], []))
        half = self.steps // 2
        for end in ends:
            start = end * bar_ticks + (0 if kind == "build" else half * sw.TICKS_PER_STEP)
            notes = [n for n in notes if not start <= int(n["start"]) < (end + 1) * bar_ticks]
            length = bar_ticks if kind == "build" else half * sw.TICKS_PER_STEP
            count = length // sw.TICKS_PER_STEP
            toms = ("tomHigh", "tomHigh", "tomMid", "tomMid", "tomLow", "tomLow", "kick", "snare")
            for k in range(count):
                drum = toms[k * len(toms) // count] if kind == "toms" else "snare"
                notes.append({"start": start + k * sw.TICKS_PER_STEP, "length": sw.TICKS_PER_STEP // 2,
                              "drum": drum, "velocity": round(0.55 + 0.45 * k / max(1, count - 1), 3)})  # fmt: skip
            if crash and end + 1 < bars:
                at = (end + 1) * bar_ticks
                notes = [n for n in notes if not (int(n["start"]) == at and n.get("drum") == "crash")]
                notes.append({"start": at, "length": sw.TICKS_PER_STEP // 2, "drum": "crash", "velocity": 0.9})
        self.put(p["id"], s["id"], notes)
        where = ", ".join(str(e + 1) for e in ends)
        return f"{kind.capitalize()} fills in {p['id']}, {s['id']}, at the end of bars {where}."

    def tool_play_only(self, part: str, section: str, bars: str) -> str:
        p, s = self.part(part), self.section(section)
        stash = self.extra.setdefault("stash", {}).setdefault(p["id"], {})
        written = stash.get(s["id"])
        if written is None:
            written = list(self.notes.get(p["id"], {}).get(s["id"], []))
        word = str(bars).strip().lower()
        bar_ticks = self.steps * sw.TICKS_PER_STEP
        if word == "all":
            kept = written
            stash.pop(s["id"], None)
        else:
            stash[s["id"]] = written
            if word == "none":
                kept = []
            else:
                first, last = bar_range(bars, int(s["bars"]))
                kept = [n for n in written if first * bar_ticks <= int(n["start"]) < (last + 1) * bar_ticks]
        self.notes.setdefault(p["id"], {})[s["id"]] = kept
        return (
            f"{p['id']} in {s['id']}: "
            + ("as written" if word == "all" else "rests" if word == "none" else f"bars {bars}")
            + f" ({len(kept)} notes)."
        )

    def energies(self) -> dict[str, float]:
        """Each section's energy, 0 to 1 against the song's busiest: notes a bar, parts playing, velocity."""
        raw = {}
        for sid, s in self.sections.items():
            bars = max(1, int(s["bars"]))
            playing = [ns for by in self.notes.values() if (ns := by.get(sid))]
            count = sum(len(ns) for ns in playing)
            loud = sum(float(n["velocity"]) for ns in playing for n in ns) / max(1, count)
            raw[sid] = (count / bars) * 0.4 + len(playing) * 2.0 + loud * 4.0 if playing else 0.0
        top = max(raw.values(), default=0.0) or 1.0
        return {sid: round(v / top, 2) for sid, v in raw.items()}

    def reference_energy(self) -> list[float]:
        """The references' sections from start to end, as their level (0 to 1 against each song's loudest), at
        five points of a song."""
        curves = []
        for song in self.setup.summary.get("perSong") or []:
            sections = [x for x in song.get("sections") or [] if isinstance(x.get("level"), int | float)]
            if len(sections) < 2:
                continue
            levels = [float(x["level"]) for x in sections]
            low, high = min(levels), max(levels)
            spread = (high - low) or 1.0
            total = sum(int(x.get("bars") or 1) for x in sections)
            curve, at = [], 0
            for x in sections:
                curve.append(((at + int(x.get("bars") or 1) / 2) / total, (float(x["level"]) - low) / spread))
                at += int(x.get("bars") or 1)
            curves.append([min(curve, key=lambda c: abs(c[0] - t))[1] for t in (0.1, 0.3, 0.5, 0.7, 0.9)])
        if not curves:
            return []
        return [round(sum(c[i] for c in curves) / len(curves), 2) for i in range(5)]

    def tool_energy(self) -> str:
        energy = self.energies()
        total = sum(int(self.sections[s]["bars"]) for s in self.order)
        lines, at = [], 0
        for sid in self.order:
            bars = int(self.sections[sid]["bars"])
            lines.append(f"bars {at + 1}-{at + bars}, {sid}: {energy.get(sid, 0):.2f}")
            at += bars
        reference = self.reference_energy()
        if reference:
            points = ", ".join(
                f"{round(100 * t)} %: {v:.2f}" for t, v in zip((0.1, 0.3, 0.5, 0.7, 0.9), reference, strict=True)
            )
            lines.append(f"The reference songs' sections (0 to 1, through the song): {points}.")
        else:
            lines.append("No reference song has its sections measured: the energies of the kinds are " + ", ".join(
                f"{k} {v:g}" for k, v in sw.ENERGY.items()) + ".")  # fmt: skip
        lines += [f"Song: {total} bars.", *self.energy_issues()]
        return "\n".join(lines)

    def energy_issues(self) -> list[str]:
        energy = self.energies()
        out = []
        by_kind: dict[str, list[float]] = {}
        for sid, s in self.sections.items():
            by_kind.setdefault(str(s["kind"]), []).append(energy.get(sid, 0.0))
        chorus, verse = max(by_kind.get("chorus", [0.0])), max(by_kind.get("verse", [0.0]))
        if "chorus" in by_kind and "verse" in by_kind and chorus <= verse:
            out.append(f"energy: the chorus ({chorus:.2f}) is not above the verse ({verse:.2f}): add layers or notes")
        values = [energy.get(sid, 0.0) for sid in self.order]
        if len(values) >= 3 and max(values) - min(values) < 0.2:
            out.append("energy: every section is about as full as the others (let layers enter and leave)")
        return out

    def issues(self, part: str = "", section: str = "") -> list[str]:
        """The checks, as lines with locations (worst first)."""
        only_part = self.part(part)["id"] if part else ""
        only_section = self.section(section)["id"] if section else ""
        style, scale = self.setup.style, sw.scale_of(self.key)
        bar_ticks = self.steps * sw.TICKS_PER_STEP
        beat = bar_ticks // max(1, style.beats_per_bar)
        out: list[str] = []
        total_bars = sum(self.sections[s]["bars"] for s in self.order)
        medians: dict[str, dict[str, int]] = {}
        for p in self.setup.written():
            pid = p["id"]
            if only_part and pid != only_part:
                continue
            by = self.notes.get(pid, {})
            if not any(by.get(s) for s in self.order):
                out.append(f"{pid}: no notes anywhere (write it, or rules_write it)")
                continue
            drums = self.drums(pid)
            base = sw.ROLES[p["role"]][2]
            for sid in self.sections:
                if only_section and sid != only_section:
                    continue
                notes = by.get(sid, [])
                if not notes or drums:
                    continue
                outside, rub = [], []
                for n in notes:
                    bar = int(n["start"]) // bar_ticks
                    where = f"bar {bar + 1} step {(int(n['start']) % bar_ticks) // sw.TICKS_PER_STEP}"
                    tones = self.chord_at(sid, min(bar, int(self.sections[sid]["bars"]) - 1))
                    if int(n["pitch"]) % 12 not in scale and int(n["pitch"]) % 12 not in tones:
                        outside.append(f"{sw.note_name(int(n['pitch']))} at {where}")
                    elif base in ("bass", "chords") and int(n["start"]) % beat == 0:
                        fits = sw.allowed_pitch_classes(
                            self.chord_at(sid, min(bar, int(self.sections[sid]["bars"]) - 1)), scale
                        )
                        if int(n["pitch"]) % 12 not in fits:
                            rub.append(f"{sw.note_name(int(n['pitch']))} at {where}")
                if outside:
                    out.append(
                        f"{pid}, {sid}: {len(outside)} notes outside {self.key['name']} and its chord: "
                        + ", ".join(outside[:3])
                        + (" ..." if len(outside) > 3 else "")
                    )
                if len(rub) > max(1, len(notes) // 10):
                    out.append(
                        f"{pid}, {sid}: {len(rub)} notes on the beat rub against the chord: "
                        + ", ".join(rub[:3])
                        + (" ..." if len(rub) > 3 else "")
                    )
                pitches = sorted(int(n["pitch"]) for n in notes)
                medians.setdefault(sid, {})[pid] = pitches[len(pitches) // 2]
                # Repetition: one bar played over and over in a long section, for a lead.
                bars_seen = Counter(
                    tuple((int(n["start"]) % bar_ticks, n["pitch"]) for n in notes if int(n["start"]) // bar_ticks == b)
                    for b in range(int(self.sections[sid]["bars"]))
                )
                if base == "lead" and int(self.sections[sid]["bars"]) >= 8 and len(bars_seen) <= 1:
                    out.append(f"{pid}, {sid}: the same bar {self.sections[sid]['bars']} times (vary the phrase)")
            if not only_section:
                played = sum(len(by.get(s, [])) for s in self.order)
                per_bar = played / max(1, total_bars)
                if drums and p["role"] == "drums":
                    want = float(((self.setup.summary.get("parts") or {}).get("drums") or {}).get("notesPerBar") or 0)
                elif not drums and base in sw.SUMMARY_PART:
                    want = style.parts[sw.SUMMARY_PART[base]].per_bar
                else:
                    want = 0.0
                if want and not 0.4 <= per_bar / want <= 2.5:
                    out.append(f"{pid}: {per_bar:.1f} notes a bar over the song; the references' play about {want:g}")
        for sid, heights in medians.items():
            ids = sorted(heights)
            for i, a in enumerate(ids):
                for b in ids[i + 1 :]:
                    if abs(heights[a] - heights[b]) <= 2:
                        out.append(
                            f"{a} and {b}, {sid}: the same register (around {sw.note_name(heights[a])}): move one "
                            "an octave, or give them different rhythms"
                        )
        if not only_part and not only_section and style.has_drums:
            drum_parts = [p["id"] for p in self.setup.written() if p["role"] == "drums" and self.notes.get(p["id"])]
            if drum_parts:
                song = self.song()
                groove = (sw.compare(song, self.setup.summary, style).get("groove")) or {}
                low = {lane: v for lane, v in groove.items() if v < 0.6}
                if low:
                    out.append(
                        "drums: the groove is unlike the references' in "
                        + ", ".join(f"{lane} ({round(100 * v)} % alike)" for lane, v in low.items())
                    )
        if not only_part:
            lengths = sorted({b for b, _ in style.lengths})
            for sid, s in self.sections.items():
                if (not only_section or sid == only_section) and int(s["bars"]) % 4:
                    out.append(f"{sid}: {s['bars']} bars, off the 4-bar phrases (the style's: {lengths})")
        if not only_part and not only_section and "arrangement" in self.groups:
            out += self.energy_issues()
        return out

    def tool_check(self, part: str = "", section: str = "") -> str:
        found = self.issues(part, section)
        if not found:
            return "OK: no issues."
        shown = found[:MOST_LINES]
        more = len(found) - len(shown)
        return (
            f"{len(found)} issue{'s' if len(found) > 1 else ''}:\n"
            + "\n".join(shown)
            + (f"\n... and {more} more" if more else "")
        )

    def tool_measure(self, section: str = "") -> str:
        only = self.section(section)["id"] if section else ""
        lines = []
        if self.levels or self.problems:
            heard = []
            for p in self.setup.written():
                if p["id"] in self.problems:
                    heard.append(f"{p['id']} could not be played ({self.problems[p['id']]})")
                elif p["id"] in self.levels:
                    db = self.levels[p["id"]]
                    heard.append(
                        f"{p['id']} " + ("made no sound" if db is None or db <= -90 else f"{float(db):.1f} dB")
                    )
            lines.append("As Spinus played each part with its sound last time: " + "; ".join(heard) + ".")
        else:
            lines.append("Not played yet: after finish, Spinus plays each part with its sound and sets its level.")
        weights = {}
        for p in self.setup.written():
            notes = [n for sid, ns in self.notes.get(p["id"], {}).items() if not only or sid == only for n in ns]
            if notes:
                weights[p["id"]] = sum(
                    float(n["velocity"]) ** 2 * min(4, int(n["length"]) // sw.TICKS_PER_STEP + 1) for n in notes
                )
        if weights:
            top = max(weights.values())
            lines.append(
                "Balance from the notes (velocity and length, loudest 0 dB): "
                + ", ".join(
                    f"{pid} {10 * math.log10(w / top):.0f} dB"
                    for pid, w in sorted(weights.items(), key=lambda kv: -kv[1])
                )
            )
        return "\n".join(lines)

    def tool_finish(self, summary: str) -> str:
        self.summary = str(summary)[:400]
        self.finished = True
        left = [p["id"] for p in self.setup.written() if not any(self.notes.get(p["id"], {}).values())]
        found = self.issues()
        words = "Finished: the song goes to the user."
        if left:
            words += f" Spinus's rules write {', '.join(left)}."
        if found:
            words += f" {len(found)} issues remain."
        return words


# ---------------------------------------------------------------------------------------------------
# The loop


def describe_call(name: str, args: dict[str, Any], result: str, failed: bool) -> str:
    """What a tool call did, in plain words, for the progress line."""
    part, section = args.get("part", ""), args.get("section", "")
    words = {
        "brief": "Reading the brief",
        "form": "Looking at the form",
        "read": f"Reading the {part} in the {section}",
        "list_sounds": f"Looking for sounds for the {part}",
        "set_form": "Setting the form",
        "set_chords": f"Setting the {section}'s chords",
        "write": f"Writing the {part} for the {section}",
        "edit": f"Fixing bars {args.get('bars', '')} of the {part} in the {section}",
        "motif": f"Making the motif '{args.get('name', '')}'",
        "place": f"Placing '{args.get('motif', '')}' in the {part}, {section}",
        "copy": f"Copying the {args.get('from_section', '')} to the {args.get('to_section', '')}",
        "pick_sound": f"Choosing the {part}'s sound",
        "rules_write": f"Letting the rules write the {part}",
        "check": "Checking" + (f" the {part}" if part else "") + (f" in the {section}" if section else ""),
        "measure": "Measuring the balance",
        "finish": "Finishing",
    }.get(name, name)
    if failed:
        return f"{words}: not done ({result[:80]})"
    if name == "check":
        return f"{words}: {result.splitlines()[0].rstrip(':')}"
    return words


@dataclass
class Run:
    """A harness session: the working copy, the best checked version and what it used."""

    workspace: Workspace
    best: Workspace | None = None
    best_issues: int = 1 << 30
    requests: list[dict[str, int]] = field(default_factory=list)
    by: str = ""
    model: str = ""
    turns: int = 0
    stopped: bool = False  # the user said stop now
    notices: list[str] = field(default_factory=list)  # what the provider changed (a reasoning level it lacks)

    def remember_if_best(self) -> None:
        count = len(self.workspace.issues())
        if count <= self.best_issues:
            self.best, self.best_issues = self.workspace.copy(), count


def first_message(ws: Workspace, revising: str = "") -> str:
    allowed = "; ".join(f"{GROUPS[g][0]}: {GROUPS[g][1]}" for g in ws.groups)
    text = "The brief:\n" + ws.tool_brief() + "\n\nThe form to start from (change it with set_form):\n" + ws.tool_form()
    text += "\n\nThe tools you may use: " + allowed + "."
    if revising:
        text += "\n\n" + revising
    else:
        text += "\n\nWrite the song with the tools, check it, fix what check finds, then finish."
    return text


async def session(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routes: list[Route],
    usage_log: UsageLog,
    run: Run,
    effort: str,
    say: Say,
    stop: Callable[[], bool],
    revising: str = "",
    spent: int = 0,
) -> Run:
    """The model works on run.workspace with the tools until it calls finish (or stop, or MOST_TURNS)."""
    ws = run.workspace
    request = Request(
        routes[0].model,
        system_for(ws.groups),
        [Message("user", [Text(first_message(ws, revising))])],
        tools_for(ws.groups),
        None,
        effort,
        cache=True,
        context_edits=context_edits(CLEAR_AFTER),
    )

    async def on_turn(completion: Completion, route: Route) -> None:
        usage_log.add(route.connection.id, compose.FEATURE, completion.usage)
        run.by, run.model = answered_by(completion.model, []), route.model
        if completion.notice and completion.notice not in run.notices:
            run.notices.append(completion.notice)
        run.requests.append(
            {
                "input": completion.usage.input_tokens,
                "output": completion.usage.output_tokens,
                "reasoning": completion.usage.reasoning_tokens,
                "cached": completion.usage.cached_tokens,
                "cleared": completion.usage.cleared_tokens,
            }
        )

    async def call(use: ToolUse) -> tuple[str, bool]:
        try:
            if use.partial:
                raise Mistake("the call was cut off at your length limit: make it smaller (fewer bars, or a loop)")
            result, failed = ws.call(use.name, dict(use.input)), False
        except Mistake as why:
            result, failed = f"Not done: {why}.", True
        except TypeError as why:  # arguments the tool does not take
            result, failed = f"Not done: {str(why).split(')', 1)[-1].strip() or why}.", True
        if use.name == "check" and not failed:
            run.remember_if_best()
        used = spent + sum(r["input"] + r["output"] for r in run.requests)
        await say(
            {
                "kind": "progress",
                "done": 0,
                "total": 1,
                "step": f"{describe_call(use.name, use.input, result, failed)} ({used:,} tokens so far)",
            }
        )
        return result, failed

    async def no_text(_: str) -> None:
        pass

    def done() -> bool:
        if stop():
            run.stopped = True
        return ws.finished or run.stopped

    run.turns = await tool_loop(client, connections, routes, request, call, on_turn, no_text, say, MOST_TURNS, done)
    return run


def result_song(
    run: Run, setup: compose.Setup, effort: str, round_number: int, before: list[dict[str, Any]]
) -> dict[str, Any]:
    """The song the session gives: the finished working copy, else the best checked version (else as it is)."""
    ws = run.workspace
    note = ""
    if not ws.finished:
        run.remember_if_best()
        assert run.best is not None
        ws = run.best
        why = (
            "you stopped it"
            if run.stopped
            else (
                f"it did not finish in {MOST_TURNS} turns"
                if run.turns >= MOST_TURNS
                else "it stopped without finishing"
            )
        )
        note = f" The AI's best checked version, as {why}."
    song = ws.song()
    requests = before + [{"round": round_number, **r} for r in run.requests]
    usage = compose.usage_of(requests)
    usage["cached"] = compose.total_of(requests, "cached")
    usage["cleared"] = compose.total_of(requests, "cleared")
    filled = [p["id"] for p in setup.written() if not any(ws.notes.get(p["id"], {}).values())]
    composed = {
        "by": run.by,
        "effort": effort,
        "harness": True,
        "tools": list(ws.groups),
        "round": round_number,
        "turns": run.turns,
        "calls": dict(ws.calls),
        "usage": usage,
        "sounds": dict(ws.sounds),
        "filled": filled,
        "byRules": sorted({p for p, _ in ws.by_rules}),
        "reason": ws.summary,
        "findings": ws.issues(),
        "workspace": ws.export(),
        "answer": {"parts": {p: {} for p in ws.notes}},  # what the notes say "left out" against
    }
    song["note"] = (
        compose.written_note(setup, composed)
        + f" Tools: {depth_words(ws.groups)}."
        + note
        + "".join(f" {n}" for n in run.notices)
    )
    if usage.get("cached"):
        song["note"] += f" {int(usage['cached']):,} of the input tokens were read from the provider's cache."
    return compose.finish(song, setup, composed)


async def write(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routing: Routing,
    usage_log: UsageLog,
    setup: compose.Setup,
    seed: int,
    size: str,
    effort: str,
    say: Say,
    stop: Callable[[], bool] = lambda: False,
    song: dict[str, Any] | None = None,
    levels: Any = None,
    problems: Any = None,
    groups: tuple[str, ...] = DEFAULT_DEPTH,
) -> dict[str, Any]:
    """A song written through the harness with the groups of tools allowed; with `song` (one it wrote) and how its
    parts played (levels, problems), a revision of it (with the groups it was written with). NoConnection without
    a connection."""
    routes = routing.resolve(compose.FEATURE)
    if not routes:
        raise NoConnection("No AI connection is set up.")
    revising, round_number, before, spent = "", 0, [], 0
    if song is not None:
        ws = Workspace.from_song(setup, song)
        composed = song.get("composed") or {}
        ws.groups = check_groups(composed.get("tools") or list(groups))
        round_number = int(composed.get("round", 0)) + 1
        before = list((composed.get("usage") or {}).get("requests") or [])
        spent = compose.total_of(before, "input") + compose.total_of(before, "output")
        ws.levels = {str(k): v for k, v in (levels or {}).items()} if isinstance(levels, dict) else {}
        ws.problems = {str(k): str(v) for k, v in (problems or {}).items()} if isinstance(problems, dict) else {}
        found = ws.issues()
        revising = (
            "Spinus played each part with its sound. "
            + ws.tool_measure()
            + "\n\nThe checks now:\n"
            + (ws.tool_check() if found else "OK: no issues.")
            + "\n\nFix what needs fixing with small edits (a part that made no sound may sit outside its "
            "instrument's range), check, then finish."
        )
        await say({"kind": "status", "text": f"Revising: the AI is fixing what Spinus heard (round {round_number})..."})
    else:
        ws = Workspace.start(setup, seed, size)
        ws.groups = groups
        await say({"kind": "status", "text": "Writing: the AI is composing with the harness..."})
    run = Run(ws)
    await session(client, connections, routes, usage_log, run, effort, say, stop, revising, spent)
    return result_song(run, setup, effort, round_number, before)


# ---------------------------------------------------------------------------------------------------
# The checks' score, for comparing ways of writing


def score(song: dict[str, Any], setup: compose.Setup) -> dict[str, Any]:
    """How the song fares on Spinus's checks: the issues found, and compare's numbers."""
    ws = Workspace(
        setup, int(song["seeds"]["song"]), str(song.get("size", "sketch")), float(song["tempo"]), dict(song["key"])
    )
    for s in song["sections"]:
        hold = 2 if len(s["chords"]) >= 2 and s["chords"][0]["roman"] == s["chords"][1]["roman"] else 1
        ws.sections[s["id"]] = {
            "id": s["id"],
            "kind": s["kind"],
            "bars": int(s["bars"]),
            "hold": hold,
            "chords": [c["roman"] for c in s["chords"][::hold]][:8],
            "energy": s["energy"],
        }
    ws.order = [e["section"] for e in song["arrangement"]]
    ws.notes = {
        pid: {sid: list(ns) for sid, ns in by.items()}
        for pid, by in song["layerParts"].items()
        if any(p["id"] == pid for p in setup.written())
    }
    checks = song.get("checks") or sw.compare(song, setup.summary, setup.style)
    groove = checks.get("groove") or {}
    return {
        "issues": len(ws.issues()),
        "notesInKey": checks.get("notesInKey"),
        "notesFitChords": checks.get("notesFitChords"),
        "groove": round(sum(groove.values()) / len(groove), 3) if groove else None,
    }


# ---------------------------------------------------------------------------------------------------
# Outside Spinus: a brief file, an MCP server over stdio, and the comparison with the one-shot writer


def make_brief(text: str, parts: list[str], seed: int, size: str) -> dict[str, Any]:
    """A brief from a description (read by Spinus's offline rules) and parts written id:role."""
    from .describe import describe

    listed = []
    for item in parts:
        pid, _, role = item.partition(":")
        listed.append({"id": pid, "name": sw.ROLES.get(role or pid, (pid,))[0], "role": role or pid})
    return {
        "kind": "spinus.harness-brief",
        "summary": describe(text),
        "parts": listed,
        "seed": seed,
        "arrangement": size,
        "candidates": {},
        "current": {},
    }


def setup_of(brief: dict[str, Any]) -> compose.Setup:
    return compose.Setup.make(
        brief["summary"],
        brief.get("parts"),
        str(brief.get("arrangement") or "sketch"),
        brief.get("candidates"),
        brief.get("current"),
    )


def stdout_line(text: str) -> None:
    sys.stdout.write(text)
    sys.stdout.flush()


def serve_mcp(
    brief: dict[str, Any],
    out: Path,
    read: Callable[[], str] = sys.stdin.readline,
    write: Callable[[str], None] = stdout_line,
    groups: tuple[str, ...] = GROUP_ORDER,
) -> None:
    """The harness's tools (the groups allowed) as an MCP server (JSON-RPC over stdio, one message a line), on a
    working copy made from the brief; finish writes the song to `out`."""
    setup = setup_of(brief)
    ws = Workspace.start(setup, int(brief.get("seed", 0)), str(brief.get("arrangement") or "sketch"))
    ws.groups = groups

    def reply(id_: Any, result: Any = None, error: dict[str, Any] | None = None) -> None:
        message: dict[str, Any] = {"jsonrpc": "2.0", "id": id_}
        message["error" if error else "result"] = error or result
        write(json.dumps(message) + "\n")

    while True:
        line = read()
        if not line:
            return
        try:
            message = json.loads(line)
        except ValueError:
            continue
        method, id_ = message.get("method"), message.get("id")
        if id_ is None:
            continue  # a notification
        if method == "initialize":
            reply(
                id_,
                {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "spinus-music-harness", "version": "1"},
                    "instructions": system_for(groups) + "\n\n" + first_message(ws),
                },
            )
        elif method == "tools/list":
            reply(
                id_,
                {
                    "tools": [
                        {"name": t.name, "description": t.description, "inputSchema": t.input_schema}
                        for t in tools_for(groups)
                    ]
                },
            )
        elif method == "tools/call":
            params = message.get("params") or {}
            try:
                text, failed = ws.call(str(params.get("name")), dict(params.get("arguments") or {})), False
            except (Mistake, TypeError) as why:
                text, failed = f"Not done: {why}.", True
            if params.get("name") == "finish" and not failed:
                out.write_text(json.dumps(result_song(Run(ws, turns=0), setup, "", 0, []), indent=1), encoding="utf-8")
                text += f" Saved to {out}."
            reply(id_, {"content": [{"type": "text", "text": text}], "isError": failed})
        elif method == "ping":
            reply(id_, {})
        else:
            reply(id_, error={"code": -32601, "message": f"Unknown method {method}"})


class FakeModel:
    """A scripted model for the comparison and the tests (no API call): Anthropic's streaming answers, the
    one-shot writer's write_song call or the harness's tool calls turn by turn. Its token counts are the request's
    characters over four, after the request's context edits as Anthropic documents clear_tool_uses_20250919 (past the
    trigger, the oldest tool calls but `keep` cleared, if that clears at least `clear_at_least`). It does not
    simulate the provider's cache."""

    def __init__(self, harness_script: list[list[tuple[str, dict[str, Any]]]], one_shot: dict[str, Any]) -> None:
        self.script, self.one_shot = harness_script, one_shot

    def __call__(self, request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content)
        if request.url.path.endswith("/count_tokens"):
            return httpx.Response(200, json={"input_tokens": len(json.dumps(body)) // 4})
        names = [t["name"] for t in body.get("tools", [])]
        size_in, cleared = cleared_size(body)
        turn = 0
        if "write_song" in names:
            calls = [("write_song", self.one_shot)]
        else:
            turn = sum(1 for m in body["messages"] if m["role"] == "assistant")
            calls = self.script[turn] if turn < len(self.script) else []
        events: list[dict[str, Any]] = [
            {
                "type": "message_start",
                "message": {"model": "fake-model", "usage": {"input_tokens": size_in, "cache_read_input_tokens": 0}},
            }
        ]
        out = 0
        for i, (name, args) in enumerate(calls):
            text = json.dumps(args)
            out += len(text) // 4 + 10
            events += [
                {
                    "type": "content_block_start",
                    "index": i,
                    "content_block": {"type": "tool_use", "id": f"t{turn}_{i}", "name": name},
                },
                {
                    "type": "content_block_delta",
                    "index": i,
                    "delta": {"type": "input_json_delta", "partial_json": text},
                },
                {"type": "content_block_stop", "index": i},
            ]
        if not calls:
            events += [
                {"type": "content_block_start", "index": 0, "content_block": {"type": "text"}},
                {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "Done."}},
                {"type": "content_block_stop", "index": 0},
            ]
            out = 3
        report = {"applied_edits": [{"type": "clear_tool_uses_20250919", "cleared_input_tokens": cleared}]}
        events.append(
            {
                "type": "message_delta",
                "context_management": report if cleared else {},
                "delta": {"stop_reason": "tool_use" if calls else "end_turn"},
                "usage": {"output_tokens": out},
            }
        )
        data = "".join(f"event: {e['type']}\ndata: {json.dumps(e)}\n\n" for e in events).encode()
        return httpx.Response(200, content=data, headers={"content-type": "text/event-stream"})


def cleared_size(body: dict[str, Any]) -> tuple[int, int]:
    """A request's size in tokens (characters over four) after its clear_tool_uses edit, and what that cleared."""
    messages = json.loads(json.dumps(body.get("messages", [])))

    def size() -> int:
        return len(json.dumps({**body, "messages": messages})) // 4

    total = size()
    for edit in (body.get("context_management") or {}).get("edits", []):
        if edit.get("type") != "clear_tool_uses_20250919" or total <= int(edit["trigger"]["value"]):
            continue
        calls = [
            block
            for m in messages
            if m["role"] == "assistant" and isinstance(m["content"], list)
            for block in m["content"]
            if block.get("type") == "tool_use" and block.get("name") not in edit.get("exclude_tools", [])
        ]
        keep = int(edit.get("keep", {}).get("value", 3))
        old = calls[: max(0, len(calls) - keep)]
        before = json.dumps(messages)
        ids = {block["id"] for block in old}
        for block in old:
            if edit.get("clear_tool_inputs"):
                block["input"] = {}
        for m in messages:
            for block in m["content"] if isinstance(m["content"], list) else []:
                if block.get("type") == "tool_result" and block.get("tool_use_id") in ids:
                    block["content"] = "[cleared]"
        after = size()
        if total - after >= int((edit.get("clear_at_least") or {}).get("value", 0)):
            return after, total - after
        messages = json.loads(before)
    return total, 0


def fake_runs(setup: compose.Setup, ws: Workspace) -> FakeModel:
    """The fake model's two ways of writing the same plain song: drums, bass and a lead by hand (a loop, a motif
    placed and copied), the rest by the rules; and the one-shot writer's whole answer for it."""
    parts = [p for p in setup.written()]
    first = next(iter(ws.sections))
    beat = "0 kick 1 110; 4 snare 1 100; 8 kick 1 105; 12 snare 1 100; " + "; ".join(
        f"{s} hat 1 70" for s in range(0, 16, 2)
    )
    tonic = 36 + int(ws.key["tonic"])
    script: list[list[tuple[str, dict[str, Any]]]] = [[("form", {})]]
    one_shot_parts: dict[str, Any] = {}
    for p in parts:
        role = p["role"]
        if sw.ROLES[role][4]:
            turn = [("write", {"part": p["id"], "section": sid, "notes": beat, "loop_bars": 1}) for sid in ws.sections]
            one_shot_parts[p["id"]] = {"sections": {sid: {"bars": 1, "notes": beat} for sid in ws.sections}}
        elif sw.ROLES[role][2] == "bass":
            riff = f"0 {tonic} 4 100; 4 {tonic} 2 90; 8 {tonic + 7} 4 90; 12 {tonic + 12} 2 80"
            turn = [("motif", {"name": "riff", "notes": riff, "bars": 1})] + [
                (
                    "place",
                    {
                        "part": p["id"],
                        "section": sid,
                        "motif": "riff",
                        "bar": 1,
                        "times": ws.sections[sid]["bars"],
                        "fit_chords": True,
                    },
                )
                for sid in ws.sections
            ]
            one_shot_parts[p["id"]] = {"sections": {sid: {"bars": 1, "notes": riff} for sid in ws.sections}}
        else:
            turn = [("rules_write", {"part": p["id"]})]
            song = compose.build_song(compose.Checked(ws.plan(), {}, {}, {}, [], {}), setup, ws.seed, ws.size)
            drums = sw.ROLES[role][4]
            one_shot_parts[p["id"]] = {
                "sections": {
                    sid: {"bars": ws.sections[sid]["bars"], "notes": compose.as_notation(ns, drums)}
                    for sid, ns in song["layerParts"].get(p["id"], {}).items()
                }
            }
        script.append(turn)
    script += [
        [("check", {})],
        [("copy", {"from_section": first, "to_section": first})],
        [("finish", {"summary": "A plain test song."})],
    ]
    one_shot = {
        "tempo": ws.tempo,
        "key": ws.key["name"],
        "order": ws.order,
        "parts": one_shot_parts,
        "sections": [
            {
                "id": s["id"],
                "kind": s["kind"],
                "bars": s["bars"],
                "chords": s["chords"],
                "barsPerChord": s["hold"],
                "energy": s["energy"],
            }
            for s in ws.sections.values()
        ],
    }
    return FakeModel(script, one_shot)


async def compare_ways(
    brief: dict[str, Any],
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routing: Routing,
    folder: Path,
    efforts: list[str],
    save: Path | None = None,
    label: str = "",
    depths: list[tuple[str, ...]] | None = None,
) -> list[dict[str, Any]]:
    """The same brief written one-shot (compose) and through the harness, at each reasoning level and with each
    depth of tools: the tokens each used, as the provider reported them, and the checks' score of each song. With
    `save`, each song is written there as <label>-<way>-<effort>.song.json (with several depths, the way is
    harness-<groups>; a slash in the label, a model's name, becomes _)."""
    depths = depths or [DEFAULT_DEPTH]
    setup = setup_of(brief)
    seed, size = int(brief.get("seed", 0)), str(brief.get("arrangement") or "sketch")
    usage_log = UsageLog(folder)

    async def quiet(_: dict[str, Any]) -> None:
        pass

    rows = []
    for effort in efforts:
        runs: list[tuple[str, tuple[str, ...]]] = [("one-shot", ())] + [("harness", d) for d in depths]
        for way, groups in runs:
            if way == "one-shot":
                song = await compose.compose(client, connections, routing, usage_log, setup, seed, size, effort, quiet)
                usage = song.get("composed", {}).get("usage") or song.get("usage") or {}
            else:
                song = await write(
                    client, connections, routing, usage_log, setup, seed, size, effort, quiet, groups=groups
                )
                usage = song["composed"]["usage"]
            if save is not None:
                save.mkdir(parents=True, exist_ok=True)
                named = way if len(depths) == 1 or not groups else f"{way}-{'+'.join(groups)}"
                name = "-".join(x for x in (label, named, effort or "default") if x).replace("/", "_")
                (save / f"{name}.song.json").write_text(json.dumps(song, indent=1), encoding="utf-8")
            rows.append(
                {
                    "way": way,
                    "tools": depth_words(groups) if groups else "-",
                    "effort": effort or "default",
                    "requests": len(usage.get("requests", [])),
                    "input": usage.get("input", 0),
                    "cached": usage.get("cached", 0),
                    "cleared": usage.get("cleared", 0),
                    "output": usage.get("output", 0),
                    "reasoning": usage.get("reasoning", 0),
                    "total": int(usage.get("input", 0)) + int(usage.get("output", 0)),
                    "usedAi": bool(song.get("composedBy")),
                    **score(song, setup),
                }
            )
    return rows


def table(rows: list[dict[str, Any]]) -> str:
    keys = [
        "way",
        "tools",
        "effort",
        "requests",
        "input",
        "cached",
        "cleared",
        "output",
        "reasoning",
        "total",
        "issues",
        "notesInKey",
        "notesFitChords",
        "groove",
        "usedAi",
    ]
    widths = {k: max(len(k), *(len(str(r.get(k))) for r in rows)) for k in keys}
    lines = ["  ".join(k.ljust(widths[k]) for k in keys)]
    lines += ["  ".join(str(r.get(k)).ljust(widths[k]) for k in keys) for r in rows]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(prog="python -m spinus_ai.harness", description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    b = sub.add_parser("brief", help="write a brief from a description")
    b.add_argument("--describe", required=True)
    b.add_argument("--parts", default="drums:drums,bass:bass,chords:chords,lead:lead", help="id:role,...")
    b.add_argument("--seed", type=int, default=1)
    b.add_argument("--arrangement", default="sketch", choices=sw.SIZES)
    b.add_argument("-o", "--out", type=Path, required=True)
    m = sub.add_parser("mcp", help="serve the harness's tools over stdio (MCP) on a brief")
    m.add_argument("--brief", type=Path, required=True)
    m.add_argument("--out", type=Path, required=True, help="where finish saves the song")
    m.add_argument(
        "--tools",
        default="full",
        help="the groups of tools: a depth (basic, arrangement, transitions, full) or groups, e.g. basic,arrangement",
    )
    e = sub.add_parser("eval", help="compare one-shot and harness writing on tokens and the checks' score")
    e.add_argument("--brief", type=Path, required=True)
    e.add_argument("--fake", action="store_true", help="a scripted model, no API call (checks the comparison)")
    e.add_argument("--provider", default="anthropic", choices=("anthropic", "openai", "openrouter", "local"))
    e.add_argument("--base-url")
    e.add_argument("--model", default="")
    e.add_argument("--key-env", default="ANTHROPIC_API_KEY", help="the environment variable holding the key")
    e.add_argument(
        "--clear-after",
        type=int,
        default=CLEAR_AFTER,
        help=f"input tokens after which old tool calls are cleared (Anthropic; 0: never; default {CLEAR_AFTER})",
    )
    e.add_argument("--save", type=Path, help="a folder to save each song in (<model>-<way>-<effort>.song.json)")
    e.add_argument("--efforts", default="", help="reasoning levels to compare, e.g. ',low,medium,high' ('' default)")
    e.add_argument(
        "--tools",
        default="arrangement",
        help="the depths of tools to compare, separated by ';': each a "
        "depth (basic, arrangement, transitions, full) or groups, e.g. 'basic;basic,arrangement;full'",
    )
    args = parser.parse_args(argv)

    if args.command == "brief":
        brief = make_brief(args.describe, [x for x in args.parts.split(",") if x], args.seed, args.arrangement)
        args.out.write_text(json.dumps(brief, indent=1), encoding="utf-8")
        print(f"Brief saved to {args.out}: {', '.join(p['id'] for p in brief['parts'])}.")
    elif args.command == "mcp":
        serve_mcp(json.loads(args.brief.read_text(encoding="utf-8")), args.out, groups=check_groups(args.tools))
    else:
        asyncio.run(run_eval(args))


async def run_eval(args: argparse.Namespace) -> None:
    import os

    global CLEAR_AFTER
    CLEAR_AFTER = args.clear_after

    from .connections import MemoryKeyStore

    brief = json.loads(args.brief.read_text(encoding="utf-8"))
    efforts = args.efforts.split(",") if args.efforts else [""]
    with tempfile.TemporaryDirectory() as folder:
        store = ConnectionStore(Path(folder), MemoryKeyStore())
        if args.fake:
            setup = setup_of(brief)
            ws = Workspace.start(setup, int(brief.get("seed", 0)), str(brief.get("arrangement") or "sketch"))
            client = httpx.AsyncClient(transport=httpx.MockTransport(fake_runs(setup, ws)))
            store.upsert("Fake", "anthropic", "https://fake.test", "fake-key")
        else:
            key = os.environ.get(args.key_env, "")
            if not key and args.provider != "local":
                raise SystemExit(f"Set {args.key_env} to your key first (it is used for this run only, never saved).")
            client = httpx.AsyncClient(timeout=httpx.Timeout(600.0, connect=20.0))
            store.upsert("Owner check", args.provider, args.base_url, key or None)
        routing = Routing(Path(folder), store)
        if args.model:
            routing.set(compose.FEATURE, [{"connection": store.connections[0].id, "model": args.model}])
        async with client:
            rows = await compare_ways(
                brief,
                client,
                store,
                routing,
                Path(folder),
                efforts,
                args.save,
                args.model or args.provider,
                [check_groups(d) for d in args.tools.split(";") if d.strip()],
            )
    print(table(rows))
    if args.fake:
        print(
            "\nA scripted model: this shows the comparison works. Its tokens are the requests' characters over four, "
            "after clearing as Anthropic documents it; it does not simulate the provider's cache."
        )


if __name__ == "__main__":
    main()
