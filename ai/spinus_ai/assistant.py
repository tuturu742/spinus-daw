"""The embedded assistant: answers with the project and the manual, proposes changes as
commands for the user to apply (AGENTS rule 3: never applied by the AI)."""

from __future__ import annotations

import json
import logging
from collections.abc import Awaitable, Callable
from dataclasses import dataclass, field
from typing import Any

import httpx

from .connections import ConnectionStore
from .gateway import Completion, Message, ProviderError, Request, Text, Tool, ToolResult, ToolUse, Usage, complete
from .manual import Manual
from .project_harness import TOOL_NAMES as HARNESS_TOOL_NAMES
from .project_harness import TOOLS as HARNESS_TOOLS
from .project_harness import ProjectHarness
from .project_tools import Snapshot, Tools
from .routing import Route, Routing
from .usage import UsageLog

log = logging.getLogger("spinus_ai.assistant")

MAX_TURNS = 12

SYSTEM = """You are the assistant inside Spinus, a free digital audio workstation for Linux that works like \
FL Studio: a Channel Rack with step sequencer, a Piano Roll, a Playlist of pattern, audio and automation \
clips, and a Mixer of inserts with effects, sends and sidechains.

Who you talk to: musicians, often beginners. Answer in plain, friendly language, briefly. Use Spinus's \
and FL Studio's words (channel, pattern, insert, Piano Roll). No code, no JSON in your answers.

How to work:
- For questions about the user's song, look first: call song_overview (sections, parts, mixer), then read_part, \
sound_settings or insert_details as needed. Refer to things by name, not ID.
- For how-to questions about Spinus, call search_manual and answer from what it returns.
- You cannot change anything yourself. To change the song, use the editing tools: write_part, motif and \
place_motif, add_part, insert_bars ("add a bridge after bar 20"), copy_bars, automate, add_effect, send, duck, \
bus, set_sound and shape_sound ("make the bass pluckier"). They collect the changes; nothing happens to the \
song yet. Then call check, fix what matters, and call propose with one sentence: the user sees the changes as \
one proposal and clicks Apply or Reject. Never say a change was made; say what the proposal will do.
- For anything the editing tools can't do, propose Spinus commands with propose_changes (check their parameters \
with command_help first). To add an instrument or effect that way, call find_devices and copy its 'device' \
object exactly.
- To change a device's settings, use set_sound (or device.setParameters). For Spinus's own devices give 'text': \
each setting by its name as the manual's Controls table lists it (or its ID), with the value as the device shows \
it: "35%", "220 ms", "1.5 s", "-6 dB", "1.2 kHz", "4:1", "30% L", "Low pass". A plain number is in the units the \
device shows (percent, milliseconds, hertz, dB), never 0 to 1. 'values' are 0 to 1 over a setting's whole range: \
use them only for other plugins, whose units you cannot know.
- Make one proposal per answer, with everything it needs.
- To have the pattern generator suggest notes for one pattern in the Piano Roll (the user accepts or discards \
them there), call write_notes.
- Music facts: notes are in ticks, 960 per beat; FL note names (C5 = MIDI 60); volumes in dB.
"""


@dataclass
class Answer:
    text: str = ""
    proposals: list[dict[str, Any]] = field(default_factory=list)
    suggestions: list[dict[str, Any]] = field(default_factory=list)  # notes from write_notes, for the Piano Roll
    model: str = ""
    connection: str = ""
    usage: Usage = field(default_factory=Usage)

    def to_json(self) -> dict[str, Any]:
        return {
            "text": self.text,
            "proposals": self.proposals,
            "suggestions": self.suggestions,
            "model": self.model,
            "connection": self.connection,
            "usage": {
                "inputTokens": self.usage.input_tokens,
                "outputTokens": self.usage.output_tokens,
                "cost": self.usage.cost,
            },
        }


Say = Callable[[dict[str, Any]], Awaitable[None]]


class NoConnection(Exception):
    pass


