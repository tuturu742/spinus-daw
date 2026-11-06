"""The song's lyrics (spinus_ai.lyrics): split into tagged sections (Spinus's one format; lyrics without tags are
not guessed at), a chorus written once repeated, each line's syllables counted by rules (roughly, in English and
Cyrillic) or by the AI (checked line by line), untagged lyrics tagged by the AI as a proposal, and passed with the
description into the summary."""

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


def test_lyrics_without_tags_are_not_guessed_at() -> None:
    read = lyrics.read("just a few words\nand a second line\n\n(Chorus)\nla la")
    assert read["sections"] == [] and read["lines"] == 0 and read["note"] == lyrics.NO_TAGS
    assert "note" not in lyrics.read("")


def test_every_documented_tag_is_its_kind() -> None:
    # The manual's table and the AI's instructions come from TAGS: each tag is read as its kind.
    for name, kind, _ in lyrics.TAGS:
        assert lyrics.known_kind(name) == kind and lyrics.known_kind(f"{name} 2") == kind
        assert kind in lyrics.KINDS
    assert lyrics.known_kind("Sax Solo") == "solo" and lyrics.known_kind("Refrain") == "chorus"
    manual = (Path(__file__).parents[2] / "docs" / "user" / "track-creator.md").read_text()
    assert all(f"| [{name}]" in manual for name, _, _ in lyrics.TAGS)


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
        lyrics.read("[Verse]\n" + "la\n" * (lyrics.MOST_LINES + 1))


def test_the_ai_reading_is_checked_against_the_lyrics() -> None:
    text = "Refrain :\nNous dansons sous la pluie\nToute la nuit\n"
    good = {"sections": [{"tag": "Chorus", "kind": "chorus", "note": "", "lines": [
        {"text": "Nous dansons sous la pluie", "syllables": 6, "stress": "001001"},
        {"text": "Toute la nuit", "syllables": 4, "stress": "1001"},
    ]}]}  # fmt: skip
    reading = lyrics.checked_reading(good, text)
    assert reading is not None
    sections, counted = reading
    assert sections[0]["kind"] == "chorus" and counted["Toute la nuit"].syllables == 4
    # A miscounted line keeps the rules' count; an unknown kind is read from the tag, a tag not in English (not
    # one of TAGS) is named by its kind.
    good["sections"][0]["lines"][1]["stress"] = "1"
    good["sections"][0]["kind"] = "refrain"
    good["sections"][0]["tag"] = "Refrain :"
    sections, counted = lyrics.checked_reading(good, text) or ([], {})
    assert sections[0]["kind"] == "chorus" and counted["Toute la nuit"].stress == ""
    good["sections"][0]["tag"] = "Couplet"
    good["sections"][0]["kind"] = "verse"
    assert (lyrics.checked_reading(good, text) or ([{}], {}))[0][0]["tag"] == "Verse"
    # Lines that are not the lyrics' own: not used.
    made_up = {"sections": [{"tag": "Verse", "kind": "verse", "lines": [
        {"text": "Words the AI made up", "syllables": 5, "stress": "10101"}]}]}  # fmt: skip
    assert lyrics.checked_reading(made_up, text) is None
    assert lyrics.checked_reading("nonsense", text) is None


def test_before_the_first_tag_nothing_is_sung_and_parentheses_are_notes() -> None:
    text = """Title: The Dust of the Road
BPM: 96

[Verse 1]
Old Tom drove a rusted Ford
(slow and soft, the band drops out)
Le vent souffle sur la ville

[Pre-Chorus]
Hold on

[Chorus]
\u6708\u5149\u7167\u5728\u7a97\u524d
"""
    read = lyrics.read(text)
    assert [(s["name"], s["kind"], len(s["lines"])) for s in read["sections"]] == [
        ("Verse 1", "verse", 2), ("Pre-Chorus", "pre-chorus", 1), ("Chorus", "chorus", 1)]  # fmt: skip
    assert read["sections"][0]["cue"].startswith("slow and soft") and "note" not in read
    assert all("Title" not in line["text"] for s in read["sections"] for line in s["lines"])
    # Spinus's format back from sections: a tag with its note, then its lines.
    again = lyrics.read(lyrics.tagged([{"tag": "Verse 1", "cue": "soft", "lines": ["Old Tom drove a rusted Ford"]}]))
    assert again["sections"][0]["name"] == "Verse 1" and again["sections"][0]["cue"] == "soft"


UNTAGGED = "Ein Tag am Meer\nDie Sonne scheint\n\nWir singen laut\nDie ganze Nacht\n"
UNTAGGED_AS_TAGGED = "[Verse 1]\nEin Tag am Meer\nDie Sonne scheint\n\n[Chorus]\nWir singen laut\nDie ganze Nacht\n"


def counting_service(tmp_path: Path, sent: list[dict[str, Any]]) -> Any:
    def handler(request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content)
        sent.append(body)
        if body["tools"][0]["name"] == "read_lyrics":
            words = body["messages"][0]["content"][0]["text"].split("\n", 1)[1]
            read = lyrics.read(words if lyrics.has_tags(words) else UNTAGGED_AS_TAGGED)
            answer = {"sections": [
                {"tag": s["name"], "kind": s["kind"],
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

    # Untagged lyrics: the AI's tagged version comes back as a proposal; nothing is read until it is applied.
    read = await run_job(service, "ai.v1.song.describe", {"text": "pop", "lyrics": UNTAGGED, "ai": True})
    assert read["lyrics"]["lines"] == 0 and read["lyrics"]["proposal"].startswith("[Verse 1]\nEin Tag am Meer")
    assert "[Chorus]\nWir singen laut" in read["lyrics"]["proposal"]
    assert lyrics.read(read["lyrics"]["proposal"])["lines"] == 4
    # Offline, untagged lyrics: the note says to add the tags.
    read = await run_job(service, "ai.v1.song.describe", {"text": "pop", "lyrics": UNTAGGED})
    assert read["lyrics"]["note"] == lyrics.NO_TAGS and "proposal" not in read["lyrics"]

    with pytest.raises(RpcError):
        await service.handle("ai.v1.song.describe", {"text": "pop", "lyrics": "la\n" * 5000}, no_notify)
