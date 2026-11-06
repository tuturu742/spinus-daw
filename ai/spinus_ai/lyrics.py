"""The Track creator's lyrics: the words a user writes for the song, read into sections and lines with how many
syllables each line has and where its stresses fall, so the song writer can size the sections to the words and
write a vocal line (played by an instrument standing in for the voice: Spinus sings nothing).

One format (owner decision): the section tags song generators take, in English, in square brackets on a line of
their own ("[Verse 1]", "[Chorus]", "[Guitar Solo]", "[Intro - soft piano]", "[Chorus x2]"), then that section's
lines, which may be in any language. TAGS lists the tags and the kind of section each one is; the manual's table
and the AI's instructions come from it. A tag with no lines under it is an instrumental section; a tag used again
with no lines repeats the lines it had before (a chorus written out once). What comes before the first tag (a
title, notes) is not sung, nor is a line wholly in parentheses (a direction or a backing vocal: its section's
note). Lyrics without tags are not guessed at: the reading says to add tags, or to let the AI read them, and the
AI's reading then proposes the lyrics in this format for the user to apply (nothing changes until they do).

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
# The tags, in English, with the kind of section each one is (songwriter.KINDS). A tag may carry a number
# ("Verse 2"), a direction after a dash or a colon ("Intro - soft piano") and a repeat ("Chorus x2"); an
# instrument's solo is any "... Solo" ("Guitar Solo", "Sax Solo"). Other words are a verse.
TAGS: tuple[tuple[str, str, str], ...] = (
    ("Intro", "intro", "the opening, usually instrumental"),
    ("Verse", "verse", "the verses, numbered or not: Verse 1, Verse 2"),
    ("Pre-Chorus", "pre-chorus", "the lead-in to the chorus"),
    ("Chorus", "chorus", "the chorus (also Refrain)"),
    ("Post-Chorus", "post-chorus", "after the chorus"),
    ("Hook", "hook", "a short catchy part"),
    ("Bridge", "bridge", "the contrasting part, usually once"),
    ("Break", "break", "a short pause in the music"),
    ("Breakdown", "breakdown", "the music strips down"),
    ("Build-up", "build", "the music rises toward a drop or a chorus"),
    ("Drop", "drop", "the big release after a build-up"),
    ("Interlude", "interlude", "a passage between parts"),
    ("Instrumental", "instrumental", "a part with no singing"),
    ("Solo", "solo", "an instrument's solo: Guitar Solo, Piano Solo"),
    ("Outro", "outro", "the ending"),
    ("End", "end", "the very end"),
    ("Fade Out", "fade-out", "the song fades away"),
)
NO_TAGS = (
    "The lyrics have no section tags: put [Verse], [Chorus] and the like on a line of their own before each part "
    "(the Track creator page of the manual lists them), or tick Let the AI read it and the AI will tag them for "
    "you to check."
)
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
    """The kind of section a tag names (TAGS, "Refrain" a chorus, "... Solo" a solo), or None."""
    lowered = re.sub(r"\s+\d+$", "", tag.lower().strip())
    for name, kind, _ in TAGS:
        if lowered == name.lower() or (kind == "solo" and lowered.endswith(" solo")):
            return kind
    for name, pattern, _ in NAMED_SECTIONS:
        if re.search(pattern, lowered) or lowered == name:
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


def has_tags(text: str) -> bool:
    """Whether the lyrics are in Spinus's format: at least one section tag on a line of its own."""
    return any(TAG.match(line) for line in text.splitlines())


def split(text: str) -> list[dict[str, Any]]:
    """The sections in order, each {tag, kind, cue, lines (texts), repeat}, from their tags; [] without tags.
    Before the first tag nothing is sung; a line wholly in parentheses is its section's note."""
    if len(text) > MOST_CHARACTERS:
        raise LyricsError(f"The lyrics are too long: at most {MOST_CHARACTERS} characters.")
    sections: list[dict[str, Any]] = []
    current: dict[str, Any] | None = None
    for raw in text.splitlines():
        line = raw.strip()
        if (m := TAG.match(line)) is not None:
            name, times = m.group(1).strip(), 1
            if (r := REPEAT.search(name)) is not None:
                times = max(1, min(MOST_REPEAT, int(r.group(1))))
                name = name[: r.start()].strip()
            parts = re.split(r"\s+[-\u2013\u2014]\s+|\s*:\s*|\s*\(", name, maxsplit=1)
            cue = parts[1].rstrip(")").strip() if len(parts) > 1 else ""
            name = parts[0].strip() or name
            current = {"tag": name, "kind": section_kind(name), "known": known_kind(name) is not None, "cue": cue,
                       "lines": [], "times": times}  # fmt: skip
            sections.append(current)
        elif not line or current is None:
            continue
        elif (p := PARENTHESES.match(line)) is not None:
            if len(current["cue"]) < 80:
                current["cue"] = (current["cue"] + "; " if current["cue"] else "") + p.group(1).strip()
        else:
            current["lines"].append(line[:MOST_LINE])
    if len(sections) > MOST_SECTIONS:
        raise LyricsError(f"Too many sections: at most {MOST_SECTIONS}.")
    if sum(len(s["lines"]) for s in sections) > MOST_LINES:
        raise LyricsError(f"Too many lines: at most {MOST_LINES}.")

    # A tag of other words sung again is the chorus; one with no lines, first or last, the intro or the outro.
    seen: dict[tuple[str, ...], dict[str, Any]] = {}
    for section in sections:
        key = tuple(section["lines"])
        if key and key in seen and not section["known"]:
            for s in (seen[key], section):
                s["kind"] = "chorus"
        seen.setdefault(key, section)
    for at, kind in ((0, "intro"), (-1, "outro")):
        if sections and not sections[at]["known"] and not sections[at]["lines"]:
            sections[at]["kind"] = kind

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
    """The lyrics read by the rules (spinus.lyrics): the sections with their lines counted; without tags none,
    and a note saying to add them (NO_TAGS)."""
    sections = split(text)
    language = language_of(text)
    counted = {t: count_line(t, language) for s in sections for t in s["lines"]}
    lyrics = summary_of(sections, counted, language, "")
    if not sections and text.strip():
        lyrics["note"] = NO_TAGS
    return lyrics