async def ask(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routing: Routing,
    usage_log: UsageLog,
    manual: Manual,
    commands: dict[str, Any],
    conversation: list[dict[str, Any]],
    snapshot_data: dict[str, Any],
    say: Say,
) -> Answer:
    """Answers the last user message of `conversation` ([{role: user|assistant, text}])."""
    routes = routing.resolve("assistant")
    if not routes:
        raise NoConnection("No AI connection is set up. Add one in Options > AI.")

    tools = Tools(Snapshot(snapshot_data), manual, commands)
    harness = ProjectHarness(snapshot_data) if isinstance(snapshot_data.get("project"), dict) else None
    messages = [
        Message("user" if m.get("role") == "user" else "assistant", [Text(str(m.get("text", "")))])
        for m in conversation
        if str(m.get("text", "")).strip()
    ]
    if not messages or messages[-1].role != "user":
        raise ValueError("The conversation must end with the user's message.")

    answer = Answer()

    async def on_text(delta: str) -> None:
        answer.text += delta
        await say({"kind": "text", "text": delta})

    async def on_turn(completion: Completion, route: Route) -> None:
        answer.model, answer.connection = completion.model, route.connection.name
        answer.usage.input_tokens += completion.usage.input_tokens
        answer.usage.output_tokens += completion.usage.output_tokens
        if completion.usage.cost is not None:
            answer.usage.cost = (answer.usage.cost or 0.0) + completion.usage.cost
        usage_log.add(route.connection.id, "assistant", completion.usage)

    async def call(use: ToolUse) -> tuple[str, bool]:
        await say({"kind": "status", "text": describe_tool(use)})
        if use.name == "write_notes":
            return await write_notes(client, connections, routing, usage_log, tools.snapshot, use.input, answer, say)
        if use.name in HARNESS_TOOL_NAMES or use.name == "propose":
            if harness is None:
                return "The project wasn't shared with you: ask the user to turn on the Project chip.", True
            if use.name == "propose":
                proposal = harness.take_proposal(str(use.input.get("summary", "")))
                if proposal is None:
                    return "Nothing to propose: no edits since the last proposal.", True
                tools.proposals.append(proposal)
                return (
                    "The changes are shown to the user with Apply and Reject buttons. They are NOT applied yet. "
                    "Tell the user briefly what they do."
                ), False
            return harness.call(use.name, use.input)
        return tools.call(use.name, use.input)

    async def after_results() -> None:
        if answer.text and not answer.text.endswith("\n"):
            await on_text("\n\n")

    request = Request(routes[0].model, SYSTEM, messages, [*tools.definitions(), *HARNESS_TOOLS, PROPOSE, WRITE_NOTES])
    await tool_loop(client, connections, routes, request, call, on_turn, on_text, say, MAX_TURNS, after=after_results)

    answer.text = answer.text.strip()
    # Edits the model made but didn't propose still reach the user, as a proposal of their own.
    if harness is not None and (left := harness.take_proposal("")) is not None:
        tools.proposals.append(left)
    answer.proposals = tools.proposals
    return answer


ToolCall = Callable[[ToolUse], Awaitable[tuple[str, bool]]]
OnTurn = Callable[[Completion, Route], Awaitable[None]]


async def tool_loop(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routes: list[Route],
    request: Request,
    call: ToolCall,
    on_turn: OnTurn,
    on_text: Callable[[str], Awaitable[None]],
    say: Say,
    most_turns: int,
    stop: Callable[[], bool] = lambda: False,
    after: Callable[[], Awaitable[None]] | None = None,
) -> int:
    """The model's turns with tools: each answer is added to request.messages, its tool calls run (`call` gives
    each result and whether it failed) and their results sent back, until it answers without a tool, `stop()`
    says so (between turns) or `most_turns` have passed. A connection that fails gives way to the next route.
    Returns how many turns were taken."""
    route_index = 0
    turns = 0
    while turns < most_turns and not stop():
        turns += 1
        request.model = routes[route_index].model
        completion, route_index = await complete_with_fallback(
            client, connections, routes, route_index, request, on_text, say
        )
        await on_turn(completion, routes[route_index])
        request.messages.append(Message("assistant", completion.blocks or [Text("")]))
        uses = completion.tool_uses
        if not uses:
            break
        results: list[ToolResult] = []
        for use in uses:
            content, is_error = await call(use)
            results.append(ToolResult(use.id, content, is_error))
        request.messages.append(Message("user", list(results)))
        if after is not None:
            await after()
    return turns


