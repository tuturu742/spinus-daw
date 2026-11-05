"""The Track creator's lyrics: the words a user writes for the song, read into sections and lines with how many
syllables each line has and where its stresses fall, so the song writer can size the sections to the words and
write a vocal line (played by an instrument standing in for the voice: Spinus sings nothing).

The lyrics are tagged the way song generators take them: a section tag on a line of its own in square brackets
("[Verse 1]", "[Chorus]", "[Guitar Solo]", "[Intro - soft piano]", "[Chorus x2]"), then that section's lines. A
tag with no lines under it is an instrumental section; a tag used again with no lines repeats the lines it had
before (a chorus written out once). Lyrics with no tags are one verse.

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
from .songwriter import KIND_WORDS, NAMED_KIND, NAMED_SECTIONS
from .usage import UsageLog

MOST_CHARACTERS = 8000
MOST_SECTIONS = 40
MOST_LINES = 200
MOST_LINE = 200  # characters of a line
MOST_SYLLABLES = 40  # a line's
MOST_REPEAT = 4  # "[Chorus x4]"

TAG = re.compile(r"^\s*\[([^\]\n]{1,60})\]\s*$")
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


def section_kind(tag: str) -> str:
    """The kind of section a tag names ("Guitar Solo": solo), a verse when it names none."""
    lowered = tag.lower()
    for name, pattern, _ in NAMED_SECTIONS:
        if re.search(pattern, lowered) or lowered.strip() == name:
            return NAMED_KIND[name]
    for word, kind in (("hook", "hook"), ("instrumental", "instrumental"), ("end", "end"), ("refrain", "chorus"),
                       ("pre", "pre-chorus"), ("spoken", "verse"), ("rap", "verse")):  # fmt: skip
        if re.search(rf"\b{word}\b", lowered):
            return kind
    return "verse"


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


def split(text: str) -> list[dict[str, Any]]:
    """The tagged sections in order, each {tag, kind, cue, lines (texts), repeat}; untagged lyrics are one verse."""
    if len(text) > MOST_CHARACTERS:
        raise LyricsError(f"The lyrics are too long: at most {MOST_CHARACTERS} characters.")
    sections: list[dict[str, Any]] = []
    current: dict[str, Any] | None = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if (m := TAG.match(line)) is not None:
            tag = m.group(1).strip()
            times = 1
            if (r := REPEAT.search(tag)) is not None:
                times = max(1, min(MOST_REPEAT, int(r.group(1))))
                tag = tag[: r.start()].strip()
            parts = re.split(r"\s*[-\u2013\u2014:]\s*|\s*\(", tag, maxsplit=1)
            cue = parts[1].rstrip(")").strip() if len(parts) > 1 else ""
            name = parts[0].strip() or tag
            current = {"tag": name, "kind": section_kind(name), "cue": cue, "lines": [], "times": times}
            sections.append(current)
            continue
        if current is None:
            current = {"tag": "Verse", "kind": "verse", "cue": "", "lines": [], "times": 1}
            sections.append(current)
        current["lines"].append(line[:MOST_LINE])
    if len(sections) > MOST_SECTIONS:
        raise LyricsError(f"Too many sections: at most {MOST_SECTIONS}.")
    if sum(len(s["lines"]) for s in sections) > MOST_LINES:
        raise LyricsError(f"Too many lines: at most {MOST_LINES}.")

    # A tag used again with no lines sings what it had before.
    out: list[dict[str, Any]] = []
    for section in sections:
        lines: list[str] = section["lines"]
        repeat = False
        if not lines:
            before = next((s for s in reversed(out) if s["tag"].lower() == section["tag"].lower() and s["lines"]),
                          None)  # fmt: skip
            if before is not None:
                lines, repeat = list(before["lines"]), True
        for _ in range(section["times"]):
            out.append({**section, "lines": lines, "repeat": repeat})
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
calling the count_syllables tool once. For every numbered line, give how many syllables it has when sung \
naturally in its language, and which syllables are stressed: a string of 1 (stressed) and 0 (unstressed), \
one mark a syllable, as long as the count. Keep the lines' numbers."""

COUNT_SYLLABLES = Tool(
    "count_syllables",
    "Each line's syllables and stresses.",
    {
        "type": "object",
        "properties": {
            "language": {"type": "string", "description": "The lyrics' language, e.g. 'English'"},
            "lines": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "line": {"type": "integer", "description": "The line's number"},
                        "syllables": {"type": "integer", "minimum": 1, "maximum": MOST_SYLLABLES},
                        "stress": {"type": "string", "description": "e.g. '0101011' (as long as syllables)"},
                    },
                    "required": ["line", "syllables", "stress"],
                },
            },
        },
        "required": ["lines"],
    },
)


def checked_counts(answer: Any, texts: list[str]) -> dict[int, Line]:
    """The AI's counts that hold: a number of syllables within limits and a stress mark for each."""
    out: dict[int, Line] = {}
    if not isinstance(answer, dict) or not isinstance(answer.get("lines"), list):
        return out
    for item in answer["lines"]:
        if not isinstance(item, dict):
            continue
        number, count, stress = item.get("line"), item.get("syllables"), item.get("stress")
        if not isinstance(number, int) or not 1 <= number <= len(texts) or isinstance(number, bool):
            continue
        if not isinstance(count, int) or isinstance(count, bool) or not 1 <= count <= MOST_SYLLABLES:
            continue
        if not isinstance(stress, str) or not re.fullmatch(r"[01]+", stress) or len(stress) != count:
            continue
        out[number - 1] = Line(texts[number - 1], count, stress)
    return out


async def read_with_ai(
    client: httpx.AsyncClient,
    connections: ConnectionStore,
    routing: Routing,
    usage_log: UsageLog,
    text: str,
    say: Say,
) -> dict[str, Any]:
    """The lyrics with the AI's count (NoConnection without one); lines it miscounts keep the rules' count, and when
    none of its answer holds, the rules' reading stands with a note."""
    sections = split(text)
    language = language_of(text)
    texts = list(dict.fromkeys(t for s in sections for t in s["lines"]))
    counted = {t: count_line(t, language) for t in texts}
    if not texts:
        return summary_of(sections, counted, language, "")
    routes = routing.resolve("patterns")
    if not routes:
        raise NoConnection("No AI connection is set up.")

    async def no_text(_: str) -> None:
        pass

    await say({"kind": "status", "text": "The AI is counting the lyrics' syllables..."})
    numbered = "\n".join(f"{i + 1}. {t}" for i, t in enumerate(texts))
    request = Request(routes[0].model, SYSTEM, [Message("user", [Text(f"The lines:\n{numbered}")])],
                      [COUNT_SYLLABLES], None)  # fmt: skip
    failed: list[str] = []
    completion, index = await complete_with_fallback(client, connections, routes, 0, request, no_text, say, failed)
    usage_log.add(routes[index].connection.id, "patterns", completion.usage)
    for use in completion.tool_uses:
        if use.name == "count_syllables" and (found := checked_counts(use.input, texts)):
            for i, line in found.items():
                counted[texts[i]] = line
            lyrics = summary_of(sections, counted, language, answered_by(completion.model, failed))
            if len(found) < len(texts):
                lyrics["note"] = f"The AI counted {len(found)} of {len(texts)} lines; Spinus counted the rest."
            return lyrics
    lyrics = summary_of(sections, counted, language, "")
    lyrics["note"] = "The AI's count was not usable; Spinus counted the syllables itself."
    return lyrics
