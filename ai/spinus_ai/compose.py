"""The Track creator's AI song writer: with an AI connection, the model writes the whole song.

Opt-in. The offline rules writer (spinus_ai.songwriter) stays the default; this is used only when the user picks
their AI connection as the writer. The model gets the style summary (from reference songs or a description), the
user's list of parts (each with its role, its register and a few candidate sounds from the sound index) and
answers by calling the write_song tool once with:

- the tempo and key;
- the sections: an id, a kind (intro, verse, chorus, bridge, outro), a length in bars, its chord loop as Roman
  numerals (each chord one or two bars) and its energy;
- the order the sections play in;
- for each part, a sound from its candidates and, for each section, its notes in a compact text notation: a loop
  of `bars` bars repeated over the section, each note "step pitch length velocity" on the sixteenth grid,
  separated by semicolons. Drum parts name the drum (kick, snare, hat, openHat, crash, tomLow, tomMid, tomHigh)
  or give its General MIDI note instead of a pitch.

Everything it says is checked (check_answer): a plan that doesn't hold together is not used at all (the rules'
song stands, and the page says so). Notes are snapped to the grid, clipped to their loop and section, folded by
octaves into the role's range, drum notes mapped to the kit's drums, NaN and junk dropped; an unknown sound is
ignored (the part keeps the sound it had). A part the model left out or botched is written by the rules for
that part only, on the model's sections and chords, and the song's note says which. The riser and impact
parts are always placed by the rules (they are effects on the Playlist).

Then Spinus checks the draft (review): notes outside the key, chord notes on the strong beats, notes moved into
range or onto the grid, each part's notes a bar against the style's, parts sitting in the same register,
section lengths off the style's phrases, and, when the Track creator has played each part with its sound
(SongWriterPanel's level check, in the out-of-process sound helper), parts that played nothing. Those
findings go back to the model for a revision (revise): it changes only what it names, and the same checks
apply. The result has the same shape as songwriter.write's, so Apply, its one undo step, Redo part and the
Replace question work as for the rules' songs.
"""

from __future__ import annotations

import contextlib
import datetime
import json
import math
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import httpx

from . import songwriter as sw
from .assistant import NoConnection, Say, answered_by, complete_with_fallback
from .connections import ConnectionStore
from .gateway import CUT_OFF, Message, ProviderError, Request, Text, Tool, count_anthropic
from .keynames import key_name
from .routing import Routing
from .usage import UsageLog

FEATURE = "patterns"  # the routing chain the Track creator's AI uses (as the plan's variation)
MOST_ROUNDS = 3
MOST_CONTINUATIONS = 4  # an answer cut off at the model's length limit is continued up to this often
MOST_CANDIDATES = 8
MOST_SECTIONS = 10
MOST_ORDER = 16
NOTES_PER_BAR = 64  # most notes a bar a part may play (after the loop is repeated)

# The notes a part of each role may play (MIDI), wider than its usual register (songwriter.layer_register).
RANGES: dict[str, tuple[int, int]] = {
    "bass": (24, 60), "sub": (21, 45), "chords": (40, 86), "pad": (36, 90), "arp": (45, 98), "lead": (45, 96),
    "counter": (40, 91), "rhythm-guitar": (40, 79), "lead-guitar": (50, 93), "acoustic-guitar": (40, 86),
    "clean-guitar": (40, 88), "piano": (21, 108), "organ": (36, 96), "strings": (28, 98), "brass": (40, 86),
    "woodwinds": (50, 98), "choir": (43, 81), "riser": (36, 96),
}  # fmt: skip
# General MIDI drums -> the drums Spinus maps to the channel's kit.
GM_DRUMS = {
    35: "kick", 36: "kick", 37: "snare", 38: "snare", 39: "snare", 40: "snare", 41: "tomLow", 42: "hat",
    43: "tomLow", 44: "hat", 45: "tomMid", 46: "openHat", 47: "tomMid", 48: "tomHigh", 49: "crash", 50: "tomHigh",
    51: "hat", 52: "crash", 53: "hat", 55: "crash", 57: "crash", 59: "hat",
}  # fmt: skip
DRUM_NAMES = {d.lower(): d for d in sw.DRUMS}
SECTION_ID = re.compile(r"^[a-z][a-z0-9-]{0,19}$")
FX_ROLES = ("riser", "impact")


class Unusable(ValueError):
    """The model's song does not hold together; the reason in plain words."""


# ---------------------------------------------------------------------------------------------------
# What the model is told


SYSTEM = """You are a songwriter and arranger working inside Spinus, a digital audio workstation. Write a new, \
original song in the style described, for the user's list of parts, by calling the write_song tool once. \
Never copy a reference song: use only the style's numbers.

Write it as a musician would for each instrument: a bass that locks with the kick, chords voiced for the \
instrument (guitar strums and power chords, piano voicings, held pads), a singable lead with phrases and \
rests, drums with a groove, fills at section ends and a crash where a section starts. Thin verses, full \
choruses; let parts come and go between sections.

Notation. Each part's notes for a section are a loop of `bars` bars (1, 2, 4 or up to the section's length) \
that Spinus repeats over the section. A note is "step pitch length velocity": step is the sixteenth from the \
loop's start (0 is its first beat; a bar has stepsPerBar steps), pitch a MIDI note (60 is middle C), length \
in sixteenths, velocity 1 to 127. Notes are separated by semicolons. For drum parts, write the drum's name \
(kick, snare, hat, openHat, crash, tomLow, tomMid, tomHigh) instead of the pitch. An empty string means the \
part rests in that section. Keep each part inside its range. Chords are Roman numerals (I, ii, iii, IV, V, vi, \
vii°, bVII, bVI, bIII, i, iv, v, V7, Imaj7, ii7, Vsus4...); prefer the style's chord vocabulary.

Parts with the role riser or impact are placed by Spinus itself: leave them out. For each other part, pick \
its sound from its candidates by id (or leave sound out to keep the current one)."""

REVISE = """Spinus wrote your song into the project and checked it. Its findings are below, with the song as it \
stands. Revise the song by calling write_song again: change only what you want to change and leave out the \
rest. Sections and order left out stay; a part left out keeps its notes and sound; a part's sections left \
out keep theirs. If you change a section's bars, write that section's notes again for every part."""

