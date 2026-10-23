"""Local embedding models for manual search: run with ONNX Runtime, downloaded on request.

ONNX Runtime and NumPy are optional: without them `missing_dependencies()` says what to install and the
manual is searched by keywords only."""

from __future__ import annotations

import asyncio
import contextlib
import hashlib
import importlib.util
import json
import os
import re
import shutil
import unicodedata
from collections.abc import Awaitable, Callable
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Protocol

import httpx

QUERY_INSTRUCTION_BGE = "Represent this sentence for searching relevant passages: "


@dataclass
class ModelFile:
    path: str  # in the Hugging Face repository
    save: str  # in the model's folder
    size: int = 0
    sha256: str = ""  # empty: not pinned (own models)
    optional: bool = False  # missing in the repository is fine


@dataclass
class ModelSpec:
    id: str
    name: str
    description: str
    license: str
    languages: str
    repo: str = ""  # Hugging Face repository
    revision: str = ""
    files: list[ModelFile] = field(default_factory=list)
    pooling: str = "mean"  # "mean" or "cls"
    query_prefix: str = ""
    document_prefix: str = ""
    tokenizer: str = "wordpiece"  # "wordpiece" (vocab.txt, built in) or "tokenizers" (tokenizer.json, library)
    folder: str = ""  # own models used where they are
    pinned: bool = True

    @property
    def size(self) -> int:
        return sum(f.size for f in self.files)


def _bge(model: str, revision: str, onnx: tuple[int, str], pooling_sha: str, description: str) -> ModelSpec:
    return ModelSpec(
        id=model.lower(),
        name=model,
        description=description,
        license="MIT",
        languages="English",
        repo=f"BAAI/{model}",
        revision=revision,
        files=[
            ModelFile("onnx/model.onnx", "model.onnx", onnx[0], onnx[1]),
            ModelFile(
                "vocab.txt", "vocab.txt", 231508, "07eced375cec144d27c900241f3e339478dec958f92fddbc551f295c992038a3"
            ),
            ModelFile("1_Pooling/config.json", "pooling.json", 190, pooling_sha),
        ],
        pooling="cls",
        query_prefix=QUERY_INSTRUCTION_BGE,
    )


# Pinned to a Hugging Face commit; each file's SHA-256 checked after download.
CATALOG: list[ModelSpec] = [
    _bge(
        "bge-small-en-v1.5",
        "5c38ec7c405ec4b44b94cc5a9bb96e735b38267a",
        (133093490, "828e1496d7fabb79cfa4dcd84fa38625c0d3d21da474a00f08db0f559940cf35"),
        "d1caf60c96f5fba2157c0c26b76d80818fad6cf0b8eb5e73ec372ff9818eba5c",
        "Recommended: good answers, quick on any computer.",
    ),
    ModelSpec(
        id="all-minilm-l6-v2",
        name="all-MiniLM-L6-v2",
        description="The smallest and fastest.",
        license="Apache-2.0",
        languages="English",
        repo="sentence-transformers/all-MiniLM-L6-v2",
        revision="1110a243fdf4706b3f48f1d95db1a4f5529b4d41",
        files=[
            ModelFile(
                "onnx/model.onnx",
                "model.onnx",
                90405214,
                "6fd5d72fe4589f189f8ebc006442dbb529bb7ce38f8082112682524616046452",
            ),
            ModelFile(
                "vocab.txt", "vocab.txt", 231508, "07eced375cec144d27c900241f3e339478dec958f92fddbc551f295c992038a3"
            ),
            ModelFile(
                "1_Pooling/config.json",
                "pooling.json",
                190,
                "4be450dde3b0273bb9787637cfbd28fe04a7ba6ab9d36ac48e92b11e350ffc23",
            ),
        ],
    ),
    _bge(
        "bge-base-en-v1.5",
        "a5beb1e3e68b9ab74eb54cfd186867f64f240e1a",
        (435811539, "9bc579acdba21c253c62a9bf866891355a63ffa3442b52c8a37d75b2ccb91848"),
        "c9bef85e8bbf4b2eab4941b3fb62bd33f88686748b478f2e264d256472d9643b",
        "The best English answers; larger and slower.",
    ),
    ModelSpec(
        id="multilingual-e5-small",
        name="multilingual-e5-small",
        description="For questions in about 100 languages. Needs the tokenizers library (AUR: python-tokenizers).",
        license="MIT",
        languages="About 100",
        repo="intfloat/multilingual-e5-small",
        revision="614241f622f53c4eeff9890bdc4f31cfecc418b3",
        files=[
            ModelFile(
                "onnx/model.onnx",
                "model.onnx",
                470268510,
                "ca456c06b3a9505ddfd9131408916dd79290368331e7d76bb621f1cba6bc8665",
            ),
            ModelFile(
                "tokenizer.json",
                "tokenizer.json",
                17082730,
                "0b44a9d7b51c3c62626640cda0e2c2f70fdacdc25bbbd68038369d14ebdf4c39",
            ),
            ModelFile(
                "1_Pooling/config.json",
                "pooling.json",
                200,
                "987f7a67a38fa564c849bb5d277c52ab9088a84368fc0be31a354125aebb12a0",
            ),
        ],
        query_prefix="query: ",
        document_prefix="passage: ",
        tokenizer="tokenizers",
    ),
]


