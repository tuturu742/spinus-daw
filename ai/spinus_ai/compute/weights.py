"""Model files the analysis engine downloads, pinned by revision and SHA-256.

Nothing is downloaded until the user agrees (Options > AI > Analysis engine, or the Analyzer's first run,
which states the size). Files go to ~/.local/share/spinus/models/analysis/<model>/; a model counts as
there only when every file is, with the right size.
"""

from __future__ import annotations

import asyncio
import hashlib
import shutil
from collections.abc import Awaitable, Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

import httpx

Progress = Callable[[int, int], Awaitable[None]]


@dataclass(frozen=True)
class WeightFile:
    path: str  # kept as, in the model's folder
    size: int
    sha256: str
    remote: str = ""  # its name where it is downloaded from, when not the same


@dataclass(frozen=True)
class AnalysisModel:
    id: str
    name: str
    description: str
    license: str
    repo: str  # Hugging Face repository
    revision: str
    files: tuple[WeightFile, ...]
    stems: tuple[str, ...] = field(default_factory=tuple)
    kind: str = "stems"  # "stems", "beats", "notes" or "similarity"
    url: str = ""  # where the files are, when not on Hugging Face
    architecture: str = "demucs"  # for stems: how the analysis engine runs it ("demucs" or "bs_roformer")
    homepage: str = ""  # where people can read about it and check its license (default: its Hugging Face page)

    @property
    def page(self) -> str:
        return self.homepage or f"https://huggingface.co/{self.repo}/tree/{self.revision}"

    def file_url(self, f: WeightFile) -> str:
        name = f.remote or f.path
        if self.url:
            return f"{self.url}/{name}"
        return f"https://huggingface.co/{self.repo}/resolve/{self.revision}/{name}"

    def file_urls(self, f: WeightFile, link: str = "") -> list[str]:
        """Where to fetch a file from: the model's own address, or `link`, one the user pasted (Spinus can't
        know where authors move their files): the file itself for a one-file model, or its folder."""
        if link:
            link = link.strip()
            name = f.remote or f.path
            if len(self.files) == 1 and not link.endswith("/"):
                return [link]
            return [f"{link.rstrip('/')}/{name}"]
        return [self.file_url(f)]

    @property
    def size(self) -> int:
        return sum(f.size for f in self.files)

    def to_json(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "name": self.name,
            "description": self.description,
            "license": self.license,
            "page": self.page,
            "size": self.size,
            "stems": list(self.stems),
            "kind": self.kind,
        }


# The MVSep Mega model's stems, in its order.
STEMS_53 = (
    "accordion", "acoustic-guitar", "back-vocal", "banjo", "bass", "bassoon", "bells", "bowed_strings", "brass",
    "cello", "clarinet", "congas", "digital-piano", "dobro", "double-bass", "drums", "electric-guitar", "flute",
    "french-horn", "glockenspiel", "guitar", "harmonica", "harp", "harpsichord", "hh", "keys", "kick",
    "lead-vocal", "mandolin", "marimba", "oboe", "organ", "percussion", "piano", "saxophone", "sitar", "snare",
    "strings", "synth", "tambourine", "timpani", "toms", "triangle", "trombone", "trumpet", "tuba", "ukulele",
    "viola", "violin", "vocal", "wind", "wind-chimes", "woodwind",
)  # fmt: skip