WRITE_SONG = Tool(
    "write_song",
    "Write (or revise) the whole song: its tempo, key, sections with chords, order, and every part's notes.",
    {
        "type": "object",
        "properties": {
            "tempo": {"type": "number", "description": "Beats a minute"},
            "key": {"type": "string", "description": "Tonic and scale, e.g. 'A minor', 'F# major'"},
            "sections": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "id": {"type": "string", "description": "Short, lowercase: verse, chorus, chorus2..."},
                        "kind": {"type": "string", "enum": list(sw.KINDS)},
                        "bars": {"type": "integer", "description": "Its length in bars, 2 to 32 (4-bar phrases)"},
                        "chords": {"type": "array", "items": {"type": "string"}, "description": "Its chord loop"},
                        "barsPerChord": {"type": "integer", "enum": [1, 2]},
                        "energy": {"type": "number", "description": "0 (quiet) to 1 (full)"},
                    },
                    "required": ["id", "kind", "bars", "chords"],
                },
            },
            "order": {"type": "array", "items": {"type": "string"}, "description": "Section ids as they play"},
            "parts": {
                "type": "object",
                "description": "Part id -> {sound, sections: {section id -> {bars, notes}}}",
                "additionalProperties": {
                    "type": "object",
                    "properties": {
                        "sound": {"type": "string", "description": "One of the part's candidate sound ids"},
                        "sections": {
                            "type": "object",
                            "additionalProperties": {
                                "type": "object",
                                "properties": {
                                    "bars": {"type": "integer", "description": "The loop's length in bars"},
                                    "notes": {"type": "string", "description": "'step pitch length velocity; ...'"},
                                },
                                "required": ["notes"],
                            },
                        },
                    },
                },
            },
            "reason": {"type": "string", "description": "In one or two sentences, the idea of the song"},
        },
    },
)


@dataclass
class Setup:
    """What the song is written for: the style, the parts and their candidate sounds."""

    summary: dict[str, Any]
    style: sw.Style
    parts: list[dict[str, str]]
    candidates: dict[str, list[dict[str, str]]] = field(default_factory=dict)  # part id -> [{id, name}]
    current: dict[str, str] = field(default_factory=dict)  # part id -> the sound it has now, in words

    @staticmethod
    def make(
        summary: dict[str, Any],
        parts: list[dict[str, str]] | None,
        size: str,
        candidates: Any = None,
        current: Any = None,
    ) -> Setup:
        style = sw.Style.from_summary(summary)
        chosen = sw.check_parts(parts) if parts is not None else sw.default_parts(style, size)
        found: dict[str, list[dict[str, str]]] = {}
        if isinstance(candidates, dict):
            for pid, items in candidates.items():
                if isinstance(items, list):
                    found[str(pid)] = [
                        {"id": str(c["id"])[:80], "name": str(c.get("name") or c["id"])[:120]}
                        for c in items[:MOST_CANDIDATES]
                        if isinstance(c, dict) and c.get("id")
                    ]
        now = {str(k): str(v)[:120] for k, v in current.items()} if isinstance(current, dict) else {}
        return Setup(summary, style, chosen, found, now)

    def written(self) -> list[dict[str, str]]:
        """The parts the model writes (the effects are the rules')."""
        return [p for p in self.parts if p["role"] not in FX_ROLES]


def part_brief(setup: Setup) -> list[dict[str, Any]]:
    out = []
    instances: dict[str, int] = {}
    for p in setup.written():
        role = p["role"]
        k = instances.get(role, 0)
        instances[role] = k + 1
        brief: dict[str, Any] = {"id": p["id"], "name": p["name"], "role": role, "drums": sw.ROLES[role][4]}
        if not brief["drums"]:
            low, high = RANGES.get(role, (24, 108))
            brief["range"] = [low, high]
            register = sw.layer_register(setup.style, role, k)
            if register:
                brief["usualNotes"] = register
        if setup.candidates.get(p["id"]):
            brief["sounds"] = setup.candidates[p["id"]]
        if setup.current.get(p["id"]):
            brief["currentSound"] = setup.current[p["id"]]
        out.append(brief)
    return out


def style_brief(setup: Setup) -> dict[str, Any]:
    style, summary = setup.style, setup.summary
    out: dict[str, Any] = {
        "tempo": {"typical": style.tempo, "range": [style.tempo_low, style.tempo_high]},
        "beatsPerBar": style.beats_per_bar,
        "stepsPerBar": style.steps,
        "keys": [f"{sw.NAMES[t]} {s}" for t, s, _ in style.keys],
        "modes": style.modes,
        "sectionLengths": [{"bars": b, "count": n} for b, n in style.lengths],
        "sectionsPerSong": style.sections_per_song,
        "songBars": style.song_bars,
        "swing": style.swing,
        "parts": {
            name: {"low": p.low, "median": p.median, "high": p.high, "notesPerBar": p.per_bar, "voices": p.voices}
            for name, p in style.parts.items()
        },
    }
    if style.has_drums:
        out["drumGroove"] = {lane: [round(v, 2) for v in values] for lane, values in style.lanes.items()}
        out["drumNotesPerBar"] = float(((summary.get("parts") or {}).get("drums") or {}).get("notesPerBar") or 0)
    vocab = {mode: style.vocabulary(mode) for mode in style.modes}
    out["chordVocabulary"] = {mode: {c: round(v.share(c), 3) for c in v.allowed()} for mode, v in vocab.items()}
    out["commonTransitions"] = {
        mode: {a: dict(after.most_common(3)) for a, after in v.transitions.items() if a in v.allowed()}
        for mode, v in vocab.items()
    }
    if style.without:
        out["leaveOut"] = sorted(style.without)
    if style.key_fixed:
        out["keyToUse"] = out["keys"][0]
    described = summary.get("described")
    if isinstance(described, dict) and described.get("understood"):
        out["description"] = str(described.get("text") or "")[:1500]
        out["understood"] = str(described["understood"])
    if summary.get("plain"):
        out["inWords"] = [str(line) for line in summary["plain"]][:12]
    return out


def first_message(setup: Setup) -> str:
    return json.dumps({"style": style_brief(setup), "parts": part_brief(setup)}, indent=1)


def answer_guess(setup: Setup) -> int:
    """The song's notes in tokens, before any reasoning: each written part in five sections of four-bar loops,
    about five tokens a note."""
    answer = 300
    for p in setup.written():
        if sw.ROLES[p["role"]][4]:
            per_bar = 12.0
        else:
            per_bar = setup.style.parts[sw.SUMMARY_PART.get(sw.ROLES[p["role"]][2], "other")].per_bar
        answer += round(5 * (15 + 4 * per_bar * 5))
    return answer


