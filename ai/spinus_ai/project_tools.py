"""The assistant's read tools, answered from the snapshot Spinus sends with each question,
never from the live project. Plus `propose_changes`, which only records a proposal."""

from __future__ import annotations

import json
from typing import Any

from .gateway import Tool
from .manual import Manual
from .schema_check import problems

TICKS_PER_BEAT = 960
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(pitch: int) -> str:
    """FL's naming: MIDI 60 is C5."""
    return f"{NOTE_NAMES[pitch % 12]}{pitch // 12}"


class Snapshot:
    """What Spinus sent: the project (project.json), the selection, device names and the context chips."""

    def __init__(self, data: dict[str, Any]) -> None:
        self.project: dict[str, Any] = data.get("project") or {}
        self.selection: dict[str, Any] = data.get("selection") or {}
        self.device_names: dict[str, str] = {str(k): str(v) for k, v in (data.get("deviceNames") or {}).items()}
        self.chips: list[str] = list(data.get("chips") or ["project", "channel", "pattern", "insert"])
        # What can be added (Spinus's devices, free plugins, installed plugins), with each one's device JSON.
        self.devices: list[dict[str, Any]] = [d for d in data.get("devices") or [] if isinstance(d, dict)]

    def device(self, device: dict[str, Any] | None) -> str:
        if not device:
            return "none"
        name = self.device_names.get(str(device.get("id")))
        if name:
            return name
        if device.get("kind") == "plugin":
            return str(device.get("plugin", {}).get("name", "plugin"))
        return str(device.get("stockType", "device"))

    def channels(self) -> list[dict[str, Any]]:
        return list(self.project.get("channels", []))

    def patterns(self) -> list[dict[str, Any]]:
        return list(self.project.get("patterns", []))

    def inserts(self) -> list[dict[str, Any]]:
        mixer = self.project.get("mixer", {})
        return [mixer["master"], *mixer.get("inserts", [])] if "master" in mixer else list(mixer.get("inserts", []))

    def find(self, items: list[dict[str, Any]], item_id: Any) -> dict[str, Any] | None:
        return next((i for i in items if i.get("id") == item_id), None)

    def name_of_insert(self, insert_id: Any) -> str:
        insert = self.find(self.inserts(), insert_id)
        return f"{insert.get('name') or 'Insert'} (insert {insert_id})" if insert else f"insert {insert_id}"

    # -----------------------------------------------------------------------------------------------

    def overview(self) -> dict[str, Any]:
        settings = self.project.get("settings", {})
        clips = self.project.get("playlist", {}).get("clips", [])
        song_end = max((c.get("start", 0) + c.get("length", 0) for c in clips), default=0)
        return {
            "tempoBpm": settings.get("tempoBpm"),
            "timeSignature": settings.get("timeSignature"),
            "key": settings.get("key"),
            "title": settings.get("title", ""),
            "songLengthBeats": round(song_end / TICKS_PER_BEAT, 2),
            "selection": self.selection,
            "channels": [
                {
                    "id": c["id"],
                    "name": c.get("name"),
                    "instrument": self.device(c.get("instrument")),
                    "insert": self.name_of_insert(c.get("targetInsert", 1)),
                    "volumeDb": c.get("volumeDb", 0),
                    "pan": c.get("pan", 0),
                    "muted": c.get("muted", False),
                }
                for c in self.channels()
            ],
            "patterns": [
                {
                    "id": p["id"],
                    "name": p.get("name"),
                    "lengthBeats": p.get("length", 0) / TICKS_PER_BEAT,
                    "notesPerChannel": {str(c["channel"]): len(c.get("notes", [])) for c in p.get("channels", [])},
                }
                for p in self.patterns()
            ],
            "inserts": [
                {
                    "id": i["id"],
                    "name": i.get("name"),
                    "effects": [self.device(e.get("device")) for e in i.get("effects", [])],
                    "volumeDb": i.get("volumeDb", 0),
                    "muted": i.get("muted", False),
                    "output": self.name_of_insert(i["output"]) if "output" in i else None,
                    "sends": [
                        {"to": self.name_of_insert(s["target"]), "levelDb": s.get("levelDb")}
                        for s in i.get("sends", [])
                    ],
                    "sidechainFrom": [self.name_of_insert(s) for s in i.get("sidechainSources", [])],
                }
                for i in self.inserts()
            ],
            "playlist": {"lanes": len(self.project.get("playlist", {}).get("lanes", [])), "clips": len(clips)},
        }

    def find_devices(self, query: str, kind: str) -> list[dict[str, Any]]:
        words = [w for w in query.lower().split() if w]
        found = []
        for d in self.devices:
            if kind in ("instrument", "effect") and d.get("type") != kind:
                continue
            text = f"{d.get('name', '')} {d.get('maker', '')} {d.get('format', '')}".lower()
            if all(w in text for w in words):
                found.append(d)
        return found[:25]

    def device_problem(self, device: dict[str, Any]) -> str | None:
        """Why a device in a proposal cannot be used (None: it can). Unknown when Spinus sent no list."""
        if not self.devices:
            return None
        if device.get("kind") == "stock":
            match = next(
                (d for d in self.devices if d.get("device", {}).get("stockType") == device.get("stockType")), None
            )
            if match is None:
                return f"there is no device with stockType '{device.get('stockType')}' (use find_devices)"
            if not match.get("available", True):
                return (
                    f"{match.get('name')} is not downloaded yet: tell the user to get it in Options > Get free "
                    "plugins, or choose another device"
                )
            return None
        name = device.get("plugin", {}).get("name")
        if not any(d.get("device", {}).get("plugin", {}).get("name") == name for d in self.devices):
            return f"there is no installed plugin named '{name}' (use find_devices)"
        return None

    def pattern_notes(self, pattern_id: int, channel_id: int) -> dict[str, Any]:
        pattern = self.find(self.patterns(), pattern_id)
        if pattern is None:
            return {"error": f"There is no pattern {pattern_id}."}
        entry = next((c for c in pattern.get("channels", []) if c.get("channel") == channel_id), None)
        notes = entry.get("notes", []) if entry else []
        return {
            "pattern": pattern.get("name"),
            "channel": channel_id,
            "lengthBeats": pattern.get("length", 0) / TICKS_PER_BEAT,
            "notes": [
                {
                    "startBeat": n.get("start", 0) / TICKS_PER_BEAT,
                    "lengthBeats": n.get("length", 0) / TICKS_PER_BEAT,
                    "pitch": n.get("pitch"),
                    "name": note_name(int(n.get("pitch", 60))),
                    "velocity": n.get("velocity", 0.8),
                }
                for n in notes
            ],
            "raw": notes,
        }

    def insert_details(self, insert_id: int) -> dict[str, Any]:
        insert = self.find(self.inserts(), insert_id)
        if insert is None:
            return {"error": f"There is no insert {insert_id}."}
        details = dict(insert)
        details["effectNames"] = [self.device(e.get("device")) for e in insert.get("effects", [])]
        details["fedBy"] = [c.get("name") for c in self.channels() if c.get("targetInsert", 1) == insert_id]
        return details


