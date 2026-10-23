#!/usr/bin/env bash
# Does the AI stack (PyTorch for the AMD gfx1151 GPU, Demucs, Basic Pitch) work on your
# machine, and how fast is it on the GPU versus the CPU?
#
# What it does (nothing is installed system-wide; no sudo):
#   1. Creates a scratch folder (default ~/spinus-ai-benchmark) with two Python environments:
#      - "venv":     AMD's TheRock PyTorch build for gfx1151 (bundles its own ROCm), plus Demucs and Basic Pitch
#      - "venv-cpu": the ordinary CPU-only PyTorch from pytorch.org, plus Demucs (CPU comparison)
#   2. Downloads the Demucs models (about 450 MB) from Hugging Face.
#   3. Generates a synthetic 4-minute test song (no copyrighted audio).
#   4. Times stem separation on the GPU (first run ever, then warm) and on the CPU, and times
#      Basic Pitch (audio to MIDI) on the CPU.
#   5. Prints a summary table. Paste the whole output into the issue or pull request that asked for it.
#
# Needs: uv (`sudo pacman -S uv` if missing), about 9 GB of free disk, internet.
# Takes about 20-30 minutes the first time (mostly downloads), about 10 minutes after that.
# Close big programs first (games, local AI chat servers, builds): they make the CPU numbers worse.
#
# Usage:
#   tools/owner-checks/ai-gpu-benchmark.sh                 # uses ~/spinus-ai-benchmark
#   tools/owner-checks/ai-gpu-benchmark.sh /some/folder    # uses another folder
# To clean up afterwards, delete the folder.

set -u

# ---- Pinned versions ----
THEROCK_INDEX=https://stable.repo.amd.com/rocm/whl-next/
TORCH_ROCM="torch[device-gfx1151]==2.14.0+rocm10.1.0"
TORCHAUDIO_ROCM="torchaudio==2.11.0.3+rocm10.1.0"
CPU_INDEX=https://download.pytorch.org/whl/cpu
TORCH_CPU="torch==2.14.0+cpu"
DEMUCS="demucs==4.1.0"
BASIC_PITCH="basic-pitch==0.4.0"
ONNXRUNTIME="onnxruntime==1.31.0"

DIR=${1:-$HOME/spinus-ai-benchmark}
mkdir -p "$DIR/work" "$DIR/cache" "$DIR/out" || exit 1
DIR=$(cd "$DIR" && pwd)
RESULTS=$DIR/results.jsonl
LOG=$DIR/benchmark.log
: > "$RESULTS"
: > "$LOG"

# Keep every download and cache inside the scratch folder.
export HF_HOME=$DIR/cache/hf
export TORCH_HOME=$DIR/cache/torch
export MIOPEN_USER_DB_PATH=$DIR/cache/miopen
export MIOPEN_CUSTOM_CACHE_DIR=$DIR/cache/miopen
export AMD_COMGR_CACHE_DIR=$DIR/cache/comgr   # otherwise about 550 MB of compiled kernels land in ~/.cache/comgr
export UV_CACHE_DIR=$DIR/cache/uv
# Never set this on gfx1151. HSA_OVERRIDE_GFX_VERSION is not needed with TheRock.
unset PYTORCH_HIP_ALLOC_CONF HSA_OVERRIDE_GFX_VERSION

if ! command -v uv >/dev/null; then
    echo "uv is missing. Install it with: sudo pacman -S uv"
    exit 1
fi

step() { echo; echo "== $*"; }
fail() { echo "FAILED: $*  (details in $LOG)"; exit 1; }

# ---- 1. Python environments ---------------------------------------------------------------------------
step "Installing Python environments into $DIR (first time: several GB of downloads)"
if [ ! -x "$DIR/venv/bin/python" ]; then
    uv venv -q --python 3.12 "$DIR/venv" >>"$LOG" 2>&1 || fail "creating venv"