# Without runs to go by: what the answer may cost against answer_guess, by the reasoning level ("": the model's
# default). The reasoning is unknown before sending, so a range; high levels can take several times more.
REASONING_RANGE = {"": (0.8, 4.0), "low": (0.8, 2.0), "medium": (1.0, 4.0), "high": (1.5, 8.0)}


class UsageRecords:
    """What composing has really used, as the providers reported it: per model and reasoning level, the parts
    written and each request's input, output and reasoning tokens. In the settings folder; no prompt, song or
    key is kept. The estimates are made from it once there are runs."""

    KEEP = 40

    def __init__(self, folder: Path) -> None:
        self.file = folder / "compose-usage.json"
        self.runs: list[dict[str, Any]] = []
        if self.file.exists():
            try:
                loaded = json.loads(self.file.read_text(encoding="utf-8"))
                self.runs = [r for r in loaded.get("runs", []) if isinstance(r, dict)]
            except (OSError, ValueError):
                self.runs = []

    def add(self, model: str, effort: str, parts: int, kind: str, requests: list[dict[str, int]]) -> None:
        self.runs.append({
            "date": datetime.date.today().isoformat(), "model": model, "effort": effort, "parts": parts,
            "kind": kind, "requests": requests,
        })  # fmt: skip
        self.runs = self.runs[-self.KEEP :]
        self.file.parent.mkdir(parents=True, exist_ok=True)
        self.file.write_text(json.dumps({"runs": self.runs}, indent=1), encoding="utf-8")

    def like(self, model: str, effort: str, kind: str) -> list[dict[str, Any]]:
        return [
            r for r in self.runs if r.get("model") == model and r.get("effort") == effort and r.get("kind") == kind
        ][-10:]


def total_of(requests: list[dict[str, int]], what: str) -> int:
    return sum(int(r.get(what, 0)) for r in requests)


@dataclass
class Harnessed:
    """What the harness's first request holds (spinus_ai.harness), for its estimate."""

    system: str
    text: str
    tools: list[Tool]
    sections: int


async def estimate(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routing: Routing,
    records: UsageRecords,
    setup: Setup,
    effort: str,
    rounds: int,
    harness: Harnessed | None = None,
) -> dict[str, Any]:
    """The tokens a song may take, as a range: the first request's input counted by the provider where it can
    (Anthropic's count_tokens; else about four characters a token), then the input and the answer (with its
    reasoning) from the user's own past runs with that model, level and way of writing; without runs, from
    REASONING_RANGE (and, through the harness, from how many turns the parts and sections may take: each turn
    sends the conversation so far again, mostly read from the provider's cache). Then each revision."""
    routes = routing.resolve(FEATURE)
    if not routes:
        return {"available": False, "reason": "No AI connection is set up: add one in Options > AI."}
    route = routes[0]
    system, text, tools = (harness.system, harness.text, harness.tools) if harness else (
        SYSTEM, first_message(setup), [WRITE_SONG])  # fmt: skip
    request = Request(route.model, system, [Message("user", [Text(text)])], tools, None, effort)
    counted = False
    input_tokens = round(len(system + text + json.dumps([t.input_schema for t in tools])) / 4)
    if route.connection.provider == "anthropic":
        key = connections.key_for(route.connection)
        if key:
            with contextlib.suppress(ProviderError):  # not counted: about, from its characters
                input_tokens, counted = await count_anthropic(client, route.connection, key, request), True
    parts = max(1, len(setup.written()))
    rounds = max(0, min(MOST_ROUNDS, rounds))
    kind = "harness" if harness else "write"
    revise_kind = "harness-revise" if harness else "revise"

    def from_runs(kind: str) -> tuple[tuple[int, int], tuple[int, int]] | None:
        """The input and output a run of this kind took, per part, the least and the most (x parts)."""
        runs = [r for r in records.like(route.model, effort, kind) if r.get("requests")]
        if not runs:
            return None
        spread = (0.8, 1.25) if len(runs) == 1 else (1.0, 1.0)

        def span(what: str) -> tuple[int, int]:
            each = [total_of(r["requests"], what) / max(1, int(r.get("parts", 1))) for r in runs]
            return round(min(each) * spread[0] * parts), round(max(each) * spread[1] * parts)

        return span("input"), span("output")

    guess = answer_guess(setup)
    lo, hi = REASONING_RANGE.get(effort, REASONING_RANGE[""])
    if harness:
        # Turns: a few for the form and the checks, one or two a part (loops, motifs, copies), up to a few a section
        # of each part; each sends the conversation again (its answers and the tools' short results).
        turns = (2 * parts + 4, 2 * parts * max(1, harness.sections) + 10)
        out_guess = (round(guess * lo), round(guess * hi * 2))
        in_guess = (
            round(turns[0] * input_tokens + out_guess[0] * turns[0] / 2),
            round(turns[1] * input_tokens + out_guess[1] * turns[1] / 2),
        )
    else:
        out_guess = (round(guess * lo), round(guess * hi))
        in_guess = (input_tokens, input_tokens)
    first = from_runs(kind)
    first_in, first_out = first or (in_guess, out_guess)
    again = from_runs(revise_kind)
    if again is None:
        sent_back = guess if not harness else 0  # the harness revises the working copy; one-shot sends the song
        again = (
            (round(first_in[0] / 2) + sent_back, first_in[1] + sent_back),
            (round(first_out[0] / 2), first_out[1]),
        )
    rev_in, rev_out = again
    return {
        "available": True,
        "connection": route.connection.name,
        "model": route.model,
        "effort": effort,
        "way": kind,
        "input": input_tokens,  # the first request's
        "inputCounted": counted,  # counted by the provider (else about)
        "inputRange": list(first_in),  # the whole first round's
        "output": list(first_out),
        "perRevision": [rev_in[0] + rev_out[0], rev_in[1] + rev_out[1]],
        "low": first_in[0] + first_out[0] + rounds * (rev_in[0] + rev_out[0]),
        "high": first_in[1] + first_out[1] + rounds * (rev_in[1] + rev_out[1]),
        "fromRuns": len(records.like(route.model, effort, kind)),  # 0: from the level's usual range
        "rounds": rounds,
        "cached": bool(harness) and route.connection.provider == "anthropic",
    }


# ---------------------------------------------------------------------------------------------------
# Checking the answer


@dataclass
class Parsed:
    notes: list[dict[str, Any]]  # in ticks from the section's start, as songwriter writes them
    tokens: int = 0
    bad: int = 0
    folded: int = 0
    snapped: int = 0


