"""Stem separation: model downloads, the separation job and its cache.

The separation itself needs PyTorch and the Demucs weights; set SPINUS_TEST_COMPUTE_PYTHON (a PyTorch
environment's python with demucs) and SPINUS_TEST_DEMUCS_FOLDER (a folder with htdemucs.yaml and
955717e8.safetensors) to run it for real.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import math
import os
import struct
from pathlib import Path
from typing import Any

import httpx
import pytest

from spinus_ai.compute import weights
from spinus_ai.compute.engine import Engine
from spinus_ai.compute.environments import MARKER, lock_hash
from spinus_ai.compute.weights import AnalysisModel, WeightFile, WeightStore
from spinus_ai.connections import MemoryKeyStore
from spinus_ai.service import RpcError, Service


def write_wav(path: Path, seconds: float = 2.0, rate: int = 44100, notes: tuple[int, ...] = ()) -> Path:
    """A stereo 32-bit float WAV: a bass note and a click every half second, or held notes (MIDI)."""
    frames = int(seconds * rate)
    samples = bytearray()
    for i in range(frames):
        t = i / rate
        if notes:
            value = sum(0.15 * math.sin(2 * math.pi * 440 * 2 ** ((n - 69) / 12) * t) for n in notes)
        else:
            value = 0.3 * math.sin(2 * math.pi * 55 * t) + (0.6 if i % (rate // 2) < 200 else 0.0)
        samples += struct.pack("<ff", value, value)
    fmt = struct.pack("<HHIIHH", 3, 2, rate, rate * 8, 8, 32)
    path.write_bytes(
        b"RIFF" + struct.pack("<I", 36 + len(samples)) + b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt
        + b"data" + struct.pack("<I", len(samples)) + bytes(samples)
    )  # fmt: skip
    return path


async def collect(service: Service, method: str, params: dict[str, Any]) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    updates: list[dict[str, Any]] = []
    finished: asyncio.Future[dict[str, Any]] = asyncio.get_running_loop().create_future()

    async def notify(name: str, payload: dict[str, Any]) -> None:
        updates.append(payload)
        if payload.get("kind") in ("done", "error") and not finished.done():
            finished.set_result(payload)

    await service.handle(method, params, notify)
    return await asyncio.wait_for(finished, 300), updates


def small_model(contents: dict[str, bytes]) -> AnalysisModel:
    files = tuple(WeightFile(name, len(data), hashlib.sha256(data).hexdigest()) for name, data in contents.items())
    return AnalysisModel("tiny", "Tiny", "For tests", "MIT", "someone/Tiny", "abc123", files, ("a", "b"))


async def test_model_files_are_downloaded_checked_and_removed(tmp_path: Path) -> None:
    contents = {"tiny.yaml": b"models: ['x']\n", "x.safetensors": b"\x00" * 3000}
    model = small_model(contents)
    asked: list[str] = []

    def handler(request: httpx.Request) -> httpx.Response:
        asked.append(str(request.url))
        name = request.url.path.rsplit("/", 1)[1]
        return httpx.Response(200, content=contents[name])

    store = WeightStore(tmp_path / "analysis")
    seen: list[tuple[int, int]] = []

    async def progress(done: int, total: int) -> None:
        seen.append((done, total))

    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
        assert not store.is_ready(model)
        await store.download(client, model, progress)
    assert store.is_ready(model)
    assert asked[0] == "https://huggingface.co/someone/Tiny/resolve/abc123/tiny.yaml"  # pinned revision
    assert seen[-1] == (model.size, model.size)

    # A file that does not match its checksum: nothing is kept.
    store.remove(model)
    contents["x.safetensors"] = b"\x01" * 3000
    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
        with pytest.raises(RuntimeError, match="checksum"):
            await store.download(client, model, progress)
    assert not store.folder(model).exists()


async def test_a_server_error_or_dropped_connection_is_tried_again(tmp_path: Path) -> None:
    contents = {"x.safetensors": b"\x00" * 3000}
    model = small_model(contents)
    answers = ["500", "drop", "ok"]

    def handler(request: httpx.Request) -> httpx.Response:
        answer = answers.pop(0) if answers else "404"
        if answer == "drop":
            raise httpx.ReadError("connection reset")
        return httpx.Response(200 if answer == "ok" else int(answer), content=contents["x.safetensors"])

    store = WeightStore(tmp_path / "analysis", retry_waits=(0, 0, 0))
    async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
        await store.download(client, model, _no_progress)
        assert store.is_ready(model)

        # Not found is not tried again; nor is a server that keeps failing, after the last wait.
        store.remove(model)
        with pytest.raises(RuntimeError, match=r"\(404\)"):
            await store.download(client, model, _no_progress)
        answers[:] = ["503"] * 4
        with pytest.raises(
            RuntimeError, match=r"its authors' server \(huggingface.co\) isn't answering \(server error 503\)"
        ):
            await store.download(client, model, _no_progress)
        assert answers == []
    assert not store.folder(model).exists()


async def _no_progress(done: int, total: int) -> None:
    pass


def test_the_demucs_models_are_pinned() -> None:
    for model in weights.MODELS:
        assert model.license in ("MIT", "Apache-2.0")  # AGPLv3-compatible, not non-commercial (AGENTS rule 5)
        # A Hugging Face commit, or a release on the authors' own server or GitHub (checked by SHA-256 either way).
        assert len(model.revision) == 40 or model.url.startswith("https://")
        assert all(len(f.sha256) == 64 and f.size > 0 for f in model.files)
    four = weights.find_model("htdemucs")
    assert four is not None and four.stems == ("drums", "bass", "other", "vocals")
    assert 80e6 < four.size < 90e6


def service_with_fake_engine(tmp_path: Path, calls: list[dict[str, Any]]) -> Service:
    engine = Engine(tmp_path / "config", tmp_path / "python", tmp_path / "cache", gpus=lambda: [], cores=lambda: 16)

    async def call(method: str, params: dict[str, Any], progress: Any = None) -> Any:
        calls.append({"method": method, **params})
        if method == "beats":
            beats = [0.25 + i * 0.5 for i in range(16)]
            return {"beats": beats, "downbeats": beats[::4], "seconds": 0.1}
        if progress is not None:
            await progress({"done": 0.5, "step": "Separating"})
        output = Path(params["output"])
        output.mkdir(parents=True, exist_ok=True)
        stems = {}
        for name in ("drums", "bass", "other", "vocals"):
            (output / f"{name}.wav").write_bytes(b"RIFF")
            stems[name] = str(output / f"{name}.wav")
        return {"stems": stems, "seconds": 1.0}

    engine.call = call  # type: ignore[method-assign]
    return Service(tmp_path / "config", MemoryKeyStore(), None, None, models=tmp_path / "models", engine=engine)


async def test_separation_is_cached_by_the_songs_contents(tmp_path: Path) -> None:
    calls: list[dict[str, Any]] = []
    service = service_with_fake_engine(tmp_path, calls)
    song = write_wav(tmp_path / "song.wav")
    params = {"file": str(song), "model": "htdemucs", "cacheFolder": str(tmp_path / "project/cache")}

    # The model must be downloaded first (the user agrees to its size).
    with pytest.raises(RpcError, match="not downloaded yet"):
        await service.handle("ai.v1.stems.separate", params, lambda *_: asyncio.sleep(0))
    model = weights.find_model("htdemucs")
    assert model is not None
    folder = service.weights.folder(model)
    folder.mkdir(parents=True)
    for f in model.files:
        with (folder / f.path).open("wb") as out:
            out.truncate(f.size)  # the right size is enough for "downloaded" here
    assert service.weights.describe()[0]["downloaded"]

    done, updates = await collect(service, "ai.v1.stems.separate", {**params, "quiet": True})
    assert done["kind"] == "done", done
    result = done["result"]
    assert set(result["stems"]) == {"drums", "bass", "other", "vocals"} and not result["cached"]
    assert Path(result["folder"]).parent == tmp_path / "project/cache/stems"
    assert calls[0]["threads"] == 12  # quiet: four cores left for the audio
    assert any(u.get("step") == "Separating" for u in updates)

    # The same song again (even copied elsewhere): from the cache, without the engine.
    copy = tmp_path / "copy.wav"
    copy.write_bytes(song.read_bytes())
    done, _ = await collect(service, "ai.v1.stems.separate", {**params, "file": str(copy)})
    assert done["result"]["cached"] and len(calls) == 1

    with pytest.raises(RpcError, match="no audio file"):
        await service.handle("ai.v1.stems.separate", {**params, "file": str(tmp_path / "nope.wav")}, None)  # type: ignore[arg-type]
    await service.close()


@pytest.mark.skipif(
    not (os.environ.get("SPINUS_TEST_COMPUTE_PYTHON") and os.environ.get("SPINUS_TEST_DEMUCS_FOLDER")),
    reason="needs a PyTorch environment with Demucs and its weights",
)
async def test_real_separation(tmp_path: Path) -> None:
    python = Path(os.environ["SPINUS_TEST_COMPUTE_PYTHON"])
    engine = Engine(tmp_path / "config", tmp_path / "python", tmp_path / "cache")
    build = engine.catalog.find(engine.choice().build)
    assert build is not None
    env = engine.environments.folder(build)
    (env / "bin").mkdir(parents=True)
    (env / "bin/python").symlink_to(python)
    (env / "pyvenv.cfg").write_text((python.parent.parent / "pyvenv.cfg").read_text())
    (env / "lib").symlink_to(python.parent.parent / "lib")
    (env / MARKER).write_text(json.dumps({"build": build.id, "lock": lock_hash(build)}))
    service = Service(tmp_path / "config", MemoryKeyStore(), None, None, models=tmp_path / "models", engine=engine)
    model = weights.find_model("htdemucs")
    assert model is not None
    service.weights.folder(model).parent.mkdir(parents=True)
    service.weights.folder(model).symlink_to(Path(os.environ["SPINUS_TEST_DEMUCS_FOLDER"]))

    song = write_wav(tmp_path / "song.wav", seconds=6.0)
    done, updates = await collect(service, "ai.v1.stems.separate", {"file": str(song), "cacheFolder": str(tmp_path)})
    assert done["kind"] == "done", done
    print(done["result"], len(updates), "updates")
    from spinus_ai.compute.worker import read_wav

    for path in done["result"]["stems"].values():
        audio, rate = read_wav(path)
        assert rate == 44100 and audio.shape[0] == 2 and audio.shape[1] > 6 * 44100 * 0.95
    await engine.stop()


def fake_download(service: Service, model_id: str) -> None:
    model = weights.find_model(model_id)
    assert model is not None
    folder = service.weights.folder(model)
    folder.mkdir(parents=True, exist_ok=True)
    for f in model.files:
        with (folder / f.path).open("wb") as out:
            out.truncate(f.size)


async def test_tempo_and_key_of_a_song(tmp_path: Path) -> None:
    calls: list[dict[str, Any]] = []
    service = service_with_fake_engine(tmp_path, calls)
    song = write_wav(tmp_path / "song.wav")

    with pytest.raises(RpcError, match=r"Beat This! .*not downloaded yet"):
        await service.handle("ai.v1.analyze.tempo", {"file": str(song)}, None)  # type: ignore[arg-type]
    fake_download(service, "beat_this_small0")  # no GPU here: the small one
    done, _ = await collect(service, "ai.v1.analyze.tempo", {"file": str(song), "cacheFolder": str(tmp_path / "c")})
    assert done["kind"] == "done", done
    result = done["result"]
    assert result["tempo"] == 120.0 and result["beatsPerBar"] == 4 and result["firstDownbeat"] == 0.25
    assert calls[-1]["checkpoint"].endswith("small0.ckpt")
    done, _ = await collect(service, "ai.v1.analyze.tempo", {"file": str(song), "cacheFolder": str(tmp_path / "c")})
    assert done["result"]["cached"] and len(calls) == 1

    # The key, from the audio itself: an A minor chord (A, C, E) over an A bass.
    chord = tmp_path / "chord.wav"
    write_wav(chord, notes=(45, 57, 60, 64))
    done, _ = await collect(service, "ai.v1.analyze.key", {"file": str(chord)})
    assert done["kind"] == "done", done
    assert done["result"]["name"] == "A minor", done["result"]
    await service.close()
