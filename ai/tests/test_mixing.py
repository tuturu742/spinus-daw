"""Mix analysis: loudness by ITU-R BS.1770-4, peaks, tone, width and masking."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest

from spinus_ai.mixing import SILENT, analyze_insert, analyze_mix, integrated_loudness, true_peak, width
from spinus_ai.service import RpcError

RATE = 48000


def sine(freq: float, seconds: float = 5.0, level: float = 1.0, rate: int = RATE) -> np.ndarray:
    t = np.arange(int(seconds * rate)) / rate
    result: np.ndarray = level * np.sin(2 * np.pi * freq * t)
    return result


@pytest.mark.parametrize("rate", [44100, 48000])
def test_loudness_meets_the_standards_reference(rate: int) -> None:
    # BS.1770: 0 dBFS of 997 Hz in one channel is -3.01 LUFS; in both, 0.0 LUFS.
    tone = sine(997.0, rate=rate)
    assert integrated_loudness(np.stack([tone, np.zeros_like(tone)]), rate) == pytest.approx(-3.01, abs=0.05)
    assert integrated_loudness(np.stack([tone, tone]), rate) == pytest.approx(0.0, abs=0.05)
    # 20 dB quieter reads 20 LU lower; silence is silence.
    assert integrated_loudness(np.stack([tone, tone]) * 0.1, rate) == pytest.approx(-20.0, abs=0.05)
    assert integrated_loudness(np.zeros((2, rate * 2)), rate) == SILENT


def test_the_gates_leave_out_silence_and_quiet_parts() -> None:
    loud = sine(997.0, 4.0, 0.5)
    song = np.concatenate([loud, np.zeros(RATE * 20), loud])  # mostly silence
    assert integrated_loudness(np.stack([song, song]), RATE) == pytest.approx(
        integrated_loudness(np.stack([loud, loud]), RATE), abs=0.2
    )


def test_peaks_tone_and_width() -> None:
    # A sine whose peaks fall between the samples: its true peak is above the sample peak.
    t = np.arange(RATE) / RATE
    between = 0.5 * np.sin(2 * np.pi * (RATE / 4) * t + np.pi / 4)
    assert true_peak(np.stack([between])) == pytest.approx(20 * np.log10(0.5), abs=0.3)
    assert 20 * np.log10(np.max(np.abs(between))) < -9.0  # the samples alone miss it by 3 dB

    bass = analyze_insert(np.stack([sine(80.0), sine(80.0)]) * 0.3, RATE)
    assert bass["bands"]["bass"] == 0.0 and bass["bands"]["air"] < -40
    assert bass["width"] == 0.0
    assert bass["crest"] == pytest.approx(bass["truePeak"] - bass["loudness"], abs=0.01)

    rng = np.random.default_rng(1)
    wide = rng.normal(size=(2, RATE))  # two unrelated sides
    assert width(wide) == pytest.approx(0.5, abs=0.02)


def test_masking_finds_parts_fighting_in_one_band() -> None:
    rng = np.random.default_rng(2)
    bass = np.stack([sine(80.0)] * 2) * 0.3
    pad = np.stack([sine(90.0) + 0.3 * sine(400.0)] * 2) * 0.3  # sits on the bass
    hats = np.stack([rng.normal(size=RATE * 5) * 0.05] * 2)
    hats = hats - np.convolve(hats[0], np.ones(8) / 8, mode="same")  # mostly high
    result = analyze_mix({"Bass": (bass, RATE), "Pad": (pad, RATE), "Hats": (hats, RATE)})
    assert set(result["inserts"]) == {"Bass", "Pad", "Hats"}
    worst = result["masking"][0]
    assert {worst["a"], worst["b"]} == {"Bass", "Pad"} and worst["band"] == "bass"
    assert not any("Hats" in (m["a"], m["b"]) and m["band"] == "bass" for m in result["masking"])


async def test_the_service_measures_recorded_inserts(tmp_path: Path) -> None:
    from test_drums import write_mono_wav  # 22.05 kHz mono writer
    from test_stems import collect, service_with_fake_engine

    service = service_with_fake_engine(tmp_path, [])
    write_mono_wav(tmp_path / "bass.wav", sine(80.0, 3.0, 0.3, 22050))
    write_mono_wav(tmp_path / "pad.wav", sine(90.0, 3.0, 0.3, 22050))
    params = {"inserts": [{"name": "Bass", "file": str(tmp_path / "bass.wav")},
                          {"name": "Pad", "file": str(tmp_path / "pad.wav")}]}  # fmt: skip
    done, _ = await collect(service, "ai.v1.mix.analyze", params)
    assert done["kind"] == "done", done
    assert done["result"]["inserts"]["Bass"]["loudness"] < 0
    assert done["result"]["masking"][0]["band"] == "bass"
    with pytest.raises(RpcError, match="no recording"):
        await service.handle("ai.v1.mix.analyze", {"inserts": [{"name": "X", "file": "/nowhere.wav"}]}, None)  # type: ignore[arg-type]
    await service.close()
