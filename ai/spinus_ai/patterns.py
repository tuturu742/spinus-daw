"""The pattern generator and adjuster.

An LLM writes notes with the write_pattern tool, in sixteenth steps and note names; Spinus then checks
them and makes them musical: snapped to the song's key, kept on real drum pads, inside the pattern, on
the grid, with optional swing and humanizing. The result is a suggestion the user accepts or discards
(AGENTS rule 3).
"""

from __future__ import annotations

import random
import re
from dataclasses import dataclass, field
from typing import Any

import httpx

from .assistant import NoConnection, Say, complete_with_fallback
from .connections import ConnectionStore
from .gateway import Message, Request, Text, Tool
from .routing import Routing
from .usage import UsageLog

TICKS_PER_STEP = 240  # a sixteenth at 960 per beat
MAX_VARIATIONS = 4

SCALES: dict[str, tuple[int, ...]] = {
    "major": (0, 2, 4, 5, 7, 9, 11),
    "minor": (0, 2, 3, 5, 7, 8, 10),
    "harmonic minor": (0, 2, 3, 5, 7, 8, 11),
    "major pentatonic": (0, 2, 4, 7, 9),
    "minor pentatonic": (0, 3, 5, 7, 10),
    "blues": (0, 3, 5, 6, 7, 10),
    "dorian": (0, 2, 3, 5, 7, 9, 10),
    "mixolydian": (0, 2, 4, 5, 7, 9, 10),
    "chromatic": tuple(range(12)),
}
NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")

SYSTEM = """You write patterns for Spinus, a digital audio workstation like FL Studio. A pattern is a \
loop of notes for one or more channels (instruments). You answer only by calling the write_pattern tool, \
once per variation asked for; no other text is needed.

How to write notes:
- Time is in sixteenth steps from the pattern's start: step 0 is the first sixteenth, step 4 the second \
beat, step 16 the second bar (in 4/4). Lengths are in sixteenths too.
- Pitches are note names with an octave, C4 being middle C (MIDI 60): for example E1, F#2, Bb3. For a \
drum channel, use the names of its pads exactly as listed.
- Stay inside the pattern's length. Use the song's key when there is one. Write idiomatic parts for the \
instrument and the style asked for, playable by a human, and fitting the other channels' notes you are \
shown (lock a bass to the kick, leave space, answer phrases).
- Velocity is 0 to 1: accents around 0.9, ghost notes around 0.4.
"""

WRITE_PATTERN = Tool(
    "write_pattern",
    "Write one variation of the pattern: the complete notes for each channel asked for.",
    {
        "type": "object",
        "properties": {
            "title": {
                "type": "string",
                "description": "A few words naming this variation, e.g. 'Syncopated funk bass'",
            },
            "parts": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "channel": {"type": "integer", "description": "The channel's id, as given"},
                        "notes": {
                            "type": "array",
                            "items": {
                                "type": "object",
                                "properties": {
                                    "step": {"type": "number", "description": "Start, in sixteenths (0 = first)"},
                                    "length": {"type": "number", "description": "Length in sixteenths"},
                                    "note": {"type": "string", "description": "Note name (C4 = middle C) or pad name"},
                                    "velocity": {"type": "number", "description": "0 to 1"},
                                },
                                "required": ["step", "length", "note"],
                            },
                        },
                    },
                    "required": ["channel", "notes"],
                },
            },
        },
        "required": ["parts"],
    },
)


# ---------------------------------------------------------------------------------------------------
# What the generator is given


@dataclass
class Pad:
    name: str
    note: int  # MIDI


@dataclass
class Target:
    channel: int
    name: str
    instrument: str = ""
    pads: list[Pad] = field(default_factory=list)  # a drum machine's pads; empty for pitched instruments
    notes: list[dict[str, Any]] = field(default_factory=list)  # its notes now (for adjusting)

    @staticmethod
    def from_json(data: dict[str, Any]) -> Target:
        return Target(
            int(data["channel"]),
            str(data.get("name", "")),
            str(data.get("instrument", "")),
            [Pad(str(p["name"]), int(p["note"])) for p in data.get("pads", [])],
            list(data.get("notes", [])),
        )


