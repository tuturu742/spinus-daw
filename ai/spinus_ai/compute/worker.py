"""The analysis engine's worker: runs inside a PyTorch environment.

    <environment>/bin/python -m spinus_ai.compute.worker [--device auto|cpu] [--threads N]

The AI service starts it and talks to it with one JSON object per line: requests
{"id", "method", "params"} on stdin; {"id", "progress"} while working, then {"id", "result"} or
{"id", "error"} on stdout. One request at a time; stopping a job means stopping the worker. Anything
PyTorch or its libraries print goes to stderr, which the service keeps in a log.

Only the standard library and the packages of ai/compute/requirements.in here (PyTorch, NumPy, Demucs,
Beat This!, transformers): the environment has nothing else.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import sys
import time
import traceback
from collections.abc import Callable
from typing import Any, BinaryIO

Say = Callable[[dict[str, Any]], None]


class Worker:
    def __init__(self, device: str, threads: int) -> None:
        import torch  # type: ignore[import-not-found]  # only in the analysis engine's environments

        self.torch = torch
        self.kind = "cpu"
        if device == "auto":
            if torch.cuda.is_available():  # Nvidia, and AMD through ROCm
                self.kind = "cuda"
            elif hasattr(torch, "xpu") and torch.xpu.is_available():
                self.kind = "xpu"
        if self.kind == "cpu" and threads > 0:
            torch.set_num_threads(threads)
        self.methods: dict[str, Callable[[dict[str, Any], Say], Any]] = {
            "info": self.info,
            "selftest": self.selftest,
            "benchmark": self.benchmark,
            "separate": self.separate,
            "beats": self.beats,
            "embed": self.embed,
        }
        self.models: dict[str, Any] = {}  # loaded stem separators, by folder

    def device_name(self) -> str:
        if self.kind == "cuda":
            return str(self.torch.cuda.get_device_name(0))
        if self.kind == "xpu":
            return str(self.torch.xpu.get_device_name(0))
        return "CPU"

    def info(self, params: dict[str, Any], say: Say) -> Any:
        torch = self.torch
        return {
            "torch": torch.__version__,
            "hip": getattr(torch.version, "hip", None) or "",
            "cuda": getattr(torch.version, "cuda", None) or "",
            "xpu": getattr(torch.version, "xpu", None) or "",
            "device": self.kind,
            "deviceName": self.device_name(),
            "threads": torch.get_num_threads(),
            "mkl": bool(torch.backends.mkl.is_available()),
        }

    def selftest(self, params: dict[str, Any], say: Say) -> Any:
        """One small matrix product on the device, compared with the CPU."""
        torch = self.torch
        start = time.perf_counter()
        generator = torch.Generator().manual_seed(1)
        a = torch.randn(512, 512, generator=generator)
        expected = a @ a
        if self.kind == "cpu":
            got = expected
        else:
            on_device = a.to(self.kind)
            got = (on_device @ on_device).cpu()
        error = float((got - expected).abs().max())
        return {"ok": error < 0.05, "error": error, "seconds": time.perf_counter() - start, **self.info({}, say)}

    def benchmark(self, params: dict[str, Any], say: Say) -> Any:
        """Matrix products per second on the device and on the CPU (for tools/owner-checks/gpu.sh)."""
        torch = self.torch
        size, rounds = int(params.get("size", 2048)), int(params.get("rounds", 20))

        def speed(device: str) -> float:
            a = torch.randn(size, size, device=device)
            (a @ a).sum().item()  # warm up, and wait for the device
            start = time.perf_counter()
            for _ in range(rounds):
                b = a @ a
            b.sum().item()
            return 2 * size**3 * rounds / (time.perf_counter() - start) / 1e9

        found = {"cpu": speed("cpu")}
        if self.kind != "cpu":
            found[self.kind] = speed(self.kind)
        return {"gflops": found, "device": self.kind, "deviceName": self.device_name()}

    def separate(self, params: dict[str, Any], say: Say) -> Any:
        """Splits a WAV file into stems with Demucs: one 32-bit float WAV per stem in `output`.

        params: input, output (folders are made), modelFolder (the model's yaml and safetensors files),
        model (its name, e.g. htdemucs), shifts (1), overlap (0.25), threads (CPU threads, 0: as set)."""
        if params.get("architecture") == "bs_roformer":
            return self.separate_many(params, say)
        torch = self.torch
        from demucs.apply import BagOfModels, apply_model  # type: ignore[import-not-found]
        from demucs.audio import encode_wav  # type: ignore[import-not-found]
        from demucs.hf import load_safetensors_model  # type: ignore[import-not-found]

        folder = str(params["modelFolder"])
        if folder not in self.models:
            names = read_bag(os.path.join(folder, str(params["model"]) + ".yaml"))
            models = [load_safetensors_model(os.path.join(folder, name + ".safetensors")) for name in names]
            bag = BagOfModels(models)
            bag.eval()
            self.models = {folder: bag}  # keep one in memory
        bag = self.models[folder]
        if int(params.get("threads", 0)) > 0 and self.kind == "cpu":
            torch.set_num_threads(int(params["threads"]))

        audio, rate = read_wav(str(params["input"]))
        wav = torch.from_numpy(audio)
        if wav.shape[0] == 1:
            wav = wav.repeat(2, 1)
        wav = wav[:2]
        if rate != bag.samplerate:
            import julius  # type: ignore[import-not-found]

            wav = julius.resample_frac(wav, rate, bag.samplerate)
        reference = wav.mean(0)
        mean, std = reference.mean(), reference.std()
        if float(std) < 1e-8:  # silence: nothing to separate, and dividing would blow up
            std = torch.tensor(1.0)

        total = max(1, len(bag.models))
        length = max(1, int(wav.shape[-1]))
        segment = int(float(bag.models[0].segment) * bag.samplerate)

        def callback(state: dict[str, Any]) -> None:
            if state.get("state") == "end":
                part = min(1.0, (int(state.get("segment_offset", 0)) + segment) / length)
                say({"done": (int(state.get("model_idx_in_bag", 0)) + part) / total, "step": "Separating"})

        start = time.perf_counter()
        with torch.no_grad():
            sources = apply_model(
                bag,
                ((wav - mean) / std)[None],
                shifts=int(params.get("shifts", 1)),
                overlap=float(params.get("overlap", 0.25)),
                split=True,
                device=self.kind if self.kind != "cpu" else "cpu",
                callback=callback,
            )[0]
        sources = sources * std + mean

        output = str(params["output"])
        os.makedirs(output, exist_ok=True)
        stems = {}
        for name, source in zip(bag.sources, sources, strict=True):
            path = os.path.join(output, name + ".wav")
            encode_wav(source.cpu(), path, bag.samplerate, as_float=True)
            stems[name] = path
        return {"stems": stems, "sampleRate": bag.samplerate, "seconds": time.perf_counter() - start}

    def separate_many(self, params: dict[str, Any], say: Say) -> Any:
        """Many instruments with BS-RoFormer (the MVSep Mega 53 stems model): only the stems heard, without
        the same sound twice (stem_choice), each a 32-bit float WAV in `output`. params as separate(), with
        modelFolder holding config.yaml and model.ckpt."""
        torch = self.torch
        import yaml  # type: ignore[import-untyped]

        from .bs_roformer import BSRoformer, demix
        from .stem_choice import choose_stems

        folder = str(params["modelFolder"])
        with open(os.path.join(folder, "config.yaml"), encoding="utf-8") as file:
            config = yaml.load(file, Loader=yaml.FullLoader)  # its tuples are tagged python/tuple
        names = [str(n) for n in config["training"]["instruments"]]
        if folder not in self.models:
            say({"done": 0.0, "step": "Loading the model"})
            model = BSRoformer(**config["model"])
            state = torch.load(os.path.join(folder, "model.ckpt"), map_location="cpu", weights_only=True)
            for key in ("state", "state_dict"):
                if isinstance(state, dict) and key in state:
                    state = state[key]
            model.load_state_dict(state)
            model.eval()
            self.models = {folder: model.to(self.kind if self.kind != "cpu" else "cpu")}  # keep one in memory
        model = self.models[folder]
        if int(params.get("threads", 0)) > 0 and self.kind == "cpu":
            torch.set_num_threads(int(params["threads"]))

        rate = int(config["audio"]["sample_rate"])
        audio, file_rate = read_wav(str(params["input"]))
        wav = torch.from_numpy(audio)
        if wav.shape[0] == 1:
            wav = wav.repeat(2, 1)
        wav = wav[:2]
        if file_rate != rate:
            import julius

            wav = julius.resample_frac(wav, file_rate, rate)

        start = time.perf_counter()
        inference = config.get("inference", {})
        chunk = int(inference.get("chunk_size", config["audio"]["chunk_size"]))
        overlap = int(inference.get("num_overlap", 2))
        device = self.kind if self.kind != "cpu" else "cpu"
        sources = demix(model, wav, chunk, overlap, device,
                        lambda done: say({"done": done, "step": "Separating instruments"}))  # fmt: skip

        energy = {name: float((sources[i] ** 2).sum()) for i, name in enumerate(names)}
        keep = choose_stems(energy, float((wav**2).sum()))
        from demucs.audio import encode_wav

        output = str(params["output"])
        os.makedirs(output, exist_ok=True)
        stems = {}
        for name in keep:
            path = os.path.join(output, name + ".wav")
            encode_wav(sources[names.index(name)], path, rate, as_float=True)
            stems[name] = path
        return {"stems": stems, "sampleRate": rate, "seconds": time.perf_counter() - start}

    def beats(self, params: dict[str, Any], say: Say) -> Any:
        """Beats and downbeats of a WAV file with Beat This!, without its DBN: params input,
        checkpoint (a .ckpt file). Times in seconds."""
        from beat_this.inference import Audio2Beats  # type: ignore[import-not-found]

        checkpoint = str(params["checkpoint"])
        key = "beats:" + checkpoint
        if key not in self.models:
            device = self.kind if self.kind != "cpu" else "cpu"
            self.models[key] = Audio2Beats(checkpoint_path=checkpoint, device=device, dbn=False)
        tracker = self.models[key]
        say({"done": 0.1, "step": "Finding the beat"})
        audio, rate = read_wav(str(params["input"]))
        start = time.perf_counter()
        beats, downbeats = tracker(audio.mean(axis=0).astype("float64"), rate)
        return {
            "beats": [float(b) for b in beats],
            "downbeats": [float(d) for d in downbeats],
            "seconds": time.perf_counter() - start,
        }

    def embed(self, params: dict[str, Any], say: Say) -> Any:
        """How sounds sound, as unit vectors (LAION-CLAP's audio tower): params model (a folder
        with config.json, preprocessor_config.json and pytorch_model.bin), clips [{file, start, end}]
        (seconds of a WAV file; CLAP hears at most 10 s). Alike sounds have vectors pointing alike."""
        import numpy as np

        folder = str(params["model"])
        key = "clap:" + folder
        if key not in self.models:
            say({"done": 0.0, "step": "Loading the sound model"})
            self.models[key] = load_clap(self.torch, folder, self.kind)
        model, features = self.models[key]
        clips = list(params.get("clips", []))
        rate = 48000
        audio: list[Any] = []
        for clip in clips:
            samples, file_rate = read_wav(str(clip["file"]))
            mono = samples.mean(axis=0)
            start = int(float(clip.get("start", 0.0)) * file_rate)
            end = int(float(clip.get("end", len(mono) / file_rate)) * file_rate)
            piece = mono[start : max(start, end)][: 10 * file_rate]
            if file_rate != rate:
                length = int(len(piece) * rate / file_rate)
                piece = np.interp(np.arange(length) * file_rate / rate, np.arange(len(piece)), piece)
            audio.append(piece.astype("float32") if len(piece) else np.zeros(rate // 10, dtype="float32"))
        start_time = time.perf_counter()
        vectors: list[list[float]] = []
        batch = 16
        for first in range(0, len(audio), batch):
            say({"done": first / max(1, len(audio)), "step": "Listening"})
            inputs = features(audio[first : first + batch], sampling_rate=rate, return_tensors="pt")
            with self.torch.no_grad():
                embeds = model(input_features=inputs["input_features"].to(self.kind)).audio_embeds
            values = embeds.cpu().numpy() if hasattr(embeds, "cpu") else np.asarray(embeds)
            values = values / np.maximum(np.linalg.norm(values, axis=1, keepdims=True), 1e-9)
            vectors += [[round(float(v), 5) for v in row] for row in values]
        return {"embeddings": vectors, "seconds": time.perf_counter() - start_time}


def load_clap(torch: Any, folder: str, device: str) -> tuple[Any, Any]:
    """CLAP's audio tower and its feature extractor from a transformers-format folder. The weights file is a
    pickle: it is read with weights_only=True (plain tensors only) after its hash was checked."""
    from transformers import (  # type: ignore[import-not-found]
        ClapAudioModelWithProjection,
        ClapConfig,
        ClapFeatureExtractor,
    )

    config = ClapConfig.from_pretrained(folder)
    state = torch.load(os.path.join(folder, "pytorch_model.bin"), map_location="cpu", weights_only=True)
    audio_state = {k: v for k, v in state.items() if k.startswith(("audio_model.", "audio_projection."))}
    model = ClapAudioModelWithProjection(config.audio_config)
    model.load_state_dict(audio_state, strict=True)
    model.eval()
    model.to(device)
    return model, ClapFeatureExtractor.from_pretrained(folder)


def read_bag(path: str) -> list[str]:
    """The models of a Demucs bag file: "models: ['955717e8']" (YAML, without needing a YAML reader)."""
    import ast

    with open(path, encoding="utf-8") as file:
        for line in file:
            if line.strip().startswith("models:"):
                return [str(name) for name in ast.literal_eval(line.split(":", 1)[1].strip())]
    raise ValueError(f"{path} names no models")


def read_wav(path: str) -> tuple[Any, int]:
    """A WAV file as float32 [channels, samples] and its sample rate: 16-, 24- or 32-bit PCM, or 32- or
    64-bit float (also in the extensible format)."""
    import struct

    import numpy as np

    with open(path, "rb") as file:
        data = file.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path} is not a WAV file")
    at, fmt, samples = 12, None, None
    while at + 8 <= len(data):
        chunk, size = data[at : at + 4], struct.unpack("<I", data[at + 4 : at + 8])[0]
        body = data[at + 8 : at + 8 + size]
        if chunk == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
            if fmt[0] == 0xFFFE and len(body) >= 26:  # extensible: the real format is in the sub-format GUID
                fmt = (struct.unpack("<H", body[24:26])[0], *fmt[1:])
        elif chunk == b"data":
            samples = body
        at += 8 + size + (size & 1)
    if fmt is None or samples is None:
        raise ValueError(f"{path} has no audio")
    kind, channels, rate, _, _, bits = fmt
    if kind == 3 and bits in (32, 64):
        audio = np.frombuffer(samples, dtype="<f4" if bits == 32 else "<f8").astype(np.float32)
    elif kind == 1 and bits == 16:
        audio = np.frombuffer(samples, dtype="<i2").astype(np.float32) / 32768.0
    elif kind == 1 and bits == 24:
        raw = np.frombuffer(samples[: len(samples) // 3 * 3], dtype=np.uint8).reshape(-1, 3)
        ints = raw[:, 0].astype(np.int32) | (raw[:, 1].astype(np.int32) << 8) | (raw[:, 2].astype(np.int32) << 16)
        audio = (np.where(ints >= 1 << 23, ints - (1 << 24), ints)).astype(np.float32) / 8388608.0
    elif kind == 1 and bits == 32:
        audio = np.frombuffer(samples, dtype="<i4").astype(np.float32) / 2147483648.0
    else:
        raise ValueError(f"{path}: this WAV format ({kind}, {bits} bits) cannot be read")
    frames = len(audio) // channels
    return np.ascontiguousarray(audio[: frames * channels].reshape(frames, channels).T), rate


def serve(worker: Worker, requests: BinaryIO, replies: BinaryIO) -> None:
    def send(message: dict[str, Any]) -> None:
        replies.write((json.dumps(message) + "\n").encode())
        replies.flush()

    for line in requests:
        try:
            request = json.loads(line)
        except ValueError:
            continue
        number = request.get("id")
        method = worker.methods.get(str(request.get("method", "")))
        if method is None:
            send({"id": number, "error": f"Unknown method {request.get('method')}"})
            continue

        def say(progress: dict[str, Any], number: Any = number) -> None:
            send({"id": number, "progress": progress})

        try:
            result = method(dict(request.get("params") or {}), say)
            send({"id": number, "result": result})
        except Exception as error:
            traceback.print_exc()
            send({"id": number, "error": str(error) or type(error).__name__})


def lower_priority() -> None:
    if sys.platform == "win32":
        import ctypes

        below_normal = 0x00004000
        kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined,unused-ignore]
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), below_normal)
        return
    with contextlib.suppress(OSError):
        os.nice(10)


def main() -> None:
    parser = argparse.ArgumentParser(prog="spinus-compute-worker")
    parser.add_argument("--device", choices=("auto", "cpu"), default="auto")
    parser.add_argument("--threads", type=int, default=0)
    args = parser.parse_args()

    # Keep the replies apart from whatever native libraries print: the real stdout is only for them.
    replies = os.fdopen(os.dup(1), "wb")
    os.dup2(2, 1)
    sys.stdout = sys.stderr
    lower_priority()  # below the audio engine and the user interface
    try:
        worker = Worker(args.device, args.threads)
    except Exception as error:
        replies.write((json.dumps({"id": None, "error": f"PyTorch could not start: {error}"}) + "\n").encode())
        replies.flush()
        raise SystemExit(1) from error
    replies.write((json.dumps({"id": None, "result": "ready"}) + "\n").encode())
    replies.flush()
    serve(worker, sys.stdin.buffer, replies)


if __name__ == "__main__":
    main()
