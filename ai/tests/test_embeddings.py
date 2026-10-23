"""Local embedding models: the built-in tokenizer, the model store and downloads (offline), and
a real model by hand (SPINUS_TEST_EMBEDDINGS=1, downloads about 133 MB once)."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
from typing import Any

import httpx
import pytest

from spinus_ai.embeddings import CATALOG, ModelFile, ModelSpec, ModelStore, WordPiece
from spinus_ai.manual import Manual

ROOT = Path(__file__).resolve().parents[2]

VOCAB = [
    "[PAD]",
    "[UNK]",
    "[CLS]",
    "[SEP]",
    "[MASK]",
    "the",
    "bass",
    "dip",
    "##s",
    "on",
    "every",
    "kick",
    "side",
    "##chain",
    "make",
    "it",
    "cafe",
    "!",
    ",",
    ".",
    "'",
    "(",
    ")",
    "-",
    "un",
    "##known",
    "##able",
    "a",
    "b",
    "##c",
    "中",
    "文",
    "eq",
    "3",
    "##x",
    "os",
    "##c",
]


def test_wordpiece_matches_hugging_face_tokenizers(tmp_path: Path) -> None:
    from tokenizers import BertWordPieceTokenizer

    vocab = tmp_path / "vocab.txt"
    vocab.write_text("\n".join(VOCAB), encoding="utf-8")
    ours = WordPiece(vocab)
    theirs = BertWordPieceTokenizer(str(vocab), lowercase=True, strip_accents=True)
    for text in [
        "Make the bass DIP on every kick!",
        "Sidechain, the café (unknownable) dips.",
        "中文 EQ-3xOsc",
        "  spaces\tand\nlines  ",
        "Zzzzz unknownable's",
        "a" * 120,
    ]:
        assert ours.encode(text, 512) == theirs.encode(text).ids, text
    assert len(ours.encode("kick " * 600, 512)) == 512


def test_catalog_is_pinned_and_allows_commercial_use() -> None:
    for spec in CATALOG:
        assert spec.revision and len(spec.revision) == 40, spec.id
        assert spec.license in ("MIT", "Apache-2.0"), spec.id
        assert all(len(f.sha256) == 64 for f in spec.files), spec.id
        assert any(f.save == "model.onnx" for f in spec.files)


def fake_spec(content: dict[str, bytes], wrong: str = "") -> ModelSpec:
    files = [
        ModelFile(name, name.split("/")[-1], len(data), "0" * 64 if name == wrong else hashlib.sha256(data).hexdigest())
        for name, data in content.items()
    ]
    return ModelSpec("fake", "Fake", "", "MIT", "English", repo="org/fake", revision="r" * 40, files=files)


def client_serving(content: dict[str, bytes]) -> httpx.AsyncClient:
    def handler(request: httpx.Request) -> httpx.Response:
        name = request.url.path.split("/resolve/")[1].split("/", 1)[1]
        return httpx.Response(200, content=content[name]) if name in content else httpx.Response(404)

    return httpx.AsyncClient(transport=httpx.MockTransport(handler))


async def test_downloads_are_checked_and_failed_ones_leave_nothing(tmp_path: Path) -> None:
    content = {"onnx/model.onnx": b"model" * 100, "vocab.txt": b"\n".join(v.encode() for v in VOCAB)}
    store = ModelStore(tmp_path, tmp_path / "models")
    seen: list[tuple[int, int]] = []

    async def progress(done: int, total: int) -> None:
        seen.append((done, total))

    good = fake_spec(content)
    await store.download(client_serving(content), good, progress)
    assert store.is_ready(good)
    assert (tmp_path / "models/fake/model.onnx").read_bytes() == content["onnx/model.onnx"]
    assert seen[-1][0] == seen[-1][1] == sum(len(c) for c in content.values())

    store.remove("fake")
    bad = fake_spec(content, wrong="vocab.txt")
    with pytest.raises(RuntimeError, match="did not match its checksum"):
        await store.download(client_serving(content), bad, progress)
    assert not (tmp_path / "models/fake").exists()


async def test_own_models_from_a_link_or_a_folder(tmp_path: Path) -> None:
    store = ModelStore(tmp_path, tmp_path / "models")
    with pytest.raises(ValueError, match="Hugging Face model link"):
        store.from_hugging_face("https://example.com/model")
    assert store.from_hugging_face("https://huggingface.co/BAAI/bge-small-en-v1.5").id == "bge-small-en-v1.5"

    spec = store.from_hugging_face("https://huggingface.co/someone/tiny-embed/tree/v2")
    assert (spec.repo, spec.revision, spec.pinned) == ("someone/tiny-embed", "v2", False)
    content = {
        "onnx/model.onnx": b"m" * 10,
        "tokenizer.json": b"{}",
        "1_Pooling/config.json": b'{"pooling_mode_cls_token": true}',
    }
    await store.download(client_serving(content), spec, _no_progress)  # vocab.txt is missing: fine
    assert store.is_ready(spec)
    assert (spec.tokenizer, spec.pooling) == ("tokenizers", "cls")
    assert spec.files[0].sha256 == hashlib.sha256(b"m" * 10).hexdigest()  # recorded after download
    assert ModelStore(tmp_path, tmp_path / "models").find(spec.id) is not None  # remembered

    folder = tmp_path / "mine"
    (folder / "onnx").mkdir(parents=True)
    with pytest.raises(ValueError, match=r"model\.onnx"):
        store.add_folder(tmp_path, "x")
    (folder / "onnx/model.onnx").write_bytes(b"m")
    with pytest.raises(ValueError, match=r"vocab\.txt or tokenizer\.json"):
        store.add_folder(folder, "x")
    (folder / "vocab.txt").write_text("[UNK]\n[CLS]\n[SEP]")
    own = store.add_folder(folder, "My model")
    assert store.is_ready(own) and own.tokenizer == "wordpiece"
    store.remove(own.id)
    assert (folder / "onnx/model.onnx").exists()  # a folder of the user's own is never deleted

    status = {m["id"]: m for m in store.describe()}
    assert status["bge-small-en-v1.5"]["downloaded"] is False
    assert status[spec.id]["downloaded"] is True and status[spec.id]["pinned"] is False


async def _no_progress(done: int, total: int) -> None:
    pass


@pytest.mark.skipif(not os.environ.get("SPINUS_TEST_EMBEDDINGS"), reason="downloads a model (SPINUS_TEST_EMBEDDINGS=1)")
async def test_a_real_model_finds_answers_by_meaning(tmp_path: Path) -> None:
    config = Path(os.environ.get("SPINUS_TEST_EMBEDDINGS_DIR", str(tmp_path)))
    store = ModelStore(config, config / "models")
    spec = store.find("bge-small-en-v1.5")
    assert spec is not None
    if not store.is_ready(spec):
        async with httpx.AsyncClient(timeout=120) as client:
            await store.download(client, spec, _no_progress)
    model = store.load(spec.id)

    vectors = model.embed(["The bass ducks when the kick hits.", "Export the song as an MP3 file."], document=True)
    query = model.embed(["make the bass dip on every kick"], document=False)[0]
    scores: Any = vectors @ query
    assert scores[0] > scores[1] + 0.1

    manual = Manual(ROOT / "docs/user")
    manual.use_model(model, config / "cache")
    found = manual.search("my bass should get quieter whenever the kick drum plays")
    print([f"{s.page}: {s.title}" for s in found])
    assert found[0].page == "ducker.md"
    assert any(p.name.startswith("manual-bge-small") for p in (config / "cache").iterdir())  # cached


def test_models_live_in_the_shared_data_folder_and_old_downloads_move_there(tmp_path: Path, monkeypatch: Any) -> None:
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "data"))
    config = tmp_path / "config"
    old = config / "models" / "bge-small-en-v1.5"
    old.mkdir(parents=True)
    (old / "model.onnx").write_bytes(b"onnx")

    store = ModelStore(config)
    assert store.folder == tmp_path / "data" / "spinus" / "models"
    assert (store.folder / "bge-small-en-v1.5" / "model.onnx").read_bytes() == b"onnx"
    assert not (config / "models").exists()
