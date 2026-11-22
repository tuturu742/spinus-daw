"""Drums to steps: the recogniser trained on Spinus-made drum sounds, and hits on the beats."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest

from spinus_ai import drums
from spinus_ai.drums import (
    KICK_LOW,
    KINDS,
    PADS,
    RATE,
    Recogniser,
    check_kicks,
    describe,
    find_hits,
    low_share,
    make_hit,
    place_hits,
)


@pytest.fixture
def model(drum_model: Recogniser) -> Recogniser:
    return drum_model


def test_every_kind_of_training_sound_is_a_clean_hit() -> None:
    rng = np.random.default_rng(3)
    for kind in KINDS:
        hit = make_hit(kind, rng)
        assert np.all(np.isfinite(hit)) and abs(np.max(np.abs(hit)) - 1.0) < 1e-6, kind
        assert describe(hit).shape == describe(make_hit("kick", rng)).shape


def test_the_recogniser_tells_the_main_drums_apart(model: Recogniser) -> None:
    rng = np.random.default_rng(42)
    for kind in ("kick", "snare", "closed_hat", "crash"):
        right = 0
        for _ in range(20):
            probabilities = model.probabilities(describe(make_hit(kind, rng) * 0.7))
            right += KINDS[int(np.argmax(probabilities))] == kind
        assert right >= 16, (kind, right)


def test_it_is_kept_and_trained_only_once(tmp_path: Path, model: Recogniser, monkeypatch: pytest.MonkeyPatch) -> None:
    model.save(tmp_path / f"drum-recogniser-{drums.VERSION}.json")
    monkeypatch.setattr(drums, "train", lambda: pytest.fail("trained again"))
    loaded = drums.recogniser(tmp_path)
    features = describe(make_hit("snare", np.random.default_rng(1)))
    assert np.allclose(loaded.probabilities(features), model.probabilities(features))
    (tmp_path / f"drum-recogniser-{drums.VERSION}.json").write_text('{"version": 0}')
    assert drums.Recogniser.load(tmp_path / f"drum-recogniser-{drums.VERSION}.json") is None  # older: train again


def test_a_beat_becomes_drum_machine_steps(model: Recogniser) -> None:
    """Kick on 1 and 3, snare on 2 and 4, closed hats on the eighths, at 120 BPM."""
    rng = np.random.default_rng(7)
    seconds = 4.0
    audio = np.zeros(int(seconds * RATE))

    def put(kind: str, at: float, level: float) -> None:
        hit = make_hit(kind, rng) * level
        start = int(at * RATE)
        audio[start : start + len(hit)] += hit[: len(audio) - start]

    for beat in range(8):
        t = beat * 0.5
        put("kick" if beat % 2 == 0 else "snare", t, 0.8)
        put("closed_hat", t + 0.25, 0.3)
    hits = find_hits(audio[None, :], RATE, model)
    kinds = [h["kind"] for h in hits]
    # Hi-hats right after a kick or snare are the hardest (about three in four are found on test sounds).
    assert kinds.count("kick") >= 3 and kinds.count("snare") >= 3 and kinds.count("closed_hat") >= 5, kinds

    beats = [i * 0.5 for i in range(9)]
    notes = place_hits(hits, beats, 0.0)
    kicks = sorted(n["start"] for n in notes if n["pitch"] == PADS["kick"])
    assert kicks[:2] == [0, 1920]
    assert all(n["start"] % 240 == 0 and n["length"] == 240 for n in notes)


async def test_the_service_turns_a_drum_stem_into_steps(tmp_path: Path, model: Recogniser) -> None:
    from test_stems import collect, service_with_fake_engine

    service = service_with_fake_engine(tmp_path, [])
    model.save(tmp_path / "config" / "cache" / f"drum-recogniser-{drums.VERSION}.json")  # trained already
    rng = np.random.default_rng(5)
    audio = np.zeros(int(2.0 * RATE))
    for i, kind in enumerate(["kick", "snare", "kick", "snare"]):
        hit = make_hit(kind, rng) * 0.8
        start = int(i * 0.5 * RATE)
        audio[start : start + len(hit)] += hit[: len(audio) - start]
    stem = tmp_path / "drums.wav"
    write_mono_wav(stem, audio)
    params = {"file": str(stem), "beats": [i * 0.5 for i in range(5)], "firstDownbeat": 0.0,
              "cacheFolder": str(tmp_path / "c")}  # fmt: skip
    done, _ = await collect(service, "ai.v1.transcribe.drums", params)
    assert done["kind"] == "done", done
    notes = done["result"]["notes"]
    assert [(n["start"], n["pitch"]) for n in notes if n["pitch"] in (PADS["kick"], PADS["snare"])] == [
        (0, PADS["kick"]), (960, PADS["snare"]), (1920, PADS["kick"]), (2880, PADS["snare"])
    ]  # fmt: skip
    groups = done["result"]["groups"]
    assert {g["name"] for g in groups} >= {"kick", "snare"}
    assert all(g["end"] > g["start"] for g in groups)
    assert list((tmp_path / "c" / "analysis").glob("*-drums-*.json"))  # kept for next time
    await service.close()


async def test_the_drum_transcription_model_finds_the_hits_when_downloaded(tmp_path: Path, model: Recogniser) -> None:
    from typing import Any

    from test_stems import collect, fake_download, service_with_fake_engine

    calls: list[dict[str, Any]] = []
    service = service_with_fake_engine(tmp_path, calls)
    model.save(tmp_path / "config" / "cache" / f"drum-recogniser-{drums.VERSION}.json")
    heard = [
        {"time": 0.0, "kind": "kick", "pad": PADS["kick"], "velocity": 1.0, "finder": drums.MODEL_FINDER},
        {"time": 0.5, "kind": "snare", "pad": PADS["snare"], "velocity": 1.0, "finder": drums.MODEL_FINDER},
    ]

    async def engine(method: str, params: dict[str, Any], progress: Any = None) -> Any:
        calls.append({"method": method, **params})
        return {"hits": heard}

    service.engine.call = engine  # type: ignore[method-assign]
    service.engine.plan = lambda: object()  # type: ignore[assignment,method-assign,return-value]
    fake_download(service, "drum_transcriber")
    stem = tmp_path / "drums.wav"
    write_mono_wav(stem, np.zeros(RATE))
    old = [{"time": 0.25, "kind": "snare", "pad": PADS["snare"], "velocity": 1.0}]  # an earlier finder's
    params = {"file": str(stem), "beats": [i * 0.5 for i in range(5)], "firstDownbeat": 0.0,
              "cacheFolder": str(tmp_path / "c"), "hits": old}  # fmt: skip
    done, _ = await collect(service, "ai.v1.transcribe.drums", params)
    assert done["kind"] == "done", done
    assert [c["method"] for c in calls] == ["drum_hits"]  # found again, with the model
    assert [(n["start"], n["pitch"]) for n in done["result"]["notes"]] == [(0, PADS["kick"]), (960, PADS["snare"])]
    # Its hits, kept in the library, are used as they are; so is the cache.
    params["hits"] = done["result"]["hits"]
    done, _ = await collect(service, "ai.v1.transcribe.drums", params)
    assert done["kind"] == "done" and len(calls) == 1
    del params["hits"]
    done, _ = await collect(service, "ai.v1.transcribe.drums", params)
    assert done["kind"] == "done" and len(calls) == 1
    await service.close()


def test_the_models_onsets_become_hits() -> None:
    from spinus_ai.drums import MODEL_LATE, model_hits, pick_peaks

    rate = 44100 / 256
    frames = int(2 * rate)
    onsets = np.zeros((3, frames))
    velocities = np.ones((3, frames))
    kicks = [int(0.5 * rate), int(0.547 * rate), int(1.5 * rate)]  # two of a fast double bass drum, 47 ms apart
    for f in kicks:
        onsets[0, f - 1 : f + 2] = [0.4, 0.9, 0.4]
    velocities[0, kicks[2]] = 0.5
    onsets[1, int(1.0 * rate)] = 0.05  # too faint
    onsets[2] += 0.95  # never above its neighbourhood: no hits
    assert list(pick_peaks(onsets[0], rate)) == kicks
    hits = model_hits(onsets, velocities, ["KD", "SD", "TT_HFT"], rate)
    assert [h["kind"] for h in hits] == ["kick"] * 3
    assert hits[0]["time"] == pytest.approx(kicks[0] / rate - MODEL_LATE, abs=1e-4)
    assert [h["velocity"] for h in hits] == [1.0, 1.0, 0.5]
    assert all(h["pad"] == PADS["kick"] and h["finder"] == drums.MODEL_FINDER for h in hits)
    assert drums.MODEL_KINDS["TT_HFT"] == "low_tom" and drums.MODEL_KINDS["TT_HMT"] == "high_tom"


def test_the_drum_transcription_model_is_pinned() -> None:
    from spinus_ai.compute import weights

    model = weights.find_model("drum_transcriber")
    assert model is not None and model.license == "Apache-2.0" and model.kind == "drums"
    assert model.size == 2544694 and model.revision in model.url


def write_mono_wav(path: Path, audio: np.ndarray) -> None:
    import wave

    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes((np.clip(audio, -1, 1) * 32767).astype("<i2").tobytes())


def test_two_different_snares_become_two_pads_and_one_kick_stays_one() -> None:
    from spinus_ai.drums import FREE_PADS, group_hits

    rng = np.random.default_rng(3)
    kick = make_hit("kick", rng)
    bright = make_hit("snare", rng)
    t = np.arange(len(bright)) / RATE
    dark = np.interp(np.arange(len(bright)) * 0.55, np.arange(len(bright)), bright) * np.exp(-t * 4)  # lower, darker
    audio = np.zeros(int(8.0 * RATE))
    hits = []
    for i in range(8):
        k, s = i * 1.0, i * 1.0 + 0.5
        level = 0.9 if i % 2 == 0 else 0.45  # accents
        audio[int(k * RATE) : int(k * RATE) + len(kick)] += kick[: len(audio) - int(k * RATE)] * level
        snare = bright if i % 2 == 0 else dark
        audio[int(s * RATE) : int(s * RATE) + len(snare)] += snare * 0.7
        hits.append({"time": k, "kind": "kick", "pad": PADS["kick"], "velocity": level})
        hits.append({"time": s, "kind": "snare", "pad": PADS["snare"], "velocity": 0.6})
    grouped, groups = group_hits(audio, RATE, hits)
    snares = [h for h in grouped if h["kind"] == "snare"]
    assert {h["pad"] for h in snares} == {PADS["snare"], FREE_PADS[0]}
    assert [h["pad"] == PADS["snare"] for h in snares] in ([True, False] * 4, [False, True] * 4)
    kicks = [h for h in grouped if h["kind"] == "kick"]
    assert {h["pad"] for h in kicks} == {PADS["kick"]}  # the same kick, louder and softer: one pad
    assert [h["velocity"] for h in kicks[:2]] == [1.0, 0.5]  # accents, the loudest at full
    assert [g["name"] for g in groups] == ["kick", "snare", "snare 2"]
    assert all(g["end"] > g["start"] for g in groups) and groups[0]["count"] == 8


def test_a_hit_without_low_end_is_no_kick() -> None:
    rng = np.random.default_rng(3)
    kick, snare = make_hit("kick", rng), make_hit("snare", rng)
    assert low_share(kick) > 0.3
    assert low_share(snare) < KICK_LOW

    # Kept from before: a "kick" heard on a snare goes, a real kick and the snare stay.
    audio = np.zeros(RATE)
    audio[: len(kick)] += kick
    audio[RATE // 2 : RATE // 2 + len(snare)] += snare
    hits = [
        {"time": 0.0, "kind": "kick", "pad": 60, "velocity": 1.0},
        {"time": 0.5, "kind": "kick", "pad": 60, "velocity": 0.8},
        {"time": 0.5, "kind": "snare", "pad": 61, "velocity": 0.8},
    ]
    assert [(h["time"], h["kind"]) for h in check_kicks(audio, RATE, hits)] == [(0.0, "kick"), (0.5, "snare")]
