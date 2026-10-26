# Spinus DAW: Design Document

**Repository:** https://github.com/tuturu742/spinus-daw
**Describes:** Spinus 0.1.0. Planned work is in §17 Roadmap.

---

## 0. How to use this document (for agents)

- This is the single source of truth for scope and architecture. If code and this document disagree, stop and raise it; do not silently diverge.
- Read in this order: §1 Principles → §2 Decisions → §4 Architecture → the section for your task → §18 Agent workflow.
- Every architectural change requires an update here, including a sentence or two on why.
- The owner tests and steers; agents write the code. Every pull request must include **"How to test"** steps a non-programmer can follow on Arch Linux.

---

## 1. Vision and principles

Spinus is a free, open-source, Linux-first digital audio workstation with an FL Studio-style workflow and built-in AI assistance.

**Principles, in priority order:**

1. **The audio never glitches.** Real-time safety beats every feature. Nothing slow (AI, disk, network, UI) ever runs on the audio thread.
2. **FL-style workflow.** Channel rack → patterns → playlist → mixer. A user coming from FL Studio should feel at home within minutes.
3. **One good tool per job.** No duplicate instruments or effects. One flagship synth, one simple synth, one sampler, one drum machine, one of each core effect.
4. **Usable out of the box.** A fresh install can make a complete, mixed, exported track with zero third-party plugins.
5. **AI proposes, the user decides.** Every AI change is previewed, approved, and lands as a single undo step. AI never silently modifies a project.
6. **Plugins just work.** Native VST3, CLAP, LV2 and VST2, plus Windows plugins on Linux without the user ever touching Wine.
7. **Everything is a command.** Every project change (from the UI, the AI or a script) goes through one command API. This makes undo, AI tooling and testing uniform.

---

## 2. Decision summary

| # | Topic | Decision |
|---|---|---|
| D1 | Name | Spinus DAW (genus of siskins, family Fringillidae) |
| D2 | License | Open source. Effective license **AGPL-3.0** (see §3) |
| D3 | Platforms | Linux first; Windows 10 and 11 (64-bit) too (§16). macOS is not planned |
| D4 | Primary distro | Arch Linux and Arch-based distributions. Primary packaging target: AUR / PKGBUILD |
| D5 | Owner's role | Tests and steers. Agents write all code |
| D6 | Workflow | FL-style, plus an Ableton-style clip launcher |
| D7 | UI | Native JUCE UI |
| D8 | Stock content | Slightly more than basic; one flagship synth plus a simple 3-oscillator synth (§11). Core effects come from open-source plugins Spinus downloads on request (§11.2) |
| D9 | Import | Audio files; MIDI files later (§17). No other DAW's project files (FL Studio's `.flp` included) |
| D10 | Plugin formats | VST3, CLAP, LV2, VST2; Windows VST2/VST3/CLAP via managed bridge |
| D11 | AI placement | Hybrid: local audio models, cloud LLM |
| D12 | Tested AI hardware | The analysis engine is tested on an AMD **gfx1151** GPU (Radeon 8060S, RDNA 3.5, unified memory) and on the CPU; other GPUs' builds are pinned but labelled untested. See §12.1 |
| D13 | Assistant authority | Acts in the project only with user approval |
| D14 | LLM provider | Pluggable; Claude by default; **OpenRouter** supported; multiple keys and per-feature model routing (§12.3) |
| D15 | Priorities | The core DAW comes first; AI features are optional and the DAW works without them |
| D16 | Docs location | GitHub repo `tuturu742/spinus-daw`, Markdown |
| D17 | Flagship synth | **Surge XT** (GPLv3); a Spinus-built flagship synth is not planned (§11.1, §17) |
| D18 | Mixer size | **Dynamic**: inserts are created on demand, no fixed count (§7.1) |

---

## 3. Licensing

| Component | License | Notes |
|---|---|---|
| JUCE 9 | AGPLv3 or commercial | Open-source use requires AGPLv3 |
| Tracktion Engine | GPLv3 or commercial | GPLv3 code may be combined with AGPLv3 code (GPLv3 §13) |
| VST3 SDK | MIT (since 3.8, Oct 2025) | No agreement with Steinberg needed |
| CLAP SDK | MIT | |
| LV2 | ISC | |
| yabridge | GPLv3+ | Bundled as a separate program |
| Rubber Band (time-stretch) | GPLv2+ | Usable under GPLv3 terms |
| Surge XT | GPLv3 | Flagship synth, downloaded on request; keep its name, credits and license visible |
| LSP Plugins | LGPL-3.0-or-later | Optional effects downloaded on request (EQ, dynamics, multiband, analyzer), §11.2 |
| Dragonfly Reverb | GPL-3.0-or-later | Reverbs downloaded on request, §11.2 |
| Odin 2, OB-Xf, PeakEater, Six Sines, TIME-12, GATE-12 | GPL-3.0 | Synths, a clipper and two shapers downloaded on request, §11 |

**Result:** Spinus source is licensed **AGPL-3.0-or-later**. All dependencies must be AGPLv3-compatible. Agents must check the license of every new dependency and every AI model's weights and record it in `THIRD_PARTY.md`. Models with non-commercial licenses (e.g. CC BY-NC) are **not allowed** as defaults. CC BY / CC BY-SA weights are allowed with credits; only code and weights licenses count, not training data.

*This is an engineering summary, not legal advice.*

---

## 4. Architecture overview

```
┌──────────────────────────────── spinus (C++20, one process) ───────────────────────────────┐
│                                                                                            │
│  UI layer (JUCE Components)                                                                │
│   Browser · Channel Rack · Piano Roll · Playlist · Mixer · Plugin windows · Assistant panel│
│                │  user actions                                                             │
│                ▼                                                                           │
│  Command API  ──►  Spinus Model (juce::ValueTree, source of truth)  ──►  Undo manager      │
│                │                          │                                                │
│                │                          ▼ incremental sync                               │
│                │                 Engine Adapter  ──►  Tracktion Engine Edit                │
│                │                                        │                                  │
│                │                                        ▼                                  │
│                │                       Audio graph (real-time thread)                      │
│                │             native plugins · stock DSP · Surge XT · autotune RT           │
│                │                                        │                                  │
│                │                                        ▼                                  │
│                │                       JUCE audio device (ALSA / JACK / PipeWire-JACK)     │
│                │                                                                           │
│  AI Bridge (JSON-RPC over local TCP)   ◄──────────────────────────────┐                    │
└────────────────┼──────────────────────────────────────────────────────┼────────────────────┘
                 │                                                      │
                 ▼                                                      │
┌──────── spinus-ai (Python 3.12, separate process) ────────┐           │
│  Job queue · progress events · cancellation               │           │
│  Local models (PyTorch ROCm gfx1151 / CPU): stems,         │           │
│    transcription, pitch, beat/key, sound matching          │           │
│  LLM gateway: Anthropic · OpenRouter · OpenAI-compatible · │           │
│    local                                                   │           │
│  Assistant: RAG index + tool calls ──► Command API ────────┼───────────┘
└────────────────────────────────────────────────────────────┘

┌──── Windows plugin bridge (separate processes) ────┐
│  Spinus-managed Wine prefix + pinned Wine build    │
│  yabridge shims ──► loaded by spinus as native     │
│  VST2 / VST3 / CLAP                                │
└────────────────────────────────────────────────────┘
```

**Key rules**

- The **Spinus Model** is the source of truth and the only thing saved to disk. The Tracktion Edit is a derived playback structure rebuilt or patched by the **Engine Adapter**.
- All changes go through the **Command API** (§13). The UI never mutates the model directly.
- The AI service is a separate process. If it crashes or hangs, the DAW keeps playing. The DAW must start and be fully usable with the AI service absent.
- Audio data passes to the AI service as files in the project cache folder, never over the socket.
- Helper programs (ffmpeg, lame, Wine, the plugin scanner, pw-dump, the AI service) start through `core::ChildProcess` (`src/core/`), never `juce::ChildProcess`: on Linux JUCE's closes its pipe's file descriptor twice, which can close another thread's file. A test fails if `juce::ChildProcess` appears in Spinus code; JUCE's `ChildProcessCoordinator`/`ChildProcessWorker` (the plugin sandbox) are fine.
- File dialogs go through `core::FileDialog` (`src/core/`), never `juce::FileChooser`: on Linux the desktop's own dialog through the XDG Desktop Portal (D-Bus, libdbus loaded at run time), waited for off the message thread and modal to Spinus's window; Spinus's own (JUCE's non-native) file browser when there is no portal; on Windows JUCE's native dialogs. JUCE's Linux chooser runs zenity/kdialog through `juce::ChildProcess`. A test fails if `juce::FileChooser` appears elsewhere in Spinus code.

---

## 5. Technology stack

| Area | Choice |
|---|---|
| Language (app) | C++20 |
| Build | CMake ≥ 3.25, Ninja |
| Framework | JUCE 9 (pinned git submodule) |
| Engine | Tracktion Engine (pinned git submodule: a `develop` commit, since its latest release predates JUCE 9; upgrades are deliberate: bump, rebuild, run all tests, record the commit in `THIRD_PARTY.md`) |
| Audio backends | ALSA and JACK via JUCE; PipeWire through its JACK interface (`pipewire-jack`), the default |
| Time-stretch | Rubber Band through Tracktion Engine |
| Flagship synth | Surge XT (pinned git submodule, built from source as a separate sub-build) |
| Free plugins | Surge XT, LSP Plugins, Dragonfly Reverb, Odin 2, OB-Xf, PeakEater, Six Sines, TIME-12, GATE-12: the projects' own releases, downloaded on request (pinned, SHA-256 checked; libcurl and libarchive on Linux, JUCE's `URL` and `plugins/ArchiveReader` on Windows) and loaded as CLAP plugins, or as VST3 where a project publishes no CLAP build (TIME-12, GATE-12). Development builds also compile Surge XT, LSP Plugins and Dragonfly Reverb from source for tests |
| Tests (C++) | Catch2 v3; `pluginval` for stock and bundled plugins |
| Language (AI) | Python 3.12, managed with `uv` |
| ML runtime | PyTorch: ROCm build for gfx1151 (TheRock packages), CPU fallback (§12.1) |
| IPC | JSON-RPC 2.0 over local TCP (127.0.0.1, with a token) |
| Secrets | API keys in the OS keyring (libsecret) |
| CI | GitHub Actions: an Arch Linux container, and a Windows (MSVC) build |

---

## 6. Repository layout