def missing_dependencies(spec: ModelSpec | None = None) -> list[str]:
    """Arch packages to install before embedding models can run."""
    missing = []
    if importlib.util.find_spec("onnxruntime") is None:
        missing.append("python-onnxruntime-cpu")
    if importlib.util.find_spec("numpy") is None:
        missing.append("python-numpy")
    if spec is not None and spec.tokenizer == "tokenizers" and importlib.util.find_spec("tokenizers") is None:
        missing.append("python-tokenizers (AUR)")
    return missing


# ---------------------------------------------------------------------------------------------------
# Tokenizers


class Tokenizer(Protocol):
    def encode(self, text: str, max_length: int) -> list[int]: ...


def _is_punctuation(char: str) -> bool:
    code = ord(char)
    if 33 <= code <= 47 or 58 <= code <= 64 or 91 <= code <= 96 or 123 <= code <= 126:
        return True
    return unicodedata.category(char).startswith("P")


def _is_cjk(code: int) -> bool:
    return (
        0x4E00 <= code <= 0x9FFF
        or 0x3400 <= code <= 0x4DBF
        or 0x20000 <= code <= 0x2A6DF
        or 0x2A700 <= code <= 0x2CEAF
        or 0xF900 <= code <= 0xFAFF
        or 0x2F800 <= code <= 0x2FA1F
    )


class WordPiece:
    """BERT's uncased WordPiece tokenizer (vocab.txt): lower case, accents removed, punctuation split."""

    def __init__(self, vocab_file: Path, lowercase: bool = True) -> None:
        self.vocab = {t: i for i, t in enumerate(vocab_file.read_text(encoding="utf-8").splitlines())}
        self.lowercase = lowercase
        self.unk, self.cls, self.sep = self.vocab["[UNK]"], self.vocab["[CLS]"], self.vocab["[SEP]"]

    def basic(self, text: str) -> list[str]:
        cleaned = []
        for ch in text:
            code = ord(ch)
            if code in (0, 0xFFFD) or (unicodedata.category(ch).startswith("C") and ch not in "\t\n\r"):
                continue
            cleaned.append(f" {ch} " if _is_cjk(code) else (" " if ch.isspace() else ch))
        words: list[str] = []
        for word in "".join(cleaned).split():
            if self.lowercase:
                word = "".join(c for c in unicodedata.normalize("NFD", word.lower()) if unicodedata.category(c) != "Mn")
            current = ""
            for ch in word:
                if _is_punctuation(ch):
                    if current:
                        words.append(current)
                    words.append(ch)
                    current = ""
                else:
                    current += ch
            if current:
                words.append(current)
        return words

    def wordpiece(self, word: str) -> list[int]:
        if len(word) > 100:
            return [self.unk]
        ids, start = [], 0
        while start < len(word):
            end, found = len(word), None
            while start < end:
                piece = ("##" if start > 0 else "") + word[start:end]
                if piece in self.vocab:
                    found = self.vocab[piece]
                    break
                end -= 1
            if found is None:
                return [self.unk]
            ids.append(found)
            start = end
        return ids

    def encode(self, text: str, max_length: int) -> list[int]:
        ids = [i for w in self.basic(text) for i in self.wordpiece(w)]
        return [self.cls, *ids[: max_length - 2], self.sep]


