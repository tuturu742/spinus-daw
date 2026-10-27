"""Makes the Spinus Starter Pack (samples/Spinus Starter Pack) from the original Signature Sounds packs.

    python3 tools/starter-pack/make_pack.py --source "<folder with the Signature Sounds packs>"

manifest.json (beside this script) lists every file: the pack it comes from, its original name, where it goes
and how it is cut. The script reads the originals (never writes there) with ffmpeg and, for each file:

- one-shots: starts at the sound (the silence before it is cut), ends where it has faded 60 dB under its
  peak (at most `maxSeconds`, 4 s unless the manifest says, with a fade-out when it is cut short), or
  before a second, separate sound that follows it in some originals (`keepSecondSound` keeps it);
- loops: kept whole from the start, or exactly `bars` bars of 4/4 at `bpm`, so they repeat in time;
- textures: `maxSeconds` from `start`, with the manifest's fades;
- mono when its two channels are the same (within 40 dB), sample rates over 48 kHz brought down to 48 or
  44.1 kHz, peak at -1 dBFS, FLAC at the original's 16 bits (dithered) or 24 bits.

Then it writes kits.json (the Drum Machine's factory kits, src/dsp/drums/StarterPack.h), README.md (where each
file comes from and its licence), LICENSE.txt (CC0 1.0) and checks the result: peaks, silence, lengths, kits,
size. --check only checks an existing pack.
"""

from __future__ import annotations

import argparse
import array
import json
import math
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

here = Path(__file__).resolve().parent
repo = here.parents[1]
PACK = "Spinus Starter Pack"
PEAK_DB = -1.0
SIZE_GOAL, SIZE_CAP = 30_000_000, 60_000_000


def probe(path: Path) -> dict:
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
         "stream=sample_rate,channels,bits_per_raw_sample,bits_per_sample,sample_fmt", "-of", "json", str(path)],
        check=True, capture_output=True, text=True,
    ).stdout  # fmt: skip
    s = json.loads(out)["streams"][0]
    bits = int(s.get("bits_per_raw_sample") or 0) or int(s.get("bits_per_sample") or 0)
    if s.get("sample_fmt", "").startswith(("flt", "dbl")):
        bits = 32
    return {"rate": int(s["sample_rate"]), "channels": int(s["channels"]), "bits": bits}


def decode(path: Path, channels: int) -> array.array:
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", str(path), "-f", "f32le", "-acodec", "pcm_f32le", "-ac", str(channels), "-"],
        check=True, capture_output=True,
    ).stdout  # fmt: skip
    samples = array.array("f")
    samples.frombytes(raw)
    return samples


def db(x: float) -> float:
    return 20 * math.log10(x) if x > 0 else -200.0


def frames_peak(samples: array.array, channels: int) -> list[float]:
    """The loudest channel of each frame."""
    if channels == 1:
        return [abs(v) for v in samples]
    return [max(abs(samples[i]), abs(samples[i + 1])) for i in range(0, len(samples) - 1, channels)]


def is_mono(samples: array.array, channels: int) -> bool:
    if channels == 1:
        return True
    side = mid = 0.0
    for i in range(0, len(samples) - 1, channels):
        left, right = samples[i], samples[i + 1]
        side += (left - right) ** 2
        mid += (left + right) ** 2
    return mid > 0 and 10 * math.log10(max(side, 1e-12) / mid) < -40


