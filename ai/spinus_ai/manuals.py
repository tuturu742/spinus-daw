"""Reading an instrument's manuals (Learn this instrument's Read the manual): the text of each manual (a PDF
through pypdf, a web page, a text file), the passages about keys and modes, and the AI's reading of them as a
proposal: the instrument's modes, how to enter each, what each suits and what its key zones do. Spinus shows it
for the user to Apply. Only facts come back (key numbers, names, a few words); the manual's text is sent to the
AI the user chose and kept nowhere.

Manuals number octaves differently (middle C, MIDI note 60, is C3 in many, C4 in others), so the AI says which
the manual uses when the manual says it, and every key comes back as a MIDI note number."""

from __future__ import annotations

import io
import logging
import re
from dataclasses import dataclass
from html.parser import HTMLParser
from pathlib import Path
from typing import Any

import httpx

from .assistant import NoConnection, Say, answered_by, complete_with_fallback
from .connections import ConnectionStore
from .gateway import Message, Request, Text, Tool
from .routing import Routing
from .usage import UsageLog

MOST_BYTES = 40 * 1024 * 1024  # a manual larger than this is not read
MOST_CHARS = 60_000  # what the AI is given of all the manuals together
OVERVIEW_CHARS = 3_000  # the start of each manual is always kept: what the instrument is
MOST_MANUALS = 6
JOBS = ("plays", "switches", "special", "unused")
VOICES = ("unknown", "chords", "one")

# Generic words that mark a passage about keys and modes (no vendor's names).
KEY_WORDS = (
    "key switch",
    "keyswitch",
    "key-switch",
    "articulation",
    "mode",
    "strum",
    "chord",
    "pattern",
    "range",
    "midi",
    "note",
    "key",
    "trigger",
    "mute",
    "slide",
    "legato",
    "velocity",
    "playable",
    "keyboard",
    "zone",
)
NOTE = re.compile(r"\b[A-G](?:#|b)?-?\d\b")


class ManualError(Exception):
    """A manual that could not be read; the message says why, for the user."""


@dataclass
class Document:
    source: str
    title: str
    text: str
    pages: int


# --- The text of a manual -------------------------------------------------------------------------------------