def tagged(sections: list[dict[str, Any]]) -> str:
    """Sections as lyrics in Spinus's format: each tag (with its note) on its own line, then its lines."""
    blocks = []
    for s in sections:
        head = f"[{s['tag']}{' - ' + s['cue'] if s.get('cue') else ''}]"
        blocks.append("\n".join([head, *s["lines"]]))
    return "\n\n".join(blocks) + "\n"


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

TAG_LIST = ", ".join(name for name, _, _ in TAGS)
SYSTEM = f"""You help Spinus, a digital audio workstation, set a user's song lyrics to music. Answer only by \
calling the read_lyrics tool once. Read the lyrics in whatever language and layout they are written: their \
sections in order, each with its tag in English (one of {TAG_LIST}; numbered when there are several, as \
"Verse 2"; an instrument's solo as "Guitar Solo"), its kind, and its sung lines exactly as written (never \
translated), each with how many syllables it has when sung naturally in its language and which are stressed (a \
string of 1 for stressed and 0 for unstressed, one mark a syllable, as long as the count). Leave out what is not \
sung: a title, lines about the song (tempo, style, mood), directions such as "(soft piano)"; a section's direction \
goes in its note, in English. A section the lyrics name again without its lines (a chorus written out once) gives \
its lines again; "x2" after a label is that section twice; a section with no sung lines is instrumental (an \
intro, a solo)."""

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
                        "tag": {"type": "string", "description": f"In English: {TAG_LIST}, e.g. 'Verse 1'"},
                        "kind": {"type": "string", "enum": list(KINDS)},
                        "note": {"type": "string", "description": "Its direction in English, if the lyrics give one"},
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
                    "required": ["tag", "kind", "lines"],
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
    rules' count); a tag that is not one of TAGS is named by its kind. None when there is nothing usable, or when
    a quarter of its lines are not in the lyrics."""
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
        tag = re.sub(r"[\[\]\n]", "", str(item.get("tag") or item.get("label") or "")).strip()[:40]
        kind = str(item.get("kind") or "")
        kind = kind if kind in KINDS else section_kind(tag)
        if known_kind(tag) is None:
            tag = KIND_WORDS.get(kind, "Verse")
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
        note = re.sub(r"[\[\]\n]", "", str(item.get("note") or "")).strip()[:80]
        repeat = bool(lines) and any(s["lines"] == lines for s in sections)
        sections.append({"tag": tag, "kind": kind, "cue": note, "lines": lines, "repeat": repeat})
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
    """The lyrics as the AI reads them (NoConnection without a connection). Tagged lyrics keep their tags'
    sections, the AI counting their lines (a line it does not count keeps the rules' count). Lyrics without tags
    are not used yet: the AI's reading (checked_reading) comes back as a "proposal", the lyrics in Spinus's
    format for the user to apply; when its answer does not hold, the note says to add the tags."""
    tags = has_tags(text)
    sections = split(text)  # the limits, and the tagged lyrics' sections
    language = language_of(text)
    if tags and not any(s["lines"] for s in sections):
        return summary_of(sections, {}, language, "")
    routes = routing.resolve("patterns")
    if not routes:
        raise NoConnection("No AI connection is set up.")

    async def no_text(_: str) -> None:
        pass

    await say(
        {"kind": "status", "text": "The AI is reading the lyrics..." if tags else "The AI is tagging the lyrics..."}
    )
    request = Request(routes[0].model, SYSTEM, [Message("user", [Text(f"The lyrics:\n{text}")])], [READ_LYRICS], None)
    failed: list[str] = []
    completion, index = await complete_with_fallback(client, connections, routes, 0, request, no_text, say, failed)
    usage_log.add(routes[index].connection.id, "patterns", completion.usage)
    by = answered_by(completion.model, failed)
    reading = next(
        (r for use in completion.tool_uses if use.name == "read_lyrics" and (r := checked_reading(use.input, text))),
        None,
    )
    rules = {t: count_line(t, language) for s in sections for t in s["lines"]}
    if tags:
        if reading is None:
            lyrics = summary_of(sections, rules, language, "")
            lyrics["note"] = "The AI's count was not usable; Spinus counted the syllables itself."
            return lyrics
        theirs = {plain(t): line for t, line in reading[1].items()}
        counted = {t: (theirs.get(plain(t)) or rules[t]) for t in rules}
        counted = {t: Line(t, line.syllables, line.stress) for t, line in counted.items()}
        return summary_of(sections, counted, language, by)
    lyrics = summary_of([], {}, language, "")
    if reading is None:
        lyrics["note"] = "The AI could not tag the lyrics. " + NO_TAGS
        return lyrics
    lyrics["proposal"] = tagged([s for s in reading[0] if not s["repeat"] or s["lines"]])
    lyrics["proposedBy"] = by
    lyrics["note"] = (
        "Your lyrics have no section tags: the AI tagged them (below). Use these tags puts them in the lyrics box "
        "and reads them; Keep mine leaves your text as it is."
    )
    return lyrics