fi
uv pip install -q --python "$DIR/venv/bin/python" --index-url "$THEROCK_INDEX" \
    "$TORCH_ROCM" "$TORCHAUDIO_ROCM" >>"$LOG" 2>&1 || fail "installing TheRock PyTorch"
# Demucs from PyPI. Basic Pitch 0.4.0 demands TensorFlow < 2.15.1, which has no Python 3.12 build,
# so install it without dependencies and add the ones it really needs plus ONNX Runtime.
# setuptools < 82 keeps pkg_resources, which resampy (a Basic Pitch dependency) still imports.
uv pip install -q --python "$DIR/venv/bin/python" "$DEMUCS" soundfile scipy "$ONNXRUNTIME" \
    "librosa>=0.8.0" "mir-eval>=0.6" "pretty-midi>=0.2.9" "resampy>=0.2.2,<0.4.3" scikit-learn \
    typing-extensions "setuptools<82" >>"$LOG" 2>&1 || fail "installing Demucs and Basic Pitch dependencies"
uv pip install -q --python "$DIR/venv/bin/python" --no-deps "$BASIC_PITCH" >>"$LOG" 2>&1 \
    || fail "installing Basic Pitch"

if [ ! -x "$DIR/venv-cpu/bin/python" ]; then
    uv venv -q --python 3.12 "$DIR/venv-cpu" >>"$LOG" 2>&1 || fail "creating venv-cpu"
fi
uv pip install -q --python "$DIR/venv-cpu/bin/python" --index-url "$CPU_INDEX" "$TORCH_CPU" >>"$LOG" 2>&1 \
    || fail "installing CPU PyTorch"
uv pip install -q --python "$DIR/venv-cpu/bin/python" "$DEMUCS" soundfile scipy >>"$LOG" 2>&1 \
    || fail "installing Demucs (CPU venv)"

# ---- 2. Helper scripts ---------------------------------------------------------------------------------
cat > "$DIR/work/info.py" <<'EOF'
import platform, subprocess, sys, time
import torch
print("python", platform.python_version())
print("torch", torch.__version__, "| HIP", torch.version.hip, "| git", torch.version.git_version[:12])
try:
    print("rocm-sdk", subprocess.run([sys.executable.replace("python", "rocm-sdk"), "version"],
                                     capture_output=True, text=True).stdout.strip())
except OSError:
    pass
print("cpu capability", torch.backends.cpu.get_cpu_capability(), "| MKL", torch.backends.mkl.is_available(),
      "| threads", torch.get_num_threads())
ok = torch.cuda.is_available()
print("gpu available", ok)
if ok:
    p = torch.cuda.get_device_properties(0)
    print("gpu", torch.cuda.get_device_name(0), "|", p.gcnArchName, "|", round(p.total_memory / 2**30, 1), "GiB visible")
    a = torch.randn(1024, 1024, device="cuda")
    err = ((a @ a).cpu() - a.cpu() @ a.cpu()).abs().max().item()
    print("gpu self-test (1024x1024 matmul vs CPU) max error", round(err, 5), "PASS" if err < 0.05 else "FAIL")
    x = torch.randn(4096, 4096, device="cuda", dtype=torch.float16); x @ x; torch.cuda.synchronize()
    t = time.perf_counter()
    for _ in range(20):
        x @ x
    torch.cuda.synchronize()
    print("gpu fp16 matmul TFLOPS", round(20 * 2 * 4096**3 / (time.perf_counter() - t) / 1e12, 1))
EOF

cat > "$DIR/work/gen_song.py" <<'EOF'
"""Synthetic test song (no copyrighted audio): drums, bass, chord pad, vocal-like formant line."""
import sys
import numpy as np
import soundfile as sf
from scipy.signal import lfilter