# Demucs (MIT; its weights are under the project's license), from the author's Hugging Face
# repositories, pinned to one commit each.
MODELS: tuple[AnalysisModel, ...] = (
    AnalysisModel(
        "htdemucs",
        "Demucs, 4 stems",
        "Splits a song into vocals, drums, bass and the rest.",
        "MIT",
        "adefossez/HTDemucs",
        "cbc8a9b1a87023b7fd74e7b3412e6321c0eab003",
        (
            WeightFile("htdemucs.yaml", 21, "239c445d0b14454d541ad8bd9bb271c9e536d267e8a4625208744cbb2e7bb66c"),
            WeightFile(
                "955717e8.safetensors", 84025440, "d9fa14133cfcc034a6758923bb3a8ca9f8dfd0b582134643bbf83f72c17576dd"
            ),
        ),
        ("drums", "bass", "other", "vocals"),
    ),
    AnalysisModel(
        "htdemucs_6s",
        "Demucs, 6 stems",
        "Also splits out piano and guitar (they are less clean than the other four).",
        "MIT",
        "adefossez/HTDemucs-6s",
        "3c5ee475be622df764938de97e4281a7b07ffa58",
        (
            WeightFile("htdemucs_6s.yaml", 21, "207405151270af8fd81c2373c25d27950916682ac91dca7884a11ce13dad6f58"),
            WeightFile(
                "5c90dfd2.safetensors", 54885744, "d2a1745f0744721f6b8ca5bf469b67c651ea5ed1b52998cab033b2158609d411"
            ),
        ),
        ("drums", "bass", "other", "vocals", "guitar", "piano"),
    ),
    # MVSep Mega 53 stems (BS-RoFormer, Roman Solovyev / ZFTurbo, Music-Source-Separation-Training release
    # v1.0.21; MIT like the repository, as its author said in its issue #245): up to 53 instruments, of
    # which the analysis engine keeps those heard (stem_choice.py). Optional and large; never a default.
    AnalysisModel(
        "mvsep_mega_53",
        "MVSep Mega, 53 instruments",
        "Tells apart up to 53 kinds of instrument (lead and backing vocals, synth, strings, flute, accordion"
        "...) and keeps those it hears. Slower; best on a graphics card with 16 GB of memory.",
        "MIT",
        "ZFTurbo/Music-Source-Separation-Training",
        "v1.0.21",
        (
            WeightFile(
                "config.yaml",
                4184,
                "7e198062a251587088adb91215a4f44ab59e67bd62fcc805cf54d6e7dfc51103",
                "mvsep_mega_model_bs_roformer_53_stems.yaml",
            ),
            WeightFile(
                "model.ckpt",
                1368919887,
                "c62820893bbf86d4e734f966bd142d9157cfc8bb8e79e9d8f9ea553f3ff3519f",
                "mvsep_mega_model_bs_roformer_53_stems_v1.ckpt",
            ),
        ),
        STEMS_53,
        url="https://github.com/ZFTurbo/Music-Source-Separation-Training/releases/download/v1.0.21",
        architecture="bs_roformer",
        homepage="https://github.com/ZFTurbo/Music-Source-Separation-Training/releases/tag/v1.0.21",
    ),
    # Beat This! (CPJKU, ISMIR 2024; code and weights MIT, explicitly), from the authors' server, as
    # Beat This! 1.1.0 itself downloads them. final0 for a GPU, small0 for the CPU.
    AnalysisModel(
        "beat_this_final0",
        "Beat This!",
        "Finds the beats and the first beat of each bar.",
        "MIT",
        "CPJKU/beat_this",
        "1.1.0",
        (WeightFile("final0.ckpt", 81058141, "8c328b45f59d8dd3dff219253ff6a8d6482be57d0133a29140e2febbf8eb8331"),),
        kind="beats",
        url="https://cloud.cp.jku.at/public.php/dav/files/7ik4RrBKTS273gp",
        homepage="https://github.com/CPJKU/beat_this",
    ),
    AnalysisModel(
        "beat_this_small0",
        "Beat This! (small)",
        "The same, smaller and faster on the CPU, a little less accurate.",
        "MIT",
        "CPJKU/beat_this",
        "1.1.0",
        (WeightFile("small0.ckpt", 8451101, "6074be2c4d490c5f6101fcc374a1ec72ae93456e23bb6019783b849f5dc7d47b"),),
        kind="beats",
        url="https://cloud.cp.jku.at/public.php/dav/files/7ik4RrBKTS273gp",
        homepage="https://github.com/CPJKU/beat_this",
    ),
    # Basic Pitch (Spotify; Apache-2.0), the ONNX model of release 0.4.0, run by the AI service itself with
    # ONNX Runtime (spinus_ai/transcribe.py); no PyTorch needed.
    AnalysisModel(
        "basic_pitch",
        "Basic Pitch",
        "Turns a melodic part (bass, vocals, piano, ...) into notes.",
        "Apache-2.0",
        "spotify/basic-pitch",
        "9991303bba609a3b93089d13ec80d1d495083596",
        (WeightFile("nmp.onnx", 230444, "2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec"),),
        kind="notes",
        url="https://raw.githubusercontent.com/spotify/basic-pitch/9991303bba609a3b93089d13ec80d1d495083596"
        "/basic_pitch/saved_models/icassp_2022",
        homepage="https://github.com/spotify/basic-pitch",
    ),
    # LAION-CLAP's music model in the transformers format (Apache-2.0). Only its audio tower is used, to
    # tell how alike two sounds are (closest preset). The weights are a PyTorch pickle (no
    # safetensors at this revision): loaded with torch.load(weights_only=True) after the hash check.
    AnalysisModel(
        "clap_music",
        "CLAP (music)",
        "Hears how alike two sounds are, to find the closest presets.",
        "Apache-2.0",
        "laion/larger_clap_music",
        "a0b4534a14f58e20944452dff00a22a06ce629d1",
        (
            WeightFile("config.json", 628, "2d7722d338bb83ea8824272b1431f088954d3425d79eb3c2d39489478516dc03"),
            WeightFile(
                "preprocessor_config.json", 541, "9739f58296aa6f9ac18008fd0150fb2649bc554985fbde86d0a4041c882ac753"
            ),
            WeightFile(
                "pytorch_model.bin", 776444665, "5c289311f4a030d768af7ffbfdecd01b008aa64824211899a4e59f4f9d154fd1"
            ),
        ),
        kind="similarity",
    ),
)