```
spinus-daw/
├── DESIGN.md                 ← this file
├── AGENTS.md                 ← short rules for coding agents (§18)
├── README.md
├── LICENSE                   ← AGPL-3.0 text
├── THIRD_PARTY.md            ← every dependency and model, with license
├── CMakeLists.txt
├── external/                 ← JUCE, tracktion_engine, Catch2, clap, fst, surge (submodules)
├── app/                      ← main executable, plugin host and plugin scanner programs
├── cmake/                    ← build modules (JUCE, Surge XT, free plugins, install rules, toolchains)
├── src/
│   ├── model/                ← Spinus Model schema, serialization, migrations
│   ├── commands/             ← Command API, undo grouping
│   ├── core/                 ← small shared utilities (ChildProcess; FileDialog, D-Bus; session logs)
│   ├── engine/               ← Engine Adapter (Model → Tracktion Edit), export, audio setup
│   ├── session/              ← the open project: recording, Freeze, autosave, presets, analysis steps
│   ├── project/              ← project folders and audio import
│   ├── plugins/              ← scanning, format hosts (CLAP, VST2), downloads, sandbox, Windows bridge
│   ├── ui/                   ← panels, windows and dialogs
│   ├── dsp/                  ← stock instruments and effects (each also builds as a plugin)
│   └── ai/                   ← AI service client and proposals
├── ai/                       ← spinus-ai Python package
│   ├── spinus_ai/            ← service, LLM gateway and routing, assistant tools, analysis and mixing
│   │   └── compute/          ← the analysis engine: PyTorch builds, locks, model weights, worker
│   └── tests/
├── docs/
│   ├── user/                 ← user manual (also searched by the assistant)
│   ├── knowledge/            ← music theory & production knowledge base (original writing)
│   └── dev/                  ← developer notes: command API, project format, real-time checklist, Windows build
├── packaging/                ← arch (PKGBUILDs), appimage, flatpak, windows (NSIS), share (desktop files)
├── tools/                    ← owner checks, lock and source generators, the showcase song
└── tests/
```

---

## 7. Data model

### 7.1 Concepts (FL-equivalent in brackets)