def second_sound(level: list[float], rate: int) -> int | None:
    """Where a one-shot's sound ends when a second, separate sound follows it (a second hit or shake at the
    end of some originals): the quietest point before a rise of 12 dB or more that comes 120 ms or more after
    the peak, from 24 dB or more under the peak to within 24 dB of it. A ringing note's beats and a clap's
    bursts stay well above that dip. None when there is no such sound."""
    step = max(1, rate // 100)  # 10 ms
    frames = [db(math.sqrt(sum(v * v for v in level[i : i + step]) / len(level[i : i + step])))
              for i in range(0, len(level), step)]  # fmt: skip
    if not frames:
        return None
    # The loudest of three frames, so a one-frame dip inside a sound (two partials cancelling) is no gap.
    frames = [max(frames[max(0, i - 1) : i + 2]) for i in range(len(frames))]
    top = max(range(len(frames)), key=lambda i: frames[i])
    low = top
    for i in range(top + 1, len(frames)):
        if frames[i] < frames[low]:
            low = i
        if (i - top >= 12 and frames[i] - frames[low] >= 12 and frames[low] <= frames[top] - 24
                and frames[i] >= frames[top] - 24):  # fmt: skip
            return low * step
    return None


def plan(entry: dict, info: dict, samples: array.array) -> dict:
    """Where to cut, the fades, mono or not, for one file (in the source's samples)."""
    rate, channels = info["rate"], info["channels"]
    level = frames_peak(samples, channels)
    count = len(level)
    peak = max(level, default=0.0)
    if peak <= 10 ** (-80 / 20):
        raise SystemExit(f"{entry['pack']}/{entry['file']}: silent")
    kind = entry["type"]
    fade_in = float(entry.get("fadeIn", 0.0))
    fade_out = float(entry.get("fadeOut", 0.0))
    if kind == "one-shot":
        loud = peak * 10 ** (-50 / 20)
        first = next(i for i, v in enumerate(level) if v > loud)
        start = max(0, first - int(0.0005 * rate))
        quiet = peak * 10 ** (-60 / 20)
        last = max(i for i, v in enumerate(level) if v > quiet)
        end = min(count, last + int(0.01 * rate))
        second = second_sound(level[start:end], rate) if not entry.get("keepSecondSound") else None
        if second is not None:
            end = start + second
            fade_out = fade_out or 0.02
        cap = start + int(float(entry.get("maxSeconds", 4.0)) * rate)
        if end > cap:
            end = cap
            fade_out = fade_out or 0.3
        fade_out = fade_out or 0.003
    elif kind == "loop":
        start = 0
        end = count
        if "bpm" in entry:
            end = min(count, round(entry["bars"] * 4 * 60 / entry["bpm"] * rate))
    else:
        start = int(float(entry.get("start", 0.0)) * rate)
        end = min(count, start + int(float(entry.get("maxSeconds", 600.0)) * rate))
    mono = is_mono(samples[start * channels : end * channels], channels)
    out_rate = rate if rate <= 48000 else (48000 if rate % 48000 == 0 else 44100)
    return {"start": start, "end": end, "mono": mono, "rate": out_rate, "fadeIn": fade_in, "fadeOut": fade_out,
            "bits": 16 if info["bits"] == 16 else 24}  # fmt: skip


def convert(source: Path, target: Path, info: dict, p: dict, work: Path) -> None:
    rate = info["rate"]
    length = (p["end"] - p["start"]) / rate
    filters = [f"atrim=start_sample={p['start']}:end_sample={p['end']}", "asetpts=PTS-STARTPTS"]
    if p["mono"] and info["channels"] > 1:
        filters.append("pan=mono|c0=0.5*c0+0.5*c1")
    elif info["channels"] > 2:
        filters.append("pan=stereo|c0=c0|c1=c1")
    if p["rate"] != rate:
        filters.append(f"aresample={p['rate']}:filter_size=64:cutoff=0.97")
    if p["fadeIn"] > 0:
        filters.append(f"afade=t=in:d={p['fadeIn']}")
    if p["fadeOut"] > 0:
        filters.append(f"afade=t=out:st={max(0.0, length - p['fadeOut'])}:d={p['fadeOut']}")
    middle = work / "middle.wav"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(source), "-af", ",".join(filters), "-c:a", "pcm_f32le",
                    str(middle)], check=True)  # fmt: skip
    channels = 1 if p["mono"] else min(2, info["channels"])
    peak = max(frames_peak(decode(middle, channels), channels))
    gain = PEAK_DB - db(peak)
    target.parent.mkdir(parents=True, exist_ok=True)
    fmt = ["-af", f"volume={gain:.4f}dB,aresample=osf=s16:dither_method=triangular", "-sample_fmt", "s16"] \
        if p["bits"] == 16 else ["-af", f"volume={gain:.4f}dB", "-sample_fmt", "s32", "-bits_per_raw_sample", "24"]
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(middle), *fmt, "-c:a", "flac", "-compression_level", "12",
                    "-map_metadata", "-1", str(target)], check=True)  # fmt: skip


def write_credits(manifest: dict, out: Path) -> None:
    lines = [
        f"# {PACK}",
        "",
        "Free samples that come with Spinus DAW: drums, loops, a few instruments and textures. The Drum",
        "Machine's factory kits (kits.json) are made of the one-shots in Drums/.",
        "",
        "## Where the sounds come from",
        "",
        "**Signature Sounds** (https://signaturesounds.org, formerly signaturesamples.co.uk). Free for any use;",
        "CC0 1.0 where marked (LICENSE.txt). Included with the authors' permission.",
        "",
        "Use them in anything, commercial or not; no credit is required (thanks to Signature Sounds all the",
        "same). The table says which Signature Sounds pack each file comes from. The files were cut,",
        "peak-normalised and converted to FLAC by Spinus's tools/starter-pack (its manifest.json lists the same).",
        "",
        "| File | Signature Sounds pack | Original file | Licence |",
        "| --- | --- | --- | --- |",
    ]
    for entry in manifest["files"]:
        source = manifest["sources"][entry["pack"]]
        licence = source["licence"] + (f" ({source['licenceFile']})" if source.get("licenceFile") else "")
        site = "" if source["site"] == "signaturesounds.org" else f", {source['site']}"
        lines.append(f"| {entry['out']} | {entry['pack']}{site} | {entry['file']} | {licence} |")
    lines += ["", "## Factory drum kits", ""]
    for kit in manifest["kits"]:
        lines.append(f"- **{kit['name']}**: {kit['description']}.")
    (out / "README.md").write_text("\n".join(lines) + "\n")
    shutil.copyfile(here / "CC0-1.0.txt", out / "LICENSE.txt")
    kits = {"version": 1, "kits": manifest["kits"]}
    (out / "kits.json").write_text(json.dumps(kits, indent=1, ensure_ascii=False) + "\n")


