# Third-party components

Every dependency, bundled program, asset and AI model used by Spinus is listed here with its license.
Spinus is licensed AGPL-3.0-or-later, so everything below must be AGPLv3-compatible.
Model weights with non-commercial licenses (e.g. CC BY-NC) are not allowed as defaults.

When you add, upgrade or remove a dependency, update this file in the same pull request.

## Source dependencies (git submodules under `external/`)

| Component | Version / pin | License | Used for | Notes |
|---|---|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) | 9.0.3 (`be29c81`) | AGPLv3 / commercial | App framework, audio devices, plugin hosting, UI | Used under AGPLv3 |
| [Tracktion Engine](https://github.com/Tracktion/tracktion_engine) | `develop` @ `e42d82f` (2026-10-08) | GPLv3-or-later / commercial | Playback engine | Pinned to a `develop` commit because the latest release (v3.2.0) predates JUCE 9. GPLv3 code may be combined with AGPLv3 code (GPLv3 §13) |
| [ASIO SDK](https://www.steinberg.net/developers/) headers (`asio.h`, `asiosys.h`, `iasiodrv.h`), inside JUCE | 2.3.4 (2025-10-15), as bundled in the pinned JUCE | GPL-3.0 (dual: GPLv3 or Steinberg ASIO License; Spinus uses GPLv3) | ASIO sound cards in the Windows build (`JUCE_ASIO`) | Identical to the files in Steinberg's official `ASIO-SDK_2.3.4_2025-10-15.zip` (SHA-256 `d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca`, download.steinberg.net). GPLv3 code may be combined with AGPLv3 code (GPLv3 §13). Spinus does not use the ASIO name or logo as a trademark |
| [Catch2](https://github.com/catchorg/Catch2) | v3.16.1 (`0809213`) | BSL-1.0 | Unit tests only (not shipped) | |
| [CLAP SDK](https://github.com/free-audio/clap) | 1.2.10 (`195b42a`) | MIT | CLAP plugin hosting (headers only) | Spinus's own CLAP host |
| [FST](https://git.iem.at/zmoelnig/FST) | v0.177.0 (`30f49e6`) | GPL-3.0-or-later | VST2 interface header for JUCE's VST2 host | Reverse-engineered without the VST2 SDK. Compiled from a generated copy with one layout fix |
| [Surge XT](https://github.com/surge-synthesizer/surge) | release_xt_1.3.4 (`f7b97c682`) | GPLv3 | Development builds only: compiled for tests; not shipped (users download Surge XT, see below) | Built as a separate sub-build |

### Code bundled inside Surge XT

Compiled into the Surge XT plugin binary only (not into Spinus). Licenses read from each library's
LICENSE file at the pinned commit; recheck on every Surge upgrade.

| Component | License |
|---|---|
| Surge's JUCE fork (JUCE 7.0.12) | GPLv3 (JUCE 7's open-source option) |
| sst-basic-blocks, sst-effects, sst-filters, sst-waveshapers | GPLv3 |
| sst-cpputils, sst-plugininfra (majority), clap-juce-extensions, tuning-library | MIT |
| fmt, simde, LuaJIT, melatonin_inspector, airwindows, Mutable Instruments eurorack | MIT |
| PEGTL | MIT |
| libsamplerate | BSD-2-Clause |
| pffft | BSD-like (FFTPACK) |
| ODDSound MTS-ESP client | Permissive (ISC-style) |
| Factory patches and wavetables (`resources/data`) | Distributed by the Surge project with Surge XT. Spinus does not distribute them: development builds use them from the submodule, and users download them from Surge's own release page |

### Code bundled inside Tracktion Engine

Compiled into Spinus as part of the Tracktion modules. Licenses as stated by each project; recheck on every Tracktion upgrade.

| Component | License |
|---|---|
| choc | ISC |
| crill | BSL-1.0 |
| libsamplerate | BSD-2-Clause |
| magic_enum | MIT |
| rigtorp (SPSCQueue / MPMCQueue) | MIT |
| rpmalloc | Public domain / MIT |
| expected (tl::expected) | CC0-1.0 (public domain dedication) |
| doctest (Tracktion's own tests, not built by Spinus) | MIT |

## Libraries linked into Spinus (system packages)

| Component | License | Used for | Notes |
|---|---|---|---|
| [libcurl](https://curl.se) | curl license (MIT-style) | Downloading the free plugins | System library; part of every Arch install |
| [libarchive](https://libarchive.org) | BSD-2-Clause | Unpacking the free plugins' archives (tar.gz, tar.xz, 7z) | System library; part of every Arch install |
| [libdbus](https://www.freedesktop.org/wiki/Software/dbus/) (`libdbus-1.so.3`) | AFL-2.1 OR GPL-2.0-or-later (Spinus uses GPL-2.0-or-later, combinable with AGPLv3) | Linux: asking the XDG Desktop Portal for the desktop's own file dialog | Not linked: loaded at run time with dlopen (no headers needed to build), so Spinus starts without it and then shows its own file browser. System library (Arch: `dbus`; in the Flatpak runtime; the AppImage uses the system's) |
| [Rubber Band Library](https://breakfastquay.com/rubberband/) | GPL-2.0-or-later | Spinus Tune's graphical mode: formant-preserving pitch shifting of vocal clips; audio clips' pitch and time-stretch, through Tracktion | Arch: `rubberband` (4.0). Used under GPLv3 terms, combinable with AGPLv3 |

## Free plugins Spinus downloads on request (not distributed by Spinus)

Get free plugins downloads each project's own release for the user's system from
its GitHub release page, pinned by version and SHA-256 in `src/plugins/PluginDownloads.cpp`, into the
user's data folder. Spinus loads them in-process as CLAP plugins (TIME-12 and GATE-12, which publish no
CLAP build, as VST3 plugins). The licenses below are shown in the
download window. Only projects whose source **and** published build are open source are offered, and
only builds published as plain archives (no installers).

| Component | Release (archive) | License | Used for |
|---|---|---|---|
| Surge XT and Surge XT Effects | 1.3.4 (`surge-xt-linux-1.3.4-pluginsonly.tar.gz`, Windows `surge-xt-win64-1.3.4-pluginsonly.zip`, `surge-xt-portable-content-1.3.4.tar.gz`) | GPL-3.0 | Flagship synth with its factory sounds; character effects |
| LSP Plugins (Linux only) | 1.2.35 (`lsp-plugins-1.2.35-Linux-x86_64.7z`) | LGPL-3.0-or-later | Parametric EQ, Compressor, Limiter, Gate, Multiband Compressor, Spectrum Analyzer |
| Dragonfly Reverb | 3.2.10 (`dragonfly-reverb-3.2.10-linux-x86_64.tar.xz`, Windows `dragonfly-reverb-3.2.10-win64.zip`) | GPL-3.0-or-later | Hall, Room, Plate reverbs, Early Reflections |
| [Odin 2](https://thewavewarden.com/odin2) (Linux only) | 2.4.1 (`Odin2.4.1Linux.zip`, under the project's `NightlyDevel` release) | GPL-3.0 (its Aldrich font: OFL-1.1) | Synth with its factory presets (built into the plugin) |
| [OB-Xf](https://github.com/surge-synthesizer/OB-Xf) | 1.0.3 (`ob-xf-Linux-v1.0.3.zip`, Windows `ob-xf-Windows-v1.0.3.zip`; on both, `ob-xf-assets-v1.0.3.zip` for the factory patches) | GPL-3.0 (the patches too: they are part of its repository, under its GPL-3.0 licence) | Analog-style polyphonic synth, and its 488 factory patches (Spinus takes only the Patches folder from the assets archive, for closest preset) |
| [PeakEater](https://github.com/vvvar/PeakEater) | 0.8.2 (`peakeater-v0.8.2-Linux-x86_64.zip`, Windows `peakeater-v0.8.2-Windows-x86_64.zip`) | GPL-3.0 | Clipper and waveshaper |
| [Six Sines](https://github.com/baconpaul/six-sines) | 1.2.1 (`six-sines-Linux-2026-07-20-18ecb36.zip`, Windows `six-sines-Windows-2026-07-20-18ecb36.zip`; not the Windows `.exe` installer) | GPL-3.0 for the builds (they include JUCE and GPL-3.0 Surge Synth Team libraries; the Linux zip carries the GPL-3.0 text); its own source is MIT | FM synth with its factory patches (built into the plugin) |
| [TIME-12](https://github.com/tiagolr/time12) | 1.2.3 (`time12-linux-v1.2.3.zip`, Windows `time12-win-v1.2.3.zip`; Spinus takes only the VST3 bundle) | GPL-3.0 (its Ubuntu Medium font: Ubuntu Font Licence 1.0) | Tempo-synced delay-time shaper |
| [GATE-12](https://github.com/tiagolr/gate12) | 1.3.3 (`gate12-linux-v1.3.3.zip`, Windows `gate12-win-v1.3.3.zip`; Spinus takes only the VST3 bundle) | GPL-3.0 (its Ubuntu Medium font: Ubuntu Font Licence 1.0) | Tempo-synced volume shaper / trance gate |

## AI service (Python, `ai/`)

Runs only while AI is turned on. The Arch package uses the system's Python packages; a
development build uses `uv` with `ai/uv.lock`.

| Component | License | Used for | Notes |
|---|---|---|---|
| [httpx](https://www.python-httpx.org) | BSD-3-Clause | Talking to model providers (streaming) | Arch: `python-httpx` |
| [keyring](https://github.com/jaraco/keyring) | MIT | Keeping connection keys in the system keyring | Arch: `python-keyring` (with SecretStorage, BSD-3-Clause, and jeepney, MIT) |
| [ONNX Runtime](https://onnxruntime.ai) | MIT | Running local embedding models for manual search (optional) | Arch: `python-onnxruntime-cpu` |
| [NumPy](https://numpy.org) | BSD-3-Clause | Listening to takes (pitch, onsets, beatbox sounds) and vectors for local embedding models (optional) | Arch: `python-numpy` |
| [tokenizers](https://github.com/huggingface/tokenizers) | Apache-2.0 | Models that need a tokenizer other than Spinus's built-in WordPiece (optional) | AUR: `python-tokenizers` |
| pytest, pytest-asyncio | MIT, Apache-2.0 | Tests only | Not shipped |
| ruff, mypy | MIT | Lint and type checks only | Not shipped |

PyTorch is not part of the AI service: see the next section.

## Also in the AppImage and the Flatpak

The packages carry what an Arch install gets from the system: a Python for the AI service, its packages
(pinned by `ai/uv.lock`), and uv. The AppImage also carries the libraries its Ubuntu 24.04 build links
against that a desktop may lack (linuxdeploy's choice; glibc, ALSA, X and GL stay the system's).

| Component | License | In | Notes |
|---|---|---|---|
| [Python](https://www.python.org) 3.12 (python-build-standalone) | PSF-2.0 | AppImage, Flatpak | Runs the AI service |
| httpx, httpcore, h11, anyio, idna, certifi, keyring, SecretStorage, jeepney, jaraco.\*, more-itertools, cryptography, cffi, pycparser, typing-extensions, NumPy, ONNX Runtime, flatbuffers, protobuf, packaging | BSD-3-Clause, MIT, Apache-2.0, MPL-2.0 (certifi), PSF-2.0 | AppImage, Flatpak, Windows | The AI service's packages, as in the table above (ONNX Runtime and its dependencies for turning parts into notes) |
| [uv](https://github.com/astral-sh/uv) | MIT or Apache-2.0 | AppImage, Flatpak, Windows | Installs the analysis engine on request |
| [Rubber Band Library](https://breakfastquay.com/rubberband/) 4.0.0 | GPL-2.0-or-later | Flatpak (built), AppImage (Ubuntu's 3.3) | See above |
| libcurl, libarchive and the libraries they use (OpenSSL: Apache-2.0; GnuTLS: LGPL-2.1-or-later; nghttp2, zstd, lz4, xz, krb5, libxml2: MIT/BSD; ICU: Unicode-3.0) | as listed | AppImage | From Ubuntu 24.04 |
| FFTW 3 (GPL-2.0-or-later) and libsamplerate (BSD-2-Clause) | as listed | AppImage | What Ubuntu's Rubber Band uses |
| [AppImage runtime](https://github.com/AppImage/type2-runtime) | MIT | AppImage | Mounts the AppImage when it starts (added by appimagetool 1.9.1) |

## Analysis engine (downloaded on request, not distributed by Spinus)

Options › AI › Analysis engine installs one PyTorch build with `uv` into `~/.local/share/spinus/python/`,
each package pinned by version and SHA-256 in `ai/spinus_ai/compute/locks/` (Windows: the same folder in the
user's own folder, with the CPU, Nvidia and Intel builds from `locks/windows/`). Spinus runs it as a
separate process (`spinus_ai.compute.worker`). The user's computer downloads it from the sources below
after a question that states the size; Spinus ships none of it.

| Component | License | Build | Notes |
|---|---|---|---|
| [uv](https://github.com/astral-sh/uv) | MIT or Apache-2.0 | all | Installs the environments; Arch: `uv` (optional dependency) |
| [Python](https://www.python.org) 3.12 (python-build-standalone) | PSF-2.0 | all | uv's own Python for the environments |
| [PyTorch](https://pytorch.org) 2.14.0 | BSD-3-Clause | all | With its pure-Python dependencies (sympy, networkx, jinja2, filelock, fsspec, typing-extensions, setuptools: BSD/MIT/PSF) and NumPy (BSD-3-Clause) |
| [ROCm](https://github.com/ROCm/TheRock) 10.1.0 (TheRock: `rocm-sdk-*`, `amd-torch-device-*`, Triton) | MIT, Apache-2.0, NCSA, BSD (per library) | `rocm-<target>` | Open source; from AMD's `repo.amd.com` |
| NVIDIA CUDA libraries (`nvidia-*`, `cuda-toolkit`, cuDNN, NCCL, ...) | NVIDIA's proprietary licenses (CUDA EULA, cuDNN SLA) | `cuda-13`, `cuda-12` | **Not open source.** Downloaded from PyPI only when the user installs an Nvidia build, after a question that says so; never distributed by Spinus |
| Intel oneAPI runtime (`intel-*`, oneMKL, oneCCL, TBB, UMF, ...) | Intel Simplified Software License, Apache-2.0 (per library) | `xpu` | Partly **not open source**; same terms as above |
| [Triton](https://github.com/triton-lang/triton) | MIT | GPU builds | |
| [Demucs](https://github.com/adefossez/demucs) 4.1.0 | MIT | all | Stem separation; with einops (MIT), julius (MIT), lameenc (LGPL-3.0-or-later, an MP3 encoder Spinus does not use), PyYAML (MIT), safetensors (Apache-2.0), sphn (Apache-2.0), tqdm (MIT and MPL-2.0), huggingface-hub (Apache-2.0, not used: Spinus downloads the weights itself) |
| [Beat This!](https://github.com/CPJKU/beat_this) 1.1.0 | MIT | all | Beats and downbeats (without madmom's DBN); with torchaudio (BSD-2-Clause), rotary-embedding-torch (MIT), soxr (LGPL-2.1-or-later) |
| [transformers](https://github.com/huggingface/transformers) 5.19.0 | Apache-2.0 | all | Loads CLAP's audio tower for closest preset; with tokenizers (Apache-2.0), regex (Apache-2.0 and CNRI-Python: the Python 1.6.1 license, GPL-compatible), typer (MIT) with rich (MIT), markdown-it-py (MIT), mdurl (MIT), Pygments (BSD-2-Clause), shellingham (ISC) and annotated-doc (MIT) |

## Windows plugin runtime (downloaded on request, not shipped)

Options › Windows plugins uses the Wine and yabridge installed on the computer when they pass
Spinus's test. Otherwise, with the user's agreement, it downloads the pair below from the projects'
GitHub release pages, pinned by version and SHA-256 in `src/plugins/windows/WindowsPlugins.cpp`, into the
user's data folder. Spinus runs them as separate programs (Wine) and loads yabridge's plugin side
in-process like any plugin. The Windows build of Dragonfly Room Reverb is downloaded to test the setup.

| Component | Release (archive) | License | Used for |
|---|---|---|---|
| [Wine](https://www.winehq.org) Staging, portable build by [Kron4ek](https://github.com/Kron4ek/Wine-Builds) | 9.21 (`wine-9.21-staging-amd64.tar.xz`) | LGPL-2.1-or-later | Runs Windows plugins |
| [yabridge](https://github.com/robbert-vdh/yabridge) | 5.1.1 (`yabridge-5.1.1.tar.gz`) | GPL-3.0-or-later | Bridges Windows VST2, VST3 and CLAP plugins to Linux |
| Dragonfly Reverb (Windows build) | 3.2.10 (`dragonfly-reverb-3.2.10-win64.zip`) | GPL-3.0-or-later | The Windows test plugin |

## Plugins built from release tarballs (development builds only)

Downloaded during development builds by `cmake/SpinusBundledPlugins.cmake` (URL plus SHA-256), built
from source for pluginval and the plugin tests. Not installed by the package.

| Component | Version / pin | License | Used for | Notes |
|---|---|---|---|---|
| [LSP Plugins](https://github.com/lsp-plugins/lsp-plugins) | 1.2.35, `lsp-plugins-src-1.2.35.tar.gz` (SHA-256 `2c95ec7b…5bb6c`) | LGPL-3.0-or-later | Parametric EQ, Compressor, Limiter, Gate, Multiband Compressor, Spectrum Analyzer | Only the modules in `SPINUS_LSP_MODULES` are built. Runtime libraries: libsndfile (LGPL-2.1+), cairo (LGPL-2.1 / MPL-1.1) |
| [Dragonfly Reverb](https://github.com/michaelwillis/dragonfly-reverb) | 3.2.10, `dragonfly-reverb-3.2.10-src.tar.xz` (SHA-256 `18af55a9…f7743`) | GPL-3.0-or-later | Hall, Room, Plate reverbs, Early Reflections | |
| Surge XT Effects | Surge XT release_xt_1.3.4 (see Surge XT above) | GPLv3 | Delay, chorus, phaser, flanger, distortion and other character effects | Built by the Surge sub-build |

### Code inside these plugins (also in the downloaded releases)

| Inside | Component | License |
|---|---|---|
| Dragonfly Reverb | DPF (DISTRHO Plugin Framework) | ISC |
| Dragonfly Reverb | Freeverb3 | GPL-2.0-or-later |
| Dragonfly Reverb | KISS FFT | BSD-3-Clause |
| Dragonfly Reverb | Bitstream Vera Sans font | Bitstream Vera license (permissive) |
| Odin 2, OB-Xf, PeakEater | JUCE (GPL-3.0 / AGPL-3.0 for open-source use), clap-juce-extensions (MIT) | As listed |
| Six Sines | JUCE (GPL-3.0), clap-wrapper and clap-helpers (MIT), sst-basic-blocks and sst-filters (GPL-3.0), sst-jucegui and sst-voicemanager (MIT), MTS-ESP client (0BSD), libsamplerate (BSD-2-Clause), PFFFT (BSD-style), fmt and SIMDe (MIT) | As listed |
| TIME-12, GATE-12 | JUCE (GPL-3.0), with the VST3 SDK it includes (under its GPL-3.0 option) | As listed |
| LSP Plugins | lsp-3rd-party: plugin API headers (CLAP MIT, LV2 ISC, LADSPA LGPL, VST3 interfaces rewritten by LSP under GPL-3.0-or-later, clean-room VST2 header under ISC) | As listed |

## Programs Spinus runs (installed separately)

| Component | License | Used for | Notes |
|---|---|---|---|
| [LAME](https://lame.sourceforge.io/) (`lame` program) | LGPL-2.0-or-later | MP3 export | Not linked or shipped: Spinus runs the system's `lame` as a separate process (Arch package `lame`, an optional dependency). MP3 export is disabled when it is missing |
| [FFmpeg](https://ffmpeg.org/) (`ffmpeg` program) | LGPL-2.1-or-later (GPL-2.0-or-later or GPL-3.0-or-later in builds with GPL parts, as Arch's) | Reading M4A, AAC and other audio files (dropped files and songs in the Analyzer), and MP3s Spinus cannot read to the end; AAC export | Not linked or shipped: Spinus runs the system's `ffmpeg` as a separate process (Arch package `ffmpeg`, an optional dependency). Without it, Spinus reads WAV, AIFF, FLAC, Ogg, MP3 and Opus |

## Test tools (not shipped)

| Component | Version / pin | License | Notes |
|---|---|---|---|
| [pluginval](https://github.com/Tracktion/pluginval) | v1.0.4 Linux binary (SHA-256 `c01c49d8…b55352`) | GPL-3.0 | Downloaded at configure time by `cmake/SpinusPluginval.cmake`; runs in CTest |

### Code that comes with JUCE

Compiled into Spinus (and into the standalone Spinus Sampler plugin) from JUCE's own copies.

| Component | License | Used for |
|---|---|---|
| VST3 SDK | MIT (since 3.8) | Hosting VST3 plugins; building the Spinus Sampler VST3 |
| LV2 / lilv / serd / sord / sratom | ISC | Hosting LV2 plugins; building the Spinus Sampler LV2 |
| libFLAC | BSD-3-Clause | The Sampler keeps its samples FLAC-compressed in its saved state, so a project or preset carries its sounds; reading FLAC files; dropped MP3, Ogg and Opus files are turned into FLAC; FLAC export |
| Ogg Vorbis (libogg, libvorbis) | BSD-3-Clause | Reading Ogg files |
| Opus (libopus, opusfile, libopusenc) | BSD-3-Clause | Reading Opus files; Opus export |

## Planned (not yet added)

Each gets a row above when it is actually added.

| Component | License | Notes |
|---|---|---|
| yabridge | GPLv3+ | Bundled as a separate program |

## AI models

### Embedding models Spinus downloads on request (not distributed by Spinus)

Downloaded from Hugging Face only when the user clicks Download, pinned to one commit, every file checked
by SHA-256 (the pins are in `ai/spinus_ai/embeddings.py`).

| Model | Code and weights license | Source |
|---|---|---|
| bge-small-en-v1.5 | MIT | https://huggingface.co/BAAI/bge-small-en-v1.5 |
| all-MiniLM-L6-v2 | Apache-2.0 | https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2 |
| bge-base-en-v1.5 | MIT | https://huggingface.co/BAAI/bge-base-en-v1.5 |
| multilingual-e5-small | MIT | https://huggingface.co/intfloat/multilingual-e5-small |

### Analysis models Spinus downloads on request (not distributed by Spinus)

Downloaded from Hugging Face only after the user agrees to the size, pinned to one commit, every file
checked by SHA-256 (the pins are in `ai/spinus_ai/compute/weights.py`). They run in the analysis engine.

| Model | Code and weights license | Source | Notes |
|---|---|---|---|
| Demucs htdemucs (4 stems) | MIT (weights under the project's MIT license) | https://huggingface.co/adefossez/HTDemucs @ `cbc8a9b` | 84 MB. Trained on MUSDB-HQ and 800 internal songs |
| Demucs htdemucs_6s (6 stems) | MIT (same) | https://huggingface.co/adefossez/HTDemucs-6s @ `3c5ee47` | 55 MB |
| Basic Pitch `nmp.onnx` (ICASSP 2022) | Apache-2.0 (Copyright 2022 Spotify AB) | https://github.com/spotify/basic-pitch @ `9991303` (release 0.4.0) | 230 KB. Run by the AI service with ONNX Runtime; its inference and note decoding are ported into `ai/spinus_ai/transcribe.py` (Apache-2.0, attributed there) |
| LAION-CLAP music (`laion/larger_clap_music`, transformers format) | Apache-2.0 | https://huggingface.co/laion/larger_clap_music @ `a0b4534` | 776 MB (`pytorch_model.bin`, a pickle: checked by SHA-256, then loaded with `weights_only=True`). Only the audio tower is used, to compare sounds (closest preset). Code of the original LAION-CLAP is CC0-1.0 |
| Beat This! `final0` and `small0` | MIT (explicit: "the code and the published model weights are released under the MIT license") | the authors' server, as `beat-this` 1.1.0 downloads them: https://cloud.cp.jku.at/public.php/dav/files/7ik4RrBKTS273gp | 81 MB and 8 MB. Upstream notes some training data is copyrighted or under limited CC licenses |
| MVSep Mega 53 stems (BS-RoFormer, `mvsep_mega_model_bs_roformer_53_stems_v1.ckpt`) | MIT (the repository's license; its author said the 53-stem weights are MIT in the repository's issue #245). Training data not published | https://github.com/ZFTurbo/Music-Source-Separation-Training/releases/tag/v1.0.21 | 1.37 GB, a pickle: checked by SHA-256, then loaded with `weights_only=True`. Optional ("Many instruments" in the Analyzer), never a default. Run with `ai/spinus_ai/compute/bs_roformer.py`, adapted from the same repository's `models/bs_roformer` (MIT, Copyright (c) 2024 Roman Solovyev (ZFTurbo), after Phil Wang's BS-RoFormer, MIT) |

### Models shipped with Spinus

None yet. Every model must list: name, version, source URL, code license, **weights license**, and checksum.
Policy: project-level licenses cover released weights;
only code and weights licenses count, not training data; CC BY / CC BY-SA allowed with credits; no NC.

## Assets (samples, fonts, icons)

| Asset | License | Notes |
|---|---|---|
| Spinus Starter Pack (`samples/Spinus Starter Pack/`: 97 FLAC sounds and the Drum Machine's factory kits) | Signature Sounds (signaturesounds.org, formerly signaturesamples.co.uk). Free for any use; [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) where marked. Included with the authors' permission | Cut, peak-normalised and converted to FLAC by `tools/starter-pack/make_pack.py` from the authors' packs; `tools/starter-pack/manifest.json` and the pack's `README.md` list every file's source pack and original name (CC0 text in the pack's `LICENSE.txt`). Credit is a courtesy, not required. Shipped with every package |
| Demo song *Siskin* (`demo/Siskin/`: the project file, plugin states and two sound effects) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), credit "Spinus DAW contributors" | Original work of the Spinus project, made by `tools/showcase-song` (the sound effects synthesised by `make_audio.py`, no samples). Shipped with every package. The plugin states are settings for Spinus's own devices and for free plugins Spinus downloads on request (Surge XT, Surge XT Effects, Dragonfly Reverb); Surge XT's include the names of its factory patches the song starts from, not the patches themselves. Only the demo song is CC BY 4.0: Spinus's code, including `tools/showcase-song`, stays AGPL-3.0-or-later. See `demo/README.md` |
| Demo song *Neon Coast* (`demo/Neon Coast/`: the project file, plugin states and three sound effects) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), credit "Spinus DAW contributors" | Original work of the Spinus project, made by `tools/demo-synthwave` (the sound effects synthesised with numpy by `make_audio.py`, no samples). The plugin states are settings for free plugins Spinus downloads on request (Surge XT, Surge XT Effects, OB-Xf, Dragonfly Reverb, PeakEater); Surge XT's and OB-Xf's hold the factory patches the song starts from, as the plugins save them. Only the demo song is CC BY 4.0: `tools/demo-synthwave` stays AGPL-3.0-or-later. See `demo/README.md` |
| Demo song *Sodium Lights* (`demo/Sodium Lights/`: the project file, plugin states and four sounds) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), credit "Spinus DAW contributors" | Original work of the Spinus project, made by `tools/demo-dnb` (the breakbeat, crackle, impact and reversed cymbal synthesised by its `make_audio.py` with numpy and scipy, no samples; the break also sits inside the Beat Slicer's state). The plugin states are settings for free plugins Spinus downloads on request (Surge XT, Surge XT Effects, Dragonfly Reverb, PeakEater); Surge XT's include the names of the factory and third-party patches that come with Surge XT, not the patches themselves. Spinus's code, including `tools/demo-dnb`, stays AGPL-3.0-or-later. See `demo/README.md` |
| Demo song *Rainy Window* (`demo/Rainy Window/`: the project file, plugin states with the samples its Sampler and Drum Machine play, and five audio clips) | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), credit "Spinus DAW contributors" | Original work of the Spinus project, made by `tools/demo-lofi` (every sound synthesised by its `make_audio.py` with numpy, no recordings or samples from elsewhere). The plugin states are settings for Spinus's own devices and for free plugins Spinus downloads on request (Surge XT, Dragonfly Reverb); Surge XT's include the names of the factory patches the song starts from, not the patches themselves. Spinus's code, including `tools/demo-lofi`, stays AGPL-3.0-or-later. See `demo/README.md` |
