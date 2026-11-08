"""Reading an instrument's manuals: the text of PDFs, web pages and text files, the passages about keys, note
names in the manual's own octave numbering, the AI's answer checked, and the whole job with a made-up
provider."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import httpx
import pytest
from test_service import anthropic_reply, make_service, no_notify, run_job

from spinus_ai.manuals import (
    Document,
    ManualError,
    checked_reading,
    document_of,
    midi_of,
    relevant,
    score,
    text_of_html,
    words,
)


def pdf_with(lines: list[str]) -> bytes:
    """A one-page PDF with the lines in Helvetica (pypdf reads its text back)."""
    stream = "BT /F1 12 Tf 72 720 Td 14 TL " + " ".join(f"({line}) '" for line in lines) + " ET"
    objects = [
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
        "/Resources << /Font << /F1 5 0 R >> >> >>",
        f"<< /Length {len(stream)} >>\nstream\n{stream}\nendstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    out = "%PDF-1.4\n"
    offsets = []
    for i, body in enumerate(objects, 1):
        offsets.append(len(out))
        out += f"{i} 0 obj\n{body}\nendobj\n"
    xref = len(out)
    out += f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n"
    out += "".join(f"{o:010d} 00000 n \n" for o in offsets)
    out += f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n"
    return out.encode("latin-1")


STRUMMER_PDF = pdf_with(
    ["Strummer mode", "Press C#6 to switch on the Strummer.", "Chord keys C3 to B3 choose the chord."]
)


def test_note_names_in_the_manuals_own_octave_numbering() -> None:
    assert midi_of("C3", "C3") == 60
    assert midi_of("C#6", "C3") == 97
    assert midi_of("C#6", "C4") == 85
    assert midi_of("C4", "C4") == 60
    assert midi_of("Bb2", "C3") == 58
    assert midi_of("E-1", "C3") == 16
    assert midi_of("C-2", "C3") == 0
    assert midi_of("C9", "C3") is None  # beyond MIDI
    assert midi_of("H2", "C3") is None
    assert midi_of("", "C3") is None


def test_long_answers_are_cut_at_a_word() -> None:
    assert words("  palm   mute ", 100) == "palm mute"
    assert words("chord keys: select chord slots 1-24 in Select mode", 30) == "chord keys: select chord..."
    assert words(None, 10) == ""


def test_the_text_of_a_pdf_a_web_page_and_a_text_file() -> None:
    pdf = document_of("Guitar_Strummer.pdf", STRUMMER_PDF)
    assert pdf.pages == 1
    assert "Press C#6 to switch on the Strummer." in pdf.text
    title, text = text_of_html(
        "<html><head><title>Bass  manual</title><script>var x = 'key';</script></head>"
        "<body><nav>Home | Shop</nav><h1>Key switches</h1><p>E0 to G0 change the articulation.</p></body></html>"
    )
    assert title == "Bass manual"
    assert text == "Key switches\nE0 to G0 change the articulation."
    page = document_of("https://example.org/manual", b"<!DOCTYPE html><p>Sustain: C1</p>", "text/html")
    assert page.text == "Sustain: C1"
    plain = document_of("notes.txt", b"C1 is a key switch")
    assert plain.text == "C1 is a key switch"


def test_a_pdf_without_text_or_not_a_pdf_says_why() -> None:
    with pytest.raises(ManualError, match="no text"):
        document_of("scan.pdf", pdf_with([]))
    with pytest.raises(ManualError, match="could not be read"):
        document_of("broken.pdf", b"%PDF-1.4\nnot really")


def test_the_passages_about_keys_come_first_within_the_budget() -> None:
    filler = "The company was founded long ago and loves music. " * 80
    keys = "Key switches: C0 palm mute, C#0 slide, D0 harmonics. The playable range is E1 to D5."
    doc = Document("m.pdf", "Guitar", filler + "\n\n" + filler + "\n\n" + keys + "\n\n" + filler, 4)
    assert score(keys) > score(filler[:400])
    text = relevant([doc], budget=3_000 + len(keys) + 10)
    assert text.startswith("=== Manual 1: Guitar ===")
    assert keys in text
    assert text.count("founded long ago") < filler.count("founded long ago") * 2  # the rest left out


def test_the_ais_answer_is_checked_and_turned_into_midi_notes() -> None:
    reading = checked_reading(
        {
            "middleC": "C3",
            "modes": [
                {
                    "name": "Strummer",
                    "enter": "the C#6 key",
                    "enterKey": "C#6",
                    "goodFor": "strummed rhythm",
                    "voices": "chords",
                    "zones": [
                        {"from": "C3", "to": "B3", "job": "special", "what": "chord keys"},
                        {"from": "B1", "to": "A1", "job": "switches", "what": "strum patterns"},  # either order
                        {"from": "C2", "to": "nonsense", "job": "plays"},  # dropped
                        {"from": "C2", "to": "C3", "job": "explodes"},  # dropped
                    ],
                },
                {"name": "", "zones": []},  # dropped
                "not a mode",
            ],
        }
    )
    assert reading is not None
    assert reading["middleC"] == "C3" and reading["middleCSaid"]
    [mode] = reading["modes"]
    assert mode["enterKey"] == 97
    assert mode["voices"] == "chords"
    assert mode["zones"] == [
        {"from": 60, "to": 71, "job": "special", "what": "chord keys"},
        {"from": 45, "to": 47, "job": "switches", "what": "strum patterns"},
    ]
    # A manual that doesn't say its middle C: C3 is taken, and Spinus is told it was not said.
    guessed = checked_reading({"middleC": "unknown", "modes": [{"name": "Normal", "zones": []}]})
    assert guessed is not None and not guessed["middleCSaid"] and guessed["middleC"] == "C3"
    assert guessed["modes"][0]["enterKey"] == -1
    assert checked_reading({"middleC": "C3", "modes": []}) is None


def tool_reply(answer: dict[str, Any]) -> bytes:
    return anthropic_reply(tool=("read_manual", answer))


async def test_the_job_reads_a_pdf_and_a_web_page_and_proposes_modes(tmp_path: Path) -> None:
    manual = tmp_path / "Guitar_Strummer.pdf"
    manual.write_bytes(STRUMMER_PDF)
    asked: list[dict[str, Any]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        if request.url.host == "example.org":
            return httpx.Response(
                200,
                content=b"<html><body><p>Palm mute: C0 key switch</p></body></html>",
                headers={"content-type": "text/html; charset=utf-8"},
            )
        if request.url.host == "gone.example.org":
            return httpx.Response(404)
        asked.append(json.loads(request.content))
        answer = {
            "middleC": "C3",
            "modes": [
                {"name": "Normal", "zones": [{"from": "C0", "to": "C0", "job": "switches", "what": "palm mute"}]},
                {
                    "name": "Strummer",
                    "enterKey": "C#6",
                    "voices": "chords",
                    "zones": [{"from": "C3", "to": "B3", "job": "special", "what": "chord keys"}],
                },
            ],
            "notes": "Strummer chord keys pick the chord; the pattern plays it.",
        }
        return httpx.Response(200, content=tool_reply(answer), headers={"content-type": "text/event-stream"})

    service = make_service(tmp_path, handler)
    await service.handle(
        "ai.v1.connections.save", {"name": "Claude", "provider": "anthropic", "key": "sk-test"}, no_notify
    )
    result = await run_job(
        service,
        "ai.v1.instrument.readManual",
        {
            "instrument": "Guitar M",
            "manuals": [
                {"file": str(manual)},
                {"url": "https://example.org/manual"},
                {"url": "https://gone.example.org/manual"},
            ],
        },
    )
    assert [m["name"] for m in result["modes"]] == ["Normal", "Strummer"]
    assert result["modes"][1]["enterKey"] == 97
    assert result["modes"][0]["zones"] == [{"from": 24, "to": 24, "job": "switches", "what": "palm mute"}]
    assert result["read"][0]["pages"] == 1
    assert result["read"][2]["problem"] == "the page answered 404."
    assert result["by"] == "claude-test"
    [request] = asked
    sent = request["messages"][0]["content"][0]["text"]
    assert "The instrument: Guitar M" in sent
    assert "Press C#6 to switch on the Strummer." in sent
    assert "Palm mute: C0 key switch" in sent
    assert request["tools"][0]["name"] == "read_manual"


async def test_the_job_says_why_when_it_cannot_read(tmp_path: Path) -> None:
    service = make_service(tmp_path, lambda request: httpx.Response(404))
    with pytest.raises(RuntimeError, match="needs an AI connection"):
        await run_job(service, "ai.v1.instrument.readManual", {"manuals": [{"file": "/nowhere.pdf"}]})
    await service.handle(
        "ai.v1.connections.save", {"name": "Claude", "provider": "anthropic", "key": "sk-test"}, no_notify
    )
    with pytest.raises(RuntimeError, match=r"No manual could be read \(nowhere.pdf: there is no file"):
        await run_job(service, "ai.v1.instrument.readManual", {"manuals": [{"file": "/nowhere.pdf"}]})
