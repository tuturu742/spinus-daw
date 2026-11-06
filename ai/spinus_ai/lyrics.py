"""The Track creator's lyrics: the words a user writes for the song, read into sections and lines with how many
syllables each line has and where its stresses fall, so the song writer can size the sections to the words and
write a vocal line (played by an instrument standing in for the voice: Spinus sings nothing).

The lyrics are tagged the way song generators take them: a section tag on a line of its own in square brackets
("[Verse 1]", "[Chorus]", "[Guitar Solo]", "[Intro - soft piano]", "[Chorus x2]"), then that section's lines. A
tag with no lines under it is an instrumental section; a tag used again with no lines repeats the lines it had
before (a chorus written out once). Other ways of marking sections are read by their layout, in any language:
a short label on a line of its own at the start of a stanza, in parentheses or before a colon ("(Verse 1)",
"Chorus:", "(Intro: soft piano)", "Куплет 1:"); lines like "Title: ..." or "BPM: 174" and a lone title line
before the first section are left out; a longer line in parentheses is a note for its section, not a sung line.
Lyrics with no labels are split at their blank lines, a stanza sung again being the chorus; one stanza is one
verse. A label's kind comes from the structure words song generators use ("Verse", "Pre-Chorus", "Guitar
Solo"...); a label in other words is a verse, or a chorus when its lines come back. With the AI reading the
lyrics, it reads their structure in any language (checked: each line it gives must be one of the lyrics' own).

Syllables are counted on this computer by rules, approximately: vowel groups, with English's silent endings
("made", "walked") and its usual exceptions; in Cyrillic every vowel letter is a syllable. Stresses are guessed
for English (a stressed syllable on every word that carries meaning, none on "the", "a", "of"...); in other
languages they are left to the song's beat. With an AI connection, when the user lets the AI read the
description, the AI counts the syllables and marks the stresses instead (checked line by line; a line it gets
wrong keeps the rules' count).

Pure Python: nothing downloaded, no model.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Any

import httpx

from .assistant import NoConnection, Say, answered_by, complete_with_fallback
from .connections import ConnectionStore
from .gateway import Message, Request, Text, Tool
from .routing import Routing
from .songwriter import KIND_WORDS, KINDS, NAMED_KIND, NAMED_SECTIONS
from .usage import UsageLog

MOST_CHARACTERS = 8000
MOST_SECTIONS = 40
MOST_LINES = 200
MOST_LINE = 200  # characters of a line
MOST_SYLLABLES = 40  # a line's
MOST_REPEAT = 4  # "[Chorus x4]"

TAG = re.compile(r"^\s*\[([^\]\n]{1,60})\]\s*$")
PARENTHESES = re.compile(r"^\s*[(\uff08](.{1,300})[)\uff09]\s*$")
COLON = re.compile(r"^\s*([^:\uff1a\n]{1,30}?)\s*[:\uff1a]\s*(.*)$")
MOST_LABEL_WORDS = 3  # a label is short: "Verse 1", "Pre-Chorus", "Guitar Solo x2"
REPEAT = re.compile(r"\s*(?:x|\u00d7)\s*(\d)\s*$", re.IGNORECASE)
WORD = re.compile(r"[^\W\d_]+(?:['\u2019][^\W\d_]+)*", re.UNICODE)

CYRILLIC_VOWELS = set("аеёиоуыэюяіїєү")
LATIN_VOWELS = set("aeiouyàáâãäåæèéêëìíîïòóôõöøùúûüýÿœ")
ENGLISH_WORDS = {"the", "and", "you", "i", "my", "your", "is", "are", "of", "to", "in", "it", "me", "we", "be",
                 "with", "that", "this", "on", "all", "for", "don't", "i'm", "can", "will", "love",
                 "night"}  # fmt: skip
# One-syllable words a sung line usually passes over lightly.
UNSTRESSED = {"a", "an", "the", "of", "to", "in", "on", "at", "by", "for", "from", "with", "and", "or", "but", "as",
              "is", "am", "are", "was", "were", "be", "been", "do", "does", "did", "has", "have", "had", "can",
              "could", "will", "would", "shall", "should", "may", "might", "must", "i", "me", "my", "you", "your",
              "he", "him", "his", "she", "her", "it", "its", "we", "us", "our", "they", "them", "their", "that",
              "than", "if", "so", "not", "no", "up", "out", "there", "this", "these", "those", "what", "when",
              "where", "who", "how", "i'm", "it's", "don't", "can't", "won't", "i'll", "you're", "we're"}  # fmt: skip
# Prefixes that usually leave the stress on the syllable after them ("become", "return", "away").
LIGHT_PREFIXES = ("a", "be", "de", "re", "un", "in", "ex", "con", "com", "pre", "pro", "for", "per", "sur")
# Words that begin like those prefixes but are stressed on their first syllable.
FIRST_STRESSED = {"into", "comfort", "concert", "contact", "conscious", "constant", "common", "promise", "problem",
                  "process", "product", "program", "progress", "perfect", "person", "purpose", "after", "answer",
                  "anger", "angel", "any", "army", "artist", "action", "absent", "anything", "anyone", "always",
                  "almost", "also", "able", "inner", "input", "income", "insect", "instant", "interest", "endless",
                  "exit", "extra", "export", "under", "undo", "rescue", "record", "recent", "really", "reason",
                  "real", "ready", "regal", "decade", "demon", "desert", "devil", "pretty", "presence", "present",
                  "precious", "proper", "prophet", "forest", "forward", "fortune", "foreign", "beauty",
                  "beautiful", "better", "body", "broken"}  # fmt: skip
# Two-letter vowel groups English says as two syllables ("lion", "piano", "create", "being").
SPLIT_GROUPS = ("ia", "io", "eo", "ua", "uo", "iu", "ea")
SPLIT_EXCEPT = ("tion", "sion", "cious", "tious", "cial", "tial", "geon", "eous", "ear", "eal", "ead", "eat", "eak",
                "ean", "eam", "eas", "eath", "each", "eav", "eau")  # fmt: skip


def known_kind(tag: str) -> str | None:
    """The kind of section a tag names in the structure words song generators use ("Guitar Solo": solo), or
    None."""
    lowered = tag.lower()
    for name, pattern, _ in NAMED_SECTIONS:
        if re.search(pattern, lowered) or lowered.strip() == name:
            return NAMED_KIND[name]
    for word, kind in (("hook", "hook"), ("instrumental", "instrumental"), ("end", "end"), ("refrain", "chorus"),
                       ("pre", "pre-chorus"), ("spoken", "verse"), ("rap", "verse")):  # fmt: skip
        if re.search(rf"\b{word}\b", lowered):
            return kind
    return None


def section_kind(tag: str) -> str:
    """The kind of section a tag names, a verse when it names none."""
    return known_kind(tag) or "verse"


def language_of(text: str) -> str:
    """ "cyrillic", "english" or "latin" (another language in the Latin alphabet): how the rules count syllables."""
    letters = [c for c in text.lower() if c.isalpha()]
    if not letters:
        return "english"
    if sum(1 for c in letters if "Ѐ" <= c <= "ӿ") > len(letters) / 2:
        return "cyrillic"
    words = re.findall(r"[a-z']+", text.lower())
    if words and sum(1 for w in words if w in ENGLISH_WORDS) >= max(1, len(words) // 12):
        return "english"
    return "latin"


def english_syllables(word: str) -> int:
    w = word.lower().replace("\u2019", "'")
    w = re.sub(r"'(s|d|ll|re|ve|m|t)$", r"\1" if w.endswith("n't") else "", w)  # "it's" is one, "didn't" two
    w = re.sub(r"[^a-z]", "", w)
    if not w:
        return 0
    if len(w) <= 3:
        return 1 if any(c in "aeiouy" for c in w) else 0
    count = len(re.findall(r"[aeiouy]+", w))
    # Silent endings: "made", "walked", "faces" stay one syllable short.
    if w.endswith("e") and not w.endswith(("le", "ee", "ye")) and count > 1:
        count -= 1
    elif w.endswith("le") and len(w) > 2 and w[-3] in "aeiouy" and count > 1:
        count -= 1  # "smile", "whale"
    if w.endswith("ed") and len(w) > 3 and w[-3] not in "td" and count > 1:
        count -= 1
    if w.endswith("es") and len(w) > 3 and not w.endswith(("ses", "xes", "zes", "ches", "shes", "ges", "ces")) \
            and count > 1:  # fmt: skip
        count -= 1
    if w.startswith("y") and count > 1 and len(w) > 1 and w[1] in "aeiou":
        count -= 1  # "you", "year": the y is a consonant
    for group in SPLIT_GROUPS:
        for m in re.finditer(group, w):
            around = w[max(0, m.start() - 2) : m.end() + 2]
            if not any(e in around for e in SPLIT_EXCEPT):
                count += 1
    return max(1, count)


def vowel_groups(word: str, vowels: set[str]) -> int:
    w = word.lower()
    count, before = 0, False
    for c in w:
        now = c in vowels
        if now and not before:
            count += 1
        before = now
    return count


def cyrillic_syllables(word: str) -> int:
    return sum(1 for c in word.lower() if c in CYRILLIC_VOWELS)


def word_syllables(word: str, language: str) -> int:
    if language == "cyrillic":
        return cyrillic_syllables(word)
    if language == "english":
        return english_syllables(word)
    return vowel_groups(word, LATIN_VOWELS)


def word_stress(word: str, syllables: int) -> str:
    """An English word's stresses, one mark a syllable ("1" stressed, "0" not): guessed."""
    w = word.lower().replace("\u2019", "'")
    if syllables == 1:
        return "0" if w in UNSTRESSED else "1"
    prefixed = (
        w not in FIRST_STRESSED
        and w.startswith(LIGHT_PREFIXES)
        and (syllables == 3 or not w.endswith(("er", "y", "ing", "le", "ow")))
    )
    stressed = 1 if prefixed and syllables <= 3 else 0
    if w.endswith(("tion", "sion", "ic", "ity")):
        stressed = max(0, syllables - 2 if w.endswith(("tion", "sion", "ic")) else syllables - 3)
    marks = ["0"] * syllables
    marks[min(stressed, syllables - 1)] = "1"
    if syllables >= 4:  # a long word's secondary stress, two syllables on
        marks[(min(stressed, syllables - 1) + 2) % syllables] = "1"
    return "".join(marks)