# ---------------------------------------------------------------------------------------------------


# The IDs each command that creates something returns, for "$<step>.<field>" references (Spinus's
# CommandRegistry getResult(); tests/ai/AiTests.cpp checks this table against it).
RESULT_FIELDS: dict[str, tuple[str, ...]] = {
    "channel.add": ("channel", "insert", "device"),
    "insert.add": ("insert",),
    "insert.addEffect": ("device",),
    "pattern.add": ("pattern",),
    "lane.add": ("lane",),
    "clip.addPattern": ("clip",),
    "clip.addAudio": ("clip", "channel", "insert"),
    "clip.addAutomation": ("clip", "lane"),
    "launcher.addTrack": ("track",),
    "launcher.addScene": ("scene",),
    "launcher.addPattern": ("clip",),
    "launcher.addAudio": ("clip", "channel", "insert"),
}


def references_in(value: Any) -> list[str]:
    if isinstance(value, str):
        return [value] if value.startswith("$") and "." in value else []
    if isinstance(value, dict):
        return [r for item in value.values() for r in references_in(item)]
    if isinstance(value, list):
        return [r for item in value for r in references_in(item)]
    return []


def devices_in(value: Any) -> list[dict[str, Any]]:
    """Device objects ({kind: stock|plugin, ...}) anywhere in a step's parameters."""
    if isinstance(value, dict):
        found = [value] if value.get("kind") in ("stock", "plugin") else []
        for item in value.values():
            found.extend(devices_in(item))
        return found
    if isinstance(value, list):
        return [d for item in value for d in devices_in(item)]
    return []