def find_model(model_id: str) -> AnalysisModel | None:
    return next((m for m in MODELS if m.id == model_id), None)


# Waits before trying a file again after a server error or a dropped connection (the hosts have bad
# minutes; one 500 from GitHub or Hugging Face should not fail a 300 MB download).
RETRY_WAITS = (2.0, 5.0, 15.0)


ANOTHER_LINK = (
    " If its authors give another link for it, use Download from another link... (in the Analyzer, under Sources)."
)


def unreachable(model: AnalysisModel, hosts: str, why: str) -> str:
    """Spinus doesn't keep copies of other people's models: each comes from its authors' own servers, and
    when those are down, the user is told whose they are and to try later (or a link the authors give)."""
    return (
        f"{model.name} can't be downloaded right now: its authors' server ({hosts}) isn't answering ({why})."
        " This is on their side, not Spinus's; please try again later." + ANOTHER_LINK
    )


class WeightStore:
    def __init__(self, root: Path, retry_waits: tuple[float, ...] = RETRY_WAITS) -> None:
        self.root = root
        self.retry_waits = retry_waits

    def folder(self, model: AnalysisModel) -> Path:
        return self.root / model.id

    def is_ready(self, model: AnalysisModel) -> bool:
        folder = self.folder(model)
        return not (folder / ".partial").exists() and all(
            (folder / f.path).is_file() and (folder / f.path).stat().st_size == f.size for f in model.files
        )

    def describe(self) -> list[dict[str, Any]]:
        return [{**m.to_json(), "downloaded": self.is_ready(m), "folder": str(self.folder(m))} for m in MODELS]

    async def download(
        self, client: httpx.AsyncClient, model: AnalysisModel, progress: Progress, link: str = ""
    ) -> None:
        """Every file, checked by SHA-256, from the first of its addresses that gives it (`link`: only the one
        the user pasted); nothing is kept if it fails or is stopped."""
        folder = self.folder(model)
        folder.mkdir(parents=True, exist_ok=True)
        (folder / ".partial").touch()
        done = 0
        try:
            for f in model.files:
                start = done
                urls = model.file_urls(f, link)
                failures: list[str] = []
                for url in urls:
                    failure = await self.fetch(client, model, f, url, folder, start, progress)
                    if failure is None:
                        break
                    failures.append(failure)
                else:
                    raise RuntimeError(self.explain(model, f, urls, failures, link))
                done = start + f.size
            (folder / ".partial").unlink()
        except BaseException:
            shutil.rmtree(folder, ignore_errors=True)
            raise

    async def fetch(
        self,
        client: httpx.AsyncClient,
        model: AnalysisModel,
        f: WeightFile,
        url: str,
        folder: Path,
        start: int,
        progress: Progress,
    ) -> str | None:
        """One file from one address, tried again after a server error or a dropped connection. None when
        it arrived whole; else why not: "busy: ...", "missing: <status>" or "checksum"."""
        why = ""
        for wait in (*self.retry_waits, None):
            digest = hashlib.sha256()
            done = start
            try:
                async with client.stream("GET", url, follow_redirects=True) as response:
                    if response.status_code != 200:
                        if not (response.status_code >= 500 or response.status_code == 429):
                            return f"missing: {response.status_code}"
                        why = f"busy: server error {response.status_code}"
                    else:
                        with (folder / f.path).open("wb") as out:
                            async for chunk in response.aiter_bytes(1 << 20):
                                out.write(chunk)
                                digest.update(chunk)
                                done += len(chunk)
                                await progress(done, max(model.size, done))
                        return None if digest.hexdigest() == f.sha256 else "checksum"
            except httpx.TransportError as error:
                why = f"busy: {type(error).__name__}"
            if wait is not None:
                await asyncio.sleep(wait)
        return why

    @staticmethod
    def explain(model: AnalysisModel, f: WeightFile, urls: list[str], failures: list[str], link: str) -> str:
        if link:
            failure = failures[-1]
            if failure == "checksum":
                return (
                    f"The file at that link isn't {model.name}: it doesn't match the model's checksum, so "
                    "Spinus didn't keep it."
                )
            if failure.startswith("missing"):
                return f"There is no {f.remote or f.path} at that link ({failure.split(': ')[1]})."
            return f"That link isn't answering ({failure.split(': ', 1)[1]}). Please try again later."
        if all(x.startswith("busy") for x in failures):
            hosts = ", ".join(dict.fromkeys(urlsplit(u).hostname or u for u in urls))
            return unreachable(model, hosts, failures[0].split(": ", 1)[1])
        if "checksum" in failures:
            return f"{f.path} did not match its checksum: download it again."
        status = next(x for x in failures if x.startswith("missing")).split(": ")[1]
        return f"Could not download {f.path} ({status})." + ANOTHER_LINK

    def remove(self, model: AnalysisModel) -> None:
        shutil.rmtree(self.folder(model), ignore_errors=True)