@dataclass
class Line:
    text: str
    syllables: int
    stress: str  # one mark a syllable, "1" stressed; "" when not known (left to the beat)

    def as_dict(self) -> dict[str, Any]:
        return {"text": self.text, "syllables": self.syllables, "stress": self.stress}


def count_line(text: str, language: str) -> Line:
    words = WORD.findall(text)
    counts = [word_syllables(w, language) for w in words]
    total = min(MOST_SYLLABLES, sum(counts))
    stress = ""
    if language == "english":
        stress = "".join(word_stress(w, n) for w, n in zip(words, counts, strict=True) if n)[:total]
    return Line(text, total, stress)


class LyricsError(ValueError):
    pass


def label_of(line: str, stanza_start: bool, brackets: bool) -> tuple[str, str] | None:
    """(name, cue) when a line is a section's label, by its layout: "[...]" anywhere; in lyrics that use no
    brackets, at a stanza's start, a short name alone in parentheses or before a colon ("(Verse 1)", "Chorus:",
    "(Intro: soft piano)")."""
    if (m := TAG.match(line)) is not None:
        return m.group(1).strip(), ""
    if not stanza_start or brackets:
        return None
    inner = m.group(1).strip() if (m := PARENTHESES.match(line)) is not None else None
    found = COLON.match(inner if inner is not None else line)
    if found is not None:
        name, cue = found.group(1).strip(), found.group(2).strip()
        if inner is None and cue and known_kind(name) is None and not re.search(r"\d", name):
            return None  # "Baby: come home" is sung; "Verse 2: ..." and "Chorus: ..." are labels
    elif inner is not None:
        name, cue = inner, ""
    else:
        return None
    if not name or len(name.split()) > MOST_LABEL_WORDS:
        return None
    return name, cue