class _TextOfHtml(HTMLParser):
    """A web page's readable text: no scripts, styles or navigation; a line per block."""

    SKIP = frozenset(("script", "style", "noscript", "nav", "footer", "header", "svg", "form"))
    BLOCKS = frozenset(("p", "div", "br", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6", "section", "table"))

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.parts: list[str] = []
        self.title = ""
        self.skipping = 0
        self.in_title = False

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag in self.SKIP:
            self.skipping += 1
        elif tag == "title":
            self.in_title = True
        elif tag in self.BLOCKS:
            self.parts.append("\n")

    def handle_endtag(self, tag: str) -> None:
        if tag in self.SKIP and self.skipping:
            self.skipping -= 1
        elif tag == "title":
            self.in_title = False
        elif tag in self.BLOCKS:
            self.parts.append("\n")

    def handle_data(self, data: str) -> None:
        if self.in_title:
            self.title += data
        elif not self.skipping:
            self.parts.append(data)

    def text(self) -> str:
        lines = (" ".join(line.split()) for line in "".join(self.parts).splitlines())
        return "\n".join(line for line in lines if line)


def text_of_html(html: str) -> tuple[str, str]:
    """(title, text) of a web page."""
    parser = _TextOfHtml()
    parser.feed(html)
    parser.close()
    return " ".join(parser.title.split()), parser.text()


def text_of_pdf(data: bytes) -> tuple[str, list[str]]:
    """(title, the text of each page) of a PDF."""
    try:
        from pypdf import PdfReader
        from pypdf.errors import PdfReadError
    except ImportError as error:
        raise ManualError("Reading PDF manuals needs the pypdf package (Arch: python-pypdf).") from error
    logging.getLogger("pypdf").setLevel(logging.ERROR)  # its notes on a PDF's small faults are not the user's
    try:
        reader = PdfReader(io.BytesIO(data))
        if reader.is_encrypted and not reader.decrypt(""):
            raise ManualError("the PDF is locked with a password.")
        pages = [page.extract_text() or "" for page in reader.pages]
        metadata = reader.metadata
        title = str(metadata.get("/Title") or "").strip() if metadata is not None else ""
    except PdfReadError as error:
        raise ManualError(f"the PDF could not be read ({error}).") from error
    if not any(p.strip() for p in pages):
        raise ManualError("the PDF has no text in it (scanned pages are pictures; Spinus can't read those).")
    return title, pages


def document_of(source: str, data: bytes, content_type: str = "") -> Document:
    """A manual's text from its bytes: a PDF, a web page or plain text, by its type or its name."""
    name = source.lower().split("?")[0]
    kind = content_type.split(";")[0].strip().lower()
    if data[:5] == b"%PDF-" or kind == "application/pdf" or name.endswith(".pdf"):
        title, pages = text_of_pdf(data)
        return Document(source, title, "\n\f\n".join(pages), len(pages))
    text = data.decode("utf-8", errors="replace")
    if (
        "html" in kind
        or name.endswith((".html", ".htm"))
        or text.lstrip()[:15].lower().startswith(("<!doctype", "<html"))
    ):
        title, body = text_of_html(text)
        return Document(source, title, body, 1)
    return Document(source, "", text, 1)


async def read_document(client: httpx.AsyncClient, manual: dict[str, Any]) -> Document:
    """One manual: {"file": path} or {"url": address}."""
    if file := str(manual.get("file") or ""):
        path = Path(file)
        if not path.is_file():
            raise ManualError(f"there is no file at {file}.")
        if path.stat().st_size > MOST_BYTES:
            raise ManualError("the file is too large to be a manual.")
        return document_of(file, path.read_bytes())
    url = str(manual.get("url") or "").strip()
    if not url.startswith(("https://", "http://")):
        raise ManualError("a web page's address starts with https://.")
    try:
        async with client.stream("GET", url, follow_redirects=True, timeout=60.0) as response:
            if response.status_code != 200:
                raise ManualError(f"the page answered {response.status_code}.")
            data = bytearray()
            async for chunk in response.aiter_bytes():
                data += chunk
                if len(data) > MOST_BYTES:
                    raise ManualError("the page is too large to be a manual.")
            return document_of(url, bytes(data), response.headers.get("content-type", ""))
    except httpx.HTTPError as error:
        raise ManualError(f"the page could not be fetched ({error}).") from error


# --- What the AI is given ---------------------------------------------------------------------------------------


def score(passage: str) -> float:
    """How much a passage says about keys and modes: generic words and note names, per its length."""
    lower = passage.lower()
    words = sum(lower.count(w) for w in KEY_WORDS)
    notes = len(NOTE.findall(passage))
    return (words + 3 * notes) / (1 + len(passage) / 400)


def relevant(documents: list[Document], budget: int = MOST_CHARS) -> str:
    """The manuals' text for the AI: the start of each (what the instrument is), then the passages that say most
    about keys and modes, in their order, up to `budget` characters in all."""
    blocks: list[tuple[int, int, str]] = []  # (document, position, text)
    used = 0
    chosen: list[tuple[float, int, int, str]] = []
    for d, doc in enumerate(documents):
        overview = doc.text[:OVERVIEW_CHARS]
        blocks.append((d, -1, overview))
        used += len(overview)
        passages = [p.strip() for p in re.split(r"\n\s*\n|\f", doc.text[OVERVIEW_CHARS:]) if p.strip()]
        chosen += [(score(p), d, i, p) for i, p in enumerate(passages)]
    for s, d, i, p in sorted(chosen, key=lambda c: -c[0]):
        if s <= 0 or used + len(p) > budget:
            continue
        blocks.append((d, i, p))
        used += len(p)
    parts: list[str] = []
    for d, doc in enumerate(documents):
        mine = sorted((b for b in blocks if b[0] == d), key=lambda b: b[1])
        heading = f"=== Manual {d + 1}: {doc.title or doc.source} ==="
        parts.append(heading + "\n" + "\n\n".join(b[2] for b in mine))
    return "\n\n".join(parts)


# --- The AI's reading -------------------------------------------------------------------------------------------

SYSTEM = (
    "You read the manuals of a software instrument (a sampled guitar, bass, piano, drum kit or synth) for a "
    "music program that writes parts for it. Say only what the manuals say: the instrument's playing modes, how "
    "to enter each, what each suits, whether it plays chords, and what the keys of each mode do: which play "
    "notes, which are key switches (articulation or mode changes such as palm mute, slide, harmonics), which "
    "have another job (chord keys, strum or pattern keys, mutes, noises) and which do nothing. Give keys as the "
    "manual names them (note names such as C#6, in the manual's own octave numbering), and say which note the "
    "manual calls middle C (MIDI note 60) when it says so. Leave out what the manuals don't say. Answer with the "
    "read_manual tool."
)

ZONE = {
    "type": "object",
    "properties": {
        "from": {"type": "string", "description": "The zone's lowest key, as the manual names it (e.g. 'E1')"},
        "to": {"type": "string", "description": "Its highest key (the same as from for one key)"},
        "job": {"type": "string", "enum": list(JOBS)},
        "what": {"type": "string", "description": "What the keys do, in a few words ('palm mute'); empty to play"},
    },
    "required": ["from", "to", "job"],
}

READ_MANUAL = Tool(
    "read_manual",
    "What the manuals say about the instrument's modes and keys.",
    {
        "type": "object",
        "properties": {
            "middleC": {
                "type": "string",
                "enum": ["C3", "C4", "C5", "unknown"],
                "description": "What the manual calls MIDI note 60 (middle C), if it says; else unknown",
            },
            "modes": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "name": {"type": "string", "description": "The manual's name for the mode ('Strummer')"},
                        "enter": {
                            "type": "string",
                            "description": "How to enter it, in a few words ('the Strummer button'); empty when the "
                            "instrument opens in it",
                        },
                        "enterKey": {
                            "type": "string",
                            "description": "The key that switches to it, as the manual names it; empty when none",
                        },
                        "goodFor": {"type": "string", "description": "Which parts it suits, in a few words"},
                        "voices": {"type": "string", "enum": list(VOICES)},
                        "zones": {"type": "array", "items": ZONE},
                    },
                    "required": ["name", "zones"],
                },
            },
            "notes": {"type": "string", "description": "Anything the user should know, in one sentence"},
        },
        "required": ["middleC", "modes"],
    },
)

