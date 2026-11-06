"""The song's lyrics (spinus_ai.lyrics): split into tagged sections, a chorus written once repeated, each line's
syllables counted by rules (roughly, in English and Cyrillic) or by the AI (checked line by line), and passed with
the description into the summary."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import httpx
import pytest
from test_service import anthropic_reply, make_service, no_notify, run_job

from spinus_ai import lyrics
from spinus_ai.service import RpcError

SONG = """[Intro]

[Verse 1]
The city lights are fading out
I walk alone without a doubt

[Chorus x2]
Hold on, hold on to the night

[Guitar Solo]

[Verse 2]
Another morning, another day

[Chorus]

[Outro - fade out]
"""


def test_sections_come_from_the_tags() -> None:
    read = lyrics.read(SONG)
    sections = read["sections"]
    assert [(s["name"], s["kind"]) for s in sections] == [
        ("Intro", "intro"),
        ("Verse 1", "verse"),
        ("Chorus", "chorus"),
        ("Chorus", "chorus"),
        ("Guitar Solo", "solo"),
        ("Verse 2", "verse"),
        ("Chorus", "chorus"),
        ("Outro", "outro"),
    ]
    assert sections[0]["instrumental"] and sections[4]["instrumental"] and sections[7]["instrumental"]
    assert sections[7]["cue"] == "fade out"
    # "[Chorus x2]" sings twice; "[Chorus]" with no lines sings what it had.
    assert not sections[2]["repeat"] and sections[3]["repeat"] and sections[6]["repeat"]
    assert sections[6]["lines"] == sections[2]["lines"]
    # Lines and syllables count what is written once, not the repeats.
    assert read["lines"] == 4 and read["language"] == "english"
    assert read["syllables"] == sum(line["syllables"] for i in (1, 2, 5) for line in sections[i]["lines"])


def test_untagged_lyrics_are_one_verse() -> None:
    read = lyrics.read("just a few words\nand a second line")
    assert [s["kind"] for s in read["sections"]] == ["verse"] and read["lines"] == 2


@pytest.mark.parametrize(
    ("line", "count"),
    [
        ("The city lights are fading out", 8),
        ("I walk alone without a doubt", 8),
        ("Hold on, hold on to the night", 7),
        ("Another morning, another day", 9),
        ("The lion played the piano", 8),
        ("Smile, you're beautiful", 5),
    ],
)
def test_english_syllables_are_about_right(line: str, count: int) -> None:
    counted = lyrics.count_line(line, "english")
    assert abs(counted.syllables - count) <= 1, (line, counted.syllables)
    assert len(counted.stress) == counted.syllables


def test_english_stress_falls_on_words_that_carry_meaning() -> None:
    assert lyrics.count_line("the night", "english").stress == "01"
    assert lyrics.count_line("alone", "english").stress == "01"


def test_cyrillic_counts_every_vowel_and_leaves_stress_to_the_beat() -> None:
    assert lyrics.language_of("Город спит под снегом") == "cyrillic"
    line = lyrics.count_line("Город спит под снегом", "cyrillic")
    assert line.syllables == 6 and line.stress == ""


def test_other_latin_languages_count_vowel_groups() -> None:
    assert lyrics.language_of("Bajo la luna llena canto") == "latin"
    assert lyrics.count_line("Bajo la luna llena canto", "latin").syllables == 9


def test_limits() -> None:
    with pytest.raises(lyrics.LyricsError):
        lyrics.read("la\n" * 5000)
    with pytest.raises(lyrics.LyricsError):
        lyrics.read("la\n" * (lyrics.MOST_LINES + 1))


def test_the_ai_reading_is_checked_against_the_lyrics() -> None:
    text = "Refrain :\nNous dansons sous la pluie\nToute la nuit\n"
    good = {"sections": [{"label": "Refrain", "kind": "chorus", "note": "", "lines": [
        {"text": "Nous dansons sous la pluie", "syllables": 6, "stress": "001001"},
        {"text": "Toute la nuit", "syllables": 4, "stress": "1001"},
    ]}]}  # fmt: skip
    reading = lyrics.checked_reading(good, text)
    assert reading is not None
    sections, counted = reading
    assert sections[0]["kind"] == "chorus" and counted["Toute la nuit"].syllables == 4
    # A miscounted line keeps the rules' count; an unknown kind is read from the label.
    good["sections"][0]["lines"][1]["stress"] = "1"
    good["sections"][0]["kind"] = "refrain"
    sections, counted = lyrics.checked_reading(good, text) or ([], {})
    assert sections[0]["kind"] == "chorus" and counted["Toute la nuit"].stress == ""
    # Lines that are not the lyrics' own: not used.
    made_up = {"sections": [{"label": "Verse", "kind": "verse", "lines": [
        {"text": "Words the AI made up", "syllables": 5, "stress": "10101"}]}]}  # fmt: skip
    assert lyrics.checked_reading(made_up, text) is None
    assert lyrics.checked_reading("nonsense", text) is None


def test_other_layouts_are_read_by_their_layout_in_any_language() -> None:
    # Labels in parentheses or before a colon, a title and headers left out, a direction kept as a note.
    text = """Title: The Dust of the Road