def split(text: str) -> list[dict[str, Any]]:
    """The sections in order, each {tag, kind, cue, lines (texts), repeat}: from their labels (label_of), or from
    the blank lines between stanzas when there are none; a header ("Title: ...", "BPM: ...") or a lone title line
    before the first label is left out, a longer line in parentheses is its section's note."""
    if len(text) > MOST_CHARACTERS:
        raise LyricsError(f"The lyrics are too long: at most {MOST_CHARACTERS} characters.")
    # Each line: ("blank",), ("label", name, cue), ("note", text), ("header", text) or ("sung", text).
    brackets = any(TAG.match(line) for line in text.splitlines())
    marks: list[tuple[str, ...]] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            marks.append(("blank",))
            continue
        labelled = any(m[0] == "label" for m in marks)
        sung = sum(1 for m in marks if m[0] == "sung")
        # A stanza starts after a blank line, a label or a note; at the top, after a title line too.
        start = not marks or marks[-1][0] in ("blank", "label", "note", "header") or (not labelled and sung <= 1)
        parenthesized = PARENTHESES.match(line)
        heading = COLON.match(line)
        if (label := label_of(line, start, brackets)) is not None:
            marks.append(("label", *label))
        elif parenthesized is not None and (len(line.split()) > MOST_LABEL_WORDS or (brackets and start)):
            marks.append(("note", parenthesized.group(1).strip()))  # a direction, not sung
        elif not labelled and heading is not None and heading.group(2) and len(heading.group(1).split()) <= 2:
            marks.append(("header", line[:MOST_LINE]))  # "Title: ...", "BPM: 174" (sung after all if no labels)
        else:
            marks.append(("sung", line[:MOST_LINE]))
    first_label = next((i for i, m in enumerate(marks) if m[0] == "label"), None)
    if first_label is None:
        marks = [("sung", m[1]) if m[0] == "header" else m for m in marks]
    else:
        # Before the first label, a lone line is the title.
        before = [i for i in range(first_label) if marks[i][0] == "sung"]
        if len(before) == 1:
            marks[before[0]] = ("header", marks[before[0]][1])

    sections: list[dict[str, Any]] = []
    current: dict[str, Any] | None = None
    unlabelled = first_label is None
    for mark in marks:
        if mark[0] == "label":
            name, cue = mark[1], mark[2]
            times = 1
            if (r := REPEAT.search(name)) is not None:
                times = max(1, min(MOST_REPEAT, int(r.group(1))))
                name = name[: r.start()].strip()
            if not cue:
                parts = re.split(r"\s+[-\u2013\u2014]\s+|\s*:\s*|\s*\(", name, maxsplit=1)
                cue = parts[1].rstrip(")").strip() if len(parts) > 1 else ""
                name = parts[0].strip() or name
            current = {"tag": name, "kind": section_kind(name), "known": known_kind(name) is not None, "cue": cue,
                       "lines": [], "times": times}  # fmt: skip
            sections.append(current)
        elif mark[0] == "note":
            if current is not None and len(current["cue"]) < 80:
                current["cue"] = (current["cue"] + "; " if current["cue"] else "") + mark[1]
        elif mark[0] == "blank" and unlabelled and current is not None and current["lines"]:
            current = None  # without labels, a blank line ends the stanza
        elif mark[0] == "sung":
            if current is None:
                current = {"tag": "Verse", "kind": "verse", "known": unlabelled, "cue": "", "lines": [], "times": 1}
                sections.append(current)
            current["lines"].append(mark[1])
    if len(sections) > MOST_SECTIONS:
        raise LyricsError(f"Too many sections: at most {MOST_SECTIONS}.")
    if sum(len(s["lines"]) for s in sections) > MOST_LINES:
        raise LyricsError(f"Too many lines: at most {MOST_LINES}.")

    # A stanza or a section of an unknown name sung again is the chorus (each time); unlabelled verses numbered.
    seen: dict[tuple[str, ...], dict[str, Any]] = {}
    for section in sections:
        key = tuple(section["lines"])
        if key and key in seen and (unlabelled or not section["known"]):
            for s in (seen[key], section):
                s["kind"] = "chorus"
                if unlabelled:
                    s["tag"] = "Chorus"
        seen.setdefault(key, section)
    # A label of other words with no lines, first or last, is the intro or the outro.
    for at, kind in ((0, "intro"), (-1, "outro")):
        if sections and not sections[at]["known"] and not sections[at]["lines"] and not unlabelled:
            sections[at]["kind"] = kind
    if unlabelled and len(sections) > 1:
        verses = [s for s in sections if s["kind"] == "verse"]
        for n, s in enumerate(verses, 1):
            s["tag"] = f"Verse {n}" if len(verses) > 1 else "Verse"

    # A tag used again with no lines sings what it had before.
    out: list[dict[str, Any]] = []
    for section in sections:
        lines: list[str] = section["lines"]
        repeat = bool(lines) and any(o["lines"] == lines for o in out)
        if not lines:
            earlier = next((o for o in reversed(out) if o["tag"].lower() == section["tag"].lower() and o["lines"]),
                          None)  # fmt: skip
            if earlier is not None:
                lines, repeat = list(earlier["lines"]), True
                section["kind"] = earlier["kind"]
        for _ in range(section["times"]):
            out.append({k: v for k, v in section.items() if k != "known"} | {"lines": lines, "repeat": repeat})
            repeat = bool(lines)  # "[Chorus x2]": the second time is a repeat
    return out