class LibraryTokenizer:
    """Hugging Face's tokenizers library with the model's tokenizer.json."""

    def __init__(self, tokenizer_file: Path) -> None:
        from tokenizers import Tokenizer as HfTokenizer

        self.tokenizer = HfTokenizer.from_file(str(tokenizer_file))
        self.tokenizer.no_truncation()
        self.tokenizer.no_padding()

    def encode(self, text: str, max_length: int) -> list[int]:
        ids: list[int] = self.tokenizer.encode(text).ids
        if len(ids) > max_length:
            ids = [*ids[: max_length - 1], ids[-1]]
        return ids


# ---------------------------------------------------------------------------------------------------
# Running a model


class EmbeddingModel:
    MAX_TOKENS = 512

    def __init__(self, spec: ModelSpec, folder: Path) -> None:
        import numpy as np
        import onnxruntime as ort

        self.np = np
        self.spec = spec
        model = folder / "model.onnx"
        if not model.exists():
            model = folder / "onnx" / "model.onnx"
        options = ort.SessionOptions()
        options.intra_op_num_threads = 4  # leave cores for the audio engine
        self.session = ort.InferenceSession(str(model), options, providers=["CPUExecutionProvider"])
        self.inputs = {i.name for i in self.session.get_inputs()}
        self.tokenizer: Tokenizer
        if spec.tokenizer == "tokenizers" or not (folder / "vocab.txt").exists():
            self.tokenizer = LibraryTokenizer(folder / "tokenizer.json")
        else:
            self.tokenizer = WordPiece(folder / "vocab.txt")

    def embed(self, texts: list[str], document: bool) -> Any:
        """Unit vectors, one row per text."""
        np = self.np
        prefix = self.spec.document_prefix if document else self.spec.query_prefix
        rows = []
        for start in range(0, len(texts), 16):
            batch = [self.tokenizer.encode(prefix + t, self.MAX_TOKENS) for t in texts[start : start + 16]]
            width = max(len(ids) for ids in batch)
            ids = np.zeros((len(batch), width), dtype=np.int64)
            mask = np.zeros((len(batch), width), dtype=np.int64)
            for r, row in enumerate(batch):
                ids[r, : len(row)] = row
                mask[r, : len(row)] = 1
            feed = {"input_ids": ids, "attention_mask": mask}
            if "token_type_ids" in self.inputs:
                feed["token_type_ids"] = np.zeros_like(ids)
            hidden = self.session.run(None, {k: v for k, v in feed.items() if k in self.inputs})[0]
            if self.spec.pooling == "cls":
                vectors = hidden[:, 0, :]
            else:
                weights = mask[:, :, None].astype(np.float32)
                vectors = (hidden * weights).sum(axis=1) / np.maximum(weights.sum(axis=1), 1e-9)
            rows.append(vectors / np.maximum(np.linalg.norm(vectors, axis=1, keepdims=True), 1e-12))
        return np.concatenate(rows, axis=0)


# ---------------------------------------------------------------------------------------------------
# The models the user has, and downloads

Progress = Callable[[int, int], Awaitable[None]]  # done, total bytes

HF_LINK = re.compile(r"^https?://huggingface\.co/([\w.\-]+/[\w.\-]+)(?:/tree/([\w.\-]+))?/?$")