NOTE_NAME = re.compile(r"^\s*([A-Ga-g])\s*(#|b|♯|♭)?\s*(-?\d{1,2})\s*$")
PITCH = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11}


def words(text: Any, most: int) -> str:
    """Up to `most` characters of a text, cut at a word."""
    plain = " ".join(str(text or "").split())
    if len(plain) <= most:
        return plain
    cut = plain[:most].rsplit(" ", 1)[0]
    return cut.rstrip(",;:(") + "..."


def midi_of(name: str, middle_c: str) -> int | None:
    """The MIDI note of a note name in a manual whose middle C (60) is `middle_c` ('C3': C3 is 60)."""
    match = NOTE_NAME.match(str(name))
    if match is None:
        return None
    letter, accidental, octave = match.groups()
    pitch = PITCH[letter.lower()] + (1 if accidental in ("#", "♯") else -1 if accidental in ("b", "♭") else 0)
    note = 60 + (int(octave) - int(middle_c[1:])) * 12 + pitch
    return note if 0 <= note <= 127 else None


def checked_reading(answer: dict[str, Any]) -> dict[str, Any] | None:
    """The AI's answer as Spinus takes it: every key a MIDI note, every field checked; None when it says nothing
    usable. When the manual doesn't say its middle C, C3 is taken (the most common) and `middleCSaid` is false, so
    Spinus may move the zones by octaves to fit what it heard."""
    said = answer.get("middleC") in ("C3", "C4", "C5")
    middle_c = str(answer["middleC"]) if said else "C3"
    modes: list[dict[str, Any]] = []
    for mode in answer.get("modes") or []:
        if not isinstance(mode, dict) or not str(mode.get("name") or "").strip():
            continue
        zones = []
        for zone in mode.get("zones") or []:
            if not isinstance(zone, dict) or zone.get("job") not in JOBS:
                continue
            low = midi_of(str(zone.get("from", "")), middle_c)
            high = midi_of(str(zone.get("to") or zone.get("from", "")), middle_c)
            if low is None or high is None:
                continue
            zones.append(
                {
                    "from": min(low, high),
                    "to": max(low, high),
                    "job": zone["job"],
                    "what": words(zone.get("what"), 100),
                }
            )
        enter_key = midi_of(str(mode.get("enterKey") or ""), middle_c)
        modes.append(
            {
                "name": words(mode["name"], 40),
                "enter": words(mode.get("enter"), 80),
                "enterKey": -1 if enter_key is None else enter_key,
                "goodFor": words(mode.get("goodFor"), 120),
                "voices": mode.get("voices") if mode.get("voices") in VOICES else "unknown",
                "zones": zones,
            }
        )
        if len(modes) == 8:
            break
    if not modes:
        return None
    return {"modes": modes, "middleC": middle_c, "middleCSaid": said, "notes": words(answer.get("notes"), 300)}