def summary_of(sections: list[dict[str, Any]], counted: dict[str, Line], language: str, by: str) -> dict[str, Any]:
    out_sections = []
    for s in sections:
        out_sections.append({
            "tag": s["tag"],
            "kind": s["kind"],
            "name": s["tag"] or KIND_WORDS.get(s["kind"], "Verse"),
            "cue": s["cue"],
            "repeat": s["repeat"],
            "instrumental": not s["lines"],
            "lines": [counted[t].as_dict() for t in s["lines"]],
        })  # fmt: skip
    sung = [s for s in out_sections if not s["repeat"]]
    return {
        "kind": "spinus.lyrics",
        "version": 1,
        "language": language,
        "countedBy": by,  # "" (Spinus's rules) or the model that counted
        "sections": out_sections,
        "lines": sum(len(s["lines"]) for s in sung),
        "syllables": sum(line["syllables"] for s in sung for line in s["lines"]),
    }


def read(text: str) -> dict[str, Any]:
    """The lyrics read by the rules (spinus.lyrics): the sections with their lines counted."""
    sections = split(text)
    language = language_of(text)
    counted = {t: count_line(t, language) for s in sections for t in s["lines"]}
    return summary_of(sections, counted, language, "")


def words(lyrics: dict[str, Any]) -> str:
    """What was read, in plain words ("Lyrics: Verse 1, Chorus, Verse 2, Chorus; 16 lines, about 130 syllables")."""
    names = [s["name"] + ("" if s["lines"] else " (instrumental)") for s in lyrics["sections"]]
    if len(names) > 8:
        names = [*names[:8], f"and {len(names) - 8} more"]
    exact = "counted by the AI" if lyrics.get("countedBy") else "counted by Spinus, roughly"
    return (f"Lyrics: {', '.join(names)}; {lyrics['lines']} lines, about {lyrics['syllables']} syllables "
            f"({exact})")  # fmt: skip


