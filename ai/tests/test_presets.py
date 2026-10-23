"""Closest preset: measuring sounds, the preset index and the service methods."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import numpy as np
import pytest

from spinus_ai.compute import weights
from spinus_ai.presets import BANDS, PresetIndex, describe_sound, stem_clips, windows
from spinus_ai.service import RpcError

RATE = 48000


def tone(freq: float, seconds: float = 1.0, bright: bool = False) -> np.ndarray:
    t = np.arange(int(seconds * RATE)) / RATE
    if bright:
        result: np.ndarray = 0.3 * (2 * ((t * freq) % 1) - 1)  # a sawtooth: many overtones
    else:
        result = 0.3 * np.sin(2 * np.pi * freq * t)
    return result


def test_measurements_tell_bright_from_dull_and_fast_from_slow() -> None:
    dull, bright = describe_sound(tone(220), RATE), describe_sound(tone(220, bright=True), RATE)
    assert len(dull) == BANDS + 2
    assert bright[BANDS] > dull[BANDS] + 0.5  # brightness, in octaves
    slow = tone(220) * np.minimum(1.0, np.arange(RATE) / (0.5 * RATE))
    assert describe_sound(slow, RATE)[BANDS + 1] > dull[BANDS + 1] + 0.5  # attack (log10 seconds)


def test_the_loudest_parts_of_a_stem_are_compared() -> None:
    audio = np.concatenate([tone(110, 2.0) * 0.1, np.zeros(RATE), tone(110, 8.0), np.zeros(RATE), tone(110, 1.0)])
    clips = stem_clips(audio, RATE, most=2, longest=6.0)
    assert len(clips) == 2
    assert abs(clips[0][0] - 3.0) < 0.02 and abs(clips[0][1] - 9.0) < 0.02  # the long loud part, 6 s of it
    assert abs(clips[1][0] - 12.0) < 0.02
    assert stem_clips(np.zeros(RATE), RATE) == [(0.0, 1.0)]


def test_clips_become_one_second_windows() -> None:
    assert windows([(0.0, 2.6), (5.0, 5.3)]) == [(0.0, 1.0), (1.0, 2.0), (2.0, 2.6)]
    assert windows([(5.0, 5.3)]) == [(5.0, 5.3)]  # too short for a window: the clip itself
    assert len(windows([(0.0, 100.0)])) == 12


def test_the_index_keeps_finds_and_ranks(tmp_path: Path) -> None:
    rng = np.random.default_rng(1)
    index = PresetIndex(tmp_path / "x.json")
    common = rng.normal(size=16) * 5  # what all presets share: must not decide anything
    vectors = {f"p{i}": common + rng.normal(size=16) for i in range(30)}
    for name, v in vectors.items():
        index.add(name, "v1", [list(v / np.linalg.norm(v))] * 3, [float(rng.normal()), 0.0])
    index.save()
    again = PresetIndex(tmp_path / "x.json")
    assert again.missing([{"id": "p1", "version": "v1"}, {"id": "p1b", "version": "v1"}]) == ["p1b"]
    assert again.missing([{"id": "p2", "version": "v2"}]) == ["p2"]  # changed since

    def unit(v: np.ndarray) -> list[float]:
        return list(v / np.linalg.norm(v))

    target = [unit(vectors["p7"] + rng.normal(size=16) * 0.1) for _ in range(3)]
    found = again.closest(target, again.entries["p7"]["features"], top=3)
    assert found[0]["id"] == "p7" and found[0]["score"] > 0.9
    assert len(found) == 3
    assert again.closest(target, [0.0, 0.0], only=["p3", "p4"])[0]["id"] in ("p3", "p4")
    assert again.closest([], [0.0, 0.0]) == []

    (tmp_path / "x.json").write_text('{"version": 0, "presets": {"a": {}}}')
    assert PresetIndex(tmp_path / "x.json").entries == {}  # an older index is measured again


def write_wav(path: Path, audio: np.ndarray) -> Path:
    import wave

    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes((np.clip(audio, -1, 1) * 32767).astype("<i2").tobytes())
    return path


async def test_the_service_measures_presets_and_finds_the_closest(tmp_path: Path) -> None:
    from test_stems import collect, fake_download, service_with_fake_engine

    calls: list[dict[str, Any]] = []
    service = service_with_fake_engine(tmp_path, calls)

    async def embed(method: str, params: dict[str, Any], progress: Any = None) -> Any:
        # A stand-in for CLAP: the spectrum's shape.
        from spinus_ai.compute.worker import read_wav

        calls.append({"method": method, **params})
        vectors = []
        for clip in params["clips"]:
            audio, rate = read_wav(clip["file"])
            mono = audio.mean(axis=0)[int(clip.get("start", 0) * rate) : int(clip.get("end", 99) * rate)]
            v = np.array(describe_sound(mono, rate)[:BANDS]) + 60
            vectors.append(list(v / np.linalg.norm(v)))
        return {"embeddings": vectors}

    service.engine.call = embed  # type: ignore[method-assign]
    presets = [{"id": "Soft", "version": "1"}, {"id": "Saw", "version": "1"}, {"id": "Deep", "version": "1"}]
    done = await service.handle("ai.v1.presets.missing", {"library": "surge-xt", "presets": presets}, None)  # type: ignore[arg-type]
    assert done == {"missing": ["Soft", "Saw", "Deep"]}
    with pytest.raises(RpcError, match="not downloaded"):
        await service.handle("ai.v1.presets.add", {"library": "surge-xt", "previews": []}, None)  # type: ignore[arg-type]
    with pytest.raises(RpcError, match="plain name"):
        await service.handle("ai.v1.presets.missing", {"library": "../x"}, None)  # type: ignore[arg-type]
    fake_download(service, "clap_music")

    previews = [
        {"id": "Soft", "version": "1", "file": str(write_wav(tmp_path / "a.wav", tone(440)))},
        {"id": "Saw", "version": "1", "file": str(write_wav(tmp_path / "b.wav", tone(440, bright=True)))},
        {"id": "Deep", "version": "1", "file": str(write_wav(tmp_path / "c.wav", tone(55)))},
    ]
    done, _ = await collect(service, "ai.v1.presets.add", {"library": "surge-xt", "previews": previews})
    assert done["kind"] == "done", done
    assert done["result"] == {"added": 3, "total": 3}
    assert not (tmp_path / "a.wav").exists()  # the recordings are gone, the numbers kept
    missing = await service.handle("ai.v1.presets.missing", {"library": "surge-xt", "presets": presets}, None)  # type: ignore[arg-type]
    assert missing == {"missing": []}

    stem = write_wav(tmp_path / "stem.wav", np.concatenate([np.zeros(RATE), tone(440, 3.0, bright=True)]))
    done, _ = await collect(service, "ai.v1.presets.match", {"library": "surge-xt", "file": str(stem), "top": 2})
    assert done["kind"] == "done", done
    assert done["result"]["presets"][0]["id"] == "Saw"
    assert calls[-1]["clips"][0]["start"] == pytest.approx(1.0, abs=0.02)
    await service.close()


def test_the_clap_model_is_pinned() -> None:
    model = weights.find_model("clap_music")
    assert model is not None and model.license == "Apache-2.0"
    assert model.size == 628 + 541 + 776444665


async def test_an_engine_from_before_closest_preset_asks_for_an_update(tmp_path: Path) -> None:
    from test_stems import collect, fake_download, service_with_fake_engine

    from spinus_ai.compute.engine import EngineError

    service = service_with_fake_engine(tmp_path, [])

    async def old_engine(method: str, params: dict[str, Any], progress: Any = None) -> Any:
        raise EngineError("No module named 'transformers'")

    service.engine.call = old_engine  # type: ignore[method-assign]
    fake_download(service, "clap_music")
    preview = write_wav(tmp_path / "a.wav", tone(440))
    done, _ = await collect(
        service, "ai.v1.presets.add", {"library": "surge-xt", "previews": [{"id": "A", "file": str(preview)}]}
    )
    assert done["kind"] == "error" and "needs an update" in done["message"], done
    await service.close()


async def test_drum_samples_are_measured_as_one_shots_and_kept(tmp_path: Path) -> None:
    from test_stems import collect, fake_download, service_with_fake_engine

    calls: list[dict[str, Any]] = []
    service = service_with_fake_engine(tmp_path, calls)

    async def embed(method: str, params: dict[str, Any], progress: Any = None) -> Any:
        calls.append({"method": method, **params})
        return {"embeddings": [[1.0, float(i)] for i, _ in enumerate(params["clips"])]}

    service.engine.call = embed  # type: ignore[method-assign]
    fake_download(service, "clap_music")
    sample = write_wav(tmp_path / "Snare 1.wav", tone(200, 0.4))
    previews = [{"id": str(sample), "version": "1", "file": str(sample)}]
    done, _ = await collect(
        service,
        "ai.v1.presets.add",
        {"library": "drum-samples", "previews": previews, "oneShot": True, "keepFiles": True},
    )
    assert done["kind"] == "done", done
    assert sample.exists()  # the user's own file stays
    assert [(c["start"], c["end"]) for c in calls[-1]["clips"]] == [(0.0, 1.0)]  # one second, one vector

    stem = write_wav(tmp_path / "drums.wav", np.concatenate([np.zeros(RATE), tone(200, 0.4), np.zeros(RATE)]))
    done, _ = await collect(
        service, "ai.v1.presets.match", {"library": "drum-samples", "file": str(stem), "start": 1.0, "end": 1.4}
    )
    assert done["kind"] == "done", done
    assert done["result"]["presets"][0]["id"] == str(sample)
    assert [(c["start"], c["end"]) for c in calls[-1]["clips"]] == [(1.0, 1.4)]  # just that hit
    await service.close()