async def read_manuals(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routing: Routing,
    usage_log: UsageLog,
    instrument: str,
    manuals: list[dict[str, Any]],
    say: Say,
) -> dict[str, Any]:
    """The AI's reading of an instrument's manuals: {modes, middleC, middleCSaid, notes, read: [{source, title,
    pages, chars} or {source, problem}], by}. NoConnection without an AI connection; ManualError when no manual
    could be read or the AI's answer says nothing usable."""
    routes = routing.resolve("patterns")
    if not routes:
        raise NoConnection("No AI connection is set up.")
    documents: list[Document] = []
    read: list[dict[str, Any]] = []
    listed = manuals[:MOST_MANUALS]
    for i, manual in enumerate(listed):
        source = str(manual.get("file") or manual.get("url") or "")
        step = f"Reading {Path(source).name or source}..."
        await say({"kind": "progress", "done": i, "total": len(listed) + 1, "step": step})
        try:
            doc = await read_document(client, manual)
        except ManualError as error:
            read.append({"source": source, "problem": str(error)})
            continue
        documents.append(doc)
        read.append({"source": source, "title": doc.title, "pages": doc.pages, "chars": len(doc.text)})
    if not documents:
        problems = "; ".join(f"{Path(r['source']).name or r['source']}: {r['problem']}" for r in read)
        raise ManualError(f"No manual could be read ({problems})." if problems else "Add a manual first.")

    async def no_text(_: str) -> None:
        pass

    step = "The AI is reading the manual..."
    await say({"kind": "progress", "done": len(listed), "total": len(listed) + 1, "step": step})
    request = Request(
        routes[0].model,
        SYSTEM,
        [Message("user", [Text(f"The instrument: {instrument}\n\n{relevant(documents)}")])],
        [READ_MANUAL],
        None,
    )
    failed: list[str] = []
    completion, index = await complete_with_fallback(client, connections, routes, 0, request, no_text, say, failed)
    usage_log.add(routes[index].connection.id, "patterns", completion.usage)
    for use in completion.tool_uses:
        if use.name == "read_manual" and (reading := checked_reading(use.input)) is not None:
            return {**reading, "read": read, "by": answered_by(completion.model, failed)}
    raise ManualError("The AI found nothing about modes or keys in the manual.")