def make(seconds, path, sr=44100):
    rng = np.random.default_rng(1234)
    n = int(seconds * sr); t = np.arange(n) / sr
    beat = 0.5  # 120 BPM
    out = np.zeros((n, 2))
    def hit(start, sig, pan):
        i = int(start * sr); j = min(n, i + len(sig))
        if i < n:
            out[i:j, 0] += sig[: j - i] * (1 - pan); out[i:j, 1] += sig[: j - i] * pan
    kt = np.arange(int(0.25 * sr)) / sr
    kick = np.sin(2 * np.pi * (50 + 100 * np.exp(-kt * 30)) * kt) * np.exp(-kt * 12)
    st = np.arange(int(0.2 * sr)) / sr
    snare = (rng.standard_normal(len(st)) * 0.6 + np.sin(2 * np.pi * 190 * st)) * np.exp(-st * 18)
    ht = np.arange(int(0.05 * sr)) / sr
    hat = np.diff(rng.standard_normal(len(ht) + 1)) * np.exp(-ht * 80) * 0.3
    for b in range(int(seconds / beat)):
        hit(b * beat, kick * 0.9, 0.5)
        if b % 4 in (1, 3):
            hit(b * beat, snare * 0.5, 0.45)
        hit(b * beat, hat, 0.65); hit(b * beat + beat / 2, hat * 0.7, 0.65)
    hz = lambda m: 440.0 * 2 ** ((m - 69) / 12)
    roots = [57, 53, 48, 55]  # Am F C G
    bar = 4 * beat
    bass = np.zeros(n); pad = np.zeros(n); voc = np.zeros(n)
    for k in range(int(seconds / bar) + 1):
        i0 = int(k * bar * sr); i1 = min(n, int((k + 1) * bar * sr))
        if i0 >= n:
            break
        tt = t[i0:i1] - k * bar; r = roots[k % 4]; third = 3 if k % 4 == 0 else 4
        bass[i0:i1] = np.tanh(2 * np.sin(2 * np.pi * hz(r - 24) * tt)) * 0.35 * (0.6 + 0.4 * np.exp(-(tt % beat) * 6))
        for m in (r, r + third, r + 7):
            pad[i0:i1] += sum(np.sin(2 * np.pi * hz(m) * d * tt) for d in (0.997, 1.0, 1.003)) * 0.03
        mel = [r + 12, r + 12 + third, r + 19, r + 12 + third]
        for q in range(4):
            a0 = i0 + int(q * beat * sr); a1 = min(i1, a0 + int(beat * 0.9 * sr))
            if a0 >= n:
                break
            u = t[a0:a1] - t[a0]
            ph = np.cumsum(hz(mel[q]) * (1 + 0.006 * np.sin(2 * np.pi * 5.5 * u))) / sr
            env = np.minimum(1, u * 20) * np.minimum(1, (u[-1] - u) * 20 + 1e-3)
            voc[a0:a1] += (2 * (ph % 1.0) - 1) * env * 0.12
    def bp(x, fc, q):
        w = 2 * np.pi * fc / sr; al = np.sin(w) / (2 * q)
        return lfilter([al, 0, -al], [1 + al, -2 * np.cos(w), 1 - al], x)
    voc = bp(voc, 700, 4) + bp(voc, 1200, 5) * 0.7
    out[:, 0] += bass + pad * 0.9 + voc * 0.9
    out[:, 1] += bass + pad * 1.1 + voc * 1.1
    out /= np.max(np.abs(out)) * 1.12
    sf.write(path, out.astype(np.float32), sr, subtype="PCM_16")

make(float(sys.argv[1]), sys.argv[2])
EOF

cat > "$DIR/work/bench_demucs.py" <<'EOF'
"""Time Demucs separation in one process. Args: label model device wav outdir. Appends JSON to results."""
import json, os, sys, time
t0 = time.perf_counter()
import torch
from pathlib import Path
from demucs.api import Separator, save_audio
label, model, device, wav, out = sys.argv[1], sys.argv[2], sys.argv[3], Path(sys.argv[4]), Path(sys.argv[5])
sep = Separator(model=model, device=device, progress=False)
if device == "cuda":
    torch.cuda.synchronize(); torch.cuda.reset_peak_memory_stats()