async def complete_with_fallback(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routes: list[Route],
    start: int,
    request: Request,
    on_text: Callable[[str], Awaitable[None]],
    say: Say,
    failed: list[str] | None = None,
) -> tuple[Completion, int]:
    """The first route's answer, else the next's, and so on; each that failed is added to `failed`."""
    last: ProviderError | None = None
    for index in range(start, len(routes)):
        route = routes[index]
        request.model = route.model
        try:
            completion = await complete(
                client, route.connection, connections.key_for(route.connection), request, on_text
            )
            if completion.notice:
                await say({"kind": "status", "text": completion.notice})
            return completion, index
        except ProviderError as error:
            last = error
            if failed is not None:
                failed.append(f"{route.connection.name} ({route.model}) did not answer: {error}")
            log.warning(
                "%s (%s, %s) failed: %s: %s",
                route.connection.name,
                route.connection.provider,
                route.model,
                error.kind,
                error,
            )
            if not error.worth_falling_back or index + 1 == len(routes):
                raise
            await say(
                {
                    "kind": "status",
                    "text": f"{route.connection.name} did not answer ({error}); trying "
                    f"{routes[index + 1].connection.name}.",
                }
            )
    assert last is not None
    raise last


def answered_by(model: str, failed: list[str]) -> str:
    """Which model answered, and why it was not the one chosen: "model-b, because Work (model-a) did not
    answer: ..."."""
    return f"{model}, because {'; '.join(failed)}" if failed else model


PROPOSE = Tool(
    "propose",
    "Show the changes the editing tools collected (write_part, add_part, insert_bars, ...) to the user as one "
    "proposal to Apply or Reject.",
    {
        "type": "object",
        "properties": {"summary": {"type": "string", "description": "One sentence for the user: what it does."}},
        "required": ["summary"],
    },
)

WRITE_NOTES = Tool(
    "write_notes",
    "Have the pattern generator write notes for one or more channels in a pattern, or change their notes. "
    "The result is shown to the user in the Piano Roll as a suggestion (Accept or Discard); nothing changes "
    "until they accept. Use it for anything musical: melodies, bass lines, beats, chords, variations.",
    {
        "type": "object",
        "properties": {
            "request": {"type": "string", "description": "What to play or how to change it, in the user's words"},
            "pattern": {"type": "integer", "description": "The pattern's id"},
            "channels": {"type": "array", "items": {"type": "integer"}, "description": "Channel ids to write for"},
            "variations": {"type": "integer", "enum": [1, 4]},
            "adjust": {"type": "boolean", "description": "Change these channels' notes instead of writing new ones"},
        },
        "required": ["request", "pattern", "channels"],
    },
)

# The Spinus Synth Kit's pads (a Drum Machine's notes, when the project does not say otherwise).
DRUM_MACHINE_PADS = [
    {"name": name, "note": 60 + i}
    for i, name in enumerate(
        [
            "Kick",
            "Snare",
            "Clap",
            "Closed Hat",
            "Open Hat",
            "Low Tom",
            "Mid Tom",
            "High Tom",
            "Rim",
            "Cowbell",
            "Crash",
            "Shaker",
            "Kick 2",
            "Snare 2",
            "Ride",
            "Perc",
        ]
    )
]