@dataclass
class Settings:
    request: str
    targets: list[Target]
    context: list[dict[str, Any]]  # other channels: {name, instrument, notes}
    tempo: float = 120.0
    beats_per_bar: int = 4
    bar_steps: int = 16  # sixteenths per bar (16 in 4/4, 12 in 3/4 and 6/8)
    signature: str = "4/4"
    length_steps: int = 16
    key: tuple[int, str] | None = None  # tonic pitch class, scale name
    variations: int = 1
    adjust: bool = False
    swing: float = 0.0  # 0..1
    humanize: float = 0.0  # 0..1
    seed: int = 0

    @staticmethod
    def from_json(data: dict[str, Any]) -> Settings:
        key = data.get("key")
        return Settings(
            str(data.get("request", "")).strip(),
            [Target.from_json(t) for t in data.get("targets", [])],
            list(data.get("context", [])),
            float(data.get("tempo", 120.0)),
            int(data.get("beatsPerBar", 4)),
            max(1, int(data.get("barTicks", 4 * int(data.get("beatsPerBar", 4)) * TICKS_PER_STEP)) // TICKS_PER_STEP),
            str(data.get("timeSignature", f"{int(data.get('beatsPerBar', 4))}/4")),
            max(1, int(data.get("lengthTicks", 3840)) // TICKS_PER_STEP),
            (int(key["tonic"]), str(key.get("scale", "major")).lower()) if isinstance(key, dict) else None,
            max(1, min(MAX_VARIATIONS, int(data.get("variations", 1)))),
            bool(data.get("adjust", False)),
            min(1.0, max(0.0, float(data.get("swing", 0.0)))),
            min(1.0, max(0.0, float(data.get("humanize", 0.0)))),
            int(data.get("seed", 0)),
        )


def output_budget(settings: Settings) -> int:
    """Room for the answer: 8192 tokens up to four bars, more for longer patterns (and more variations)."""
    bars = settings.length_steps / max(1, settings.bar_steps)
    return int(min(32000, 8192 * max(1.0, bars / 4) * max(1.0, settings.variations / 2)))


# ---------------------------------------------------------------------------------------------------
# Note names


def note_name(midi: int) -> str:
    """Scientific pitch: C4 = 60."""
    return f"{NAMES[midi % 12]}{midi // 12 - 1}"


NOTE = re.compile(r"^\s*([A-Ga-g])\s*([#♯b♭]*)\s*(-?\d+)\s*$")


def parse_note(text: str) -> int | None:
    match = NOTE.match(text)
    if not match:
        return None
    letter, accidentals, octave = match.groups()
    pitch = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}[letter.upper()]
    pitch += accidentals.count("#") + accidentals.count("♯") - accidentals.count("b") - accidentals.count("♭")
    midi = (int(octave) + 1) * 12 + pitch
    return midi if 0 <= midi < 128 else None


def in_key(midi: int, key: tuple[int, str]) -> bool:
    return (midi - key[0]) % 12 in SCALES.get(key[1], SCALES["major"])


def nearest_in_key(midi: int, key: tuple[int, str]) -> int:
    for distance in range(0, 7):
        for candidate in (midi - distance, midi + distance):  # the lower one first: a tie goes down
            if 0 <= candidate < 128 and in_key(candidate, key):
                return candidate
    return midi


def find_pad(text: str, pads: list[Pad]) -> int | None:
    wanted = text.strip().lower()
    for pad in pads:
        if pad.name.lower() == wanted:
            return pad.note
    for pad in pads:  # "closed hat" for "Closed Hat 2", "hat" for "Closed Hat"
        if wanted in pad.name.lower() or pad.name.lower() in wanted:
            return pad.note
    midi = parse_note(text)
    if midi is not None and any(p.note == midi for p in pads):
        return midi
    return None


# ---------------------------------------------------------------------------------------------------
# Describing the song to the model


def describe_notes(notes: list[dict[str, Any]], pads: list[Pad]) -> str:
    names = {p.note: p.name for p in pads}
    parts = []
    for n in sorted(notes, key=lambda n: (int(n.get("start", 0)), int(n.get("pitch", 0)))):
        step = int(n.get("start", 0)) / TICKS_PER_STEP
        length = int(n.get("length", TICKS_PER_STEP)) / TICKS_PER_STEP
        pitch = int(n.get("pitch", 60))
        parts.append(f"{step:g}:{names.get(pitch, note_name(pitch))}/{length:g}")
    return " ".join(parts) if parts else "(no notes)"


def describe(settings: Settings) -> str:
    lines = [
        f"Request: {settings.request or 'something that fits'}",
        f"Tempo {settings.tempo:g} BPM, time signature {settings.signature} (a bar is {settings.bar_steps} sixteenth "
        f"steps), pattern length {settings.length_steps} sixteenth steps "
        f"({settings.length_steps / settings.bar_steps:g} bars).",
        f"Key: {NAMES[settings.key[0]]} {settings.key[1]}." if settings.key else "Key: not set.",
        "",
        "Write notes for these channels:",
    ]
    for t in settings.targets:
        kind = f"drums; pads: {', '.join(p.name for p in t.pads)}" if t.pads else "pitched"
        lines.append(f"- channel {t.channel}: '{t.name}' ({t.instrument or 'instrument'}, {kind})")
        if settings.adjust:
            lines.append(f"  its notes now (step:note/length): {describe_notes(t.notes, t.pads)}")
    if settings.context:
        lines += ["", "The other channels in the pattern (step:note/length), to fit with:"]
        for c in settings.context:
            pads = [Pad(str(p["name"]), int(p["note"])) for p in c.get("pads", [])]
            lines.append(
                f"- '{c.get('name', '')}' ({c.get('instrument', '')}): {describe_notes(c.get('notes', []), pads)}"
            )
    lines.append("")
    if settings.adjust:
        lines.append(
            "Change the notes now as the request says, keeping what it does not ask to change. Return the "
            "complete new notes for each channel."
        )
    count = settings.variations
    lines.append(
        f"Call write_pattern {count} times, each a clearly different variation."
        if count > 1
        else "Call write_pattern once."
    )
    return "\n".join(lines)


# ---------------------------------------------------------------------------------------------------
# Checking and finishing what the model wrote


def finish(tool_input: dict[str, Any], settings: Settings, rng: random.Random) -> dict[str, Any] | None:
    """The model's variation as Spinus notes ({channel id: [notes in ticks]}), or None if nothing usable."""
    length = settings.length_steps * TICKS_PER_STEP
    targets = {t.channel: t for t in settings.targets}
    parts: dict[str, list[dict[str, Any]]] = {}

    for part in tool_input.get("parts", []) if isinstance(tool_input.get("parts"), list) else []:
        try:
            target = targets.get(int(part.get("channel", -1)))
        except (TypeError, ValueError):
            continue
        if target is None or not isinstance(part.get("notes"), list):
            continue

        notes: dict[tuple[int, int], dict[str, Any]] = {}
        for raw in part["notes"]:
            try:
                step = float(raw["step"])
                steps_long = max(0.25, float(raw.get("length", 1)))
                velocity = min(1.0, max(0.05, float(raw.get("velocity", 0.8))))
            except (KeyError, TypeError, ValueError):
                continue
            text = str(raw.get("note", ""))
            if target.pads:
                pitch = find_pad(text, target.pads)
            else:
                pitch = parse_note(text)
                if pitch is not None and settings.key:
                    pitch = nearest_in_key(pitch, settings.key)
            if pitch is None:
                continue

            start = round(step) * TICKS_PER_STEP  # on the sixteenth grid
            if start < 0 or start >= length:
                continue
            ticks = max(TICKS_PER_STEP // 2, round(steps_long * 4) * TICKS_PER_STEP // 4)
            ticks = min(ticks, length - start)
            notes[(start, pitch)] = {"start": start, "length": ticks, "pitch": pitch, "velocity": velocity}

        finished = sorted(notes.values(), key=lambda n: (n["start"], n["pitch"]))
        for n in finished:
            # Swing: every second sixteenth later, up to a third of a sixteenth.
            if settings.swing > 0 and (n["start"] // TICKS_PER_STEP) % 2 == 1:
                n["start"] += round(settings.swing * TICKS_PER_STEP / 3)
            if settings.humanize > 0:
                n["start"] = max(0, n["start"] + round(rng.uniform(-1, 1) * settings.humanize * 20))
                n["velocity"] = min(1.0, max(0.05, n["velocity"] + rng.uniform(-1, 1) * settings.humanize * 0.12))
            n["length"] = min(n["length"], length - n["start"])
            n["velocity"] = round(n["velocity"], 3)
        if finished:
            parts[str(target.channel)] = finished

    if not parts:
        return None
    title = str(tool_input.get("title", "")).strip()[:80]
    return {"title": title, "parts": parts}


# ---------------------------------------------------------------------------------------------------


async def generate(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routing: Routing,
    usage_log: UsageLog,
    settings: Settings,
    say: Say,
) -> dict[str, Any]:
    if not settings.targets:
        raise ValueError("Choose at least one channel to write notes for.")
    routes = routing.resolve("patterns")
    if not routes:
        raise NoConnection("No AI connection is set up. Add one in Options > AI.")

    async def no_text(_: str) -> None:
        pass

    await say({"kind": "status", "text": "Writing the pattern..."})
    request = Request(
        routes[0].model, SYSTEM, [Message("user", [Text(describe(settings))])], [WRITE_PATTERN], output_budget(settings)
    )
    completion, index = await complete_with_fallback(client, connections, routes, 0, request, no_text, say)
    usage_log.add(routes[index].connection.id, "patterns", completion.usage)

    rng = random.Random(settings.seed)
    variations = []
    for use in completion.tool_uses:
        if use.name == "write_pattern":
            finished = finish(use.input, settings, rng)
            if finished is not None:
                variations.append(finished)
    variations = variations[: settings.variations]
    if not variations:
        raise ValueError(
            "The AI did not write any usable notes. Try again, or describe what you want differently."
            + (f" It said: {completion.text.strip()[:200]}" if completion.text.strip() else "")
        )

    return {
        "variations": variations,
        "model": completion.model,
        "connection": routes[index].connection.name,
        "usage": {
            "inputTokens": completion.usage.input_tokens,
            "outputTokens": completion.usage.output_tokens,
            "cost": completion.usage.cost,
        },
    }