def parse_notes(text: str, drums: bool, role: str, loop_bars: int, section_bars: int, steps: int) -> Parsed:
    """One part's notes for a section: the loop's notes repeated over the section, each checked."""
    out = Parsed([])
    loop_steps, section_steps = loop_bars * steps, section_bars * steps
    low, high = RANGES.get(role, (24, 108))
    loop: list[tuple[int, Any, int, float]] = []
    for raw in re.split(r"[;\n]+", text or "")[: 64 * max(1, loop_bars) * 2]:
        fields = raw.replace(",", " ").split()
        if not fields:
            continue
        out.tokens += 1
        if len(fields) not in (3, 4):
            out.bad += 1
            continue
        try:
            step = float(fields[0])
            length = float(fields[2])
            velocity = float(fields[3]) if len(fields) == 4 else 100.0
        except ValueError:
            out.bad += 1
            continue
        if not all(math.isfinite(v) for v in (step, length, velocity)) or step < 0 or length <= 0:
            out.bad += 1
            continue
        whole = round(step)
        if abs(step - whole) > 0.01:
            out.snapped += 1
        if whole >= loop_steps:
            out.bad += 1
            continue
        pitch: Any
        if drums:
            word = fields[1]
            if word.lower() in DRUM_NAMES:
                pitch = DRUM_NAMES[word.lower()]
            elif word.isdigit() and int(word) in GM_DRUMS:
                pitch = GM_DRUMS[int(word)]
            else:
                out.bad += 1
                continue
        else:
            try:
                value = float(fields[1])
            except ValueError:
                out.bad += 1
                continue
            if not math.isfinite(value) or not 0 <= value <= 127:
                out.bad += 1
                continue
            pitch = round(value)
            if pitch < low or pitch > high:
                out.folded += 1
                while pitch < low:
                    pitch += 12
                while pitch > high:
                    pitch -= 12
                pitch = max(low, min(high, pitch))
        steps_long = max(1, min(round(length), loop_steps - whole))
        loop.append((whole, pitch, steps_long, max(0.05, min(1.0, velocity / 127.0))))
    loop.sort(key=lambda n: (n[0], str(n[1])))
    for repeat in range(math.ceil(section_bars / max(1, loop_bars))):
        for start, pitch, length, velocity in loop:
            at = repeat * loop_steps + start
            if at >= section_steps:
                continue
            note: dict[str, Any] = {
                "start": at * sw.TICKS_PER_STEP,
                "length": min(length, section_steps - at) * sw.TICKS_PER_STEP
                - (sw.TICKS_PER_STEP // 2 if drums else 0),
                "velocity": round(velocity, 3),
            }
            note["drum" if drums else "pitch"] = pitch
            out.notes.append(note)
    out.notes = out.notes[: NOTES_PER_BAR * section_bars]
    return out


def as_int(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float) and math.isfinite(value) and value == round(value):
        return int(value)
    return None


def check_plan(answer: dict[str, Any], setup: Setup) -> dict[str, Any]:
    """The tempo, key, sections and order, checked; raises Unusable with the reason."""
    style = setup.style
    tempo_value = answer.get("tempo")
    tempo = float(tempo_value) if isinstance(tempo_value, int | float) and not isinstance(tempo_value, bool) else 0.0
    if not math.isfinite(tempo) or tempo <= 0:
        tempo = style.tempo
    low = max(40.0, min(style.tempo_low, style.tempo) * 0.9)
    high = min(240.0, max(style.tempo_high, style.tempo) * 1.1)
    tempo = float(round(min(max(tempo, low), high)))

    parsed = sw.key_of_name(str(answer.get("key") or "").strip())
    if style.key_fixed or parsed is None:
        tonic, scale = style.keys[0][0], style.keys[0][1]
    else:
        tonic, scale = parsed

    sections = answer.get("sections")
    if not isinstance(sections, list) or not 1 <= len(sections) <= MOST_SECTIONS:
        raise Unusable(f"the song needs 1 to {MOST_SECTIONS} sections")
    checked: list[dict[str, Any]] = []
    for s in sections:
        if not isinstance(s, dict):
            raise Unusable("a section is not described")
        sid = str(s.get("id") or "").strip().lower()
        kind = s.get("kind")
        bars = as_int(s.get("bars"))
        if not SECTION_ID.match(sid) or any(c["id"] == sid for c in checked):
            raise Unusable(f"the section id '{sid}' is not usable (short, lowercase, each its own)")
        if kind not in sw.KINDS:
            raise Unusable(f"'{kind}' is not a kind of section")
        if bars is None or not 2 <= bars <= sw.SECTION_BARS[1]:
            raise Unusable(f"the {sid} is not 2 to {sw.SECTION_BARS[1]} bars long")
        chords = s.get("chords")
        if not isinstance(chords, list) or not 1 <= len(chords) <= 8:
            raise Unusable(f"the {sid} has no chord loop (1 to 8 chords)")
        romans = [str(c).strip() for c in chords]
        wrong = [c for c in romans if sw.parse_roman(c) is None]
        if wrong:
            raise Unusable(f"the {sid}'s chords {', '.join(wrong)} are not Roman numerals Spinus reads")
        hold = 2 if as_int(s.get("barsPerChord")) == 2 else 1
        energy_value = s.get("energy")
        energy = (
            float(energy_value) if isinstance(energy_value, int | float) and not isinstance(energy_value, bool) else -1
        )
        if not math.isfinite(energy) or not 0 <= energy <= 1:
            energy = sw.ENERGY[str(kind)]
        checked.append({"id": sid, "kind": str(kind), "bars": bars, "chords": romans, "hold": hold, "energy": energy})

    order_value = answer.get("order")
    ids = {s["id"] for s in checked}
    if not isinstance(order_value, list) or not 1 <= len(order_value) <= MOST_ORDER:
        raise Unusable(f"the order needs 1 to {MOST_ORDER} sections")
    order = [str(o).strip().lower() for o in order_value]
    missing = [o for o in order if o not in ids]
    if missing:
        raise Unusable(f"the order names sections that aren't there ({', '.join(missing[:3])})")
    by_id = {s["id"]: s for s in checked}
    total = sum(by_id[o]["bars"] for o in order)
    if not 4 <= total <= sw.MOST_BARS:
        raise Unusable(f"the song is {total} bars long (4 to {sw.MOST_BARS})")
    return {
        "tempo": tempo,
        "key": {"tonic": tonic, "scale": scale, "name": key_name(tonic, scale)},
        "sections": [s for s in checked if s["id"] in order],
        "order": order,
    }


def merged(previous: dict[str, Any] | None, answer: dict[str, Any]) -> dict[str, Any]:
    """A revision on top of the answer before it: what it leaves out stays."""
    if previous is None:
        return answer
    out: dict[str, Any] = json.loads(json.dumps(previous))
    for key in ("tempo", "key", "sections", "order", "reason"):
        if key in answer:
            out[key] = answer[key]
    parts = answer.get("parts")
    if isinstance(parts, dict):
        mine = out.setdefault("parts", {})
        for pid, part in parts.items():
            if not isinstance(part, dict):
                continue
            entry = mine.setdefault(str(pid), {})
            if "sound" in part:
                entry["sound"] = part["sound"]
            if isinstance(part.get("sections"), dict):
                entry.setdefault("sections", {}).update(part["sections"])
    return out


@dataclass
class Checked:
    plan: dict[str, Any]
    notes: dict[str, dict[str, list[dict[str, Any]]]]  # part id -> section id -> notes
    parsed: dict[str, dict[str, Parsed]]
    sounds: dict[str, str]
    filled: list[str]  # part ids the rules wrote, the model having left them out or botched them
    why_filled: dict[str, str]


def check_answer(answer: Any, setup: Setup) -> Checked:
    if not isinstance(answer, dict):
        raise Unusable("there was no song in the answer")
    plan = check_plan(answer, setup)
    by_id = {s["id"]: s for s in plan["sections"]}
    given = answer.get("parts") if isinstance(answer.get("parts"), dict) else {}
    assert isinstance(given, dict)
    notes: dict[str, dict[str, list[dict[str, Any]]]] = {}
    parsed: dict[str, dict[str, Parsed]] = {}
    sounds: dict[str, str] = {}
    filled: list[str] = []
    why: dict[str, str] = {}
    for p in setup.written():
        pid, role = p["id"], p["role"]
        entry = given.get(pid)
        if not isinstance(entry, dict) or not isinstance(entry.get("sections"), dict):
            filled.append(pid)
            why[pid] = "left out"
            continue
        sound = entry.get("sound")
        if isinstance(sound, str) and any(c["id"] == sound for c in setup.candidates.get(pid, [])):
            sounds[pid] = sound
        sections = entry["sections"]
        per: dict[str, Parsed] = {}
        for sid, section in by_id.items():
            value = sections.get(sid)
            if not isinstance(value, dict):
                value = {"notes": value} if isinstance(value, str) else {"notes": ""}
            loop = as_int(value.get("bars")) or int(section["bars"])
            loop = max(1, min(int(section["bars"]), loop))
            per[sid] = parse_notes(
                str(value.get("notes") or ""),
                bool(sw.ROLES[role][4]),
                role,
                loop,
                int(section["bars"]),
                setup.style.steps,
            )
        tokens = sum(x.tokens for x in per.values())
        bad = sum(x.bad for x in per.values())
        if not any(x.notes for x in per.values()):
            filled.append(pid)
            why[pid] = "no notes" if not tokens else "no usable notes"
            continue
        if bad > 0.3 * tokens:
            filled.append(pid)
            why[pid] = f"{bad} of its {tokens} notes not readable"
            continue
        notes[pid] = {sid: x.notes for sid, x in per.items()}
        parsed[pid] = per
    if not notes and setup.written():
        raise Unusable("none of the parts had usable notes")
    return Checked(plan, notes, parsed, sounds, filled, why)


# ---------------------------------------------------------------------------------------------------
# The song from the checked answer


def section_chords(section: dict[str, Any], tonic: int) -> list[dict[str, Any]]:
    loop, hold = section["chords"], int(section["hold"])
    chords = []
    for bar in range(int(section["bars"])):
        roman = loop[(bar // hold) % len(loop)]
        chords.append(
            {"bar": bar, "roman": roman, "name": sw.chord_label(roman, tonic), "tones": sw.chord_tones(roman, tonic)}
        )
    return chords


def build_song(checked: Checked, setup: Setup, seed: int, size: str) -> dict[str, Any]:
    """The song: the model's plan and notes, the rules' notes for the parts it left out (on its sections and
    chords), the rules' effects."""
    style, plan_in = setup.style, checked.plan
    tonic = int(plan_in["key"]["tonic"])
    plan = {
        "tempo": plan_in["tempo"],
        "key": plan_in["key"],
        "beatsPerBar": style.beats_per_bar,
        "sections": [
            {"id": s["id"], "kind": s["kind"], "bars": s["bars"], "contour": "arch", "progression": s["chords"]}
            for s in plan_in["sections"]
        ],
        "order": plan_in["order"],
        "size": size,
        "parts": setup.parts,
    }
    seeds = sw.new_seeds(seed, plan)
    keep = {
        "sections": [
            {"id": s["id"], "chords": section_chords(s, tonic), "loop": s["chords"]} for s in plan_in["sections"]
        ],
        "parts": {},
    }
    song = sw.build(style, plan, seeds, keep=keep)
    energy = {s["id"]: s["energy"] for s in plan_in["sections"]}
    kinds = {s["id"]: s["kind"] for s in plan_in["sections"]}
    counts: dict[str, int] = {}
    for s in song["sections"]:
        s["energy"] = round(float(energy[s["id"]]), 2)
        kind = kinds[s["id"]]
        counts[kind] = counts.get(kind, 0) + 1
        if counts[kind] > 1:
            s["name"] = f"{sw.KIND_WORDS[kind]} {counts[kind]}"

    # The layers: every part of the list, each with the model's notes or the rules'.
    rules_layers = {layer["id"]: layer for layer in song["layers"]}
    layers, layer_parts = [], {}
    instances: dict[str, int] = {}
    for p in setup.parts:
        role = p["role"]
        k = instances.get(role, 0)
        instances[role] = k + 1
        layer = rules_layers.get(p["id"]) or {
            "id": p["id"], "name": p["name"], "role": role, "family": sw.ROLES[role][1], "part": sw.ROLES[role][2],
            "soundRole": sw.ROLES[role][3], "drums": sw.ROLES[role][4], "instance": k,
        }  # fmt: skip
        if p["id"] in checked.notes:
            layer_parts[p["id"]] = checked.notes[p["id"]]
            pitches = sorted(int(n["pitch"]) for ns in checked.notes[p["id"]].values() for n in ns if "pitch" in n)
            if pitches:
                layer["register"] = [pitches[0], pitches[len(pitches) // 2], pitches[-1]]
        elif p["id"] in song["layerParts"]:
            layer_parts[p["id"]] = song["layerParts"][p["id"]]
        if p["id"] in layer_parts or p["id"] in song["fx"]:
            layers.append(layer)
    song["layers"], song["layerParts"] = layers, layer_parts

    # The four parts the checks compare (and Redo part keeps): the first part of each, as the project plays it.
    for base in sw.PARTS:
        source = next((layer["id"] for layer in layers if layer["role"] == base and layer["id"] in layer_parts), None)
        source = source or next(
            (layer["id"] for layer in layers if layer["part"] == base and layer["id"] in layer_parts), None
        )
        if source is not None:
            song["parts"][base] = {s["id"]: list(layer_parts[source].get(s["id"], [])) for s in song["sections"]}
    return song


# ---------------------------------------------------------------------------------------------------
# Spinus's checks of the draft


def review(
    song: dict[str, Any], setup: Setup, checked: Checked | None, levels: Any = None, problems: Any = None
) -> list[str]:
    """What Spinus found in the draft, in plain words (for the model's revision, and shown to the user)."""
    style = setup.style
    found: list[str] = []
    by_id = {s["id"]: s for s in song["sections"]}
    bar_ticks = int(song["barTicks"])
    scale = sw.scale_of(song["key"])
    played_bars = int(song["bars"])
    layers = {layer["id"]: layer for layer in song["layers"]}
    names = {p["id"]: p["name"] for p in setup.parts}
    medians: dict[str, tuple[int, set[str]]] = {}
    for pid, by_section in song["layerParts"].items():
        layer = layers.get(pid)
        if layer is None or (checked is not None and pid not in checked.notes):
            continue
        name = names.get(pid, pid)
        played = [(by_id[e["section"]], n) for e in song["arrangement"] for n in by_section.get(e["section"], [])]
        per_bar = len(played) / max(1, played_bars)
        if layer["drums"]:
            want = float(((setup.summary.get("parts") or {}).get("drums") or {}).get("notesPerBar") or 0)
            if layer["role"] == "drums" and want and not 0.4 <= per_bar / want <= 2.5:
                found.append(f"{name} plays {per_bar:.1f} hits a bar; the style's drums play about {want:g}.")
            continue
        pitches = [int(n["pitch"]) for _, n in played]
        if not pitches:
            continue
        outside = sum(1 for p in pitches if p % 12 not in scale)
        if outside > 0.12 * len(pitches):
            found.append(
                f"{name}: {round(100 * outside / len(pitches))} % of its notes are outside {song['key']['name']}."
            )
        if layer["part"] in ("bass", "chords"):
            strong = [
                (s, n)
                for s, n in played
                if (int(n["start"]) % bar_ticks) % (bar_ticks // max(1, song["beatsPerBar"])) == 0
            ]
            if strong:
                off = 0
                for s, n in strong:
                    bar = min(int(s["bars"]) - 1, int(n["start"]) // bar_ticks)
                    tones = s["chords"][bar]["tones"]
                    if int(n["pitch"]) % 12 not in sw.allowed_pitch_classes(tones, scale):
                        off += 1
                if off > 0.3 * len(strong):
                    found.append(
                        f"{name}: {round(100 * off / len(strong))} % of its notes on the beats rub against the chords."
                    )
        if layer.get("instance", 0) == 0 and layer["part"] in sw.SUMMARY_PART:
            want = style.parts[sw.SUMMARY_PART[layer["part"]]].per_bar
            if want and not 0.4 <= per_bar / want <= 2.5:
                found.append(
                    f"{name} plays {per_bar:.1f} notes a bar; the style's {layer['part']} plays about {want:g}."
                )
        if checked is not None and pid in checked.parsed:
            folded = sum(x.folded for x in checked.parsed[pid].values())
            snapped = sum(x.snapped for x in checked.parsed[pid].values())
            bad = sum(x.bad for x in checked.parsed[pid].values())
            low, high = RANGES.get(layer["role"], (24, 108))
            if folded:
                found.append(
                    f"{name}: {folded} notes were outside its range ({sw.note_name(low)} to {sw.note_name(high)}) "
                    "and moved by octaves."
                )
            if snapped:
                found.append(f"{name}: {snapped} notes were off the sixteenth grid and moved onto it.")
            if bad:
                found.append(f"{name}: {bad} notes could not be read and were left out.")
        pitches.sort()
        medians[pid] = (pitches[len(pitches) // 2], {s["id"] for s, _ in played})
    ids = sorted(medians)
    for i, a in enumerate(ids):
        for b in ids[i + 1 :]:
            (ma, sa), (mb, sb) = medians[a], medians[b]
            if abs(ma - mb) <= 2 and len(sa & sb) >= max(1, len(sa | sb) // 2):
                found.append(
                    f"{names.get(a, a)} and {names.get(b, b)} sit in the same register (around {sw.note_name(ma)}): "
                    "move one up or down an octave, or give them different rhythms."
                )
    lengths = sorted({b for b, _ in style.lengths})
    for s in song["sections"]:
        if int(s["bars"]) % 4:
            found.append(
                f"The {s['name'].lower()} is {s['bars']} bars: the style's sections are 4-bar phrases "
                f"({', '.join(map(str, lengths))} bars)."
            )
    if style.song_bars and not 0.6 <= played_bars / style.song_bars <= 1.5:
        found.append(f"The song is {played_bars} bars; the style's songs are about {style.song_bars:g}.")
    if checked is not None:
        for pid in checked.filled:
            found.append(
                f"{names.get(pid, pid)} was written by Spinus's rules ({checked.why_filled.get(pid, 'left out')}): "
                "write it."
            )
    if isinstance(problems, dict):
        for pid, why in problems.items():
            if pid in names:
                found.append(f"{names[pid]} could not be played with its sound ({str(why)[:120]}).")
    if isinstance(levels, dict):
        for pid, db in levels.items():
            if pid in names and isinstance(db, int | float) and db <= -90:
                found.append(
                    f"{names[pid]} made no sound with its sound: its notes may be outside the instrument's range, or "
                    "too short for it."
                )
    return found


# ---------------------------------------------------------------------------------------------------
# Talking to the model


def as_notation(notes: list[dict[str, Any]], drums: bool) -> str:
    out = []
    for n in notes:
        step = int(n["start"]) // sw.TICKS_PER_STEP
        length = max(1, round(int(n["length"]) / sw.TICKS_PER_STEP))
        out.append(f"{step} {n['drum'] if drums else n['pitch']} {length} {round(float(n['velocity']) * 127)}")
    return "; ".join(out)


def song_for_model(song: dict[str, Any], answer: dict[str, Any], setup: Setup) -> dict[str, Any]:
    """The song as it stands, in the tool's own shape: the model's loops as it wrote them, the rules' parts in
    full."""
    out: dict[str, Any] = {k: answer[k] for k in ("tempo", "key", "sections", "order") if k in answer}
    out["tempo"], out["key"] = song["tempo"], song["key"]["name"]
    parts: dict[str, Any] = {}
    given = answer.get("parts") if isinstance(answer.get("parts"), dict) else {}
    assert isinstance(given, dict)
    drums = {layer["id"]: bool(layer["drums"]) for layer in song["layers"]}
    for pid, by_section in song["layerParts"].items():
        if pid in given and isinstance(given[pid], dict) and pid not in (song.get("composed") or {}).get("filled", []):
            parts[pid] = given[pid]
        else:
            parts[pid] = {
                "sections": {
                    sid: {"bars": next(int(s["bars"]) for s in song["sections"] if s["id"] == sid),
                          "notes": as_notation(notes, drums.get(pid, False))}
                    for sid, notes in by_section.items()
                }
            }  # fmt: skip
    out["parts"] = parts
    return out


def salvage(text: str) -> dict[str, Any] | None:
    """What can be read of tool arguments cut off midway: the JSON up to its last complete value, with the
    open brackets closed (an unfinished string or number is left out)."""
    stack: list[str] = []
    cuts: list[tuple[int, str]] = []  # where a value ended (after a comma outside strings), the brackets then open
    in_string = escaped = False
    for i, c in enumerate(text):
        if in_string:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == '"':
                in_string = False
            continue
        if c == '"':
            in_string = True
        elif c in "{[":
            stack.append("}" if c == "{" else "]")
        elif c in "}]" and stack:
            stack.pop()
            cuts.append((i + 1, "".join(reversed(stack))))
        elif c == ",":
            cuts.append((i, "".join(reversed(stack))))
    for at, closing in reversed(cuts[-400:]):
        try:
            value = json.loads(text[:at] + closing)
        except ValueError:
            continue
        return value if isinstance(value, dict) else None
    return None


CONTINUE = """Your write_song call was cut off at your length limit. What arrived is below: Spinus keeps it. Call \
write_song again with only what is missing (the sections and parts not in it, or not finished), in the same \
shape; Spinus joins the two."""


@dataclass
class Asked:
    answer: dict[str, Any] | None
    by: str
    requests: list[dict[str, int]]  # each request's tokens, as the provider reported them
    model: str = ""  # the model asked (as the routing names it: what the estimates look up)
    continued: int = 0  # how often the answer, cut off, was continued


async def ask(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routing: Routing,
    usage_log: UsageLog,
    text: str,
    effort: str,
    say: Say,
    spent: int = 0,
) -> Asked:
    """The model's write_song call (None when it made none). The answer may be as long as the model allows;
    one cut off at that limit is continued (the pieces joined) instead of being thrown away. `spent`: the tokens
    used before, for the running total."""
    routes = routing.resolve(FEATURE)
    if not routes:
        raise NoConnection("No AI connection is set up.")

    async def no_text(_: str) -> None:
        pass

    asked = Asked(None, "", [])
    prompt = text
    for attempt in range(MOST_CONTINUATIONS + 1):
        request = Request(routes[0].model, SYSTEM, [Message("user", [Text(prompt)])], [WRITE_SONG], None, effort)
        failed: list[str] = []
        completion, index = await complete_with_fallback(client, connections, routes, 0, request, no_text, say, failed)
        usage_log.add(routes[index].connection.id, FEATURE, completion.usage)
        asked.by = answered_by(completion.model, failed)
        asked.model = routes[index].model
        asked.requests.append({
            "input": completion.usage.input_tokens, "output": completion.usage.output_tokens,
            "reasoning": completion.usage.reasoning_tokens,
        })  # fmt: skip
        used = spent + sum(r["input"] + r["output"] for r in asked.requests)
        await say({"kind": "progress", "done": 0, "total": 1, "step": f"{used:,} tokens so far"})
        call = next((u for u in completion.tool_uses if u.name == "write_song"), None)
        piece: dict[str, Any] | None = None
        if call is not None:
            piece = salvage(call.partial) if call.partial else call.input
        cut = completion.stop_reason in CUT_OFF or (call is not None and bool(call.partial))
        if piece is not None:
            asked.answer = merged(asked.answer, piece) if asked.answer is not None else piece
        if not cut or attempt == MOST_CONTINUATIONS:
            break
        asked.continued += 1
        await say({"kind": "status", "text": "The AI's answer was cut off at its length limit: asking it to go on..."})
        prompt = text + "\n\n" + CONTINUE + "\n\n" + json.dumps(asked.answer or {}, indent=0)
    return asked


def finish(song: dict[str, Any], setup: Setup, composed: dict[str, Any]) -> dict[str, Any]:
    song["composed"] = composed
    song["composedBy"] = composed["by"]
    song["sounds"] = composed.get("sounds", {})
    return sw.finish(song, setup.summary, setup.style)


def rules_song(setup: Setup, seed: int, size: str, note: str, requests: list[dict[str, int]]) -> dict[str, Any]:
    song = sw.write(setup.summary, seed, size, setup.parts)
    song["note"] = note
    song["usage"] = usage_of([{"round": 0, **r} for r in requests])
    song["plain"] = sw.describe(song)
    return song


def usage_of(requests: list[dict[str, int]]) -> dict[str, Any]:
    """Each request's tokens (with its round) and the totals."""
    return {
        "requests": requests,
        "input": total_of(requests, "input"),
        "output": total_of(requests, "output"),
        "reasoning": total_of(requests, "reasoning"),
    }


async def compose(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routing: Routing,
    usage_log: UsageLog,
    records: UsageRecords,
    setup: Setup,
    seed: int,
    size: str,
    effort: str,
    say: Say,
) -> dict[str, Any]:
    """The model's song, checked; the rules' song (with a note saying why) when its answer is not usable.
    NoConnection without a connection; ProviderError when every connection failed."""
    await say({"kind": "status", "text": "Writing: the AI is writing the song..."})
    asked = await ask(client, connections, routing, usage_log, first_message(setup), effort, say)
    records.add(asked.model, effort, len(setup.written()), "write", asked.requests)
    if asked.answer is None:
        return rules_song(setup, seed, size, f"Written by the rules: the AI ({asked.by}) answered without a song.",
                          asked.requests)  # fmt: skip
    try:
        checked = check_answer(asked.answer, setup)
    except Unusable as why:
        return rules_song(
            setup, seed, size, f"Written by the rules: the AI's song was not usable ({why}).", asked.requests
        )
    await say({"kind": "status", "text": "Checking the AI's song..."})
    song = build_song(checked, setup, seed, size)
    composed = {
        "by": asked.by,
        "effort": effort,
        "answer": asked.answer,
        "round": 0,
        "usage": usage_of([{"round": 0, **r} for r in asked.requests]),
        "continued": asked.continued,
        "sounds": checked.sounds,
        "filled": checked.filled,
        "reason": str(asked.answer.get("reason") or "")[:400],
    }
    composed["findings"] = review(song, setup, checked)
    song["note"] = written_note(setup, composed)
    return finish(song, setup, composed)


async def revise(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routing: Routing,
    usage_log: UsageLog,
    records: UsageRecords,
    setup: Setup,
    song: dict[str, Any],
    levels: Any,
    problems: Any,
    effort: str,
    say: Say,
) -> dict[str, Any]:
    """The model's song revised from Spinus's findings (with the levels each part played at, when measured).
    The song before stands, with a note, when the revision is not usable."""
    composed = dict(song.get("composed") or {})
    previous = composed.get("answer")
    if not isinstance(previous, dict):
        raise ValueError("This song was not written by the AI, so there is nothing to revise.")
    seed = int(song["seeds"]["song"])
    size = str(song.get("size", "sketch"))
    findings = review(song, setup, None, levels, problems) + [
        f for f in composed.get("findings", []) if "rules" in f or "range" in f or "grid" in f or "read" in f
    ]
    findings = list(dict.fromkeys(findings))
    heard = {}
    if isinstance(levels, dict):
        names = {p["id"]: p["name"] for p in setup.parts}
        heard = {names[k]: round(float(v), 1) for k, v in levels.items() if k in names and isinstance(v, int | float)}
    round_number = int(composed.get("round", 0)) + 1
    before = list((composed.get("usage") or {}).get("requests") or [])
    await say({"kind": "status", "text": f"Revising: the AI is revising the song (round {round_number})..."})
    text = first_message(setup) + "\n\n" + REVISE + "\n\n" + json.dumps({
        "findings": findings or ["Nothing stood out: improve what you think would make the song better."],
        "levelsDb": heard,
        "song": song_for_model(song, previous, setup),
    }, indent=1)  # fmt: skip
    spent = total_of(before, "input") + total_of(before, "output")
    asked = await ask(client, connections, routing, usage_log, text, effort, say, spent)
    records.add(asked.model, effort, len(setup.written()), "revise", asked.requests)
    requests = before + [{"round": round_number, **r} for r in asked.requests]

    def unchanged(why: str) -> dict[str, Any]:
        kept: dict[str, Any] = json.loads(json.dumps(song))
        kept["composed"]["usage"] = usage_of(requests)
        kept["composed"]["round"] = round_number
        kept["note"] = f"The AI's revision was not usable ({why}); the song before it stands."
        kept["plain"] = sw.describe(kept)
        return kept

    if asked.answer is None:
        return unchanged("it answered without a song")
    whole = merged(previous, asked.answer)
    try:
        checked = check_answer(whole, setup)
    except Unusable as why:
        return unchanged(str(why))
    revised = build_song(checked, setup, seed, size)
    composed = {
        "by": asked.by,
        "effort": effort,
        "answer": whole,
        "round": round_number,
        "usage": usage_of(requests),
        "continued": int(composed.get("continued", 0)) + asked.continued,
        "sounds": {**(composed.get("sounds") or {}), **checked.sounds},
        "filled": checked.filled,
        "reason": str(asked.answer.get("reason") or composed.get("reason") or "")[:400],
        "fixed": findings,
    }
    composed["findings"] = review(revised, setup, checked)
    revised["note"] = written_note(setup, composed)
    return finish(revised, setup, composed)


def written_note(setup: Setup, composed: dict[str, Any]) -> str:
    names = {p["id"]: p["name"] for p in setup.parts}
    usage = composed.get("usage") or {}
    words = f"Written by the AI ({composed['by']}"
    words += f", reasoning {composed['effort']})" if composed.get("effort") else ")"
    if composed.get("round"):
        words += f", revised {composed['round']} time{'s' if composed['round'] > 1 else ''}"
    words += (
        f". Tokens used: {int(usage.get('input', 0)):,} in, {int(usage.get('output', 0)):,} out"
        + (f" (of them {int(usage['reasoning']):,} reasoning)" if usage.get("reasoning") else "")
        + "."
    )
    if composed.get("continued"):
        words += f" Its answer was cut off at the model's length limit and continued ({composed['continued']}x)."
    given = (composed.get("answer") or {}).get("parts") or {}
    if composed.get("filled"):
        words += (
            " Written by Spinus's rules instead: "
            + ", ".join(
                f"{names.get(p, p)} ({'left out' if p not in given else 'not usable'})" for p in composed["filled"]
            )
            + "."
        )
    if composed.get("reason"):
        words += f" The idea: {composed['reason']}"
    return words


def redo_kept(
    song: dict[str, Any], summary: dict[str, Any], seed: int, part: str | None, section: str | None
) -> dict[str, Any]:
    """Redo part or section on the AI's song: the rules write what is redone, everything else stays as the AI
    wrote it."""
    again = sw.redo(song, summary, seed, part, section)
    if not song.get("composedBy"):
        return again
    old = song.get("layerParts") or {}
    for pid, by_section in again.get("layerParts", {}).items():
        if pid not in old:
            continue
        if part is not None and pid != part:
            again["layerParts"][pid] = old[pid]
        elif section is not None:
            again["layerParts"][pid] = {
                sid: (notes if sid == section else old[pid].get(sid, notes)) for sid, notes in by_section.items()
            }
    for key in ("composed", "composedBy", "sounds"):
        if key in song:
            again[key] = song[key]
    for s in again["sections"]:
        named = next((x for x in song["sections"] if x["id"] == s["id"]), None)
        if named is not None:
            s["name"], s["energy"] = named["name"], named["energy"]
    style = sw.Style.from_summary(summary)
    return sw.finish(again, summary, style)