runs = []
for _ in range(int(os.environ.get("BENCH_REPEAT", "1"))):
    t = time.perf_counter()
    _, stems = sep.separate_audio_file(wav)
    if device == "cuda":
        torch.cuda.synchronize()
    runs.append(round(time.perf_counter() - t, 1))
out.mkdir(parents=True, exist_ok=True)
for name, src in stems.items():
    save_audio(src, str(out / f"{name}.wav"), samplerate=sep.samplerate)
r = {"label": label, "task": f"Demucs {model}", "device": device, "seconds": runs,
     "process_total": round(time.perf_counter() - t0, 1), "threads": torch.get_num_threads(),
     "peak_gpu_gib": round(torch.cuda.max_memory_allocated() / 2**30, 2) if device == "cuda" else None,
     "load": os.getloadavg()[0]}
print(json.dumps(r))
with open(os.environ["BENCH_RESULTS"], "a") as f:
    f.write(json.dumps(r) + "\n")
EOF

cat > "$DIR/work/bench_basic_pitch.py" <<'EOF'
"""Time Basic Pitch (ONNX Runtime on CPU). Args: label wav repeat. Appends JSON to results."""
import json, logging, os, sys, time, warnings
logging.disable(logging.WARNING); warnings.filterwarnings("ignore")
import onnxruntime as ort
from basic_pitch import ICASSP_2022_MODEL_PATH
from basic_pitch.inference import predict, Model
label, wav, repeat = sys.argv[1], sys.argv[2], int(sys.argv[3])
model = Model(ICASSP_2022_MODEL_PATH)
runs = []
for _ in range(repeat):
    t = time.perf_counter()
    _, midi, notes = predict(wav, model)
    runs.append(round(time.perf_counter() - t, 1))
r = {"label": label, "task": "Basic Pitch (" + ICASSP_2022_MODEL_PATH.name + ")", "device": "cpu",
     "seconds": runs, "notes": len(notes), "onnxruntime": ort.__version__,
     "providers": ort.get_available_providers(), "load": os.getloadavg()[0]}
print(json.dumps(r))
with open(os.environ["BENCH_RESULTS"], "a") as f:
    f.write(json.dumps(r) + "\n")
EOF

cat > "$DIR/work/summary.py" <<'EOF'
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
print(f"{'Run':46} {'Device':6} {'Times (s)':22} {'Best':>6}  {'Load':>5}  Target")
print("-" * 120)
for r in rows:
    best = min(r["seconds"])
    target = ""
    if r["task"] == "Demucs htdemucs":
        limit = 60 if r["device"] == "cuda" else 240
        target = f"<= {limit} s: " + ("PASS" if best <= limit else "FAIL")
    extra = f" peak {r['peak_gpu_gib']} GiB" if r.get("peak_gpu_gib") else ""
    times = ", ".join(str(s) for s in r["seconds"])
    print(f"{r['label']:46} {r['device']:6} {times:22} {best:6.1f}  {r['load']:5.1f}  {target}{extra}")
EOF

PY=$DIR/venv/bin/python
PYCPU=$DIR/venv-cpu/bin/python
export BENCH_RESULTS=$RESULTS
W=$DIR/work

# ---- 3. System and GPU check ----------------------------------------------------------------------------
step "System"
uname -r
grep -m1 "model name" /proc/cpuinfo
echo "load average now: $(cut -d' ' -f1-3 /proc/loadavg)  (above ~4 means other programs are busy)"
for f in /sys/class/drm/card*/device/gpu_busy_percent; do
    [ -r "$f" ] && echo "GPU busy now: $(cat "$f") %  (above 0 means another program, e.g. a local AI chat server, uses the GPU)"
