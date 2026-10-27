"""The ai.v1 JSON-RPC methods and the jobs they start."""

from __future__ import annotations

import asyncio
import contextlib
import functools
import hashlib
import importlib.util
import json
import logging
import os
import uuid
from collections.abc import Awaitable, Callable
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

import httpx

from . import PROTOCOL_VERSION, __version__
from .assistant import NoConnection, ask
from .compute.engine import Engine, EngineError
from .compute.environments import InstallError
from .compute.weights import WeightStore, find_model
from .connections import DEFAULT_BASE_URLS, ConnectionStore, KeyStore
from .embeddings import ModelStore, default_models_folder, missing_dependencies, run_blocking
from .gateway import ProviderError, list_models
from .manual import Manual
from .routing import Routing
from .usage import UsageLog

log = logging.getLogger("spinus_ai.service")

Notify = Callable[[str, dict[str, Any]], Awaitable[None]]

# Beatbox sounds the listening features tell apart (spinus_ai.listening.SOUNDS, which needs NumPy).
BEATBOX_SOUNDS = ("kick", "snare", "hat")

# Where local model servers usually listen: Ollama, then llama.cpp's server.
LOCAL_SERVERS = ("http://localhost:11434/v1", "http://localhost:8080/v1")


def file_digest(path: Path) -> str:
    """SHA-256 of a file's contents (read in pieces)."""
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


class RpcError(Exception):
    def __init__(self, code: int, message: str) -> None:
        super().__init__(message)
        self.code = code


def load_commands(schema_file: Path | None) -> dict[str, Any]:
    if schema_file is None or not schema_file.exists():
        return {}
    data = json.loads(schema_file.read_text(encoding="utf-8"))
    return {
        name: {"summary": c.get("description", ""), "parameters": c.get("parameters", {})}
        for name, c in data.get("commands", {}).items()
    }


