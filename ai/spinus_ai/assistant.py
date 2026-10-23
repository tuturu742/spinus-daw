"""The embedded assistant: answers with the project and the manual, proposes changes as
commands for the user to apply (AGENTS rule 3: never applied by the AI)."""

from __future__ import annotations

import json
from collections.abc import Awaitable, Callable
from dataclasses import dataclass, field
from typing import Any

import httpx

from .connections import ConnectionStore
from .gateway import Completion, Message, ProviderError, Request, Text, Tool, ToolResult, ToolUse, Usage, complete
from .manual import Manual
from .project_tools import Snapshot, Tools
from .routing import Route, Routing
from .usage import UsageLog

MAX_TURNS = 12

SYSTEM = """You are the assistant inside Spinus, a free digital audio workstation for Linux that works like \
FL Studio: a Channel Rack with step sequencer, a Piano Roll, a Playlist of pattern, audio and automation \
clips, and a Mixer of inserts with effects, sends and sidechains.

Who you talk to: musicians, often beginners. Answer in plain, friendly language, briefly. Use Spinus's \
and FL Studio's words (channel, pattern, insert, Piano Roll). No code, no JSON in your answers.

How to work:
- For questions about the user's song, look first: call project_overview, then pattern_notes or \
insert_details as needed. Refer to things by name, not ID.
- For how-to questions about Spinus, call search_manual and answer from what it returns.
- You cannot change anything yourself. To change the song, call propose_changes with Spinus commands \
(check their parameters with command_help first). The user sees the steps and clicks Apply or Reject. \
Never say a change was made; say what the proposal will do if they apply it.
- Make one proposal per answer, with everything it needs in the right order.
- To add an instrument or effect, call find_devices and copy its 'device' object exactly.
- To change a device's settings, use device.setParameters. For Spinus's own devices (stockType spinus.*) \
give 'text': each setting by its name as the manual's Controls table lists it (or its ID), with the value as \
the device shows it: "35%", "220 ms", "1.5 s", "-6 dB", "1.2 kHz", "4:1", "30% L", "Low pass". A plain number \
is in the units the device shows (percent, milliseconds, hertz, dB), never 0 to 1. 'values' are 0 to 1 over a \
setting's whole range: use them only for other plugins, whose units you cannot know.
- To write or change notes musically (a bass line, a beat, chords, "more syncopated"), call write_notes: \
the pattern generator writes them and the user sees them in the Piano Roll as a suggestion to accept or \
discard. Prefer it to proposing pattern.setNotes yourself.
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
    messages = [
        Message("user" if m.get("role") == "user" else "assistant", [Text(str(m.get("text", "")))])
        for m in conversation
        if str(m.get("text", "")).strip()
    ]
    if not messages or messages[-1].role != "user":
        raise ValueError("The conversation must end with the user's message.")

    answer = Answer()
    route_index = 0

    async def on_text(delta: str) -> None:
        answer.text += delta
        await say({"kind": "text", "text": delta})

    for _ in range(MAX_TURNS):
        request = Request(routes[route_index].model, SYSTEM, messages, [*tools.definitions(), WRITE_NOTES])
        completion, route_index = await complete_with_fallback(
            client, connections, routes, route_index, request, on_text, say
        )
        route = routes[route_index]
        answer.model, answer.connection = completion.model, route.connection.name
        answer.usage.input_tokens += completion.usage.input_tokens
        answer.usage.output_tokens += completion.usage.output_tokens
        if completion.usage.cost is not None:
            answer.usage.cost = (answer.usage.cost or 0.0) + completion.usage.cost
        usage_log.add(route.connection.id, "assistant", completion.usage)

        messages.append(Message("assistant", completion.blocks or [Text("")]))
        uses = completion.tool_uses
        if not uses:
            break
        results: list[ToolResult] = []
        for use in uses:
            await say({"kind": "status", "text": describe_tool(use)})
            if use.name == "write_notes":
                content, is_error = await write_notes(
                    client, connections, routing, usage_log, tools.snapshot, use.input, answer, say
                )
            else:
                content, is_error = tools.call(use.name, use.input)
            results.append(ToolResult(use.id, content, is_error))
        messages.append(Message("user", list(results)))
        if answer.text and not answer.text.endswith("\n"):
            await on_text("\n\n")

    answer.text = answer.text.strip()
    answer.proposals = tools.proposals
    return answer


async def complete_with_fallback(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routes: list[Route],
    start: int,
    request: Request,
    on_text: Callable[[str], Awaitable[None]],
    say: Say,
) -> tuple[Completion, int]:
    last: ProviderError | None = None
    for index in range(start, len(routes)):
        route = routes[index]
        request.model = route.model
        try:
            return await complete(
                client, route.connection, connections.key_for(route.connection), request, on_text
            ), index
        except ProviderError as error:
            last = error
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
    }
    detail = use.input.get("query") if use.name == "search_manual" else None
    return names.get(use.name, use.name) + (f": {json.dumps(detail)}" if detail else "")