class Tools:
    """The tools the assistant may call, and their answers."""

    def __init__(self, snapshot: Snapshot, manual: Manual, commands: dict[str, Any]) -> None:
        self.snapshot = snapshot
        self.manual = manual
        self.commands = commands  # name -> {summary, parameters (JSON schema)}
        self.proposals: list[dict[str, Any]] = []

    def definitions(self) -> list[Tool]:
        command_list = "; ".join(f"{name}: {c.get('summary', '')}" for name, c in self.commands.items())
        return [
            Tool(
                "project_overview",
                "The project: tempo, channels (with their instruments and mixer inserts), "
                "patterns, mixer inserts (effects, routing, sends) and the user's selection. Call this first.",
                {"type": "object", "properties": {}},
            ),
            Tool(
                "pattern_notes",
                "The notes one channel plays in one pattern (beats, pitch names in FL style: C5 = "
                "MIDI 60, velocity 0-1). 'raw' has the notes as the pattern.setNotes command takes them (ticks, "
                "960 per beat).",
                {
                    "type": "object",
                    "properties": {"pattern": {"type": "integer"}, "channel": {"type": "integer"}},
                    "required": ["pattern", "channel"],
                },
            ),
            Tool(
                "insert_details",
                "Everything about one mixer insert (1 is the master): effects with device IDs, "
                "volume, pan, routing, sends, sidechain, and which channels feed it.",
                {"type": "object", "properties": {"insert": {"type": "integer"}}, "required": ["insert"]},
            ),
            Tool(
                "search_manual",
                "Search the Spinus user manual. Use it for how-to questions about Spinus.",
                {"type": "object", "properties": {"query": {"type": "string"}}, "required": ["query"]},
            ),
            Tool(
                "command_help",
                "The exact parameters of a Spinus command, before proposing it. Commands: " + command_list,
                {"type": "object", "properties": {"command": {"type": "string"}}, "required": ["command"]},
            ),
            Tool(
                "find_devices",
                "Instruments and effects that can be added (Spinus's own, the free open-source ones, installed "
                "plugins), searched by words in the name, maker or format. Each result has the exact 'device' "
                "object to put in channel.add or insert.addEffect. Never invent a device.",
                {
                    "type": "object",
                    "properties": {
                        "query": {"type": "string"},
                        "type": {"type": "string", "enum": ["instrument", "effect", "any"]},
                    },
                },
            ),
            Tool(
                "propose_changes",
                "Propose changes to the project. Nothing changes until the user clicks Apply. Each step is a "
                "Spinus command with its parameters (see command_help). A parameter may use an earlier step's "
                'result as "$<step number>.<field>". Results: '
                + "; ".join(f"{c} gives {', '.join(fs)}" for c, fs in RESULT_FIELDS.items())
                + ' (e.g. "$1.insert" for the insert step 1 adds). Times are ticks (960 per beat).',
                {
                    "type": "object",
                    "properties": {
                        "summary": {"type": "string", "description": "One sentence for the user: what it does."},
                        "steps": {
                            "type": "array",
                            "items": {
                                "type": "object",
                                "properties": {"command": {"type": "string"}, "parameters": {"type": "object"}},
                                "required": ["command", "parameters"],
                            },
                        },
                    },
                    "required": ["summary", "steps"],
                },
            ),
        ]

    def call(self, name: str, arguments: dict[str, Any]) -> tuple[str, bool]:
        """The tool's answer as text, and whether it is an error."""
        try:
            if name == "project_overview":
                return json.dumps(self.snapshot.overview()), False
            if name == "pattern_notes":
                return json.dumps(
                    self.snapshot.pattern_notes(int(arguments["pattern"]), int(arguments["channel"]))
                ), False
            if name == "insert_details":
                return json.dumps(self.snapshot.insert_details(int(arguments["insert"]))), False
            if name == "find_devices":
                devices = self.snapshot.find_devices(str(arguments.get("query", "")), str(arguments.get("type", "any")))
                if not devices:
                    return "No such device. Try fewer or other words.", False
                return json.dumps(devices), False
            if name == "search_manual":
                found = self.manual.search(str(arguments.get("query", "")))
                if not found:
                    return "Nothing found in the manual.", False
                return "\n\n".join(f"[{s.page}] {s.title}\n{s.text[:1500]}" for s in found), False
            if name == "command_help":
                command = self.commands.get(str(arguments.get("command")))
                if command is None:
                    return f"Unknown command. Known: {', '.join(self.commands)}", True
                return json.dumps(command), False
            if name == "propose_changes":
                return self.propose(arguments)
        except (KeyError, ValueError, TypeError) as error:
            return f"Bad arguments for {name}: {error}", True
        return f"Unknown tool {name}.", True

    def propose(self, arguments: dict[str, Any]) -> tuple[str, bool]:
        steps = arguments.get("steps")
        if not isinstance(steps, list) or not steps:
            return "A proposal needs at least one step.", True
        unknown = [s.get("command") for s in steps if not isinstance(s, dict) or s.get("command") not in self.commands]
        if unknown:
            return f"Unknown commands: {unknown}. Use command_help.", True
        mistakes = []
        for number, step in enumerate(steps, start=1):
            schema = self.commands[step["command"]].get("parameters", {})
            for problem in problems(step.get("parameters", {}), schema):
                mistakes.append(f"step {number} ({step['command']}): {problem}")
            for reference in references_in(step.get("parameters", {})):
                target, _, field = reference[1:].partition(".")
                earlier = steps[int(target) - 1]["command"] if target.isdigit() and 0 < int(target) < number else None
                fields = RESULT_FIELDS.get(earlier or "", ())
                if earlier is None:
                    mistakes.append(f"step {number}: {reference} must name an earlier step")
                elif field not in fields:
                    known = ", ".join(f"${target}.{f}" for f in fields) or "nothing (it creates no new object)"
                    mistakes.append(f"step {number}: {reference} is not a result of {earlier}; it gives {known}")
            for device in devices_in(step.get("parameters", {})):
                if (device_issue := self.snapshot.device_problem(device)) is not None:
                    mistakes.append(f"step {number} ({step['command']}): {device_issue}")
        if mistakes:
            return "Not proposed, fix these and propose again: " + "; ".join(mistakes), True
        self.proposals.append({"summary": str(arguments.get("summary", "")), "steps": steps})
        return (
            "The proposal is shown to the user with Apply and Reject buttons. It is NOT applied yet. Tell "
            "the user briefly what it does; do not claim it was done."
        ), False
