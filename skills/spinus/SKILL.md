---
name: spinus
description: Work with Spinus DAW as a coding agent, using only an installed Spinus (no source code). Use when asked to look at, change, mix, check or render a Spinus project or the song open in Spinus, or to build something with Spinus from the command line. For writing a whole song from nothing, also read the spinus-songwriter skill.
---

# Working with Spinus DAW

Spinus is a free digital audio workstation for Linux and Windows, laid out like FL Studio: a **Channel Rack**
(instruments), the **Piano Roll** (notes), **patterns** placed as clips on the **Playlist**, and a **Mixer** of
inserts with effects, sends and sidechains. Every change to a project is a **command**, and each command is one
undo step. You use the same commands its buttons use, in one of two ways:

| Way in | When | What you get |
|---|---|---|
| **The live interface** (MCP) | The user has Spinus open and turned on **Options › AI › Coding agents** | You change the song they are looking at; they hear it at once and can undo you |
| **The command line** | Anything else: new songs, batch work, checks, renders | Scripts run on a project without a window; you save a new project or a copy |

Pick the live interface when it's set up (you have a `spinus` MCP server). Otherwise use the command line.

## Ground rules

1. **Never open Spinus's window yourself.** Only `spinus` alone or `spinus <project>` opens it, on the user's
   screen. On Linux run every command as `env -u DISPLAY -u WAYLAND_DISPLAY spinus --<option> ...`, so even a
   typo can't open a window. Every `--option` prints JSON, one object per line; an unknown one exits with code 1.
2. **Don't overwrite the user's work.** With the command line, save into a new folder (`--save <new folder>`),
   or `--open` their project and `--save` a copy, unless they ask you to change it in place. With the live
   interface, every change is one undo step named "Agent: ..."; tell the user what you changed.
3. **Stay inside what's allowed.** Only use files in the project's folder, the user's sample folders and
   Spinus's starter pack. Don't edit Spinus's settings or its config folder. Never put API keys or tokens in
   files, scripts or messages.
4. **Use what's there.** Prefer Spinus's own instruments and effects (they work on every computer), then the
   free plugins the user has downloaded (`bundled.*`), then their own plugins only when they ask.
5. **Check and measure your work.** You can't hear: check the project and render it after every big change,
   and read the numbers (below).
6. **Original music only.** Don't use copyrighted audio, and don't copy a real recording note for note.

## Finding Spinus

| Install | Run it as (`spinus` below) | Demos, starter samples, manual |
|---|---|---|
| Flatpak | `flatpak run io.github.tuturu742.spinus` | `$(flatpak info --show-location io.github.tuturu742.spinus)/files/share/spinus/{demo,samples}`; manual in `.../files/lib/spinus/ai/docs/user` |
| AppImage | the AppImage file | `--appimage-mount` prints a mount folder (and keeps running until you stop that PID): `<mount>/usr/share/spinus`, manual in `<mount>/usr/lib/spinus/ai/docs/user` |
| Arch package | `spinus` | `/usr/share/spinus/{demo,samples}`; manual in `/usr/share/doc/spinus` |
| Windows (PowerShell) | `& "$env:LOCALAPPDATA\Programs\Spinus DAW\spinus.exe" ... \| Out-Host` | beside `spinus.exe`: `demo\`, `samples\`, `docs\` |

If `spinus --commands` opens a window instead of printing JSON, that Spinus is too old: ask the user to update.

## The live interface (MCP)

The user turns it on in **Options › AI › Coding agents...** and gives you the setup it shows: the address
`http://127.0.0.1:47219/mcp` and a token, sent as `Authorization: Bearer <token>`. For Claude Code:

```sh
claude mcp add --transport http spinus http://127.0.0.1:47219/mcp --header "Authorization: Bearer <token>"
```

It works only on this computer, and the user can **Disconnect** you or **Undo the agent's changes** at any time.