BPM: 96

(Verse 1)
Old Tom drove a rusted Ford
With a map he did not need

(Chorus)
Oh the road goes on
(slow and soft, the band drops out)

Couplet 2 :
Le vent souffle sur la ville
Et la nuit tombe doucement

(Chorus)
"""
    read = lyrics.read(text)
    assert [(s["name"], s["kind"], len(s["lines"])) for s in read["sections"]] == [
        ("Verse 1", "verse", 2),
        ("Chorus", "chorus", 1),
        ("Couplet 2", "verse", 2),
        ("Chorus", "chorus", 1),
    ]
    assert read["sections"][1]["cue"].startswith("slow and soft") and read["sections"][3]["repeat"]
    assert all("Title" not in line["text"] for s in read["sections"] for line in s["lines"])

    # A title line, labels of other words; full-width marks (Chinese); an instrumental label last is the outro.
    chinese = "\u591c\u66f2\n\n\u4e3b\u6b4c\uff1a\n\u6708\u5149\u7167\u5728\u7a97\u524d\n\n\uff08\u5c3e\u5974\uff09\n"
    read = lyrics.read(chinese)
    assert [(s["kind"], len(s["lines"])) for s in read["sections"]] == [("verse", 1), ("outro", 0)]

    # Lyrics with brackets: a short line in parentheses at a stanza's start is a direction, not a label; a
    # hyphenated tag is one name.
    read = lyrics.read("[Intro]\n(Spoken warmly)\nYeah man\n\n[Pre-Chorus]\nHold on\n")
    assert [(s["name"], s["kind"]) for s in read["sections"]] == [("Intro", "intro"), ("Pre-Chorus", "pre-chorus")]
    assert read["sections"][0]["cue"] == "Spoken warmly" and read["sections"][0]["lines"][0]["text"] == "Yeah man"


def test_unlabelled_stanzas_are_sections_and_the_one_sung_again_the_chorus() -> None:
    chorus = "Wir singen laut\nDie ganze Nacht\n"
    text = f"Ein Tag am Meer\nDie Sonne scheint\n\n{chorus}\nDer Wind ist kalt\nDer Weg ist weit\n\n{chorus}"
    read = lyrics.read(text)
    assert [(s["name"], s["kind"], s["repeat"]) for s in read["sections"]] == [
        ("Verse 1", "verse", False), ("Chorus", "chorus", False), ("Verse 2", "verse", False),
        ("Chorus", "chorus", True)]  # fmt: skip
    assert read["lines"] == 6


def counting_service(tmp_path: Path, sent: list[dict[str, Any]]) -> Any:
    def handler(request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content)
        sent.append(body)
        if body["tools"][0]["name"] == "read_lyrics":
            read = lyrics.read(body["messages"][0]["content"][0]["text"].split("\n", 1)[1])
            answer = {"sections": [
                {"label": s["name"], "kind": s["kind"],
                 "lines": [{"text": line["text"], "syllables": 2, "stress": "01"} for line in s["lines"]]}
                for s in read["sections"]]}  # fmt: skip
            tool = ("read_lyrics", answer)
        else:
            tool = ("read_description", {"genre": "synthpop"})
        return httpx.Response(200, content=anthropic_reply("", tool), headers={"content-type": "text/event-stream"})

    return make_service(tmp_path, handler)


async def test_the_service_reads_lyrics_with_the_description(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    service = counting_service(tmp_path, sent)

    # Offline: Spinus counts.
    read = await run_job(service, "ai.v1.song.describe", {"text": "a synthpop song", "lyrics": SONG})
    assert read["lyrics"]["countedBy"] == "" and read["lyrics"]["lines"] == 4 and not sent
    assert read["plain"][-1].startswith("Lyrics: Intro (instrumental), Verse 1, Chorus")
    assert "counted by Spinus, roughly" in read["plain"][-1]
    # No lyrics: none in the summary.
    assert "lyrics" not in await run_job(service, "ai.v1.song.describe", {"text": "a synthpop song", "lyrics": " "})

    # With the AI: it counts (each written line once).
    await service.handle(
        "ai.v1.connections.save", {"name": "Claude", "provider": "anthropic", "key": "sk-test"}, no_notify
    )
    read = await run_job(service, "ai.v1.song.describe", {"text": "a synthpop song", "lyrics": SONG, "ai": True})
    assert read["lyrics"]["countedBy"] == "claude-test" and read["lyrics"]["syllables"] == 8
    reading = [b for b in sent if b["tools"][0]["name"] == "read_lyrics"]
    assert len(reading) == 1 and "Another morning, another day" in reading[0]["messages"][0]["content"][0]["text"]

    with pytest.raises(RpcError):
        await service.handle("ai.v1.song.describe", {"text": "pop", "lyrics": "la\n" * 5000}, no_notify)