# ---------------------------------------------------------------------------------------------------
# The AI's count (optional)

SYSTEM = """You help Spinus, a digital audio workstation, set a user's song lyrics to music. Answer only by \
calling the read_lyrics tool once. Read the lyrics in whatever language and layout they are written: their \
sections in order, each with its label as written (or a usual name when it has none), its kind, and its sung \
lines exactly as written, each with how many syllables it has when sung naturally in its language and which \
are stressed (a string of 1 for stressed and 0 for unstressed, one mark a syllable, as long as the count). Leave \
out what is not sung: a title, lines about the song (tempo, style, mood), directions such as "(soft piano)"; a \
section's direction goes in its note. A section the lyrics name again without its lines (a chorus written out \
once) gives its lines again; "x2" after a label is that section twice; a section with no sung lines is \
instrumental (an intro, a solo)."""

READ_LYRICS = Tool(
    "read_lyrics",
    "The lyrics' sections in order, each with its sung lines counted.",
    {
        "type": "object",
        "properties": {
            "language": {"type": "string", "description": "The lyrics' language, e.g. 'English'"},
            "sections": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "label": {"type": "string", "description": "As written, e.g. 'Verse 1', 'Refrain'"},
                        "kind": {"type": "string", "enum": list(KINDS)},
                        "note": {"type": "string", "description": "Its direction, if the lyrics give one"},
                        "lines": {
                            "type": "array",
                            "items": {
                                "type": "object",
                                "properties": {
                                    "text": {"type": "string", "description": "The line exactly as written"},
                                    "syllables": {"type": "integer", "minimum": 1, "maximum": MOST_SYLLABLES},
                                    "stress": {"type": "string", "description": "e.g. '0101011'"},
                                },
                                "required": ["text", "syllables", "stress"],
                            },
                        },
                    },
                    "required": ["label", "kind", "lines"],
                },
            },
        },
        "required": ["sections"],
    },
)