class Service:
    def __init__(
        self,
        config: Path,
        keys: KeyStore,
        docs: Path | None,
        schema: Path | None,
        client: httpx.AsyncClient | None = None,
        keyring_note: str = "",
        models: Path | None = None,
        engine: Engine | None = None,
    ) -> None:
        self.config = config
        self.connections = ConnectionStore(config, keys)
        self.routing = Routing(config, self.connections)
        self.usage = UsageLog(config)
        self.manual = Manual(docs)
        self.commands = load_commands(schema)
        self.models = ModelStore(config, models)
        cache = Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") / "spinus"
        self.engine = engine or Engine(config, (models or default_models_folder()).parent / "python", cache)
        self.weights = WeightStore((models or default_models_folder()) / "analysis")
        self.model_state = "keywords"  # "keywords", "loading", "ready", or an error message
        self.model_task: asyncio.Task[None] | None = None
        self.client = client or httpx.AsyncClient(timeout=httpx.Timeout(120.0, connect=10.0))
        self.keyring_note = keyring_note
        self.jobs: dict[str, asyncio.Task[None]] = {}
        self.install_job = ""  # the analysis engine's installation, for a reopened Options window
        self.stopping = asyncio.Event()

    async def close(self) -> None:
        for task in list(self.jobs.values()):
            task.cancel()
        await self.engine.stop()
        await self.client.aclose()

    async def handle(self, method: str, params: dict[str, Any], notify: Notify) -> Any:
        handlers: dict[str, Callable[[dict[str, Any], Notify], Awaitable[Any]]] = {
            "ai.v1.hello": self.hello,
            "ai.v1.connections.list": self.list_connections,
            "ai.v1.connections.save": self.save_connection,
            "ai.v1.connections.remove": self.remove_connection,
            "ai.v1.connections.test": self.test_connection,
            "ai.v1.connections.findLocal": self.find_local,
            "ai.v1.routing.get": self.get_routing,
            "ai.v1.routing.set": self.set_routing,
            "ai.v1.assistant.ask": self.assistant_ask,
            "ai.v1.job.cancel": self.cancel_job,
            "ai.v1.usage.get": self.get_usage,
            "ai.v1.shutdown": self.shutdown,
            "ai.v1.embeddings.status": self.embeddings_status,
            "ai.v1.embeddings.download": self.embeddings_download,
            "ai.v1.embeddings.addFolder": self.embeddings_add_folder,
            "ai.v1.embeddings.addFromHuggingFace": self.embeddings_add_hf,
            "ai.v1.embeddings.use": self.embeddings_use,
            "ai.v1.embeddings.remove": self.embeddings_remove,
            "ai.v1.pattern.generate": self.pattern_generate,
            "ai.v1.listen.melody": self.listen_melody,
            "ai.v1.listen.beats": self.listen_beats,
            "ai.v1.listen.vocal": self.listen_vocal,
            "ai.v1.listen.teach": self.listen_teach,
            "ai.v1.listen.sounds": self.listen_sounds,
            "ai.v1.listen.forget": self.listen_forget,
            "ai.v1.compute.status": self.compute_status,
            "ai.v1.compute.install": self.compute_install,
            "ai.v1.compute.remove": self.compute_remove,
            "ai.v1.compute.setForceCpu": self.compute_force_cpu,
            "ai.v1.compute.test": self.compute_test,
            "ai.v1.stems.models": self.stems_models,
            "ai.v1.stems.download": self.stems_download,
            "ai.v1.stems.remove": self.stems_remove,
            "ai.v1.stems.separate": self.stems_separate,
            "ai.v1.analyze.tempo": self.analyze_tempo,
            "ai.v1.analyze.key": self.analyze_key,
            "ai.v1.transcribe.notes": self.transcribe_notes,
            "ai.v1.transcribe.drums": self.transcribe_drums,
            "ai.v1.samples.make": self.make_samples,
            "ai.v1.presets.missing": self.presets_missing,
            "ai.v1.presets.add": self.presets_add,
            "ai.v1.presets.match": self.presets_match,
            "ai.v1.mix.analyze": self.mix_analyze,
            "ai.v1.mix.plan": self.mix_plan,
            "ai.v1.mix.toward": self.mix_toward,
            "ai.v1.style.summarize": self.style_summarize,
            "ai.v1.song.write": self.song_write,
        }
        handler = handlers.get(method)
        if handler is None:
            raise RpcError(-32601, f"Unknown method {method}")
        return await handler(params, notify)

    # -----------------------------------------------------------------------------------------------

    def start(self) -> None:
        """Loads the chosen embedding model in the background (the service answers meanwhile)."""
        if self.models.active and self.model_task is None:
            self.model_task = asyncio.create_task(self.load_model(self.models.active))

    async def load_model(self, model_id: str) -> None:
        self.model_state = "loading"
        try:
            model = await run_blocking(lambda: self.models.load(model_id))
            await run_blocking(lambda: self.manual.use_model(model, self.models.cache))
            self.model_state = "ready"
        except Exception as error:
            log.warning("Could not use the search model %s: %s", model_id, error)
            self.manual.use_model(None, None)
            self.model_state = f"Could not use the model: {error}"
        finally:
            self.model_task = None

    async def hello(self, params: dict[str, Any], notify: Notify) -> Any:
        return {
            "version": __version__,
            "protocol": PROTOCOL_VERSION,
            "keyring": self.keyring_note,
            "manualSections": len(self.manual.sections),
            "commands": len(self.commands),
        }

    async def list_connections(self, params: dict[str, Any], notify: Notify) -> Any:
        store = self.connections
        return {
            "connections": [c.public(store.key_for(c) is not None) for c in store.connections],
            "defaultBaseUrls": DEFAULT_BASE_URLS,
        }

    async def save_connection(self, params: dict[str, Any], notify: Notify) -> Any:
        try:
            connection = self.connections.upsert(
                str(params.get("name", "")),
                str(params.get("provider", "")),
                params.get("baseUrl") or None,
                params.get("key") or None,
                params.get("id") or None,
            )
        except ValueError as error:
            raise RpcError(-32602, str(error)) from error
        return {"id": connection.id}

    async def remove_connection(self, params: dict[str, Any], notify: Notify) -> Any:
        return {"removed": self.connections.remove(str(params.get("id", "")))}

    async def test_connection(self, params: dict[str, Any], notify: Notify) -> Any:
        """Lists the provider's models: proves the address and the key work, and fills the model choice."""
        connection = self.connections.find(str(params.get("id", "")))
        if connection is None:
            raise RpcError(-32602, "Unknown connection.")
        key = self.connections.key_for(connection)
        if self.connections.needs_key(connection) and not key:
            return {"ok": False, "message": "This connection has no key."}
        try:
            models = await list_models(self.client, connection, key)
        except ProviderError as error:
            log.warning(
                "Connection test of %s (%s) failed: %s: %s", connection.name, connection.provider, error.kind, error
            )
            return {"ok": False, "message": str(error)}
        log.info("Connection test of %s (%s): %d models", connection.name, connection.provider, len(models))
        connection.models = sorted(models)
        self.connections.save()
        return {"ok": True, "message": f"Works: {len(models)} models available.", "models": connection.models}

    async def find_local(self, params: dict[str, Any], notify: Notify) -> Any:
        """Local model servers running on this computer (Ollama, llama.cpp), with their models."""
        from .connections import Connection

        found = []
        for url in LOCAL_SERVERS:
            probe = Connection("probe", "Local", "local", url)
            try:
                models = await asyncio.wait_for(list_models(self.client, probe, None), 3.0)
            except (ProviderError, TimeoutError):
                continue
            found.append({"baseUrl": url, "models": sorted(models)})
        return {"servers": found}

    async def get_routing(self, params: dict[str, Any], notify: Notify) -> Any:
        resolved = {
            f: [{"connection": r.connection.id, "model": r.model} for r in self.routing.resolve(f)]
            for f in self.routing.get()
        }
        return {"chosen": self.routing.get(), "effective": resolved}

    async def set_routing(self, params: dict[str, Any], notify: Notify) -> Any:
        try:
            self.routing.set(str(params.get("feature", "")), list(params.get("chain", [])))
        except ValueError as error:
            raise RpcError(-32602, str(error)) from error
        return await self.get_routing({}, notify)

    async def get_usage(self, params: dict[str, Any], notify: Notify) -> Any:
        return {"usage": self.usage.get()}

    async def shutdown(self, params: dict[str, Any], notify: Notify) -> Any:
        self.stopping.set()
        return {}

    # -----------------------------------------------------------------------------------------------

    async def embeddings_status(self, params: dict[str, Any], notify: Notify) -> Any:
        return {
            "models": self.models.describe(),
            "active": self.models.active,
            "state": self.model_state,
            "missing": missing_dependencies(),
        }

    def job(self, notify: Notify, work: Callable[[Callable[[dict[str, Any]], Awaitable[None]]], Awaitable[Any]]) -> str:
        job = uuid.uuid4().hex[:12]

        async def say(update: dict[str, Any]) -> None:
            await notify("ai.v1.job.update", {"job": job, **update})

        async def run() -> None:
            try:
                result = await work(say)
                await say({"kind": "done", "result": result})
            except asyncio.CancelledError:
                await say({"kind": "error", "message": "Stopped."})
            except Exception as error:
                await say({"kind": "error", "message": str(error)})
            finally:
                self.jobs.pop(job, None)

        self.jobs[job] = asyncio.create_task(run())
        return job

    async def embeddings_download(self, params: dict[str, Any], notify: Notify) -> Any:
        spec = self.models.find(str(params.get("id", "")))
        if spec is None or spec.folder:
            raise RpcError(-32602, "Unknown model.")

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            async def progress(done: int, total: int) -> None:
                await say({"kind": "progress", "done": done, "total": total})

            await self.models.download(self.client, spec, progress)
            return {"id": spec.id}

        return {"job": self.job(notify, work)}

    async def embeddings_add_folder(self, params: dict[str, Any], notify: Notify) -> Any:
        try:
            spec = self.models.add_folder(Path(str(params.get("folder", ""))), str(params.get("name", "")))
        except ValueError as error:
            raise RpcError(-32602, str(error)) from error
        return {"id": spec.id}

    async def embeddings_add_hf(self, params: dict[str, Any], notify: Notify) -> Any:
        try:
            spec = self.models.from_hugging_face(str(params.get("link", "")))
        except ValueError as error:
            raise RpcError(-32602, str(error)) from error
        return await self.embeddings_download({"id": spec.id}, notify)

    async def embeddings_use(self, params: dict[str, Any], notify: Notify) -> Any:
        """Uses a model for manual search ("" = keywords only); answers when it is ready."""
        model_id = str(params.get("id", ""))
        if self.model_task is not None:
            self.model_task.cancel()
            self.model_task = None
        self.models.active = model_id
        self.models.save()
        if not model_id:
            self.manual.use_model(None, None)
            self.model_state = "keywords"
            return await self.embeddings_status({}, notify)
        await self.load_model(model_id)
        if self.model_state != "ready":
            problem, self.model_state = self.model_state, "keywords"  # search goes on with keywords
            self.models.active = ""
            self.models.save()
            raise RpcError(-32000, problem)
        return await self.embeddings_status({}, notify)

    async def embeddings_remove(self, params: dict[str, Any], notify: Notify) -> Any:
        model_id = str(params.get("id", ""))
        if model_id == self.models.active:
            await self.embeddings_use({"id": ""}, notify)
        self.models.remove(model_id)
        return await self.embeddings_status({}, notify)

    async def pattern_generate(self, params: dict[str, Any], notify: Notify) -> Any:
        """Writes (or adjusts) a pattern for some channels; the result is variations to suggest."""
        from .patterns import Settings, generate

        try:
            settings = Settings.from_json(params)
        except (KeyError, TypeError, ValueError) as error:
            raise RpcError(-32602, f"Bad pattern request: {error}") from error

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            return await generate(self.client, self.connections, self.routing, self.usage, settings, say)

        return {"job": self.job(notify, work)}

    # -----------------------------------------------------------------------------------------------
    # The analysis engine: PyTorch, installed when the user agrees.

    def compute_build(self, params: dict[str, Any]) -> Any:
        build = self.engine.catalog.find(str(params.get("build", "")))
        if build is None:
            raise RpcError(-32602, "Unknown build.")
        return build

    async def compute_status(self, params: dict[str, Any], notify: Notify) -> Any:
        if params.get("refresh"):
            self.engine.gpus(refresh=True)
        return {**self.engine.status(), "installJob": self.install_job if self.install_job in self.jobs else ""}

    async def compute_install(self, params: dict[str, Any], notify: Notify) -> Any:
        """Installs a build (the user agreed to its size in Options > AI); progress 0..1 with a step."""
        build = self.compute_build(params)
        if self.engine.environments.installing:
            raise RpcError(-32000, "Another build is being installed.")

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            async def progress(done: float, step: str) -> None:
                await say({"kind": "progress", "done": done, "total": 1.0, "step": step})

            await self.engine.stop()
            try:
                await self.engine.environments.install(build, progress)
            except InstallError as error:
                raise RuntimeError(str(error)) from error
            return self.engine.status()

        self.install_job = self.job(notify, work)
        return {"job": self.install_job}

    async def compute_remove(self, params: dict[str, Any], notify: Notify) -> Any:
        build = self.compute_build(params)
        await self.engine.stop()
        try:
            await self.engine.environments.remove(build)
        except InstallError as error:
            raise RpcError(-32000, str(error)) from error
        return await self.compute_status({}, notify)

    async def compute_force_cpu(self, params: dict[str, Any], notify: Notify) -> Any:
        await self.engine.set_force_cpu(bool(params.get("on")))
        return await self.compute_status({}, notify)

    async def compute_test(self, params: dict[str, Any], notify: Notify) -> Any:
        """Starts the engine and runs its self-test (on the GPU when there is one)."""

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            await say({"kind": "progress", "done": 0.0, "total": 1.0, "step": "Starting PyTorch"})
            try:
                test = await self.engine.test()
            except EngineError as error:
                raise RuntimeError(str(error)) from error
            return {"test": test, "status": self.engine.status()}

        return {"job": self.job(notify, work)}

    # -----------------------------------------------------------------------------------------------
    # Stem separation: Demucs in the analysis engine, results cached by the song's contents.

    def stem_model(self, params: dict[str, Any]) -> Any:
        model = find_model(str(params.get("model", "htdemucs")))
        if model is None:
            raise RpcError(-32602, "Unknown stem model.")
        return model

    async def stems_models(self, params: dict[str, Any], notify: Notify) -> Any:
        return {"models": self.weights.describe()}

    async def stems_download(self, params: dict[str, Any], notify: Notify) -> Any:
        """Downloads a separation model (the user agreed to its size); progress in bytes. `link`: an address
        the user pasted (one the model's authors gave), used instead of the known ones; the files must still
        match the model's checksums."""
        model = self.stem_model(params)
        link = str(params.get("link") or "").strip()
        if link and urlsplit(link).scheme not in ("http", "https"):
            raise RpcError(-32602, "Paste a web link (starting with https://).")

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            async def progress(done: int, total: int) -> None:
                await say({"kind": "progress", "done": done, "total": total})

            await self.weights.download(self.client, model, progress, link)
            return {"models": self.weights.describe()}

        return {"job": self.job(notify, work)}

    async def stems_remove(self, params: dict[str, Any], notify: Notify) -> Any:
        self.weights.remove(self.stem_model(params))
        return {"models": self.weights.describe()}

    async def stems_separate(self, params: dict[str, Any], notify: Notify) -> Any:
        """Splits a song (a WAV file) into stems: {file, model, cacheFolder, quiet}. The result, kept in
        cacheFolder/stems/ by the song's contents and the model, is {stems: {name: file}, cached}. `quiet`
        (while Spinus plays) leaves CPU cores free for the audio when the CPU does the work."""
        model = self.stem_model(params)
        song = Path(str(params.get("file", "")))
        if not song.is_file():
            raise RpcError(-32602, f"There is no audio file at {song}.")
        if not self.weights.is_ready(model):
            raise RpcError(-32000, f"{model.name} is not downloaded yet ({model.size / 1e6:.0f} MB).")
        cache_root = Path(str(params.get("cacheFolder", ""))) if params.get("cacheFolder") else self.config / "cache"
        quiet = bool(params.get("quiet", False))

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            await say({"kind": "progress", "done": 0.0, "total": 1.0, "step": "Reading the song"})
            digest = await run_blocking(lambda: file_digest(song))
            folder = cache_root / "stems" / f"{digest[:20]}-{model.id}-{model.revision[:8]}"
            done_file = folder / "stems.json"
            if done_file.exists():
                stems = json.loads(done_file.read_text(encoding="utf-8"))
                if all(Path(p).exists() for p in stems.values()):
                    return {"stems": stems, "cached": True, "folder": str(folder)}

            async def progress(update: dict[str, Any]) -> None:
                await say({"kind": "progress", "done": float(update.get("done", 0.0)), "total": 1.0,
                           "step": str(update.get("step", "Separating"))})  # fmt: skip

            threads = max(1, self.engine.cores() - 4) if quiet else 0
            try:
                result = await self.engine.call(
                    "separate",
                    {"input": str(song), "output": str(folder), "modelFolder": str(self.weights.folder(model)),
                     "model": model.id, "threads": threads, "architecture": model.architecture},
                    progress,
                )  # fmt: skip
            except EngineError as error:
                raise RuntimeError(str(error)) from error
            stems = dict(result["stems"])
            done_file.write_text(json.dumps(stems, indent=1) + "\n", encoding="utf-8")
            return {"stems": stems, "cached": False, "folder": str(folder), "seconds": result.get("seconds")}

        return {"job": self.job(notify, work)}

    # -----------------------------------------------------------------------------------------------
    # Tempo, beats and key.

    def beat_model(self) -> Any:
        """Beat This!: the full model on a GPU, the small one on the CPU; whichever is downloaded."""
        plan = self.engine.plan()
        preferred = "beat_this_final0" if plan is not None and plan.device != "cpu" else "beat_this_small0"
        for model_id in (preferred, "beat_this_final0", "beat_this_small0"):
            model = find_model(model_id)
            if model is not None and self.weights.is_ready(model):
                return model
        wanted = find_model(preferred)
        assert wanted is not None
        raise RpcError(-32000, f"{wanted.name} is not downloaded yet ({wanted.size / 1e6:.0f} MB).")

    async def analyze_tempo(self, params: dict[str, Any], notify: Notify) -> Any:
        """Beats, downbeats, tempo and beats per bar of a song (a WAV file): {file, cacheFolder}. Cached
        by the song's contents."""
        from .analysis import rhythm

        song = Path(str(params.get("file", "")))
        if not song.is_file():
            raise RpcError(-32602, f"There is no audio file at {song}.")
        model = self.beat_model()
        cache_root = Path(str(params.get("cacheFolder", ""))) if params.get("cacheFolder") else self.config / "cache"

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            digest = await run_blocking(lambda: file_digest(song))
            cached = cache_root / "analysis" / f"{digest[:20]}-{model.id}.json"
            if cached.exists():
                # The beats are what was kept; the tempo and bars are worked out again (as this version does).
                kept = json.loads(cached.read_text(encoding="utf-8"))
                return {**rhythm(list(kept.get("beats") or []), list(kept.get("downbeats") or [])), "cached": True}

            async def progress(update: dict[str, Any]) -> None:
                await say({"kind": "progress", "done": float(update.get("done", 0.0)), "total": 1.0,
                           "step": str(update.get("step", "Finding the beat"))})  # fmt: skip

            try:
                found = await self.engine.call(
                    "beats",
                    {"input": str(song), "checkpoint": str(self.weights.folder(model) / model.files[0].path)},
                    progress,
                )
            except EngineError as error:
                raise RuntimeError(str(error)) from error
            result = rhythm(list(found["beats"]), list(found["downbeats"]))
            cached.parent.mkdir(parents=True, exist_ok=True)
            cached.write_text(json.dumps(result) + "\n", encoding="utf-8")
            return {**result, "cached": False}

        return {"job": self.job(notify, work)}

    async def analyze_key(self, params: dict[str, Any], notify: Notify) -> Any:
        """The key of a song (a WAV file; without drums is better): {tonic, scale, name, confidence, others}."""
        if importlib.util.find_spec("numpy") is None:
            raise RpcError(-32000, "Finding the key needs NumPy: install the python-numpy package.")
        song = Path(str(params.get("file", "")))
        if not song.is_file():
            raise RpcError(-32602, f"There is no audio file at {song}.")

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            from .analysis import chroma, key_of
            from .compute.worker import read_wav

            def find() -> dict[str, Any]:
                audio, rate = read_wav(str(song))
                return key_of(chroma(audio, rate))

            await say({"kind": "progress", "done": 0.0, "total": 1.0, "step": "Finding the key"})
            return await run_blocking(find)

        return {"job": self.job(notify, work)}

    async def transcribe_notes(self, params: dict[str, Any], notify: Notify) -> Any:
        """Notes of a melodic stem with Basic Pitch: {file, beats, firstDownbeat, cacheFolder}.
        The result: {notes: [{start, length (ticks from the first downbeat), pitch, velocity}], seconds:
        [{start, end, pitch, velocity}]}, cached by the stem's contents. With `known` (the seconds Spinus kept
        in its analysis library from an earlier time) Basic Pitch is not run: they are only placed on the beats."""
        known = params.get("known")
        if isinstance(known, list):
            if importlib.util.find_spec("numpy") is None:
                raise RpcError(-32000, "Turning parts into notes needs NumPy: install the python-numpy package.")
            known_beats = [float(b) for b in params.get("beats", [])]
            known_first = float(params.get("firstDownbeat", known_beats[0] if known_beats else 0.0))

            async def place(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
                from .transcribe import place_on_beats

                found = [dict(n) for n in known]
                return {"notes": place_on_beats(found, known_beats, known_first), "seconds": found}

            return {"job": self.job(notify, place)}
        missing = [m for m in ("numpy", "onnxruntime") if importlib.util.find_spec(m) is None]
        if missing:
            raise RpcError(-32000, "Turning parts into notes needs python-numpy and python-onnxruntime-cpu.")
        model = find_model("basic_pitch")
        assert model is not None
        if not self.weights.is_ready(model):
            raise RpcError(-32000, f"{model.name} is not downloaded yet ({model.size / 1e3:.0f} KB).")
        stem = Path(str(params.get("file", "")))
        if not stem.is_file():
            raise RpcError(-32602, f"There is no audio file at {stem}.")
        beats = [float(b) for b in params.get("beats", [])]
        first_downbeat = float(params.get("firstDownbeat", beats[0] if beats else 0.0))
        cache_root = Path(str(params.get("cacheFolder", ""))) if params.get("cacheFolder") else self.config / "cache"

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            from .compute.worker import read_wav
            from .transcribe import Model, place_on_beats, transcribe

            await say({"kind": "progress", "done": 0.0, "total": 1.0, "step": "Listening for notes"})
            digest = await run_blocking(lambda: file_digest(stem))
            cached = cache_root / "analysis" / f"{digest[:20]}-basic_pitch.json"
            if cached.exists():
                found = json.loads(cached.read_text(encoding="utf-8"))
            else:

                def run() -> list[dict[str, Any]]:
                    audio, rate = read_wav(str(stem))
                    return transcribe(Model(self.weights.folder(model) / "nmp.onnx"), audio, rate)

                found = await run_blocking(run)
                cached.parent.mkdir(parents=True, exist_ok=True)
                cached.write_text(json.dumps(found) + "\n", encoding="utf-8")
            return {"notes": place_on_beats(found, beats, first_downbeat), "seconds": found}

        return {"job": self.job(notify, work)}

    async def transcribe_drums(self, params: dict[str, Any], notify: Notify) -> Any:
        """Drum hits of a drum stem: {file, beats, firstDownbeat, cacheFolder}. The result:
        {notes: [{start, length, pitch (a Drum Machine pad), velocity}], hits: [{time, kind, pad, velocity}],
        groups: [{pad, kind, name, count, start, end}]}: hits of one kind that sound clearly different get a
        pad each, with accents relative to each group's loudest hit. The first time, the
        recogniser is trained on this computer (a few seconds). With `hits` (kept in Spinus's analysis library
        from an earlier time: [{time, kind, pad, velocity}]) the hits are not looked for again."""
        if importlib.util.find_spec("numpy") is None:
            raise RpcError(-32000, "Turning drums into steps needs NumPy: install the python-numpy package.")
        known = params.get("hits")
        stem = Path(str(params.get("file", "")))
        if not stem.is_file():
            raise RpcError(-32602, f"There is no audio file at {stem}.")
        beats = [float(b) for b in params.get("beats", [])]
        first_downbeat = float(params.get("firstDownbeat", beats[0] if beats else 0.0))
        cache_root = Path(str(params.get("cacheFolder", ""))) if params.get("cacheFolder") else self.config / "cache"

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            from . import drums
            from .compute.worker import read_wav

            found = [dict(h) for h in known] if isinstance(known, list) else await self.drum_hits(stem, cache_root, say)
            audio, rate = await run_blocking(lambda: read_wav(str(stem)))
            hits, groups = await run_blocking(lambda: drums.group_hits(audio, rate, found))
            return {"notes": drums.place_hits(hits, beats, first_downbeat), "hits": hits, "groups": groups}

        return {"job": self.job(notify, work)}

    async def drum_hits(
        self, stem: Path, cache_root: Path, say: Callable[[dict[str, Any]], Awaitable[None]]
    ) -> list[dict[str, Any]]:
        """The drum hits of a stem, kept in the cache by its contents."""
        from . import drums
        from .compute.worker import read_wav

        model_file = self.config / "cache" / f"drum-recogniser-{drums.VERSION}.json"
        if not model_file.exists():
            await say({"kind": "progress", "done": 0.0, "total": 1.0,
                       "step": "Learning Spinus's drum sounds (once, a few seconds)"})  # fmt: skip
        model = await run_blocking(lambda: drums.recogniser(self.config / "cache"))
        await say({"kind": "progress", "done": 0.5, "total": 1.0, "step": "Listening for the drums"})
        digest = await run_blocking(lambda: file_digest(stem))
        cached = cache_root / "analysis" / f"{digest[:20]}-drums-{drums.VERSION}.json"
        if cached.exists():
            hits: list[dict[str, Any]] = json.loads(cached.read_text(encoding="utf-8"))
            return hits

        def run() -> list[dict[str, Any]]:
            audio, rate = read_wav(str(stem))
            return drums.find_hits(audio, rate, model)

        hits = await run_blocking(run)
        cached.parent.mkdir(parents=True, exist_ok=True)
        cached.write_text(json.dumps(hits) + "\n", encoding="utf-8")
        return hits

    async def make_samples(self, params: dict[str, Any], notify: Notify) -> Any:
        """Samples from a stem: {file, drums (true for a drum stem), beats, firstDownbeat,
        beatsPerBar, cacheFolder}. Drums: {samples: [{pad, kind, start, end}], notes} (the clearest hit of
        each drum, for the Drum Machine's pads); otherwise {samples: [{key, start, end, tick, length}], notes}
        (phrases for the Sampler, one key each). Times in seconds of the stem, notes in ticks."""
        if importlib.util.find_spec("numpy") is None:
            raise RpcError(-32000, "Making samples needs NumPy: install the python-numpy package.")
        stem = Path(str(params.get("file", "")))
        if not stem.is_file():
            raise RpcError(-32602, f"There is no audio file at {stem}.")
        beats = [float(b) for b in params.get("beats", [])]
        first_downbeat = float(params.get("firstDownbeat", beats[0] if beats else 0.0))
        beats_per_bar = max(1, int(params.get("beatsPerBar", 4)))
        cache_root = Path(str(params.get("cacheFolder", ""))) if params.get("cacheFolder") else self.config / "cache"

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            from . import drums, samples
            from .compute.worker import read_wav

            if params.get("drums"):
                found = await self.drum_hits(stem, cache_root, say)
                audio, rate = await run_blocking(lambda: read_wav(str(stem)))
                hits, _ = await run_blocking(lambda: drums.group_hits(audio, rate, found))
                chosen = samples.choose_drum_samples(hits, audio.shape[-1] / rate)
                return {"samples": chosen, "notes": drums.place_hits(hits, beats, first_downbeat)}
            await say({"kind": "progress", "done": 0.3, "total": 1.0, "step": "Finding the phrases"})

            def run() -> list[dict[str, Any]]:
                audio, rate = read_wav(str(stem))
                return samples.find_phrases(audio, rate, beats, first_downbeat, beats_per_bar)

            phrases = await run_blocking(run)
            return {"samples": phrases, "notes": samples.phrase_notes(phrases)}

        return {"job": self.job(notify, work)}

    # -----------------------------------------------------------------------------------------------
    # Closest preset: an index per instrument of how its presets sound (CLAP vectors and plain
    # measurements of short recordings Spinus makes), and the presets nearest a stem.

    def clap_model(self) -> Any:
        model = find_model("clap_music")
        assert model is not None
        if not self.weights.is_ready(model):
            raise RpcError(-32000, f"{model.name} is not downloaded yet ({model.size / 1e6:.0f} MB).")
        return model

    def preset_index(self, library: str) -> Any:
        from .presets import PresetIndex

        if not library or not all(c.isalnum() or c in "-_." for c in library):
            raise RpcError(-32602, "The preset library needs a plain name, such as surge-xt.")
        return PresetIndex(self.config / "cache" / "presets" / f"{library}-clap_music.json")

    async def presets_missing(self, params: dict[str, Any], notify: Notify) -> Any:
        """Which presets have no measurements yet: {library, presets: [{id, version}]} -> {missing: [id]}."""
        index = self.preset_index(str(params.get("library", "")))
        return {"missing": index.missing(list(params.get("presets", [])))}

    async def embed(self, clips: list[dict[str, Any]], say: Callable[[dict[str, Any]], Awaitable[None]],
                    step: str) -> list[list[float]]:  # fmt: skip
        model = self.clap_model()

        async def progress(update: dict[str, Any]) -> None:
            await say({"kind": "progress", "done": float(update.get("done", 0.0)), "total": 1.0, "step": step})

        try:
            found = await self.engine.call(
                "embed", {"model": str(self.weights.folder(model)), "clips": clips}, progress
            )
        except EngineError as error:
            if "transformers" in str(error):  # installed before closest preset existed
                raise RuntimeError(
                    "The analysis engine needs an update for this: Options > AI > Analysis engine, Update."
                ) from error
            raise RuntimeError(str(error)) from error
        vectors: list[list[float]] = found["embeddings"]
        return vectors

    async def presets_add(self, params: dict[str, Any], notify: Notify) -> Any:
        """Measures preset recordings and keeps the numbers: {library, previews: [{id, version, file}],
        oneShot, keepFiles}. The recordings (WAV files Spinus made for this) are deleted afterwards, unless
        keepFiles (sample files of the user's own). oneShot: a sample, heard over its first second (drum
        samples for the Drum Machine) rather than the three-note preview phrase."""
        library = str(params.get("library", ""))
        index = self.preset_index(library)
        previews = [p for p in params.get("previews", []) if Path(str(p.get("file", ""))).is_file()]
        one_shot = bool(params.get("oneShot", False))  # a drum sample: its first second, one vector
        keep = bool(params.get("keepFiles", False))  # the user's own files: never deleted
        self.clap_model()

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            from .compute.worker import read_wav
            from .presets import NOTE_STARTS, describe_sound

            starts = (0.0,) if one_shot else NOTE_STARTS
            clips = [{"file": str(p["file"]), "start": t, "end": t + 1.0} for p in previews for t in starts]
            vectors = await self.embed(
                clips, say, "Listening to the sounds" if one_shot else "Listening to the presets"
            )

            def measure() -> list[list[float]]:
                result = []
                for p in previews:
                    audio, rate = read_wav(str(p["file"]))
                    mono = audio.mean(axis=0)
                    result.append(describe_sound(mono[:rate] if one_shot else mono, rate))
                return result

            features = await run_blocking(measure)
            notes = len(starts)
            for i, (p, measured) in enumerate(zip(previews, features, strict=True)):
                index.add(str(p["id"]), str(p.get("version", "")), vectors[i * notes : (i + 1) * notes], measured)
            await run_blocking(index.save)
            if not keep:
                for p in previews:
                    Path(str(p["file"])).unlink(missing_ok=True)
            return {"added": len(previews), "total": len(index.entries)}

        return {"job": self.job(notify, work)}

    async def presets_match(self, params: dict[str, Any], notify: Notify) -> Any:
        """The presets that sound most like a stem: {library, file, top (5), only ([id], optional), start and
        end (seconds, optional: just that part, such as one drum hit)} -> {presets: [{id, score}]}."""
        index = self.preset_index(str(params.get("library", "")))
        stem = Path(str(params.get("file", "")))
        if not stem.is_file():
            raise RpcError(-32602, f"There is no audio file at {stem}.")
        if not index.entries:
            raise RpcError(-32000, "No presets have been measured yet.")
        self.clap_model()
        top = max(1, min(50, int(params.get("top", 5))))
        only = [str(i) for i in params["only"]] if isinstance(params.get("only"), list) else None

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            import numpy as np

            from .compute.worker import read_wav
            from .presets import describe_sound, stem_clips, windows

            audio, rate = await run_blocking(lambda: read_wav(str(stem)))
            clips = [(float(params["start"]), float(params["end"]))] if "start" in params else stem_clips(audio, rate)
            vectors = await self.embed([{"file": str(stem), "start": a, "end": b} for a, b in windows(clips)], say,
                                       "Listening to the stem")  # fmt: skip
            mono = audio.mean(axis=0)
            joined = np.concatenate([mono[int(a * rate) : int(b * rate)] for a, b in clips])
            features = describe_sound(joined, rate)
            return {"presets": index.closest(vectors, features, top, only)}

        return {"job": self.job(notify, work)}

    # -----------------------------------------------------------------------------------------------
    # Automixer: measuring each insert from recordings Spinus makes.

    async def mix_analyze(self, params: dict[str, Any], notify: Notify) -> Any:
        """Every insert's loudness, peaks, tone and width, and the masking between them: {inserts: [{name,
        file (a WAV recording of that insert)}]} -> {inserts: {name: {...}}, masking: [{a, b, band,
        amount}]} (spinus_ai.mixing). Needs NumPy only."""
        if importlib.util.find_spec("numpy") is None:
            raise RpcError(-32000, "The automixer needs NumPy: install the python-numpy package.")
        inserts = [(str(i.get("name", "")), Path(str(i.get("file", "")))) for i in params.get("inserts", [])]
        for name, file in inserts:
            if not name or not file.is_file():
                raise RpcError(-32602, f"There is no recording of {name or 'an insert'} at {file}.")

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            from .compute.worker import read_wav
            from .mixing import analyze_mix

            loaded = {}
            for i, (name, file) in enumerate(inserts):
                await say({"kind": "progress", "done": i / max(1, len(inserts)), "total": 1.0,
                           "step": f"Listening to {name}"})  # fmt: skip
                loaded[name] = await run_blocking(functools.partial(read_wav, str(file)))
            return await run_blocking(lambda: analyze_mix(loaded))

        return {"job": self.job(notify, work)}

    # -----------------------------------------------------------------------------------------------
    # Track creator: the style of reference songs (spinus_ai.style).

    async def style_summarize(self, params: dict[str, Any], notify: Notify) -> Any:
        """What reference songs share: {songs: [song]}, each song either {features} (from an earlier result's
        perSong) or the Analyzer's results for it ({name, file (the mix as WAV, for the sections), tempo,
        beatsPerBar, firstDownbeat, beats, downbeats, key, loudness, parts, drums, stems {name: WAV file}};
        spinus_ai.style.analyze_song).
        The result is the style summary, with each song's features in perSong (kept by Spinus, so a song
        is measured once). Needs NumPy only."""
        if importlib.util.find_spec("numpy") is None:
            raise RpcError(-32000, "The Track creator needs NumPy: install the python-numpy package.")
        songs = params.get("songs")
        if not isinstance(songs, list) or not songs or not all(isinstance(s, dict) for s in songs):
            raise RpcError(-32602, "songs must be a list of songs.")
        for song in songs:
            file = song.get("file")
            if file and not Path(str(file)).is_file():
                raise RpcError(-32602, f"There is no audio file at {file}.")

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            from . import style
            from .compute.worker import read_wav

            features = []
            for i, song in enumerate(songs):
                if isinstance(song.get("features"), dict):
                    found = song["features"]
                    if found.get("version") != style.FEATURES_VERSION:
                        raise RuntimeError(f"{found.get('name', 'A song')} was measured by an older Spinus: "
                                           "remove it and add it again.")  # fmt: skip
                    features.append(found)
                    continue
                await say({"kind": "progress", "done": i / len(songs), "total": 1.0,
                           "step": f"Measuring the style of {song.get('name', 'a song')}"})  # fmt: skip

                def measure(song: dict[str, Any] = song) -> dict[str, Any]:
                    # The drums and the harmonic stems, for the drums' grid, the key and the chords.
                    wanted = ("drums", *style.HARMONIC)
                    stems = {
                        name: read_wav(str(path))
                        for name, path in (song.get("stems") or {}).items()
                        if name in wanted and Path(str(path)).is_file()
                    }
                    if song.get("file"):
                        audio, rate = read_wav(str(song["file"]))
                        return style.analyze_song(song, audio, rate, stems)
                    return style.analyze_song(song, stems=stems)

                features.append(await run_blocking(measure))
            return await run_blocking(lambda: style.summarize(features))

        return {"job": self.job(notify, work)}

    async def song_write(self, params: dict[str, Any], notify: Notify) -> Any:
        """A new song in the style of a summary (spinus_ai.songwriter): {summary (ai.v1.style.summarize's
        result), seed, refine (let the AI vary the plan; default false), song and redo ({part} or {section}):
        that song with one part or section written anew from the seed} -> the song. Rules only, with no model
        and no AI connection; with refine and a connection, the AI may vary the plan (checked), the rules still
        write every note, and without one (or when it fails) the rules' song stands, with a note saying so."""
        from . import songwriter

        summary = params.get("summary")
        if not isinstance(summary, dict) or summary.get("kind") != "spinus.style-summary":
            raise RpcError(-32602, "summary must be the Track creator's style summary.")
        try:
            seed = int(params.get("seed", 0)) & 0x7FFFFFFF
        except (TypeError, ValueError) as error:
            raise RpcError(-32602, "seed must be a number.") from error
        redo = params.get("redo")
        if redo is not None and (not isinstance(redo, dict) or not (redo.get("part") or redo.get("section"))):
            raise RpcError(-32602, "redo names a part or a section.")
        if redo is not None:
            try:
                songwriter.check_song(params.get("song"))
            except ValueError as error:
                raise RpcError(-32602, str(error)) from error

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            if redo is not None:
                part, section = redo.get("part"), redo.get("section")
                return songwriter.redo(dict(params["song"]), summary, seed,
                                       str(part) if part else None, str(section) if section else None)  # fmt: skip
            song = songwriter.write(summary, seed)
            if not params.get("refine"):
                return song
            try:
                return await songwriter.refine(self.client, self.connections, self.routing, self.usage, summary,
                                               song, say)  # fmt: skip
            except NoConnection:
                song["note"] = "Written by the rules only: no AI connection is set up."
            except ProviderError as error:
                song["note"] = f"Written by the rules only: the AI failed ({error})."
            song["plain"] = songwriter.describe(song)
            return song

        return {"job": self.job(notify, work)}

    async def mix_plan(self, params: dict[str, Any], notify: Notify) -> Any:
        """Changes to the mix, each with a reason: {mix (from ai.v1.mix.analyze), master (the whole mix's
        measurements, optional), target ("streaming", "club" or "none"), reference (a song's measurements
        whose tonal balance to match, optional), review (ask the AI to review;
        default true)} -> {items, reviewedBy (the model, or ""), note}. The rules' plan stands when no AI
        connection is set up or it fails (spinus_ai.mixplan)."""
        from .mixplan import TARGETS, check, review, rules

        mix = dict(params.get("mix") or {})
        master = params.get("master") if isinstance(params.get("master"), dict) else None
        reference = params.get("reference") if isinstance(params.get("reference"), dict) else None
        target = str(params.get("target", "none"))
        if target not in TARGETS and target != "none":
            raise RpcError(-32602, "The loudness target is streaming, club or none.")
        names = set(mix.get("inserts", {})) | {"Master"}

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            planned = [c for i in rules(mix, master, target, reference) if (c := check(i, names)) is not None]
            if not params.get("review", True) or not planned:
                return {"items": planned, "reviewedBy": "", "note": ""}
            try:
                reviewed, model = await review(
                    self.client, self.connections, self.routing, self.usage, mix, master, target, planned, say
                )
            except NoConnection:
                return {"items": planned, "reviewedBy": "", "note": "Made by the rules only: no AI connection."}
            except ProviderError as error:
                return {"items": planned, "reviewedBy": "", "note": f"Made by the rules only: the AI failed ({error})."}
            if not reviewed:
                return {"items": planned, "reviewedBy": "", "note": "The AI's review was not usable; the rules' plan."}
            return {"items": reviewed, "reviewedBy": model, "note": ""}

        return {"job": self.job(notify, work)}

    async def mix_toward(self, params: dict[str, Any], notify: Notify) -> Any:
        """Effects and levels moving the Track creator's chosen sounds toward the reference songs: {mix (from
        ai.v1.mix.analyze: each part's sound as "<part>" and its stem as "<part> reference"), parts ([part]),
        referenceLoudness (LUFS, optional)} -> {items, estimate} (spinus_ai.mixplan.toward_references). Rules
        only: no AI connection is needed."""
        from .mixplan import toward_references

        mix = params.get("mix")
        parts = params.get("parts")
        if not isinstance(mix, dict) or not isinstance(parts, list):
            raise RpcError(-32602, "mix must be ai.v1.mix.analyze's result and parts a list of names.")
        loudness = params.get("referenceLoudness")
        target = float(loudness) if isinstance(loudness, int | float) else None
        return toward_references(mix, [str(p) for p in parts], target)

    # -----------------------------------------------------------------------------------------------
    # Listening: a take's notes or beatbox hits. Results are cached by the take's contents.

    def taught_sounds(self) -> dict[str, list[list[float]]]:
        file = self.config / "beatbox.json"
        data: dict[str, list[list[float]]] = json.loads(file.read_text(encoding="utf-8")) if file.exists() else {}
        return data

    def take_of(self, params: dict[str, Any]) -> Path:
        if importlib.util.find_spec("numpy") is None:
            raise RpcError(-32000, "Listening needs NumPy: install the python-numpy package.")
        take = Path(str(params.get("file", "")))
        if not take.is_file():
            raise RpcError(-32602, f"There is no recording at {take}.")
        return take

    async def cached(self, take: Path, kind: str, extra: str, analyse: Callable[[], dict[str, Any]]) -> Any:
        from . import listening

        digest = hashlib.sha256(take.read_bytes() + extra.encode()).hexdigest()[:24]
        file = self.config / "cache" / "listen" / f"{digest}-{kind}-v{listening.VERSION}.json"
        if file.exists():
            return json.loads(file.read_text(encoding="utf-8"))
        result = await run_blocking(analyse)
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(json.dumps(result), encoding="utf-8")
        return result

    async def listen_melody(self, params: dict[str, Any], notify: Notify) -> Any:
        """The notes sung, hummed or played in a take (seconds and fractional MIDI pitch), and its tempo."""
        take = self.take_of(params)

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            from . import listening

            return await self.cached(take, "melody", "", lambda: listening.analyse_melody(take))

        return {"job": self.job(notify, work)}

    async def listen_vocal(self, params: dict[str, Any], notify: Notify) -> Any:
        """A vocal's pitch as sung and its notes, for Spinus Tune's editor."""
        take = self.take_of(params)

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            from . import listening

            return await self.cached(take, "vocal", "", lambda: listening.analyse_vocal(take))

        return {"job": self.job(notify, work)}

    async def listen_beats(self, params: dict[str, Any], notify: Notify) -> Any:
        """The beatboxed hits in a take: when, and which sound (kick, snare or hat)."""
        take = self.take_of(params)
        taught = self.taught_sounds()

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            from . import listening

            return await self.cached(
                take, "beats", json.dumps(taught, sort_keys=True), lambda: listening.analyse_beats(take, taught)
            )

        return {"job": self.job(notify, work)}

    async def listen_teach(self, params: dict[str, Any], notify: Notify) -> Any:
        """Learns one beatbox sound from a take of it repeated (replacing what was taught for it)."""
        sound = str(params.get("sound", ""))
        if sound not in BEATBOX_SOUNDS:
            raise RpcError(-32602, f"Unknown sound '{sound}': use kick, snare or hat.")
        take = self.take_of(params)
        from . import listening

        async def work(say: Callable[[dict[str, Any]], Awaitable[None]]) -> Any:
            examples = await run_blocking(lambda: listening.learn_sound(take))
            if len(examples) < 2:
                raise ValueError(f"Heard {len(examples)} {sound} in the recording: make it at least 3 times, apart.")
            taught = self.taught_sounds()
            taught[sound] = examples
            (self.config / "beatbox.json").parent.mkdir(parents=True, exist_ok=True)
            (self.config / "beatbox.json").write_text(json.dumps(taught), encoding="utf-8")
            return {"sound": sound, "learned": len(examples), **(await self.listen_sounds({}, notify))}

        return {"job": self.job(notify, work)}

    async def listen_sounds(self, params: dict[str, Any], notify: Notify) -> Any:
        taught = self.taught_sounds()
        return {
            "sounds": {sound: len(taught.get(sound, [])) for sound in BEATBOX_SOUNDS},
            "missing": [] if importlib.util.find_spec("numpy") is not None else ["python-numpy"],
        }

    async def listen_forget(self, params: dict[str, Any], notify: Notify) -> Any:
        """Forgets the taught beatbox sounds."""
        (self.config / "beatbox.json").unlink(missing_ok=True)
        return await self.listen_sounds({}, notify)

    async def assistant_ask(self, params: dict[str, Any], notify: Notify) -> Any:
        job = uuid.uuid4().hex[:12]

        async def say(update: dict[str, Any]) -> None:
            await notify("ai.v1.job.update", {"job": job, **update})

        async def run() -> None:
            try:
                answer = await ask(
                    self.client,
                    self.connections,
                    self.routing,
                    self.usage,
                    self.manual,
                    self.commands,
                    list(params.get("conversation", [])),
                    dict(params.get("context", {})),
                    say,
                )
                await say({"kind": "done", "result": answer.to_json()})
            except asyncio.CancelledError:
                await say({"kind": "error", "message": "Stopped."})
            except (NoConnection, ProviderError, ValueError) as error:
                await say({"kind": "error", "message": str(error)})
            except Exception as error:
                await say({"kind": "error", "message": f"Something went wrong in the assistant: {error}"})
            finally:
                self.jobs.pop(job, None)

        self.jobs[job] = asyncio.create_task(run())
        return {"job": job}

    async def cancel_job(self, params: dict[str, Any], notify: Notify) -> Any:
        task = self.jobs.get(str(params.get("job", "")))
        if task is None:
            return {"cancelled": False}
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task
        return {"cancelled": True}