**Spinus's own tools** (always there):

- `song_summary`: tempo, key, length, channels, inserts, patterns, lanes, clips, markers, and what's missing.
- `run_commands`: `{summary, steps: [{command, params}]}`, run in order as **one** undo step.
  - `"$1.channel"` uses step 1's result.
  - Automation points may take `text` ("-12 dB").
- `command_help`: one command's parameters, or the list of commands.
- `list_devices`: instruments and effects that can be added, found by words. Each comes with the device object
  that `channel.add` and `insert.addEffect` take.
- `render`: an offline render that measures the mix's LUFS and true peak, each section's LUFS (between
  markers), and with `stems: true` each insert's LUFS and how long it sounds. It takes as long as the song.
- `undo_last`: undoes your last change, if nothing newer came after it.

**The music tools** need AI turned on in Spinus (**Options › AI**). They run on this computer, with no internet.

| Purpose | Tools |
|---|---|
| Read | `song_overview` (call it first: sections, parts, roles, sounds, mixer), `read_part` |
| Notes | `write_part` (a part over some bars, `loop_bars` to repeat), `motif`, `place_motif` (transpose, invert, octave, rhythm, displace) |
| Form | `add_part` (an instrument found by words, a preset, a family, a colour), `insert_bars` (makes room and adds a marker; `copy_from` fills it), `copy_bars` |
| Sound | `set_sound` (a preset and/or named settings), `shape_sound` ("pluckier", "brighter", "warmer", "wider"…) |
| Mix | `add_effect` (to a part, a bus or `master`), `send` (to "Reverb", "Delay" or any insert), `duck` (sidechain), `bus`, `automate` (bars from 1, values like "-12 dB", "30% L", "1.2 kHz") |
| Test | `check`: out-of-key notes, masking parts, a muddy low end, unused parts, sections off 4-bar phrases, the master limiter, loudness per section after a render |

A good loop:
1. Read with `song_overview`.
2. Make one change.
3. Run `check`.
4. `render` after big changes.
5. Tell the user what changed, in plain words.

## The command line

