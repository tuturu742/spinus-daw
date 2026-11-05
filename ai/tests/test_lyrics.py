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


def test_the_ai_count_is_checked_line_by_line() -> None:
    texts = ["one line here", "two"]
    found = lyrics.checked_counts(
        {"lines": [{"line": 1, "syllables": 3, "stress": "101"}, {"line": 2, "syllables": 2, "stress": "1"}]}, texts
    )
    assert list(found) == [0] and found[0].stress == "101"  # the second's stress is not as long as its count
    assert lyrics.checked_counts({"lines": [{"line": 9, "syllables": 1, "stress": "1"}]}, texts) == {}
    assert lyrics.checked_counts("nonsense", texts) == {}


def counting_service(tmp_path: Path, sent: list[dict[str, Any]]) -> Any:
    def handler(request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content)
        sent.append(body)
        if body["tools"][0]["name"] == "count_syllables":
            lines = body["messages"][0]["content"][0]["text"].splitlines()[1:]
            answer = {"lines": [{"line": i + 1, "syllables": 2, "stress": "01"} for i in range(len(lines))]}
            tool = ("count_syllables", answer)
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
    counting = [b for b in sent if b["tools"][0]["name"] == "count_syllables"]
    assert len(counting) == 1 and "4. Another morning, another day" in counting[0]["messages"][0]["content"][0]["text"]

    with pytest.raises(RpcError):
        await service.handle("ai.v1.song.describe", {"text": "pop", "lyrics": "la\n" * 5000}, no_notify)