- **Project**: tempo, time signature, key/scale (optional, used by AI and autotune), sample rate, metadata.
- **Channel** [channel rack entry]: one instrument (stock or plugin) or an audio sampler; volume, pan, pitch, mute/solo; target **mixer insert**; an optional **group** name (FL's channel rack filter groups: the rack shows all, one group, or none; a group can play through a new bus, `insert.addBus`).
- **Pattern** [pattern]: named, colored, has a length in bars. Holds, *per channel*: a step sequence (one bar of its time signature in the grid view: 16 sixteenth steps in 4/4, or 24 with triplet steps, a per-pattern switch) and/or piano-roll notes. Also holds pattern automation. Its length follows its notes as in FL Studio: whole bars of its time signature, at least one; note and time-signature commands keep it fitted (`Project::fittedLength`), and Playlist clips that were as long as the pattern follow it.
- **Note**: start (ticks), length (ticks), pitch (0–127), velocity, release velocity, pan, fine pitch, per-note modulation by name (for CLAP note expressions; per-note volume is stored there as `volume`). The Piano Roll's lane edits velocity, release velocity, pan, fine pitch and volume, switched like FL's event lane. Pan, fine pitch and volume reach Spinus's instruments and CLAP plugins (as CLAP note expressions) on a MIDI channel per note; other plugins get plain notes, and release velocity reaches every instrument. This is MPE's layout (channels 2–14, pitch bend for fine pitch, CC 10 pan, CC 7 volume; `dsp/NoteExpression.h`), so the project format needs nothing new; more than 13 overlapping notes with values share channels and so share values. MPE controllers' live input uses the same path: per-note pitch bend (48 semitones), channel pressure and CC 74 on channels 2–16 reach Spinus's instruments (3x Osc: swell, filter) and CLAP plugins as tuning, pressure and brightness note expressions; recording keeps the notes only (§17).
- **Playlist** [playlist]: unlimited lanes. Lanes are *not* tied to channels (FL behavior). A lane can be grouped under another (`Lane::group`, nesting allowed, FL's track groups): a folded (`collapsed`) group hides its lanes in the Playlist (their clips drawn faintly on its row), and muting it mutes them (`Project::isLaneMuted`). Lanes contain **clips**:
  - Pattern clip: references a pattern, with start, length, offset.
  - Audio clip: references an audio file, with gain, pitch, time-stretch mode, fades. It plays through a **channel** (FL's audio clip channel; dropping a file on the playlist creates one, with its own insert). Files are copied into the project's `audio/` folder: at once in a saved project, on first save otherwise. Pitch, speed (time-stretch) and reverse per clip (§11.3).
  - Automation clip: a curve targeting any automatable parameter.
- **Mixer** (dynamic): Master plus any number of inserts.
  - Inserts are created on demand. A new project starts with Master only; the mixer always shows one empty "+" strip at the end.
  - Adding a channel creates and routes it to a new insert automatically (setting: "Auto-create insert for new channels", on by default). The user can also route several channels into one insert.
  - Each insert has a stable internal ID; its display number and position can be reordered freely without breaking routing, automation or sends.
  - "Remove unused inserts" command cleans up inserts with no inputs and no effects.
  - No hard limit; the UI stays usable with at least 500 inserts (virtualized strip rendering).
  - Each insert: any number of effects (at least 10 slots shown, always one empty; FL's fixed 10 is a common complaint), volume, pan, stereo width, mute/solo, routing to other inserts (sends with level), sidechain sources, name, color, icon.
- **Clip launcher**: grid of scenes × launcher tracks, reusing patterns and audio clips. Launcher tracks are not tied to channels (as lanes are not): a pattern clip plays on its pattern's channels, an audio clip through its channel. Each slot holds one clip with a length and a loop switch; launching quantises to the next bar, beat or at once. Launching stops the other clips of that launcher track; a channel with a launched clip stops playing the Playlist until "Back to the Playlist" (Ableton's rule). What plays is transport state, not part of the project. "Record to Playlist" (`session/LauncherRecorder`) notes each launched clip's song position while armed and writes a Playlist clip per pass, in one undo step, when playback stops.
- **Timing**: 960 PPQ internally. Tempo map supports tempo changes.

### 7.2 Mapping to Tracktion Engine

The Engine Adapter compiles the model into a Tracktion Edit:

- Each **channel** → a hidden Tracktion `AudioTrack` holding the instrument plugin.
- Each **pattern clip** on the playlist → `MidiClip`s on the tracks of every channel that has notes in that pattern, regenerated when the pattern changes.
- Each **mixer insert** → a submix/bus structure with the insert's effect chain. Channel tracks output to their insert. Inserts are added and removed at runtime without stopping playback.
- **Frozen** channels and inserts (Freeze) → their plugins are unloaded and a `WaveAudioClip` of the rendered audio plays on their track instead; the `Freezer` (`src/session/`) renders it in the background into `cache/freeze/`, named by a hash of what the target plays (the pruned project, sample rate and a format version), so nothing but the flag is saved and reopening a project finds its audio again. Export ignores Freeze and renders from the plugins, so exports are exact. An insert cannot be frozen when something inside it also plays elsewhere (an output, send or sidechain leaving it): that sound would be lost (`model::whyCannotFreeze`).
- **Launcher clips** → clips in Tracktion's clip slots on the tracks of the channels they play on, one slot index per launcher clip; launched together on one quantised beat. Spinus does not use Tracktion's scenes. Exports render the Playlist only (Tracktion's renderer turns slot clips off), so a jam reaches an export by recording it to the Playlist; frozen channels' launcher clips are silent until unfrozen.
- **Automation clips** → Tracktion automation curves on the target parameter. The adapter rebuilds each automated parameter's curve from all its clips after every change (song mode only; the value holds between clips, FL behaviour); device parameters are found by Spinus's parameter ID (§13).

The mapping supports editing a pattern while it plays, many clips of one pattern, adding/removing inserts during playback, sends, sidechain, and plugin delay compensation, all on Tracktion's high-level `Edit` API, with no clicks while the project changes during playback; `tracktion_graph` is not needed. The Tracktion Edit is never saved, only rebuilt from the Spinus Model. Details:

- Each **pattern clip** becomes one `MidiClip` per channel with notes in that pattern; a pattern edit rewrites only the clips of channels whose notes changed. One clip per placement rather than one flattened clip per channel: flattening rewrites every note of the channel on each edit, while a clip per placement keeps Playlist operations (move, slice, mute one clip) local.
- Each **insert** is an `AudioTrack` with no clips. **Sends** use `AuxSendPlugin`/`AuxReturnPlugin`, one bus per target insert.
- The transport loops only while it plays (`Session::applyLoop`): pattern mode loops the pattern (one bar when there is none), song mode its loop region; stopping turns looping off, and a song region cleared during playback becomes a loop over the rest of the song until it stops. Why: Tracktion's play head switches looping on before it stores the loop range, and its audio thread divides by that range's length; switched on while the play head still held an empty range (a new project's pattern mode, looping while stopped) it crashed Spinus with SIGFPE. Starting playback sets the range first. (If Tracktion fixes `PlayHead::setLoopRange` upstream, the rule can go.)
- The Engine Adapter keeps its own stable-ID → track map and re-asserts track-to-track routing after any track deletion, because Tracktion resolves those routes by track number (if Tracktion fixes that upstream, the step can go).
- `EngineHost` raises Tracktion's `EditLimits` (defaults: 400 tracks, 16 plugins per track) to 4,096 tracks and 64 plugins per track.
- The Engine Adapter (`src/engine/EngineAdapter`) syncs by comparing the project with the one applied last time, so it runs after every command. Tracktion recomputes "muted because the destination is muted" only when a mute/solo flag changes, so the adapter refreshes mute states after every sync. Tracktion has 32 aux buses, so at most 32 inserts can receive sends.

### 7.3 Project files

- A project is a plain folder `SongName/`, opened through the file in it:
  - `SongName.spn`: the model (JSON), with a `schemaVersion` field. Migrations live in `src/model/migrations/`.
  - `plugin-states/`: binary plugin state blobs, one file per plugin instance.
  - `audio/`: audio files used by the project (copied in on import, with a "keep external" option).
  - `cache/`: AI results, waveform peaks, Freeze audio. Never required to open the project; safe to delete.
- There are no single-file projects: a project is shared as its folder (no zip code to keep in step with the folder layout; samples in Sampler states and `audio/` make the folder self-contained). Save As never writes into a folder that holds other files.
- Autosave every 2 minutes to `SongName/.autosave/` (keep last 10), only when something changed. A never-saved project autosaves to its own folder under `~/.config/Spinus DAW/Unsaved/`, deleted when it is saved elsewhere or closed without saving.
- `ProjectDocument` (`src/session/`) ties the open Session to its folder: unsaved-change tracking (the command history's state ID plus a comparison of device states), the session lock, autosave, recovery and the recent projects list.

---

## 8. User interface

### 8.1 Layout

- **Top bar:** transport (play, stop, record, pattern/song mode toggle, tempo, time signature, metronome), CPU/DSP meter, master meter, current pattern selector.
- **Left dock:** Browser (stock instruments and effects, installed plugins with format badges `VST3`, `CLAP`, `LV2`, `VST2`, `WIN`, the user's sample folders (listed, searched by name, previewed through `engine::SamplePreview` outside the song, dragged to the rack, Playlist or Audio tracks; no samples are shipped), and projects (`ProjectList`, `src/session/`, kept in the user's settings like the sample folders, so not a project command): the shipped demo songs, every project opened or saved, and the projects in folders the user adds, searched on a thread of its own (six folders deep, skipping hidden folders and the inside of projects); opening one goes through the normal open path. Presets in the Browser are in §17.
- **Center:** dockable/floating panels: Channel Rack (instrument channels only), Audio tracks (the channels audio clips play through, `Channel::audioTrack`: vocals, stems, long audio; files over 20 s dropped on the rack go there), Piano Roll, Playlist (with the Piano Roll's editing: Draw/Paint/Slice/Select tools, a selection moved, resized or deleted as one undo step, a grid choice; Slice is `clip.split`), Mixer, the Routing panel (read-only: channels, inserts by distance from the Master, outputs, sends with levels, sidechains; click selects the insert in the Mixer), and the Automation editor (one automation clip, large: points snapped to a grid, bends drawn as Tracktion plays them via `model::automationValueAt`, Draw/Line/Paint/Select tools, shapes, values typed in the control's units). Any panel can be detached into its own window (FL behavior).
- **Right dock:** Assistant panel (collapsible).
- **Window layouts:** named sets of open panels and their places, with the Browser and Assistant (`ui::WindowLayouts`; `Workspace::getLayout`/`setLayout`, bounds as fractions of the workspace). Compose, Arrange and Mix are built in; the user saves their own or changes those. Kept in the user's settings (`layouts.xml`), not in projects; the layout at closing returns at the next start.
- **Bottom:** hint bar (FL-style: shows what the hovered control does).

### 8.2 FL-compatible shortcuts (default keymap; remappable)

| Key | Action |
|---|---|
| Space | Play/stop |
| L | Toggle pattern/song mode |
| F5 / F6 / F7 / F9 | Playlist / Channel Rack / Piano Roll / Mixer |
| Ctrl+Z / Ctrl+Alt+Z / Ctrl+Shift+Z | Undo / step back through history / redo |
| Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S | New / Open / Save / Save as |
| Ctrl+R | Export audio |
| Ctrl+Alt+1 / 2 / 3 | Compose / Arrange / Mix window layout |
| B / P / D / C / S / T / Z | Paint, pencil, delete, slice, select, mute, zoom tools |

### 8.3 Look

- Dark theme by default; light theme available. All colors via a theme file (JSON) so themes are user-editable.
- 60 fps target for playhead, meters and piano roll scrolling. UI must remain responsive with 200 playlist clips and 5,000 notes in view.
- HiDPI scaling (100–300%).
- File dialogs are the desktop's own (KDE's, GNOME's) on Linux through the XDG Desktop Portal, also in the Flatpak; Spinus's own file browser where no portal runs.

---

## 9. Audio engine requirements

- **Real-time rules (enforced by review checklist and tests):** no heap allocation, locks, file I/O, logging, or system calls on the audio thread. Use lock-free FIFOs for UI↔audio messages.
- Sample rates 44.1–192 kHz; buffer sizes 32–2048 samples; 64-bit floating-point mix bus.
- Plugin delay compensation across all paths including sends and sidechain.
- Recording: audio (mono/stereo, multiple inputs) and MIDI (to piano roll, with optional quantize). MIDI: every hardware MIDI input is routed to the selected channel's track with monitoring on; `MidiRecorder` (`src/session/`) timestamps notes with Tracktion's stream-to-timeline mapping (loop and latency included) and writes them into the current pattern as one `pattern.setNotes` command when recording stops. Tracktion's own clip recording is not used, since the model is the source of truth. Audio takes: a recording goes on a new audio track, or into one (Audio tracks › Record into); over audio already there it becomes another take (`clip.addTake`: a lane "Take N" grouped under the first take's lane, the same channel, the earlier takes' overlapped parts cut and muted). Comping is `clip.useTake` (Alt+click on the Playlist): that clip plays and the channel's other clips are muted where it plays, cut at its edges; with the Slice tool across the take lanes this gives per-phrase comping. Takes are plain audio clips, so they play, export and freeze as any. Multi-track recording: every sound card input in use is one Tracktion recording input, so all takes come from the same audio callback, sample-aligned; armed audio tracks (`Channel::recordInput`, `recordStereo`, `recordArmed`) each record their own input into their own take in the same audio callback, placed on their tracks as one undo step.
- MIDI input: any ALSA/JACK MIDI controller; MIDI learn for any parameter.
- **Export:** WAV (16/24/32f), FLAC, Opus, MP3 (the `lame` program), AAC (`ffmpeg`, when installed); full mix, current pattern, selected range, or stems per insert; optional normalization and dither. Offline by default (`engine::ExportJob`, its own Edit). The mix and all stems come from one render: a `StemCapturePlugin` last on each stem's insert writes what the insert puts out, placed by the time Tracktion gives it (latency included); a stem is the insert's output in the song without the Master (a return's stem is its wet sound), solo is ignored (a second render when something is soloed). The Master's mute is a `MutePlugin` after its effects (the Master is no Tracktion track); a muted Master exports a silent mix, not silent stems. Or in real time while the song plays (`LiveExport`: an `OutputCapturePlugin` last on the Master, applying the Master fader that Tracktion runs after the list, feeds a lock-free FIFO that a writer thread drains; song only, at the device's rate).
- Default audio setup: the JACK backend when a JACK server (normally PipeWire's) is reachable, otherwise ALSA on the `pipewire`/`default` device; Spinus never starts `jackd` itself. Default buffer **256 samples at 48 kHz**; 128 recommended for low-latency work; 64 drops out on ordinary PipeWire setups. PipeWire reports the device's latency as one buffer, so through JACK the output latency is about two buffers (5.3 ms at 128); PipeWire's ALSA plugin keeps three periods, so ALSA has more latency at the same buffer size. The settings dialog shows total output latency (buffer + device latency reported by the driver), an xrun counter, a warning when another device clocks the output, and measured round-trip latency where a loopback is available.

---

## 10. Plugin hosting

### 10.1 Formats

| Format | How | Notes |
|---|---|---|
| VST3 (Linux) | JUCE built-in host | Primary format |
| LV2 | JUCE built-in host (Linux) | |
| CLAP | Spinus-written `AudioPluginFormat` using the CLAP SDK (MIT) | JUCE 9 has no CLAP hosting; `src/plugins/clap/`. Owning it also lets Spinus use CLAP features JUCE would not expose (note expressions, preset discovery); if JUCE gains CLAP hosting, compare and consider switching. The free open-source effects load through it. Plugin windows (X11 embedding with timer and fd support) and a sidechain input bus are implemented |
| VST2 (Linux) | JUCE built-in host compiled against **FST**, a documented reverse-engineered GPL-3.0-or-later header | Steinberg's VST2 SDK is not used. JUCE assumes 8 inline `VstSpeakerArrangement` entries where FST declares a flexible array, so the build compiles against a generated copy of `fst.h` with `speakers[8]` (configuration fails if FST changes that line). Spinus names the format but never calls itself "VST compatible" (a Steinberg trademark) |
| Windows VST2/VST3/CLAP | yabridge shims loaded as the native formats above | §10.3 |

### 10.2 Scanning and stability

- Scanning runs **out of process**. A plugin that crashes or hangs during scan is blacklisted and shown in a "Problem plugins" list with a retry button.
- **Invalid-audio guard:** Spinus checks every hosted plugin's output each block. A block containing NaN or infinity is replaced by silence and the plugin is flagged ("Plugin produced invalid audio"), so one plugin cannot silence or damage the whole mix. The check is real-time safe.
- Plugin states are saved on every project save. If a plugin fails to load, the project still opens; the slot shows "Missing: <name>" and keeps the saved state so nothing is lost.
- **Plugins in a separate process** (offered after a crash rather than used for every plugin, since it costs a block of latency and a process per plugin): when a third-party plugin crashes Spinus, the crash guard notes which plugin's code crashed, and the next start offers to run that plugin in `spinus-plugin-host`; a switch in the plugin's menu changes it per plugin. There a crash or hang only switches the plugin off (effects pass audio through, instruments go silent) with a **Reload** offer; audio crosses through shared memory with one block of extra latency, and the plugin's window is embedded in Spinus's (on Linux; on Windows such a plugin has no window yet, §17). No sidechain input, MIDI output or more than eight channels there. Windows plugins on Linux are also out of process through Wine. The crash guard (`engine/PluginCrashGuard`) notes the libraries each plugin loads and looks for them on the crashing stack; it cannot tell plugins apart in shared code they both load, and `dladdr` in a signal handler is not formally async-signal-safe (it can only hang if the crash came while a library was loading).

### 10.3 Windows plugin bridge ("it just works")

Goal: the user never sees Wine, prefixes or yabridgectl.

- Spinus manages its own Wine prefix (its "Windows folder") at `~/.local/share/Spinus DAW/windows/prefix`, its yabridge files in `windows/bridged/`, and its own runtime (if downloaded) in `windows/runtime/`.
- Wine and yabridge are optional: nothing is needed or downloaded until the user adds a Windows plugin. Then Spinus uses the Wine and yabridge already installed **if they pass Spinus's test** (loading a small test plugin through them); otherwise it offers a **pinned, tested Wine and yabridge** to download (Wine Staging 9.21 and yabridge 5.1.1, the pair yabridge supports; the user agrees first; pinned and checksum-checked like the free plugins, §11). Wine stays at 9.21 until yabridge supports newer Wine releases. Spinus's own runtime is used only by Spinus: it goes first on Spinus's `PATH` and in `WINELOADER`. The test loads a pinned Windows build of Dragonfly Room Reverb through Spinus's out-of-process scanner. Because yabridge compatibility breaks across Wine releases, the test runs again whenever Wine's version changes, and the health check has a **Retest** button and a choice to use the pinned versions.
- Spinus creates yabridge's chainloader files itself (no `yabridgectl`), so the user's own yabridge setup is never touched. VST2: the VST2 chainloader copied as `<name>.so` beside a link to `<name>.dll`. VST3: the VST3 chainloader as `Contents/x86_64-linux/<name>.so` inside the Windows bundle. CLAP: the CLAP chainloader copied as `<name>.clap` beside a link `<name>.clap-win`. yabridge finds the Wine prefix from the `drive_c` folder that holds the Windows file.
- **Add Windows plugin** dialog:
  1. User picks a Windows installer `.exe` *or* a plugin file (`.dll`, `.vst3`, `.clap`).
  2. Installers run inside the managed prefix; Spinus watches the standard plugin folders inside the prefix.
  3. Spinus writes yabridge's files into its bridged folder (a chainloader copy plus a link to the Windows file, per plugin), scans them, and they appear in the browser with a `WIN` badge. 64-bit plugins only.
- **Health check** screen: **Retest** button, Wine runtime OK (installed or pinned), prefix OK, yabridge version, real-time privileges (locked-memory limit; on Arch the `realtime` group from `realtime-privileges`), per-plugin status, "repair" button.
- The window lists the plugins found in the prefix, each with **Remove** (asks first; refused while the open project uses it; deletes the plugin's file in the prefix and its bridge, and the Browser drops it). A switch draws plugin windows without the graphics card (Wine's Direct3D renderer `gdi`), for plugins drawing with Direct2D whose windows do not update on some drivers.
- **Flatpak**: it shares the native Spinus's Windows folder but can run only Spinus's own Wine (as `wine64`; Wine's 32-bit starter, for 32-bit installers and their helpers, runs with Flatpak's 32-bit libraries, the `org.freedesktop.Platform.Compat.i386` extension, and the `--allow=multiarch` permission (without it the sandbox kills 32-bit programs); and without it Spinus refuses a 32-bit installer with the command that installs it), with its own runtime choice and test; `/tmp` is shared so both use one Wine server per prefix. Its plugin list is its own; the system's plugins are read at `/run/host/usr` and kept in projects as `/usr/...`, so a project opens in both builds. A system plugin that does not load with the runtime's libraries but does with the computer's (found by the scan, which retries it with them; the list is kept in `plugins-with-system-libraries.json`) always runs in a separate process, started by `spinus-plugin-host-system` through the computer's own loader (`/run/host/usr/lib/ld-linux-x86-64.so.2 --library-path <its library folders>`) with the computer's OpenGL drivers; this needs a computer C library at least as new as the runtime's (`plugins/HostLibraries.h`). Plugins that load with the runtime's libraries stay in Spinus's process. Process numbers mean nothing across the sandbox, so a project's lock is held with `flock` on Linux.
- **A plugin's own windows**: a Windows plugin may ask something in a window of its own (registration, licence) from inside a call Spinus makes on its message thread, and waits for the answer. While Spinus's message thread does not respond for a second (a heartbeat), or a `PluginDialogs::ScopedWait` is open (the scanner), a background thread with its own X11 connection activates the managed Wine windows of processes Spinus started (`_NET_ACTIVE_WINDOW`, source 2, which focus-stealing prevention lets through) and notes the wait in Spinus's window titles. A window is activated again only while one of Spinus's windows (or none) is active, so a browser the plugin opened keeps the front. X11 only; the user's own Wine programs and embedded plugin editors are never touched.
- Known limits (shown in the UI, not hidden): some copy protection (e.g. iLok) does not work under Wine; some plugin GUIs misbehave.

---

## 11. Stock instruments and effects

Stock devices come from two sources:

- **Free open-source plugins** (Surge XT, Surge XT Effects, LSP Plugins, Dragonfly Reverb, Odin 2, OB-Xf, PeakEater, Six Sines, TIME-12, GATE-12): not shipped with Spinus. On first launch (and from Options > Get free plugins, or the Browser) Spinus downloads the projects' own pinned releases for the user's system into the user's data folder, or uses copies installed by the system's package manager. Only projects whose source and published build are both open source, published as plain archives, are offered, and each system lists only the packs published for it (LSP Plugins and Odin 2 are Linux-only). Spinus distributes no third-party plugin binaries or sound content this way, and the package stays small and quick to build. Not offered for those rules: Vital (account-gated), OB-Xd and the CHOW plugins (installers only), Vitalium and Airwindows Consolidated (no pinnable release). Not offered after testing: RipplerX 1.5.19 (NaNs and a crash under pluginval's automation test and Spinus's random-parameter test), Resonarium 0.1.1 (a beta; its Windows zip names its files with backslashes, and one held note takes over 40 % of a 128-sample block). Pinned URLs can disappear upstream: the hidden `[.network]` and `[unpack]` tests must run when a pin changes. They load in-process like stock devices, under their own names with credits: as CLAP where the project publishes a CLAP build, otherwise as VST3 (a pack's file is then a `.vst3` bundle folder). Because JUCE's VST3 host and the plugin decide when its library is unloaded, Remove counts the devices made from a pack (`engine::isBundledDeviceOpenFrom`) rather than loaded libraries. Projects name them by stock type (`bundled.*`), so a project opens with them missing until they are downloaded. The same window removes a pack Spinus downloaded (after asking; not packs the system installed), and refuses while the open project uses it or any copy of it is loaded, on every system (a loaded plugin's file cannot be deleted on Windows, and pulling it from under a running plugin is unsafe anywhere).
- **Spinus-built devices**, for what no bundled plugin provides in the form Spinus needs, and for the basic mixing effects (EQ, compressor, limiter, gate), which must work on every platform without a download. Each is built as an internal processor **and** as a standalone VST3/LV2 plugin (useful for testing with `pluginval`, and for users of other DAWs). Spinus hosts them through an internal plugin format ("Spinus", identifiers like `spinus:sampler`, code in `src/dsp/`), so state, windows and automation work as for any plugin. The Sampler keeps its samples inside its state (FLAC, 24 bits), so they travel with the project, its autosaves and undo with no extra code, at the cost of larger states. The standalone builds are VST3 and LV2 (CLAP builds would need clap-juce-extensions, MIT; §17).

### 11.1 Instruments

| Device | Purpose | Key features |
|---|---|---|
| **Surge XT** (free download) | The one serious synth | Downloaded on request from the Surge project's pinned release, with its factory sounds, and loaded in-process like a stock device. Appears first in the browser's Instruments list, under its own name with credits. Comes with Surge XT's full factory library. Opens on Surge XT's own editor, as its makers designed it: presets are picked in its own patch browser and its macros are in its window. |
| **Odin 2** (free download, Linux) | Second synth with character | TheWaveWarden's GPL-3.0 synth: analog-style, wavetable, FM and chiptune oscillators, factory presets built in |
| **OB-Xf** (free download) | Analog-style polysynth | The Surge Synth Team's GPL-3.0 successor of OB-Xd; its factory patches are not downloaded (they live outside the pack's folder) |
| **Six Sines** (free download) | FM synth | Paul Walker's six-operator phase/FM synth (code MIT, builds GPL-3.0); factory patches built in, picked in its own window (it has no CLAP preset discovery, so closest preset does not list them) |
| **3x Osc** | Quick, simple synth (FL 3xOsc equivalent) | 3 oscillators (sine/tri/saw/square/noise; saw and square band-limited with PolyBLEP), coarse/fine tune, level, pan, phase; amp envelope; filter with envelope amount; 16 voices; 23 factory presets built in |
| **Sampler** | Play any sample | One-shot and multi-sample, root note, ADSR, filter, loop points, round-robin |
| **Drum Machine** | Drum kits | 16 pads, choke groups, per-pad tuning/ADSR/filter, kit presets. Pads play from C5 up (the Channel Rack's step note). New instances start with the synthesized "Spinus Synth Kit"; kits save as `.spinuskit` files. Factory sample kits are planned with the starter sample pack (§11.4, §17) |
| **Beat Slicer** | Chop loops | Auto-slice by transients or grid, each slice mapped to a key, export slices to pattern |

**Surge XT integration rules**

- Users get the Surge project's own Linux release (CLAP), with its factory content as `SurgeXTData` beside the plugin file. Development builds compile it as a separate CMake sub-build (it pins its own JUCE fork) for tests.

- Spinus does not fork Surge XT's DSP. Patches to Surge XT go upstream where possible; any local patch would live in `external/patches/surge/` with a reason (there are none).
- **No Spinus wrapper**: Surge XT's window is its own editor, like any plugin, with no Spinus panel, preset list or styling around it. The same holds for every free plugin Spinus suggests: one way to use them, the one their own manuals describe; Spinus's own devices keep Spinus's look. Spinus lists Surge XT's patch files only for closest preset and sound matching.
- The AI sound-matching feature (§12.7) targets **3x Osc, Surge XT, Odin 2 and OB-Xf** with profiles
  of Spinus's own, and any other instrument plugin experimentally.
- Replacement path: if Spinus ever ships its own synth, Surge XT stays available so existing projects still open.

### 11.2 Effects

The basic mixing effects are Spinus-built so they work on Linux, Windows and any later platform with nothing to download; LSP Plugins (Linux-only downloads) stays as an optional extra. The rest is curated from the free open-source plugins where possible. Only plugin modules that pass `pluginval` and the random-parameter fuzz test are offered. Spinus loads them as CLAP where they have a CLAP build: their CLAP state restores exactly, while VST3 state reads a few parameters back differently.

| Group | Device | Provided by |
|---|---|---|
| Core | Parametric EQ (8 bands: bell, shelves, cuts 12/24/48 dB, notch; draggable curve over a live spectrum; sections matched to their analog prototypes, Vicanek-style) | **Spinus-built** (`spinus.eq`); LSP Parametric Equalizer optional |
| Core | Compressor (soft knee, peak or RMS, auto makeup, parallel mix, sidechain) | **Spinus-built** (`spinus.compressor`); LSP Compressor optional |
| Core | Limiter (look-ahead reported as latency, true peak, never above the ceiling) | **Spinus-built** (`spinus.limiter`); LSP Limiter optional |
| Core | Gate (hysteresis, hold, range, sidechain) | **Spinus-built** (`spinus.gate`); LSP Gate optional |
| Core | Reverb | Dragonfly Hall, Room, Plate and Early Reflections; Surge XT Effects reverbs |
| Core | Delay (sync, ping-pong) | Surge XT Effects: Delay (tempo sync; ping-pong via crossfeed) |
| Character | Saturation/Distortion | Surge XT Effects: Distortion, Waveshaper, Tape, CHOW, Bonsai; PeakEater (clipper) |
| Character | Chorus, Phaser, Flanger | Surge XT Effects (plus Ensemble, Rotary) |
| Character | Volume shaper / trance gate (tempo-synced, drawn) | GATE-12 (VST3) |
| Character | Delay-time shaper: stutter, tape stop, scratch (tempo-synced, drawn) | TIME-12 (VST3) |
| Character | Filter (with LFO and envelope follower) | **Spinus-built** |
| Mix & master | Multiband Compressor | LSP Multiband Compressor |
| Mix & master | Sidechain ducker (one-knob) | **Spinus-built** |
| Mix & master | Spectrum Analyzer | LSP Spectrum Analyzer |
| Mix & master | Stereo Utility (gain, pan, width, mono, phase) | **Spinus-built** |
| Mix & master | Loudness Meter (LUFS momentary, short-term, integrated; true peak; BS.1770-4 / EBU R128) | **Spinus-built** |
| Vocal | **Spinus Tune** (autotune, §12.6) | **Spinus-built** |

The browser shows these devices in a "Free open source" group with their own names and credits, marked when not downloaded yet. LSP Chorus, Flanger, Phaser, Dynamics Processor and Clipper are **not** offered: they produced invalid audio or crashed under random parameter changes in testing.

### 11.3 Audio clip tools

Time-stretch and pitch-shift on audio clips (Rubber Band). `AudioClip` has `pitchSemitones`, `speed` and `reversed`, and `stretchMode` `off` (as recorded), `stretch` / `stretch-drums` (Rubber Band melodic / percussive: pitch and speed apart) or `resample` (speed times the pitch ratio, like a record); `model::getPlaybackSpeed`. `clip.setAudio` changes them, with `fit` keeping the same audio in the clip (length, offset and fades scale with the speed). Tracktion is built with `TRACKTION_ENABLE_TIMESTRETCH_RUBBERBAND` against the system library (a generated forwarding header) and stretches in real time (`setUsesProxy (false)`: heard at once, ~4% of the audio time per stretched clip); a reversed clip plays from Tracktion's rendered copy, which exports wait for. The Playlist: right-click Reverse / Pitch / Speed / Pitch and speed, Alt+drag the end to stretch. The `tempo-sync` mode plays as `off`; tempo-sync and slicing audio clips by transients are in §17 (the Beat Slicer already slices by transients).

### 11.4 Factory content

No sample pack is shipped. Planned (§17): a small royalty-free starter sample pack (drums, one-shots, a few loops), with license recorded in `THIRD_PARTY.md`. Size budget: ≤ 300 MB.

The demo song *Siskin* ships with every package as a read-only project folder (`demo/Siskin/`, about 4.5 MB, CC BY 4.0 "Spinus DAW contributors" while the code stays AGPL-3.0-or-later). It is made by `tools/showcase-song` through the Command API from Spinus's own devices and the free plugins Spinus downloads on request (§10, Surge XT, Surge XT Effects, Dragonfly; all for Linux and Windows), with its two sound effects synthesised by `make_audio.py`; the generator strips this computer's paths from the plugin states. Installed at `<prefix>/share/spinus/demo/` (Arch, AppImage, Flatpak) or beside `spinus.exe` in `demo\` (Windows), and found from the program's folder (`src/session/DemoSong.h`). Every project folder in the demo folder is a demo song, listed in the Browser's Projects as "*Name* (demo)", Siskin first (§8.1). Opening one never opens or writes the installed folder: it is copied to the user's Music folder (`Music/<Name>`) and opens the copy through the normal open path; an existing copy is opened again as it is, so the user's changes are kept (a fresh copy is had by renaming or deleting it). Missing free plugins are offered on opening, like any project's. `spinus --self-test` checks the demo songs are installed and read.

---

## 12. AI subsystem

**Off until the user turns it on.** A new Spinus has every AI feature
switched off: no AI service starts, no model is downloaded, nothing is sent anywhere, and AI menus and
panels only offer to set AI up. The user turns AI on in Options › AI, which also asks for a connection
(a provider key or a local model server, §12.3). Turning it off again stops the service and hides the
features; projects keep working unchanged either way.

### 12.1 spinus-ai service

- Started by Spinus only when AI is turned on (above), and then lazily, when an AI feature is first used. Listens on 127.0.0.1 on a port of its choosing and answers only Spinus, which proves itself with a token passed in a private file: any program can connect to a local port, and the service holds the user's provider keys. One transport for Linux and Windows. Spinus starts it as `python ai/run_spinus_ai.py --log <file>`, so even import errors reach the log, and with `--session-log <run>-ai.log` for its part of the session log (§14), redacted the same way; requests' parameters (what the user asked) are never logged.
- JSON-RPC methods are versioned (`ai.v1.stems.separate`, …). Every long task is a **job** with id, progress events, cancel, and result.
- Models are downloaded on first use to `~/.local/share/spinus/models/` with checksum verification and a visible size/progress prompt. Nothing is downloaded without the user agreeing.
- Results are cached in the project `cache/` folder keyed by audio-file hash and model version. The exception is the **analysis library** (§12.7): what the Analyzer and the Track creator learn about a song describes the song, not a project, so it is kept in the settings folder.

**Compute device**

- The official PyTorch ROCm wheels from pytorch.org **do not support gfx1151**. Use AMD's TheRock pip-packaged ROCm + PyTorch builds from the **stable** multi-arch channel (index `https://stable.repo.amd.com/rocm/whl-next/`, `torch[device-gfx1151]`), which need no system-wide ROCm install and no `HSA_OVERRIDE_GFX_VERSION`. The older per-family nightly index is legacy. GPU access needs the user in the `render` and `video` groups; the first GPU run takes about 10 s longer while kernels compile.
- The GPU's main benefit is that it leaves the CPU free for the audio engine (Demucs is only about 1.7 times faster there on gfx1151, but a CPU separation takes every core).
- **CPU fallback** uses the pytorch.org CPU wheel in its own venv, not the TheRock build (which is a much slower CPU build: OpenBLAS instead of MKL, and one thread per logical CPU; if it ever runs on the CPU, cap its threads at the physical core count). CPU jobs run at low priority with fewer threads than physical cores while the transport plays, to keep the audio engine free.
- GPU kernel caches (MIOpen, comgr) go under `~/.cache/spinus/gpu/`; `cudnn.benchmark` stays off.
- Pin exact torch builds in `ai/spinus_ai/compute/locks/<build>.txt` (written by `tools/compute-locks.py`), one per GPU family and vendor: each needs a different index and build, pytorch.org's ROCm wheels skip some cards (gfx1151 among them), and an unpinned upgrade can break a GPU without warning. Upgrading them requires re-running `tools/owner-checks/ai-gpu-benchmark.sh` and `tools/owner-checks/gpu.sh`.
- Do **not** set `PYTORCH_HIP_ALLOC_CONF`; it is reported to crash PyTorch on gfx1151.
- **Other GPUs:** every GPU target TheRock's stable channel builds (AMD Radeon RDNA 1-4), pytorch.org's CUDA 13 and CUDA 12 builds for Nvidia, and its XPU build for Intel Arc and Core Ultra graphics. Each is pinned with hashes in `ai/spinus_ai/compute/locks/`; only gfx1151 and the CPU build are tested, the others are labelled "untested". Nvidia's and Intel's libraries inside their builds are not open source: Spinus never distributes them; the user's computer downloads them from PyPI and pytorch.org after a question that says so.
- **Analysis engine:** PyTorch runs in a worker process started from the chosen build's environment (`spinus_ai.compute.worker`), not in the AI service, so a GPU crash or a stopped job never takes the service down.
- Device selection order: the GPU build suited to the detected GPU (Nvidia, then AMD, then Intel) → CPU. A startup self-test runs one tiny tensor operation on the GPU; on any failure (e.g. memory access fault) Spinus falls back to CPU for that session and says why in Options › AI › Analysis engine.
- Every model must have a working CPU path: the CPU build is the fallback for every GPU problem, and on a modern many-core CPU it is not far behind an integrated GPU.
- On GPUs with unified memory large models fit; the AI service still caps its own memory use (default 16 GB, configurable) so the DAW and plugins are never starved.
- Options › AI › Analysis engine shows: detected GPU, the suggested build and the CPU build (sizes, tested or not, Install/Remove), which one is in use, the self-test result ("Test"), and "Use the CPU only".

### 12.2 Model registry (license policy in §3; every model used has a `THIRD_PARTY.md` row)

| Task | Default | Notes |
|---|---|---|
| Stem separation | Demucs htdemucs (4-stem), htdemucs_6s | MIT (project-level). Optional "Many instruments": MVSep Mega 53 stems (BS-RoFormer, MIT) |
| Polyphonic audio → MIDI | Spotify Basic Pitch via ONNX Runtime (CPU) | Apache-2.0; ONNX Runtime MIT. Its inference is ported into `spinus_ai/transcribe.py`, since the package demands TensorFlow, which has no Python 3.12 build |
| Drum transcription | Spinus in-house: onset detection + a small kick/snare/hat/tom/cymbal classifier trained on the user's computer from Spinus's own synthesised drum sounds, no downloaded weights | Per-user calibration for beatbox |
| Vocal pitch tracking | pYIN with NumPy | RMVPE not used (unclear weights origin) |
| Beat, tempo, downbeat | Beat This! (MIT code and weights), without the madmom DBN | |
| Key detection | Chroma and Krumhansl-Kessler profiles, NumPy | Essentia's MTG models are non-commercial and not used |
| Timbre similarity | LAION-CLAP audio encoder (`laion/larger_clap_music`, Apache-2.0, through transformers) | Per-note vectors ranked together with Spinus's own spectral measurements. Published only as a pickle: checked by SHA-256, then loaded with `weights_only=True` |
| Manual search | Keywords (BM25); optional local embedding models | bge-small, all-MiniLM, bge-base, multilingual-e5-small (MIT or Apache-2.0); meaning and keyword results combined by reciprocal rank fusion, so exact terms still count |

Rejected, so they are not proposed again: the madmom models, Essentia's MTG models, ADTOF and its PyTorch port, LarsNet, MERT and MuQ (non-commercial weights); MS-CLAP (MS-PL weights, GPL-incompatible); BEATs, Omnizart's drum model, Magenta's E-GMD drums, the viperx/UVR RoFormer weights and `basic-pitch-torch` (no license stated); EnCodec (weights license ambiguous).

### 12.3 LLM gateway

**Providers** (one interface: `complete(messages, tools, schema) → result`, streaming supported):

| Provider | Use |
|---|---|
| Anthropic (Claude) | Default for assistant and generation |
| OpenRouter | One key for many models (Claude, GPT, open models) via its OpenAI-compatible API |
| OpenAI-compatible endpoint | Any other compatible service |
| Local | Ollama or llama.cpp server; practical on computers with plenty of memory (or a GPU with unified memory) |

**Keys and profiles**

- The user can store **multiple keys**, each as a named *connection* (provider + key + optional base URL), e.g. "Anthropic personal", "Anthropic work", "OpenRouter". Keys live in the OS keyring; projects and settings files never contain keys.
- **Per-feature model routing** (`spinus_ai/routing.py`): Options › AI picks the connection and model for the assistant and, optionally, for the pattern generator (otherwise the assistant's); the automixer's review uses the "mixing" route. Manual search uses keywords or a local embedding model (no API cost).
- **Fallback chain**: when a connection is down, busy or refuses the key, the other connections are tried in the order they were added; the UI shows which model actually answered.
- **Usage tracking**: tokens per month, connection and feature, with the cost where the provider reports it. A monthly limit per connection is in §17.
- All AI features that need an LLM degrade gracefully (clear message) when no connection is configured.

### 12.4 Feature: AI pattern generator / adjuster (F3)

- **Generate:** user selects one or more channels and gives: style prompt (free text), length, key/scale (defaults to project), density, and optionally a reference pattern.
- The LLM returns **structured note data** (JSON schema enforced), then deterministic post-processing: snap to scale (optional), quantize/swing, velocity humanize, range limiting per instrument type.
- Results appear as **ghost notes** in the piano roll/step sequencer. Buttons: Accept, Regenerate, Variations (×4), Discard.
- **Adjust** existing patterns with commands such as "more syncopated", "simplify", "add a fill in bar 4", "humanize", "transpose to relative minor", "make the bass follow the kick".
- **How it works:** the Piano Roll's Generate window (request, channels, 1 or 4 variations, swing,
  humanize; write new or change existing notes) → `ai.v1.pattern.generate` → `spinus_ai/patterns.py`. The
  model answers with one `write_pattern` tool call per variation (sixteenth steps, scientific note names
  or drum pad names, given the other channels' notes); the service keeps notes on the grid and inside
  the pattern, snaps pitched notes to the key, maps drum names to the Drum Machine's pads, then swings and
  humanizes (seeded). Length is the pattern's (place a note in a later bar to lengthen it). Its own route: the "patterns" feature,
  falling back to the assistant's. Per-instrument range limiting is left to the model (§17).

### 12.5 Feature: AI automixer (F4)

1. **Analyze** every insert: integrated LUFS, peak, crest factor, spectral balance, stereo width, and a **masking matrix** (which inserts compete in which frequency bands).
2. **Plan:** rules plus the LLM produce a `MixPlan`: gain staging, pan, EQ cuts/boosts, compression, reverb/delay sends, sidechain suggestions, and a master chain targeting a chosen loudness (e.g. −14 LUFS streaming, −9 club).
3. **Preview:** the plan is shown as a list of changes with reasons ("Cut 250 Hz on Pad: masks Bass"). User can untick items.
4. **Apply** as one undo step, with an **A/B toggle** comparing before/after at matched loudness.
5. Optional: match the mix's tonal balance to a dropped-in reference track.

**How it works:** `ui/Automixer` (View › Automixer) renders every insert and the mix with the exporter's stems, `ai.v1.mix.analyze` measures them (`spinus_ai/mixing.py`: BS.1770-4 loudness, true peak, crest, 7 bands, width, masking), and `ai.v1.mix.plan` (`spinus_ai/mixplan.py`) makes items from rules (headroom, masking cuts, high-pass, gentle compression, reference tone, loudness target) that an LLM may review (checked and limited; the rules' plan stands without a connection). Ticked items become one proposal of commands (`insert.set`, `insert.addEffect` with the Spinus EQ, Compressor and Limiter, `device.setParameters`): one undo step. A/B is undo and redo of that step with a listening-only trim on the master (`EngineAdapter::setMonitorTrim`) matching the loudness. Pan, reverb and sidechain suggestions are not made (§17).

### 12.6 Feature: Spinus Tune (autotune) (F6)

Two modes, two implementations:

- **Real-time mode** (C++ stock plugin, no Python): low-latency pitch detection (YIN family) and pitch shifting (PSOLA) with key/scale, retune speed (hard "robot" to natural), humanize, formant preservation, and MIDI-controlled target notes. Latency target ≤ 10 ms at 48 kHz. **How it works:** `src/dsp/tune/`, 10 ms delay ("Normal" range; a "Low voice" range takes 20 ms); target notes come from a channel chosen per insert (`Insert::notesFrom`), whose MIDI clips the adapter copies onto the insert's track. The range is not automatable, because it changes the reported delay.
- **Graphical mode** (Melodyne-lite): vocal clip analyzed by spinus-ai into note blobs with pitch curves; user drags notes, adjusts drift and vibrato; rendered offline in C++ with formant-preserving shifting. **How it works:** `AudioClip::tune` (`clip.setTune`), `ai.v1.listen.vocal`, `engine/TuneRender` (Rubber Band R3, formant-preserving, pitch scale per 256 samples), the `Tuner` (renders in the background into `cache/tune/`, like the Freezer) and `ui/SpinusTune` (opened by double-clicking an audio clip).

### 12.7 Feature: Track analyzer (F5)

User drops a song into the Analyzer panel. Pipeline, each step shown with progress:

**Experimental:** the Analyzer is shown only while **Options › AI › Show experimental features** is on (off by default; kept with "AI on" in the AI service's `settings.json`). While it is off the panel is unavailable in the `Workspace` (hidden, and no menu, shortcut or layout opens it); turning it on or off takes effect at once. The Track creator (§12.10) is gated the same way. The analysis engine runs on Linux only; on Windows the Analyzer's stems, tempo and key are not available (§17).

1. **Tempo, beat grid, key** detection. Offer to set the project tempo/key.
2. **Stem separation** (vocals, drums, bass, other; 6-stem when available: + piano, guitar).
3. User **selects stems** to process.
4. For each selected stem, **transcribe** to a pattern (drums → drum machine steps; melodic → piano roll notes).
5. For each selected stem, **recreate the sound**, user picks one per stem:
   - **Sample-based (exact):** slice the isolated stem into the Sampler / Drum Machine (kicks, snares, one-shots, chopped phrases). Most faithful result.
   - **Synth match (approximate):** optimize a synth's settings, as named by a synth profile, to match the stem's timbre: Spinus's profiles for **3x Osc**, **Surge XT**, **Odin 2** and **OB-Xf**, the user's own for any synth, and for any other instrument plugin an automatic profile picked from its parameters' names (experimental). A hidden copy of the synth inside Spinus renders each try (a plugin that crashes while being tuned takes Spinus down; running the tries in a separate process is in §17); a sweep and then sep-CMA-ES over a multi-resolution spectral and loudness distance, starting from the closest preset (Surge XT) or the synth's default sound. Expect "good starting point", not a clone.
   - **Closest preset:** rank factory presets and the user's installed instrument presets by timbre-embedding similarity: Surge XT's patches, OB-Xf's factory patches and Odin 2's built-in presets (given to the plugin as its state, since neither loads presets for a host), and for any other instrument the presets it lists through CLAP preset discovery or its programs (experimental). Whether a plugin has presets or enough settings to tune is known only once it is loaded, so it is decided when the user asks: no plugin is loaded just to fill a list (that could crash on every plugin the user has).
6. Output: new channels and patterns in the current project, grouped and colored under an "Analysis: <song>" folder, plus, per stem as the user chooses, the stem itself as an audio clip on a Playlist lane of its own (or nothing).

The panel shows a short notice that analyzing copyrighted songs is for study and personal use; the user is responsible for rights when releasing.

**Analysis library** (`session/AnalysisLibrary`, owner decision 2026-10-26), shared by the Analyzer and the Track creator (§12.10): what Spinus learns about a song is kept in the settings folder (`ai/library/<first 20 hex digits of the song's SHA-256>/`), never in a project, so a song renamed, moved or used in another project is recognised. `song.json` (with a `version`; an entry of another version is ignored and listed for deletion) holds the tempo and beats, the key, the loudness, the notes and drum hits per separation (`"<model>/<stem>"`, in seconds) and the Track creator's style features; `stems/<model>/<stem>.flac` the stems, as 24-bit FLAC (JUCE writes it, no new dependency; smaller than the 32-bit float WAV the AI service writes: 24 of 32 bits, then compressed losslessly) with a `stems.json` written last, so only complete separations are reused. Stems are reused only for the same separation model (which fixes the number of stems), decoded back to WAV in the panel's cache (`kept-stems/`) for the AI service, which reads only WAV; kept notes and hits go back to `ai.v1.transcribe.notes`/`.drums` as `known`/`hits`, which then only place them on the beats. Keeping and restoring stems run on a thread of their own (`BackgroundTask`). When a dropped song is in the library, the Analyzer stops after preparing it and offers **Continue from the existing analysis** (its tempo, key and kept stems, then the kept notes) or **Analyze anew** (everything found again; the kept notes and hits of the old stems are dropped, the measured style stays). **Saved analyses...** (in both panels, `ui/AnalysisLibraryView`) lists every song with the space it takes and how much is stems, the total, and deletes one song's stems, a song, all stems or everything; only the library's files, never the user's audio. Why the settings folder and not the project cache: references belong to no project, and an album's stems are reused across projects instead of being separated again in each.

**How it works:** the analysis engine; `ai.v1.stems.*` (Demucs 4 or 6 stems), `ai.v1.analyze.tempo` (Beat This!, without the DBN; the tempo is the mean of the steady beat intervals, since one interval on the model's 20 ms frames is too coarse: the median alone jumped between 130.4, 136.4 and 142.9 BPM; the beats are what is cached, the tempo is worked out again from them) and `.key` (chroma and Krumhansl-Kessler profiles, NumPy); `ui/Analyzer` (View › Analyzer): prepares the song (JUCE decoding, MP3 included) into `cache/analysis/`, finds tempo and key with "Use for the song", separates on request; each stem is then left out, added as audio on a Playlist track aligned to the first downbeat, or replicated, all in one undo step ("Add to the project"). Replicated: Basic Pitch notes on 3x Osc, drum hits as Drum Machine steps, or "the song's own sounds" (`ai.v1.samples.make`: the clearest hit of each drum on the pads, melodic stems as phrases in a Sampler, `session/StemSamples`), "the closest presets" of Surge XT, OB-Xf, Odin 2 or any instrument with presets (`session/InstrumentPresets` finds and loads them, `session/PresetPreviews` records each preset once, `ai.v1.presets.*` measures and matches, per instrument), or a synth tuned to each stem (`session/SynthMatch`, synth profiles; any installed instrument experimentally, with an automatic profile). Drums: hits of one kind that sound clearly different get pads of their own, with accents per pad; with matched sounds, each drum sound gets the closest samples from the Synth Kit and the user's drum samples folder (`ai.v1.presets.*` with one-shot measuring).

### 12.8 Feature: Hum / sing / beatbox to pattern (F5b)

- **Record** into the Analyzer or directly in the piano roll ("Record idea" button).
- **Melody:** pitch track → note segmentation → quantize to project tempo, snap to key → pattern on the chosen channel.
- **Rhythm:** beatbox → onsets → classify kick/snare/hat → Drum Machine pattern.
- **Tempo from voice:** an optional "detect tempo from my take" is planned (§17); the project tempo is used.
- Results land as ghost notes with Accept/Discard, same as F3.
- **Analysis:** in spinus-ai on the CPU with NumPy only: pYIN pitch tracking, spectral-flux
  onsets, note segmentation, tempo; beatbox sounds are matched to examples the user teaches once
  (`ai.v1.listen.*`). Results are cached by the take's contents. No PyTorch is needed.

### 12.9 Feature: Embedded assistant (F7)

- Right-dock chat panel. Context shown as chips the user can remove: current project, selected channel, selected pattern, current mixer insert.
- **Knowledge:** the Spinus manual (`docs/user/`), searched by keywords (BM25 over sections, built when the service starts, `spinus_ai/manual.py`) or by a local embedding model. Every user-visible feature has a manual page (§18). Music theory and production guides (`docs/knowledge/`, original writing) and Surge XT's own manual are in §17.
- **Live project tools** (read): list channels/patterns/inserts, get notes, get plugin parameters, run mix analysis, get selection.
- Read tools answer from a snapshot of the project sent with each question, never from the live project.
- **Action tools** (write): every write tool produces a **Proposal** (steps from `docs/dev/commands.schema.json`; a step may use an earlier step's result as `"$1.channel"`; Spinus tries them on a copy of the project to check and describe them): a list of commands shown as a diff ("Add channel: Sampler", "Set Insert 3 EQ band 2: −3 dB at 300 Hz"). User clicks Apply (one undo step) or Reject. Never auto-applied.
- Can trigger the other AI features (generate pattern, automix, analyze) on the user's behalf, still through proposals.

### 12.10 Feature: Track creator (experimental)

The user gives a few reference songs (an album or a selection) and, in the end, gets a new, editable Spinus project in a similar style. Experimental and gated exactly as the Analyzer (§12.7): **Options › AI › Show experimental features**, panel `trackCreator`, View › Track creator (experimental). Owner decision 2026-10-26: started before 0.1.0 as an experimental feature that needs no polish for the release. Built in four milestones:

1. **References → style summary** (in 0.1.0). The panel `ui/TrackCreator` takes audio files or a folder (an album: its songs and those one folder down) as a list with each song's progress, removable one by one or cleared. Each song goes through the Analyzer's steps, shared with it (`session/SongPreparation`: decoding to WAV, named by contents; `ai/AnalysisJobs`: running a job, checking the engine and model files): `ai.v1.analyze.tempo` and `.key`, `ai.v1.mix.analyze` on the whole song for loudness, `ai.v1.stems.separate` (Demucs, 4 stems), `ai.v1.transcribe.drums` on the drums and `ai.v1.transcribe.notes` on the other stems. Then `ai.v1.style.summarize` (`spinus_ai/style.py`, NumPy only, no model) measures the song's *features*, also from the stems' audio (the panel passes their WAV files):
   - **Key**, by vote: the Analyzer's key, and the mix's chroma, the harmonic stems' chroma (no drums or voice) and the harmonic notes, each with Krumhansl-Kessler's and Temperley's profiles; z-scored fits are added. A key named by more than half of four or more estimates stands; otherwise, between keys sharing their notes or a fifth apart that fit almost as well, the one whose tonic the bass plays most (three times at phrase starts) wins. Each song gets a confidence (share of estimates agreeing: high two thirds, medium two fifths, low below) and the other estimates as alternatives, said in plain words when low. Evaluated without a listener on 23 real songs by agreement between these independent estimates; the files carried no key or BPM tags.
   - **Chords**, one per bar, named as Roman numerals from that key: notes weighted by length and loudness, those under 0.12 s left out, the bass note held longest as the root's hint, and the harmonic stems' chroma for the bar added as a second opinion.
   - **Drums** from the drum stem itself (`drum_grid`), not only from guessed drum kinds: onsets (spectral flux) in a kick (40-120 Hz), snare (1.5-5 kHz) and hats (7-11 kHz) band, each against the song's own levels (three-way k-means), with kick and snare told apart by their balance in that song; the beats are first moved onto the drums' onsets (beat times and attacks were 10-30 ms apart, which made fast straight songs read as swung). The recogniser's hits are a second opinion on kick and snare (each alone misread real kits). When the snares fall on beats 1 and 3, the bar is taken to start a beat later (Beat This! sometimes puts the downbeat a beat off). Swing is where the off-beat sixteenths fall, only when the song plays them in a fifth of its eighths.
   - **Tempo doubt**: "half" when a song of 140 BPM or more has its snare on every beat, "double" when one of 85 or less has it on the off-beat eighths (the tempo is not changed, only flagged).
   - **Parts**: each part's register, notes per bar and voices.
   - **Sections**: from a novelty curve over the bars' chroma and loudness, on the song's 4-bar phrase grid (its offset the one the strongest changes fall on); sections under two phrases join the next, ones over eight phrases are split where they change most; alike sections get the same letter.
   - **Loudness**: LUFS, true peak, crest, section contrast. Everything found goes into the analysis library (§12.7), stems included, and every step first uses what the library has (from the Analyzer or an earlier time): a song analyzed in the Analyzer needs no beat finding, key or separation here, and a song whose style was measured is ready at once. The decoded song and the stems' WAV files are made in a temporary folder deleted after each song. The style's version is `TrackCreator::analysisVersion`, equal to `style.FEATURES_VERSION` (a test checks); older features are measured again from what is kept. The list of songs (path, digest, size and time, so unchanged files are not read again) and the last summary are remembered per user in `ai/track-creator.json`: at the next start the songs whose style is in the library are done and the summary is shown as it was, and the others wait for **Analyze again**, so starting Spinus never starts the AI service. Once all songs are analyzed, the same method with every song's features gives the **style summary** (`kind: spinus.style-summary`, `version`): tempo median and range, meters, keys and mode shares, chord shares, bars per chord, top 4-chord loops with counts (counted whichever chord they are heard from, shown as heard most; `shared` only when two songs or more play one at least three times), chord-to-chord transition frequencies, drum grooves grouped by likeness (cosine 0.8; per-step probabilities for each group, the biggest as `lanes`, so unlike grooves are not averaged), swing, tempo doubts, key confidences, per-part song count, register (MIDI low/median/high), notes per bar and voices, section lengths and forms, loudness, every song's features (`perSong`), and `plain`: the same in plain sentences, shown in the panel. **Save summary...** writes it as JSON. Nothing changes the project, so no command is needed.

Milestones 2 to 4 are planned:

2. **Sound library index.** The installed instruments' presets are rendered once, in the background, in a separate process (so a crashing plugin cannot take Spinus down), into timbre embeddings, extending what closest preset does (§12.7, `ai.v1.presets.*`) to every installed instrument.
3. **Sounds per part.** For each part of the summary, the closest instrument and preset from that index, then synth match (§12.7) toward the references; effects picked and set the same way; then the automixer (§12.5) balances the mix. Every instrument and effect plugin the user has is considered, besides Spinus's own devices and the free downloads.
4. **The new song.** Sections, chords, bass, drums and a melody are written from the summary, both ways, offline by default: a **rules-based generator** working from the summary's explicit numbers (per-step drum probabilities, progression and transition frequencies, section lengths, each part's register and note density) needs no AI connection and is the default; when an LLM connection is set up, the Assistant (§12.9) can refine or vary it, as the pattern generator does (§12.4). The result is one proposal applied in one undo step through commands; one part at a time can be redone ("new bassline", "darker chords"). **Similar, never copied:** only the style is taken; the references' melodies, lyrics and audio are not reproduced. Notes and settings are generated, not audio: no audio-generating models (their weights are mostly non-commercial, §3).

The panel shows the Analyzer's notice: analyzing songs is for study and personal use; only the style is measured; the user is responsible for rights when releasing.

---

## 13. Command API

- Every mutation is a `Command` with: name, parameters (JSON-serializable), `perform()`, `undo()`, and a human-readable description.
- Commands can be grouped into a transaction = one undo step. A continuous gesture (one fader or knob drag) is applied as it moves and joined into one undo step.
- The same command set is exposed:
  - to the UI (C++ calls),
  - to the AI service (JSON-RPC `project.v1.apply` with a proposal id),
  - to tests (scripted command sequences against a headless engine).
- A JSON schema of all commands is generated at build time and used both for LLM tool definitions and for validation. **Adding a command means updating that schema, the docs, and tests.**
- Plugin settings are normally the device's own state (`plugin-states/`), changed in the plugin's window. Settings a feature must change as a step of its own (the automixer's EQ bands) go through `device.setParameters`: values 0–1 by Spinus's parameter ID, kept in the model (`Device::parameters`) and applied by the engine adapter over the state; undo gives back the value from before. **Spinus's parameter ID** is the one ID for a setting everywhere (`engine/ParameterIds.h`): `device.setParameters`, automation targets, the device menu's automation list, the last tweaked control, Copy effect and synth match all use it. It is the ID Tracktion's automation gives the parameter: a Spinus device's own parameter ID (`cutoff`), and for any other plugin the parameter's position in the plugin's list (`"0"`, `"1"`...), not the plugin's own ID (a CLAP `clap_id` or VST3 `ParamID`, which for OB-Xf or Surge XT Effects is some other number). Why: automation already saved positions (Tracktion keys external plugins' automation that way), so projects keep their automation, and one ID means a value the AI or a script sets is the one automation moves. Projects saved before this may hold plugins' own IDs in `Device::parameters` (Copy effect wrote them): a key is read as Spinus's ID first, and only a key that is no parameter's Spinus ID as the plugin's own; where both kinds name the same setting, the one by Spinus's ID counts. The adapter applies only values that changed since it last applied them, so a knob turned by hand stays until a command sets it again. 3x Osc's factory presets (`dsp/threeosc/ThreeOscPresets`, also its programs) load the same way, every setting at once, with `device.loadPreset`.

---

## 14. Undo, autosave, crash safety

- Undo history: unlimited by default within memory budget; "History" panel lists steps by description.
- Autosave as in §7.3. On startup after a crash, Spinus offers to recover the newest autosave.
- Plugin crash (Windows bridge): the slot goes bypassed with an error; audio continues.
- **Session logs** (`core::Logs`, `src/core/Log.h`): one log file per run in the settings folder's `logs/`
  (`~/.config/Spinus DAW/logs`, `%APPDATA%\Spinus DAW\logs`; the Flatpak's own config folder), named by the start time
  (`spinus-2026-10-24_14-03-12.log`), the newest 10 runs kept and each file capped at 4 MB. A header names the
  version, package, system, JUCE and Tracktion versions and the CPU; then a line per notable event: start and
  quit steps, the audio device and dropouts, project open/save/autosave/recovery/export with durations, plugin
  scans, plugin load failures and sandboxed plugin crashes, Windows plugin setup, the AI service's state, failed
  commands, and anything shown to the user as a problem. Never per block or per paint. Why: a tester's problem can
  be told from a file they attach (Help › Open log folder), and Help › Report a problem collects
  `Logs::recentFiles()`.
  - Every line is **redacted** before it is written (the home folder becomes `~`, other users' folder names,
    keys, tokens, email addresses and any key Spinus passed on are masked); the AI service applies the same rules
    (`ai/spinus_ai/logs.py`).
  - Each line is one `write` (one `WriteFile`) to a file opened for appending, nothing buffered in Spinus, so the
    lines before a crash are on disk, and the helper programs (plugin scanner, plugin host; told the file by
    `SPINUS_LOG_FILE`) append to the same file, labelled. The crash handler adds a fixed line. A clean quit ends
    the file with "Spinus closed normally."; a run holds a lock on its file (`flock`, `LockFileEx`) until it
    ends, so the next start tells a run that ended unexpectedly from one still going (two Spinus at once).
  - **Real time:** nothing on the audio thread logs. Dropouts are read from the driver's counter by a timer on
    the message thread (`session::AudioDeviceLog`); a sandboxed plugin's hang, noticed on the audio thread, is a
    flag that the message thread logs.
  - Tests and the command-line tools (`--self-test`, `--scan-plugins`, `--snapshot`) write no logs.

---

## 15. Performance targets

Reference system: a 16-core laptop CPU with an integrated GPU, 48 kHz, 128-sample buffer.

| Metric | Target |
|---|---|
| 64 channels with stock instruments + 4 stock effects each | ≤ 50 % DSP load, no dropouts |
| 16 instances of Surge XT playing chords | ≤ 50 % DSP load |
| Project open (no AI cache rebuild) | ≤ 3 s for a 100-channel project |
| UI | 60 fps while playing |
| Spinus Tune real-time latency | ≤ 10 ms (measured: output ≈ 5.3 ms at 128 samples through PipeWire's JACK; round trip not measured) |
| Stem separation, 4-min song | ≤ 60 s on GPU; ≤ 4 min on CPU fallback, with progress (measured on the reference system: 15 s GPU, 24 s CPU with htdemucs) |

---

## 16. Packaging (Arch first)

- `packaging/arch/PKGBUILD` for `spinus-git` (AUR), building the app, Spinus's own plugins and the AI service virtualenv. The free open-source plugins are not packaged; the package depends on what they need at runtime (`libsndfile`, `cairo`) and on `curl` and `libarchive` to download them, and lists the Arch repositories' CLAP packages as optional alternatives. Audio: depends on the virtual `jack` provider and recommends `pipewire-jack` (which replaces `jack2` on Arch).
- AI service installed into `/usr/lib/spinus/ai`, run by the system's Python. PyTorch is not part of the package: when the user asks in Options › AI › Analysis engine, Spinus installs the torch build matching the detected GPU (TheRock for AMD, CUDA for Nvidia, XPU for Intel, or CPU) with `uv` into `~/.local/share/spinus/python/<build>/`, after stating the download and disk size (about 1.8 GB and 6.3 GB for gfx1151, 0.2 GB and 0.8 GB for CPU). `uv` is an optional dependency.
- Desktop entry, icon, MIME type for `.spn` project files, so a double-click opens the project.
- `packaging/arch/PKGBUILD` builds the app, its helpers and the AI service with `cmake --install` rules (`cmake/SpinusInstall.cmake`). It builds neither Surge XT nor the effects (`SPINUS_BUILD_SURGE=OFF`, `SPINUS_BUILD_BUNDLED_PLUGINS=OFF`) and does not check out `external/surge`.
- `packaging/arch/release/PKGBUILD` builds a tagged release the same way.
- AppImage (`packaging/appimage/`) and Flatpak (`packaging/flatpak/`) packages are built too; Flathub is in §17.
- **Windows** (`packaging/windows/`, [docs/dev/windows-build.md](docs/dev/windows-build.md)): a per-user NSIS installer (`spinus.nsi`, staged by `stage.py`) with the embeddable Python for the AI service, built with MSVC by `.github/workflows/windows.yml` or on Linux with clang-cl and xwin (`build-on-linux.sh`). ASIO, WASAPI and DirectSound; Windows plugins load natively, so the Wine bridge is left out.

---

## 17. Roadmap

### In 0.1.0

What Spinus does in its first release (the [changelog](CHANGELOG.md) lists it for users):

- **Core DAW:** Channel Rack with step sequencer, Piano Roll, Playlist with pattern, audio and
  automation clips, and a dynamic mixer whose inserts can be added, reordered and removed while playing
  (§7, §8). Sampler, Drum Machine, 3x Osc (with factory presets) and Beat Slicer; Spinus's own EQ,
  Compressor, Limiter, Gate, Filter, Sidechain Ducker, Stereo Utility, Loudness Meter and Spinus Tune;
  Surge XT, LSP Plugins (Linux), Dragonfly Reverb, Odin 2 (Linux), OB-Xf, PeakEater, Six Sines, TIME-12
  and GATE-12 downloaded on request (§11). MIDI and audio recording, takes and comping, multi-track recording (§9). Undo and redo
  for everything, autosave and recovery, project folders (§7.3, §14). **Freeze**. Export to
  WAV, FLAC, Opus, MP3 and AAC: full mix, a pattern, or stems from the same render.
- **Plugins:** VST3, CLAP, LV2 and VST2, scanned out of process; Windows plugins through a managed Wine
  and yabridge (§10.3); a crashed plugin can run in its own process (§10.2). Free open-source plugins shown as their
  makers made them; Piano Roll note property lanes with note expressions.
- **Clip launcher** beside the Playlist.
- **AI (optional, off by default):** the LLM gateway with several connections and fallback, and the
  Assistant with proposals (§12.3, §12.9); pattern generator and adjuster, hum, sing or beatbox to
  pattern, Spinus Tune in both modes (§12.4, §12.6, §12.8); the automixer with A/B and reference
  matching (§12.5).
- **Experimental** (shown only with Options › AI › Show experimental features, §12.7): the track
  analyzer with all three sound-recreation options; synth match and closest preset for any instrument
  plugin; the Track creator (§12.10), built in milestones: the first, a style summary of reference
  songs, is done; the others follow while it stays experimental.
- **Packages:** the Arch packages, an AppImage, a Flatpak, and a Windows installer for Windows 10 and 11,
  64-bit (§16). **macOS** is not planned (releasing for it needs a paid Apple developer account).
- The flagship synth is **Surge XT**; a Spinus-built flagship synth is not planned.

### Later

- **Dancing Spinus** (for fun): a small "useless" visual plugin like FL
  Studio's Fruity Dance: the Spinus bird (a siskin, as in the logo) dances in its own little window, in
  time with the song (tempo and playing state from the transport; livelier when the Master is louder). It
  makes no sound and passes audio through; it can sit on any insert or as an instrument without notes.
- **Flathub:** publish the Flatpak there. **Windows:** a signed installer.
- **The track analyzer out of experimental** (§12.7), and the analysis engine on Windows.
- **Spinus Plugins** (after 0.1.0): new open-source instruments and effects for sounds that open source
  doesn't cover well yet, in their own repository ([spinus-plugins](https://github.com/tuturu742/spinus-plugins),
  GPL-3.0-or-later). They are built as CLAP, VST3 and LV2 for Linux and Windows, so they work in any DAW.
  Each comes with a synth-match profile and tagged presets. Spinus offers them under Get free plugins.
  Each is named after a *Spinus* species. The first is **Psaltria**, an additive resynthesis synth that
  works with the track analyzer (§12.7); its `PLUGINS.md` lists the others (Tristis, Magellanicus, Pinus,
  Cucullatus). Ideas that a good open-source plugin already covers are dropped, and Get free plugins
  offers that plugin instead: Six Sines rather than an FM synth, TIME-12 and GATE-12 rather than a volume
  and time shaper. Spinus's own devices (§11) stay in Spinus as the defaults that are always there, except Spinus Tune
  (next item).
- **Spinus Tune moves to Spinus Plugins in 0.2.0** (§12.6). The pitch engine, the live plugin and its
  window move to the spinus-plugins repository, where Tristis also uses the engine. The engine is
  released there under the GPL-3.0-or-later as well; Spinus includes GPL code under its AGPL. They take
  with them the shared pieces they use: the device controls (`dsp/DeviceControls`) and the parameter text
  rules (`dsp/ParameterText`). Spinus's other devices then use those pieces from there too.
  - Spinus includes spinus-plugins as a git submodule, pinned to a release, and builds Spinus Tune
    with every package as before. It stays a default that is always there.
  - Graphical mode stays in Spinus (`ui/SpinusTune`, `session/Tuner`, `engine/TuneRender`), because it
    edits the project through commands. It uses the engine from the submodule.
  - Spinus Tune keeps its device type and saved state, so older projects open unchanged.
  - Before the move, spinus-plugins is public and its CI builds and tests on Linux and Windows.
- **Automation editor with finer control.**
- An MPE switch for VST3/LV2 synths; recording MPE controllers' per-note bends and pressure.
- Presets in the Browser (§8.1).
- Audio clips: tempo-sync to the project and slicing by transients (§11.3).
- Plugins in a separate process: their windows on Windows, sidechain inputs, MIDI out (§10.2); synth
  match tries in a separate process, so a crashing synth cannot take Spinus down (§12.7).
- **Remove everything installed for Windows plugins** (§10.3): removing a plugin takes it out of Spinus,
  but what its installer added to Spinus's Windows folder (sample libraries, helper apps such as a
  vendor's download manager) stays; one button clears it.
- CLAP builds of Spinus's own devices (§11).
- Hum to pattern: "detect tempo from my take" (§12.8); graphical Spinus Tune: moving notes in time;
  per-instrument range limits in the pattern generator; a dedicated symbolic-music model for grooves
  (§12.4).
- Automixer: pan, reverb send and sidechain suggestions (§12.5).
- LLM usage: an optional monthly limit per connection that warns at 80 % and asks before exceeding
  (§12.3).
- The assistant's knowledge: music theory and production guides in `docs/knowledge/`, and Surge XT's
  own manual once its license is confirmed (§12.9).
- Synth match profiles for more free synths (Vital, Dexed first); the AI service on another computer on
  the home network.
- The starter sample pack with factory drum kits (§11.4).
- MIDI file import (D9), so parts can come from other DAWs; editing several channels' notes at once in the
  Piano Roll (today a ghost note's double-click switches to its channel).

---

## 18. Agent workflow

- **AGENTS.md** (short, at repo root) repeats the non-negotiables: real-time rules, Command API only, license check for new deps, docs and tests with every feature, no AI on the audio thread.
- **Branches & PRs:** one task per branch, named `<area>/<short-task>` (e.g. `piano-roll/chord-stamp`); PR description includes what changed, how it was tested, **"How to test" for the owner**, and screenshots/recordings for UI changes.
- **CI must pass:** build (Release + Debug with AddressSanitizer), unit tests, `pluginval` on stock plugins, clang-tidy, Python tests (`pytest`), and a docs check (every command and user-visible feature has documentation).
- **Real-time review checklist** for any code touching `engine/` or `dsp/`.
- **Architectural changes** update this document, with the reason in a sentence or two.
- **Golden-audio tests:** render reference projects headlessly and compare against stored renders within tolerance, to catch audio regressions.
- **Hardware-dependent checks** (GPU, audio latency) cannot run in CI; agents write a script under `tools/owner-checks/` and ask the owner to run it and paste the output into the PR.
- Agents never commit API keys, model weights, or copyrighted audio.

---

## 19. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| FL-style model fits Tracktion poorly | High | Mapping measured first; fallback to `tracktion_graph` |
| Wine updates break yabridge | High | Pinned Wine runtime; health check; system Wine only opt-in |
| VST2 hosting legal/technical issues | Medium | Clean-room header; VST2 can be disabled without affecting other formats |
| gfx1151 PyTorch builds are nightly-quality (crashes, memory faults) | Medium | Pinned tested build; GPU self-test with automatic CPU fallback; every model has a working CPU path |
| Synth sound-matching quality is low | Medium | Sample-based option always available; set expectations in UI |
| Surge XT UI feels foreign next to Spinus | Low | Accepted: it is shown as its makers made it, credited by name, and its own manual applies |
| Bundled effects have upstream bugs or look foreign | Medium | Curated modules only; pluginval and fuzz tests in CI; invalid-audio guard (§10.2); pinned versions; shown as their makers made them |
| LLM cost or provider changes | Low | Multiple connections, fallback chain, local option, usage tracking |
| Scope creep | High | Test releases: the owner tests each alpha and release candidate, and they continue until the release is polished |

---

## 20. Owner questions

The owner's answers so far are recorded in §2 and in the sections they concern. New questions go to the owner as GitHub
issues labeled `owner-decision`.