| Option | What it does |
|---|---|
| `spinus --commands` | Every command and its parameters (JSON Schema). Read it before writing a script. |
| `spinus --describe <device>` / `--describe all` | Each device's setting IDs, values at 0/0.25/0.5/0.75/1, `choices`, presets, Drum Machine pads and kits. The last line, `notInstalled`, lists free plugins not yet downloaded. |
| `spinus --run <script.json> [--open <project>] --save <folder>` | Runs a script's steps and saves a project. Without `--save` it's a dry run. It stops at the first failing step and saves nothing, unless you pass `--keep-going`. |
| `spinus --check <project>` | What the project holds, `used` per channel, `unusedChannels`, and `problems` (missing plugins or files). Exit code 0 = fine, 3 = something missing, 1 = won't open. |
| `spinus --render <project> --out <file> [--stems <folder>] [--tail <s>]` | Renders WAV, FLAC, Opus or MP3 (by the file's extension). Prints `seconds`, `lufs`, `soundingSeconds` and `truePeakDb` for the mix and each stem. |

A script is a JSON list of steps:

```json
[
 {"command": "project.setTempo", "params": {"bpm": 120}},
 {"command": "channel.add", "params": {"name": "Bass", "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}}, "as": "bass"},
 {"command": "device.loadPreset", "params": {"device": "$bass.device", "preset": "Reese Bass"}},
 {"command": "pattern.add", "params": {"name": "Loop", "length": 7680}, "as": "loop"},
 {"command": "pattern.setNotes", "params": {"pattern": "$loop.pattern", "channel": "$bass.channel",
   "notes": [{"start": 0, "length": 1800, "pitch": 33, "velocity": 0.8}]}},
 {"command": "lane.add", "params": {"name": "Music"}, "as": "lane"},
 {"command": "clip.addPattern", "params": {"lane": "$lane.lane", "pattern": "$loop.pattern", "start": 0}},
 {"command": "marker.add", "params": {"at": 0, "name": "Intro"}}
]
```

- `as` names a step's result, so later steps can use `"$bass.channel"`, `"$bass.insert"` or `"$bass.device"`.
  `"$3.pattern"` is step 3's.
- Two **actions** change sounds rather than the project:
  - `{"action": "loadKit", "params": {"device": ..., "kit": "Magpie"}}`;
  - `{"action": "loadSamples", "params": {"device": ..., "files": [...], "pad": 1}}`.
- Audio files are copied into the project's `audio` folder when it's saved. Give absolute paths, or names
  beside the script.

## Facts that are easy to get wrong

- **Time:** 960 ticks per quarter note, so a 4/4 bar is 3840. Notes are MIDI numbers; the Piano Roll's C5 is 60.
- **Devices:** a device is `{"kind": "stock", "stockType": "spinus.3x-osc"}`. Spinus's own include
  `spinus.3x-osc`, `spinus.drum-machine`, `spinus.sampler`, `spinus.eq`, `spinus.compressor`, `spinus.limiter`,
  `spinus.filter` and `spinus.ducker`.
  `--describe all` lists every one.
- **Settings:** `device.setParameters` takes real units as text (`{"cutoff": "1.2 kHz", "attack": "20 ms"}`).
  A setting with a list takes one of its `choices` exactly (`"Low cut"`, `"Bell"`). A wrong name is refused,
  and the error lists the right ones.
- **Automation targets:**
  - `{"kind": "deviceParameter", "device": <id>, "parameterId": "<id>"}`;
  - `channelVolume`, `channelPan`, `insertVolume`.
- **Automation points:** in `--run` scripts and `run_commands`, give `{"time": 0, "text": "-12 dB"}` rather than a
  0–1 `value`. A value stays where its clip ends, so add a point back to rest.
- **Mono presets:** 3x Osc's lead and bass presets are mono. Set `voices` to "Poly" before playing chords.
- **Drum Machine kits** share one layout: kick on pad 1 (note 60), snare on pad 2 (61), closed and open hats
  on 4 and 5.
- **Reverb and delay** come from the free plugins (Dragonfly reverbs; Surge XT Effects for delay and chorus).
  They are usable only once the user has downloaded them in **Options › Get free plugins**.
- **Plugins the user installed** are named in a project as `--check` shows them. Their own presets, and sounds
  chosen in their windows, can't be set from a script.
- **Project files:** a project is a folder holding a `.spn` file (plain JSON) and an `audio` folder. Change it
  through commands, never by editing the JSON.

## Reading the numbers

- **Loudness:** about -14 to -9 LUFS for pop or electronic, quieter for ambient. True peak no higher than
  -1 dBTP. For Opus, a limiter ceiling of -2 dB is safer.
- **Stems:**
  - A silent stem that should play is a bug.
  - Short parts (risers, crashes) read loud in LUFS: look at their `soundingSeconds` and peak.
  - The kick and bass should be the only things below about 120 Hz.
- **Sections:** the first chorus shouldn't be the loudest moment; quiet sections should have fewer parts, not
  just lower faders.

## Handing over

Tell the user, in plain words:
- where the project and any render are;
- what you changed, and why;
- anything you couldn't do;
- how to open the project: `spinus "<folder>/<name>.spn"`, or File › Open.

Everything you made is ordinary Spinus channels, patterns and clips that they can change by hand.

## More

- The manual pages in the install: `command-line.md`, `agents.md`, and one page per device (`3x-osc.md`,
  `drum-machine.md`, `sampler.md`, `mixer.md`…).
- The demo songs (`demo/*/*.spn`): run `--check` on one to see what a finished song looks like.
- The `spinus-songwriter` skill: the full craft of planning, writing, mixing and rendering a song from nothing.