def check(manifest: dict, out: Path) -> bool:
    ok = True

    def fail(text: str) -> None:
        nonlocal ok
        ok = False
        print("FAILED:", text)

    wanted = {e["out"]: e for e in manifest["files"]}
    found = {str(p.relative_to(out)): p for p in out.rglob("*.flac")}
    for name in sorted(set(wanted) - set(found)):
        fail(f"{name} is missing")
    for name in sorted(set(found) - set(wanted)):
        fail(f"{name} is not in the manifest")
    total = 0
    for name, path in sorted(found.items()):
        if name not in wanted:
            continue
        entry = wanted[name]
        info = probe(path)
        level = frames_peak(decode(path, info["channels"]), info["channels"])
        seconds = len(level) / info["rate"]
        peak = db(max(level, default=0.0))
        lead = next((i for i, v in enumerate(level) if v > 10 ** ((peak - 50) / 20)), len(level)) / info["rate"]
        total += path.stat().st_size
        if not -1.2 <= peak <= -0.8:
            fail(f"{name}: its peak is {peak:.2f} dBFS, not {PEAK_DB}")
        if entry["type"] == "one-shot" and lead > 0.002:
            fail(f"{name}: {lead * 1000:.0f} ms of silence before the sound")
        if entry["type"] == "one-shot" and not 0.02 <= seconds <= float(entry.get("maxSeconds", 4.0)) + 0.01:
            fail(f"{name}: {seconds:.2f} s long")
        if entry["type"] == "loop" and "bpm" in entry:
            bars = seconds * entry["bpm"] / 240
            if abs(bars - entry["bars"]) > 0.01:
                fail(f"{name}: {bars:.3f} bars, not {entry['bars']}")
        print(f"{name}: {seconds:.2f} s, {info['rate']} Hz, {info['channels']} ch, {info['bits']} bit, "
              f"{path.stat().st_size // 1024} KB")  # fmt: skip
    kits = json.loads((out / "kits.json").read_text())["kits"]
    for kit in kits:
        if len(kit["pads"]) != 16:
            fail(f"kit {kit['name']}: {len(kit['pads'])} pads, not 16")
        for pad in kit["pads"]:
            if pad["file"] not in found:
                fail(f"kit {kit['name']}: {pad['file']} is not in the pack")
    for extra in ("README.md", "LICENSE.txt", "kits.json"):
        if not (out / extra).is_file():
            fail(f"{extra} is missing")
    size = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    print(f"{len(found)} sounds, {len(kits)} kits, {size / 1e6:.1f} MB")
    if size > SIZE_CAP:
        fail(f"the pack is {size / 1e6:.1f} MB, over {SIZE_CAP / 1e6:.0f} MB")
    elif size > SIZE_GOAL:
        print(f"note: over the {SIZE_GOAL / 1e6:.0f} MB goal")
    return ok


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, help="the folder with the Signature Sounds packs (read only)")
    parser.add_argument("--out", type=Path, default=repo / "samples" / PACK)
    parser.add_argument("--check", action="store_true", help="only check the pack that is there")
    args = parser.parse_args()
    manifest = json.loads((here / "manifest.json").read_text())
    out: Path = args.out.resolve()

    if not args.check:
        if args.source is None:
            parser.error("--source is needed to make the pack")
        source: Path = args.source.resolve()
        if out == source or source in out.parents:
            raise SystemExit("The pack must not be made inside the source folder")
        if out.name != PACK:
            raise SystemExit(f"--out must be a folder named {PACK!r}")
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)
        with tempfile.TemporaryDirectory() as tmp:
            for entry in manifest["files"]:
                original = source / entry["pack"] / entry["file"]
                info = probe(original)
                p = plan(entry, info, decode(original, info["channels"]))
                convert(original, out / entry["out"], info, p, Path(tmp))
                print(f"{entry['out']}  <-  {entry['pack']}/{entry['file']}{'  (mono)' if p['mono'] else ''}")
        write_credits(manifest, out)

    sys.exit(0 if check(manifest, out) else 1)


if __name__ == "__main__":
    main()