def plain(text: str) -> str:
    """A line as it is compared with the lyrics: spaces, case and quotes aside."""
    return re.sub(r"\s+", " ", text.strip().strip("\"'\u201c\u201d\u00ab\u00bb")).lower()


def checked_reading(answer: Any, text: str) -> tuple[list[dict[str, Any]], dict[str, Line]] | None:
    """The AI's reading when it holds: sections of known kinds whose lines are the lyrics' own (each one of the
    written lines, or a part of one), each counted with a stress mark a syllable (a line miscounted keeps the
    rules' count). None when there is nothing usable, or when a quarter of its lines are not in the lyrics."""
    if not isinstance(answer, dict) or not isinstance(answer.get("sections"), list):
        return None
    written = [plain(line) for line in text.splitlines() if line.strip()]
    language = language_of(text)
    sections: list[dict[str, Any]] = []
    counted: dict[str, Line] = {}
    given = foreign = 0
    for item in answer["sections"][:MOST_SECTIONS]:
        if not isinstance(item, dict):
            continue
        label = str(item.get("label") or "").strip()[:60] or "Verse"
        kind = str(item.get("kind") or "")
        kind = kind if kind in KINDS else section_kind(label)
        lines: list[str] = []
        for line in item.get("lines") or []:
            if not isinstance(line, dict) or not isinstance(line.get("text"), str) or not line["text"].strip():
                continue
            given += 1
            said = line["text"].strip()[:MOST_LINE]
            found = plain(said)
            if not any(found == w or (len(found) >= 3 and found in w) for w in written):
                foreign += 1  # not in the lyrics: left out
                continue
            lines.append(said)
            count, stress = line.get("syllables"), line.get("stress")
            good = (
                isinstance(count, int) and not isinstance(count, bool) and 1 <= count <= MOST_SYLLABLES
                and isinstance(stress, str) and re.fullmatch(r"[01]+", stress) is not None and len(stress) == count
            )  # fmt: skip
            counted[said] = Line(said, count, stress) if good else count_line(said, language)  # type: ignore[arg-type]
        note = str(item.get("note") or "").strip()[:80]
        repeat = bool(lines) and any(s["lines"] == lines for s in sections)
        sections.append({"tag": label, "kind": kind, "cue": note, "lines": lines, "repeat": repeat})
    if not sections or given == 0 or foreign * 4 > given:
        return None
    if sum(len(s["lines"]) for s in sections if not s["repeat"]) > MOST_LINES:
        return None
    return sections, counted


async def read_with_ai(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routing: Routing,
    usage_log: UsageLog,
    text: str,
    say: Say,
) -> dict[str, Any]:
    """The lyrics as the AI reads them (NoConnection without a connection): their structure in any language and
    each line counted (checked_reading); when its answer does not hold, the rules' reading stands with a note."""
    sections = split(text)  # the limits, and the reading when the AI's does not hold
    language = language_of(text)
    if not any(s["lines"] for s in sections):
        return summary_of(sections, {}, language, "")
    routes = routing.resolve("patterns")
    if not routes:
        raise NoConnection("No AI connection is set up.")

    async def no_text(_: str) -> None:
        pass

    await say({"kind": "status", "text": "The AI is reading the lyrics..."})
    request = Request(routes[0].model, SYSTEM, [Message("user", [Text(f"The lyrics:\n{text}")])], [READ_LYRICS], None)
    failed: list[str] = []
    completion, index = await complete_with_fallback(client, connections, routes, 0, request, no_text, say, failed)
    usage_log.add(routes[index].connection.id, "patterns", completion.usage)
    for use in completion.tool_uses:
        if use.name == "read_lyrics" and (reading := checked_reading(use.input, text)) is not None:
            return summary_of(reading[0], reading[1], language, answered_by(completion.model, failed))
    counted = {t: count_line(t, language) for s in sections for t in s["lines"]}
    lyrics = summary_of(sections, counted, language, "")
    lyrics["note"] = "The AI's reading was not usable; Spinus read the lyrics itself."
    return lyrics