done
step "TheRock PyTorch (GPU venv)"
"$PY" "$W/info.py" 2>>"$LOG" || fail "TheRock PyTorch does not start"
step "Plain CPU PyTorch (CPU venv)"
"$PYCPU" "$W/info.py" 2>>"$LOG" || fail "CPU PyTorch does not start"

# ---- 4. Test audio and models ---------------------------------------------------------------------------
step "Generating synthetic test songs and downloading Demucs models"
[ -f "$W/song240.wav" ] || "$PY" "$W/gen_song.py" 240 "$W/song240.wav" || fail "generating song"
[ -f "$W/song30.wav" ] || "$PY" "$W/gen_song.py" 30 "$W/song30.wav" || fail "generating song"
"$PY" -c "from demucs.pretrained import get_model; [get_model(n) for n in ('htdemucs', 'htdemucs_ft')]" \
    >>"$LOG" 2>&1 || fail "downloading Demucs models"
echo "ok"

bench() { "$@" 2>>"$LOG" | grep '^{' >/dev/null || echo "  run failed: $* (see $LOG)"; }

# ---- 5. Demucs on the GPU ------------------------------------------------------------------------------
step "Demucs on the GPU (each line takes up to 2 minutes)"
rm -rf "$DIR/cache/miopen" "$DIR/cache/comgr"   # forget compiled GPU kernels so the first run is a true first run
echo "  first run ever (GPU kernels are compiled and cached)"
bench "$PY" "$W/bench_demucs.py" "htdemucs 4 min, GPU, first run" htdemucs cuda "$W/song240.wav" "$DIR/out/gpu"
echo "  new process, three runs in a row (what a running Spinus AI service would see)"
BENCH_REPEAT=3 bench "$PY" "$W/bench_demucs.py" "htdemucs 4 min, GPU, warm" htdemucs cuda "$W/song240.wav" "$DIR/out/gpu"
echo "  fine-tuned model (4 models in one, slower, better quality)"
BENCH_REPEAT=2 bench "$PY" "$W/bench_demucs.py" "htdemucs_ft 4 min, GPU, warm" htdemucs_ft cuda "$W/song240.wav" "$DIR/out/gpu_ft"

# ---- 6. Demucs on the CPU ------------------------------------------------------------------------------
step "Demucs on the CPU (each line takes 1-5 minutes)"
BENCH_REPEAT=2 bench "$PYCPU" "$W/bench_demucs.py" "htdemucs 4 min, CPU, pytorch.org CPU build" htdemucs cpu "$W/song240.wav" "$DIR/out/cpu"
OMP_NUM_THREADS=16 BENCH_REPEAT=2 bench "$PY" "$W/bench_demucs.py" "htdemucs 4 min, CPU, TheRock build, 16 thr" htdemucs cpu "$W/song240.wav" "$DIR/out/cpu"
bench "$PY" "$W/bench_demucs.py" "htdemucs 4 min, CPU, TheRock build, default" htdemucs cpu "$W/song240.wav" "$DIR/out/cpu"
bench "$PYCPU" "$W/bench_demucs.py" "htdemucs_ft 4 min, CPU, pytorch.org CPU build" htdemucs_ft cpu "$W/song240.wav" "$DIR/out/cpu_ft"

# ---- 7. Basic Pitch on the CPU -------------------------------------------------------------------------
step "Basic Pitch (audio to MIDI) on the CPU"
bench "$PY" "$W/bench_basic_pitch.py" "Basic Pitch 30 s" "$W/song30.wav" 2
bench "$PY" "$W/bench_basic_pitch.py" "Basic Pitch 4 min" "$W/song240.wav" 2

# ---- 8. Summary ----------------------------------------------------------------------------------------
step "Summary (times are seconds of processing, not counting Python start-up and model loading)"
"$PY" "$W/summary.py" "$RESULTS"
echo
echo "Load = system load average during the run. Disk used by $DIR: $(du -sh "$DIR" | cut -f1)"
echo "Full log: $LOG"
