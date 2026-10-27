"""The Track creator's whole path for real: the CPU analysis engine installed with uv, the models it needs
downloaded, and a short made-up song taken through every step the Track creator runs, up to a written song.

It downloads about 300 MB (PyTorch for the CPU, Demucs, Beat This!, Basic Pitch) and takes some minutes,
so it runs only with SPINUS_TEST_REAL_ENGINE=1 (the Windows workflow sets it: there is no other Windows
check of the engine).
"""

from __future__ import annotations

import asyncio
import math
import os
import random
import re
import struct
from pathlib import Path
from typing import Any

import pytest

from spinus_ai.connections import MemoryKeyStore
from spinus_ai.service import Service

pytestmark = pytest.mark.skipif(
    not os.environ.get("SPINUS_TEST_REAL_ENGINE"), reason="installs the engine and downloads models"
)

RATE = 44100
TEMPO = 120.0
# Am F C G, a bar each (MIDI notes), with the bass an octave and more below.
CHORDS = [(57, 60, 64), (53, 57, 60), (48, 52, 55), (55, 59, 62)]
LEAD = [69, 72, 76, 74, 72, 69, 65, 67]


def made_song(path: Path, bars: int = 12) -> Path:
    """A stereo float WAV: kick on the beats, snare on 2 and 4, hats on the eighths, bass, chords and a lead."""
    beat = 60.0 / TEMPO
    frames = int(bars * 4 * beat * RATE)
    noise = random.Random(7)
    out = [0.0] * frames

    def add(start: float, seconds: float, voice: Any) -> None:
        first = int(start * RATE)
        for i in range(first, min(frames, first + int(seconds * RATE))):
            out[i] += voice((i - first) / RATE)

    def tone(note: int, level: float, decay: float) -> Any:
        frequency = 440.0 * 2 ** ((note - 69) / 12)
        return lambda t: level * math.exp(-t / decay) * math.sin(2 * math.pi * frequency * t)

    def kick(t: float) -> float:
        return 0.8 * math.exp(-t / 0.05) * math.sin(2 * math.pi * (50 + 80 * math.exp(-t / 0.02)) * t)

    for bar in range(bars):
        chord = CHORDS[bar % len(CHORDS)]
        at = bar * 4 * beat
        for b in range(4):
            add(at + b * beat, 0.25, kick)
            if b in (1, 3):
                add(at + b * beat, 0.2, lambda t: 0.4 * math.exp(-t / 0.06) * (noise.random() * 2 - 1))
            add(at + b * beat, beat, tone(chord[0] - 12, 0.3, 0.4))
        for e in range(8):
            add(at + e * beat / 2, 0.05, lambda t: 0.12 * math.exp(-t / 0.01) * (noise.random() * 2 - 1))
        for note in chord:
            add(at, 4 * beat, tone(note, 0.1, 2.0))
        for n, note in enumerate(LEAD[(bar % 2) * 4 : (bar % 2) * 4 + 4]):
            add(at + n * beat, beat * 0.9, tone(note + 12, 0.15, 0.5))

    samples = b"".join(struct.pack("<ff", v * 0.5, v * 0.5) for v in out)
    fmt = struct.pack("<HHIIHH", 3, 2, RATE, RATE * 8, 8, 32)
    path.write_bytes(
        b"RIFF" + struct.pack("<I", 36 + len(samples)) + b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt
        + b"data" + struct.pack("<I", len(samples)) + samples
    )  # fmt: skip
    return path


async def job(service: Service, method: str, params: dict[str, Any], timeout: float = 1800) -> Any:
    """Runs a job as the Track creator does and returns its result (or fails with its message)."""
    finished: asyncio.Future[dict[str, Any]] = asyncio.get_running_loop().create_future()

    async def notify(name: str, payload: dict[str, Any]) -> None:
        if payload.get("kind") in ("done", "error") and not finished.done():
            finished.set_result(payload)

    await service.handle(method, params, notify)
    payload = await asyncio.wait_for(finished, timeout)
    assert payload["kind"] == "done", f"{method}: {payload}"
    return payload["result"]


async def test_the_track_creator_runs_on_the_real_engine(tmp_path: Path) -> None:
    service = Service(tmp_path / "config", MemoryKeyStore(), None, None, models=tmp_path / "data" / "models")
    try:
        await job(service, "ai.v1.compute.install", {"build": "cpu"})
        status = await service.handle("ai.v1.compute.status", {}, None)  # type: ignore[arg-type]
        assert status["uses"]["build"] == "cpu", status
        test = (await job(service, "ai.v1.compute.test", {}))["test"]
        assert test["ok"], test
        for model in ("htdemucs", "beat_this_small0", "basic_pitch"):
            try:
                await job(service, "ai.v1.stems.download", {"model": model})
            except AssertionError as error:
                # The models come from their authors' servers (Beat This! from its university's): when one
                # keeps answering with a server error, that is their outage, not Spinus's.
                if re.search(r"Could not download .* \(5\d\d\)", str(error)):
                    pytest.skip(f"{model}'s server is down: {error}")
                raise

        song = made_song(tmp_path / "song.wav")
        cache = tmp_path / "work"
        rhythm = await job(service, "ai.v1.analyze.tempo", {"file": str(song), "cacheFolder": str(cache)})
        assert abs(rhythm["tempo"] - TEMPO) < 3 or abs(rhythm["tempo"] - TEMPO / 2) < 2, rhythm["tempo"]
        key = await job(service, "ai.v1.analyze.key", {"file": str(song)})
        loudness = (await job(service, "ai.v1.mix.analyze", {"inserts": [{"name": "song", "file": str(song)}]}))[
            "inserts"
        ]["song"]
        separated = await job(
            service, "ai.v1.stems.separate", {"file": str(song), "model": "htdemucs", "cacheFolder": str(cache)}
        )
        stems: dict[str, str] = separated["stems"]
        assert set(stems) == {"drums", "bass", "other", "vocals"}, stems

        listen = {
            "beats": rhythm["beats"],
            "firstDownbeat": rhythm.get("firstDownbeat", 0.0),
            "cacheFolder": str(cache),
        }
        drums = (await job(service, "ai.v1.transcribe.drums", {**listen, "file": stems["drums"]}))["hits"]
        assert drums, "no drum hits"
        parts = {}
        for name in ("bass", "other", "vocals"):
            parts[name] = (await job(service, "ai.v1.transcribe.notes", {**listen, "file": stems[name]}))["seconds"]
        assert parts["bass"] and parts["other"], {k: len(v) for k, v in parts.items()}

        measured = {
            "name": "made up", "file": str(song), "tempo": rhythm["tempo"], "beatsPerBar": rhythm["beatsPerBar"],
            "firstDownbeat": rhythm.get("firstDownbeat"), "beats": rhythm["beats"], "downbeats": rhythm["downbeats"],
            "key": key, "loudness": loudness, "parts": parts, "drums": drums, "stems": stems,
        }  # fmt: skip
        summary = await job(service, "ai.v1.style.summarize", {"songs": [measured, {**measured, "name": "again"}]})
        written = await job(service, "ai.v1.song.write", {"summary": summary, "seed": 1})
        assert written["sections"], written.keys()
        print("tempo", rhythm["tempo"], "key", key, "drum hits", len(drums), "sections", len(written["sections"]))
    finally:
        await service.close()