def pattern_request(snapshot: Snapshot, arguments: dict[str, Any]) -> dict[str, Any]:
    """The pattern generator's request (ai.v1.pattern.generate's parameters) from the assistant's tool call."""
    from .patterns import TICKS_PER_STEP

    pattern = snapshot.find(snapshot.patterns(), int(arguments["pattern"]))
    if pattern is None:
        raise ValueError(f"There is no pattern {arguments['pattern']}.")
    wanted = {int(c) for c in arguments.get("channels", [])}
    notes_of = {str(n["channel"]): n.get("notes", []) for n in pattern.get("channels", []) if isinstance(n, dict)}

    def describe_channel(channel: dict[str, Any], with_id: bool) -> dict[str, Any]:
        instrument = channel.get("instrument") or {}
        result: dict[str, Any] = {
            "name": channel.get("name", ""),
            "instrument": snapshot.device(instrument) if instrument else "",
            "pads": DRUM_MACHINE_PADS if instrument.get("stockType") == "spinus.drum-machine" else [],
            "notes": notes_of.get(str(channel.get("id")), []),
        }
        if with_id:
            result["channel"] = channel.get("id")
        return result

    channels = snapshot.channels()
    targets = [describe_channel(c, True) for c in channels if c.get("id") in wanted]
    if not targets:
        raise ValueError("None of those channels exist.")
    context = [
        describe_channel(c, False) for c in channels if c.get("id") not in wanted and notes_of.get(str(c.get("id")))
    ]
    settings = snapshot.project.get("settings", {})
    signature = pattern.get("timeSignature") or settings.get("timeSignature", {})
    request: dict[str, Any] = {
        "request": str(arguments.get("request", "")),
        "targets": targets,
        "context": context,
        "tempo": settings.get("tempoBpm", 120),
        "beatsPerBar": signature.get("numerator", 4),
        "barTicks": int(signature.get("numerator", 4)) * 4 * 960 // max(1, int(signature.get("denominator", 4))),
        "timeSignature": f"{signature.get('numerator', 4)}/{signature.get('denominator', 4)}",
        "lengthTicks": pattern.get("length", 16 * TICKS_PER_STEP),
        "variations": 4 if int(arguments.get("variations", 1)) > 1 else 1,
        "adjust": bool(arguments.get("adjust", False)),
    }
    if isinstance(settings.get("key"), dict):
        request["key"] = settings["key"]
    return request


async def write_notes(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routing: Routing,
    usage_log: UsageLog,
    snapshot: Snapshot,
    arguments: dict[str, Any],
    answer: Answer,
    say: Say,
) -> tuple[str, bool]:
    """Runs the pattern generator for the assistant; its notes become the answer's suggestion."""
    from .patterns import Settings, generate

    try:
        request = pattern_request(snapshot, arguments)
        result = await generate(client, connections, routing, usage_log, Settings.from_json(request), say)
    except (KeyError, TypeError, ValueError, ProviderError, NoConnection) as error:
        return f"The pattern generator could not write it: {error}", True

    answer.suggestions = [{"pattern": int(arguments["pattern"]), "variations": result["variations"]}]
    names = {str(c.get("id")): c.get("name", "") for c in snapshot.channels()}
    summary = "; ".join(
        f"'{v.get('title') or 'variation'}': "
        + ", ".join(f"{len(notes)} notes for {names.get(channel, channel)}" for channel, notes in v["parts"].items())
        for v in result["variations"]
    )
    return (
        f"Written ({summary}). The user now sees it in the Piano Roll as a suggestion to accept or discard; "
        "tell them so briefly, and what you asked for.",
        False,
    )


def describe_tool(use: ToolUse) -> str:
    names = {
        "project_overview": "Looking at the project",
        "pattern_notes": "Reading notes",
        "insert_details": "Looking at the mixer",
        "search_manual": "Searching the manual",
        "command_help": "Checking a command",
        "find_devices": "Looking for instruments and effects",
        "propose_changes": "Preparing a proposal",
        "write_notes": "Writing notes",
        "song_overview": "Looking at the song",
        "read_part": "Reading a part",
        "sound_settings": "Looking at a sound",
        "write_part": "Writing a part",
        "motif": "Writing a motif",
        "place_motif": "Placing a motif",
        "add_part": "Adding a part",
        "insert_bars": "Making room for a section",
        "copy_bars": "Copying bars",
        "automate": "Drawing automation",
        "add_effect": "Adding an effect",
        "send": "Setting a send",
        "duck": "Setting sidechain ducking",
        "bus": "Making a bus",
        "set_sound": "Changing a sound",
        "shape_sound": "Shaping a sound",
        "check": "Checking the song",
        "propose": "Preparing a proposal",
    }
    detail = use.input.get("query") if use.name == "search_manual" else None
    return names.get(use.name, use.name) + (f": {json.dumps(detail)}" if detail else "")