def default_models_folder() -> Path:
    """Where downloaded models go: ~/.local/share/spinus/models, or under $XDG_DATA_HOME."""
    data = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(data) / "spinus" / "models"


class ModelStore:
    def __init__(self, config: Path, models: Path | None = None) -> None:
        self.folder = models or default_models_folder()
        self.file = config / "embeddings.json"
        self.move_old_downloads(config / "models")
        self.cache = config / "cache"
        data: dict[str, Any] = json.loads(self.file.read_text(encoding="utf-8")) if self.file.exists() else {}
        self.active: str = data.get("active", "")
        self.own: list[ModelSpec] = []
        for m in data.get("own", []):
            files = [ModelFile(**f) for f in m.pop("files", [])]
            self.own.append(ModelSpec(**m, files=files))

    def move_old_downloads(self, old: Path) -> None:
        """Models downloaded before they moved to the shared folder (they were in the settings) come along."""
        if not old.is_dir() or old.resolve() == self.folder.resolve():
            return
        self.folder.mkdir(parents=True, exist_ok=True)
        for item in old.iterdir():
            if not (self.folder / item.name).exists():
                shutil.move(str(item), str(self.folder / item.name))
        with contextlib.suppress(OSError):
            old.rmdir()  # only when everything moved

    def save(self) -> None:
        self.file.parent.mkdir(parents=True, exist_ok=True)
        data = {"active": self.active, "own": [asdict(m) for m in self.own]}
        self.file.write_text(json.dumps(data, indent=2), encoding="utf-8")

    def all(self) -> list[ModelSpec]:
        return [*CATALOG, *self.own]

    def find(self, model_id: str) -> ModelSpec | None:
        return next((m for m in self.all() if m.id == model_id), None)

    def folder_of(self, spec: ModelSpec) -> Path:
        return Path(spec.folder) if spec.folder else self.folder / spec.id

    def is_ready(self, spec: ModelSpec) -> bool:
        folder = self.folder_of(spec)
        has_model = (folder / "model.onnx").exists() or (folder / "onnx" / "model.onnx").exists()
        has_tokenizer = (folder / "vocab.txt").exists() or (folder / "tokenizer.json").exists()
        return has_model and has_tokenizer and not (folder / ".partial").exists()

    def describe(self) -> list[dict[str, Any]]:
        return [
            {
                "id": m.id,
                "name": m.name,
                "description": m.description,
                "license": m.license,
                "languages": m.languages,
                "size": m.size,
                "downloaded": self.is_ready(m),
                "own": m in self.own,
                "pinned": m.pinned,
                "active": m.id == self.active,
                "missing": missing_dependencies(m),
                "source": m.folder or (f"https://huggingface.co/{m.repo}" if m.repo else ""),
            }
            for m in self.all()
        ]

    async def download(self, client: httpx.AsyncClient, spec: ModelSpec, progress: Progress) -> None:
        """Every file into the model's folder, checked by SHA-256 when pinned. Nothing is kept if it fails."""
        target = self.folder_of(spec)
        target.mkdir(parents=True, exist_ok=True)
        (target / ".partial").touch()
        total, done = spec.size, 0
        try:
            for f in spec.files:
                url = f"https://huggingface.co/{spec.repo}/resolve/{spec.revision or 'main'}/{f.path}"
                destination = target / f.save
                digest = hashlib.sha256()
                async with client.stream("GET", url, follow_redirects=True) as response:
                    if response.status_code == 404 and f.optional:
                        continue
                    if response.status_code != 200:
                        raise RuntimeError(f"Could not download {f.path} ({response.status_code}).")
                    with destination.open("wb") as out:
                        async for chunk in response.aiter_bytes(1 << 20):
                            out.write(chunk)
                            digest.update(chunk)
                            done += len(chunk)
                            await progress(done, max(total, done))
                if f.sha256 and digest.hexdigest() != f.sha256:
                    raise RuntimeError(f"{f.path} did not match its checksum: download it again.")
                if not f.sha256:
                    f.sha256, f.size = digest.hexdigest(), destination.stat().st_size
            if not ((target / "vocab.txt").exists() or (target / "tokenizer.json").exists()):
                raise RuntimeError(
                    "The model has neither vocab.txt nor tokenizer.json, so Spinus cannot read text for it."
                )
            (target / ".partial").unlink()
            if spec in self.own:
                spec.tokenizer = "wordpiece" if (target / "vocab.txt").exists() else "tokenizers"
                spec.pooling = self.read_pooling(target)
                self.save()
        except BaseException:
            shutil.rmtree(target, ignore_errors=True)
            raise

    def add_folder(self, folder: Path, name: str) -> ModelSpec:
        has_model = (folder / "model.onnx").exists() or (folder / "onnx" / "model.onnx").exists()
        if not has_model:
            raise ValueError("The folder needs model.onnx (or onnx/model.onnx).")
        has_vocab = (folder / "vocab.txt").exists()
        if not has_vocab and not (folder / "tokenizer.json").exists():
            raise ValueError("The folder needs vocab.txt or tokenizer.json.")
        spec = ModelSpec(
            id="own-" + hashlib.sha256(str(folder.resolve()).encode()).hexdigest()[:8],  # the same every run
            name=name or folder.name,
            description=f"Your model in {folder}.",
            license="your responsibility",
            languages="?",
            folder=str(folder),
            pinned=False,
            tokenizer="wordpiece" if has_vocab else "tokenizers",
            pooling=self.read_pooling(folder),
        )
        self.own = [m for m in self.own if m.id != spec.id] + [spec]
        self.save()
        return spec

    def from_hugging_face(self, link: str) -> ModelSpec:
        match = HF_LINK.match(link.strip())
        if match is None:
            raise ValueError("Use a Hugging Face model link, like https://huggingface.co/BAAI/bge-small-en-v1.5")
        repo, revision = match.group(1), match.group(2) or "main"
        known = next((m for m in CATALOG if m.repo == repo), None)
        if known is not None:
            return known
        spec = ModelSpec(
            id="hf-" + repo.replace("/", "--").lower(),
            name=repo,
            description=f"From {link.strip()}.",
            license="see its page",
            languages="?",
            repo=repo,
            revision=revision,
            pinned=False,
            files=[
                ModelFile("onnx/model.onnx", "model.onnx"),
                ModelFile("vocab.txt", "vocab.txt", optional=True),
                ModelFile("tokenizer.json", "tokenizer.json", optional=True),
                ModelFile("1_Pooling/config.json", "pooling.json", optional=True),
            ],
        )
        self.own = [m for m in self.own if m.id != spec.id] + [spec]
        self.save()
        return spec

    @staticmethod
    def read_pooling(folder: Path) -> str:
        for name in ("pooling.json", "1_Pooling/config.json"):
            if (folder / name).exists():
                config = json.loads((folder / name).read_text(encoding="utf-8"))
                return "cls" if config.get("pooling_mode_cls_token") else "mean"
        return "mean"

    def remove(self, model_id: str) -> None:
        spec = self.find(model_id)
        if spec is None:
            return
        if not spec.folder:
            shutil.rmtree(self.folder_of(spec), ignore_errors=True)
        if spec in self.own:
            self.own.remove(spec)
        if self.active == model_id:
            self.active = ""
        self.save()

    def load(self, model_id: str) -> EmbeddingModel:
        spec = self.find(model_id)
        if spec is None or not self.is_ready(spec):
            raise ValueError("That model is not downloaded.")
        if missing := missing_dependencies(spec):
            raise ValueError("Install " + " and ".join(missing) + " to use local models.")
        if (self.folder_of(spec) / "pooling.json").exists() or spec.folder:
            spec.pooling = self.read_pooling(self.folder_of(spec))
        return EmbeddingModel(spec, self.folder_of(spec))


async def run_blocking[T](function: Callable[[], T]) -> T:
    return await asyncio.to_thread(function)
