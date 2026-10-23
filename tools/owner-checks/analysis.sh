#!/usr/bin/env bash
# The Analyzer's and Automixer's AI on your graphics card, with the real models.
#
# What it does, on a song you choose (WAV, or anything ffmpeg can read):
#   1. Checks that the analysis engine is installed (if not: Options > AI > Analysis engine, or
#      tools/owner-checks/gpu.sh) and which model files are downloaded; asks before downloading any
#      (Demucs 84 MB, Beat This! 81 MB, Basic Pitch 230 KB, CLAP 776 MB), into the same folder Spinus uses.
#   2. Finds the tempo, beats and key; splits the song into 4 stems; turns the bass into notes and the
#      drums into steps; hears the stems with CLAP; measures them as the Automixer does.
#   3. Prints what it found and how long each step took. Paste the whole output into the issue.
#
# Run it from the spinus-daw folder:
#   tools/owner-checks/analysis.sh ~/Music/some-song.wav
set -u
cd "$(dirname "$0")/../.." || exit 1
if [ $# -lt 1 ] || [ ! -f "$1" ]; then
    echo "Give a song file: tools/owner-checks/analysis.sh ~/Music/some-song.wav"
    exit 1
fi
song="$1"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
case "$song" in
    *.wav|*.WAV) ;;
    *)
        if ! command -v ffmpeg >/dev/null; then
            echo "This is not a WAV file and ffmpeg is missing: sudo pacman -S ffmpeg, or give a WAV file."
            exit 1
        fi
        ffmpeg -loglevel error -i "$song" -ar 44100 -ac 2 "$work/song.wav" || exit 1
        song="$work/song.wav"
        ;;
esac
PYTHONPATH="$PWD/ai" exec "${SPINUS_CHECK_PYTHON:-python3}" - "$song" "$work" <<'PY'
import asyncio, sys, time
from pathlib import Path
from typing import Any

from spinus_ai.compute.weights import find_model
from spinus_ai.connections import MemoryKeyStore
from spinus_ai.embeddings import default_models_folder
from spinus_ai.service import Service

song, work = Path(sys.argv[1]), Path(sys.argv[2])
config = work / "config"


async def run(service: Service, method: str, params: dict[str, Any]) -> Any:
    finished: asyncio.Future[dict[str, Any]] = asyncio.get_running_loop().create_future()

    async def notify(name: str, payload: dict[str, Any]) -> None:
        if payload.get("kind") in ("done", "error") and not finished.done():
            finished.set_result(payload)

    started = time.perf_counter()
    await service.handle(method, params, notify)
    done = await finished
    if done["kind"] == "error":
        raise RuntimeError(f"{method}: {done.get('message')}")
    print(f"   ({time.perf_counter() - started:.1f} s)")
    return done["result"]


async def main() -> int:
    service = Service(config, MemoryKeyStore(), None, None, models=default_models_folder())
    plan = service.engine.plan()
    status = await service.handle("ai.v1.compute.status", {}, None)  # type: ignore[arg-type]
    print("== Analysis engine")
    if not status.get("uses"):
        print("Not installed. Install it in Spinus (Options > AI > Analysis engine) or run tools/owner-checks/gpu.sh.")
        return 1
    print("uses:", status["uses"])
    gpu = plan is not None and plan.device != "cpu"
    wanted = ["htdemucs", "beat_this_final0" if gpu else "beat_this_small0", "basic_pitch", "clap_music"]
    missing = [m for m in wanted if not service.weights.is_ready(find_model(m))]  # type: ignore[arg-type]
    if missing:
        size = sum(find_model(m).size for m in missing) / 1e6  # type: ignore[union-attr]
        answer = input(f"Download {', '.join(missing)} ({size:.0f} MB) into {default_models_folder()}? [y/N] ")
        if answer.strip().lower() != "y":
            return 1
        for m in missing:
            print("downloading", m)
            await run(service, "ai.v1.stems.download", {"model": m})

    print("== Tempo and key")
    rhythm = await run(service, "ai.v1.analyze.tempo", {"file": str(song), "cacheFolder": str(work)})
    print(f"tempo {rhythm['tempo']} BPM, {rhythm['beatsPerBar']} beats per bar, first downbeat {rhythm['firstDownbeat']} s")
    key = await run(service, "ai.v1.analyze.key", {"file": str(song)})
    print("key", key.get("name"))

    print("== Stems (Demucs, 4)")
    stems = (await run(service, "ai.v1.stems.separate",
                       {"file": str(song), "model": "htdemucs", "cacheFolder": str(work)}))["stems"]
    print(", ".join(stems))

    print("== Notes from the bass, steps from the drums")
    beats = rhythm.get("beats", [])
    notes = await run(service, "ai.v1.transcribe.notes",
                      {"file": stems["bass"], "beats": beats, "firstDownbeat": rhythm["firstDownbeat"], "cacheFolder": str(work)})
    print(len(notes["notes"]), "bass notes; first pitches", [n["pitch"] for n in notes["notes"][:8]])
    drums = await run(service, "ai.v1.transcribe.drums",
                      {"file": stems["drums"], "beats": beats, "firstDownbeat": rhythm["firstDownbeat"], "cacheFolder": str(work)})
    kinds: dict[str, int] = {}
    for h in drums["hits"]:
        kinds[h["kind"]] = kinds.get(h["kind"], 0) + 1
    print(len(drums["hits"]), "drum hits:", kinds, "| groups:", [g["name"] for g in drums["groups"]])

    print("== CLAP (closest preset and drum samples)")
    started = time.perf_counter()
    clips = [{"file": stems[name], "start": 30.0, "end": 31.0} for name in stems]
    found = await service.embed(clips, lambda _: asyncio.sleep(0), "CLAP")  # type: ignore[arg-type]
    print(f"{len(found)} vectors of {len(found[0])} in {time.perf_counter() - started:.1f} s")

    print("== Mix measurements (as the Automixer)")
    mix = await run(service, "ai.v1.mix.analyze", {"inserts": [{"name": n, "file": f} for n, f in stems.items()]})
    for name, m in mix["inserts"].items():
        print(f"{name}: {m['loudness']} LUFS, true peak {m['truePeak']} dBTP, width {m['width']}")
    print("masking:", mix["masking"][:4])
    print("== Done")
    await service.close()
    return 0


sys.exit(asyncio.run(main()))
PY
