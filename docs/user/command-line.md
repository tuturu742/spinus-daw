# Spinus from the command line

Spinus can make, check and render songs without its window, from a terminal. This is for scripts and for
AI coding agents: they write a song as a list of steps, and Spinus carries the steps out with the same
commands its own buttons use, then saves an ordinary project you open as usual.

None of these use a sound card or open a window, and none of them change your settings, your recent
projects or your logs.

| Option | What it does |
|---|---|
| `spinus --run <script.json> --save <folder>` | Runs a script's steps on a new project (or on `--open <project>`) and saves it into the folder. |
| `spinus --check <project>` | Opens a project and prints what it holds, and what is missing on this computer. |
| `spinus --render <project> --out <file>` | Renders the song, as File › Export does, and prints how loud it is. |
| `spinus --describe <device>` | Prints a device's settings, presets, pads and kits. `--describe all` covers every one. |
| `spinus --commands` | Prints every command and its parameters (JSON Schema). |
| `spinus --help` | Lists these options. |

Each option prints JSON, one object per line, so a program can read it. An option Spinus doesn't know
(`--hepl`, `--list`) is refused with "unknown option" and the list above, exit code 1: none of these ever
opens Spinus's window. Only `spinus` alone, or `spinus <project>`, does.

**In the Flatpak**, start Spinus as `flatpak run io.github.tuturu742.spinus` followed by the option:

```sh
flatpak run io.github.tuturu742.spinus --check ~/Music/Song
```

From the **AppImage**, run the AppImage file with the option. **On Windows**, use
`"$env:LOCALAPPDATA\Programs\Spinus DAW\spinus.exe"` in PowerShell, and add `| Out-Host` so the output
shows.

## A script

A script is a JSON file holding a list of steps. Most steps are a **command**: its name and its parameters,
as `spinus --commands` lists them.

```json
{"command": "channel.add", "params": {"name": "Bass", "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}}, "as": "bass"}
```

**`as` names the step's result.** `channel.add`'s result is the new channel's IDs: `{"channel": 5,
"insert": 7, "device": 6}`. A later step writes `"$bass.channel"`, `"$bass.insert"` or `"$bass.device"`
where it needs one of them. `"$3.pattern"` is the third step's result.

Without `--save`, nothing is saved: a dry run that checks every step. Each command is one undo step, exactly
as if you had done it yourself. A step that fails stops the script:
nothing after it runs, and nothing is saved. Add `--keep-going` to run the rest anyway and save what
worked.

Times are in **ticks**: 960 per quarter note, so a 4/4 bar is 3840. Notes are MIDI numbers: the Piano Roll's
C5 is 60.

Two steps change a device's sounds rather than the project. They are **actions**:

| Action | Parameters | What it does |
|---|---|---|
| `loadKit` | `device`, `kit` | Loads a Drum Machine kit: "Spinus Synth Kit", a [factory kit](starter-pack.md) ("Shrike", "Starling", "Magpie", "Raven"), or a `.spinuskit` file. |
| `loadSamples` | `device`, `files`, `pad` | Loads audio files into a [Sampler](sampler.md) (all of them, as a new set of zones), or into a Drum Machine's pads, one per pad from `pad` (1 to 16). |

```json
{"action": "loadKit", "params": {"device": "$drums.device", "kit": "Magpie"}}
```

A file named without a folder (`"loop.flac"`) is looked for beside the script first. Audio files a song uses
are copied into the project's `audio` folder when it is saved, as when you save a project yourself.

## An example song

This script makes two bars of drums, a Reese bass and a warm pad. The pad dips on every kick and goes to
a reverb on a send. It plays twice on the Playlist, with a filter sweep.

```json
[
 {"command": "project.setTempo", "params": {"bpm": 120}},
 {"command": "channel.add", "params": {"name": "Drums", "instrument": {"kind": "stock", "stockType": "spinus.drum-machine"}}, "as": "drums"},
 {"action": "loadKit", "params": {"device": "$drums.device", "kit": "Magpie"}},
 {"command": "channel.add", "params": {"name": "Bass", "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}}, "as": "bass"},
 {"command": "device.loadPreset", "params": {"device": "$bass.device", "preset": "Reese Bass"}},
 {"command": "channel.add", "params": {"name": "Pad", "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}}, "as": "pad"},
 {"command": "device.loadPreset", "params": {"device": "$pad.device", "preset": "Warm Pad"}},

 {"command": "insert.add", "params": {"name": "Reverb"}, "as": "reverb"},
 {"command": "insert.addEffect", "params": {"insert": "$reverb.insert", "device": {"kind": "stock", "stockType": "bundled.dragonfly.hall"}}},
 {"command": "insert.setSend", "params": {"insert": "$pad.insert", "target": "$reverb.insert", "levelDb": -12}},
 {"command": "insert.addEffect", "params": {"insert": "$pad.insert", "device": {"kind": "stock", "stockType": "spinus.ducker"}}},
 {"command": "insert.setSidechain", "params": {"insert": "$pad.insert", "source": "$drums.insert"}},
 {"command": "insert.addEffect", "params": {"insert": "$pad.insert", "device": {"kind": "stock", "stockType": "spinus.filter"}}, "as": "filter"},

 {"command": "pattern.add", "params": {"name": "Loop", "length": 7680}, "as": "loop"},
 {"command": "pattern.setNotes", "params": {"pattern": "$loop.pattern", "channel": "$drums.channel", "notes": [
   {"start": 0, "length": 240, "pitch": 60, "velocity": 0.9}, {"start": 960, "length": 240, "pitch": 61, "velocity": 0.8},
   {"start": 1920, "length": 240, "pitch": 60, "velocity": 0.9}, {"start": 2880, "length": 240, "pitch": 61, "velocity": 0.8},
   {"start": 3840, "length": 240, "pitch": 60, "velocity": 0.9}, {"start": 4800, "length": 240, "pitch": 61, "velocity": 0.8},
   {"start": 5760, "length": 240, "pitch": 60, "velocity": 0.9}, {"start": 6720, "length": 240, "pitch": 61, "velocity": 0.8}]}},
 {"command": "pattern.setNotes", "params": {"pattern": "$loop.pattern", "channel": "$bass.channel", "notes": [
   {"start": 0, "length": 1800, "pitch": 33, "velocity": 0.8}, {"start": 1920, "length": 1800, "pitch": 33, "velocity": 0.8},
   {"start": 3840, "length": 1800, "pitch": 29, "velocity": 0.8}, {"start": 5760, "length": 1800, "pitch": 31, "velocity": 0.8}]}},
 {"command": "pattern.setNotes", "params": {"pattern": "$loop.pattern", "channel": "$pad.channel", "notes": [
   {"start": 0, "length": 3840, "pitch": 57, "velocity": 0.6}, {"start": 0, "length": 3840, "pitch": 60, "velocity": 0.6},
   {"start": 0, "length": 3840, "pitch": 64, "velocity": 0.6}, {"start": 3840, "length": 3840, "pitch": 53, "velocity": 0.6},
   {"start": 3840, "length": 3840, "pitch": 57, "velocity": 0.6}, {"start": 3840, "length": 3840, "pitch": 60, "velocity": 0.6}]}},

 {"command": "lane.add", "params": {"name": "Music"}, "as": "lane"},
 {"command": "clip.addPattern", "params": {"lane": "$lane.lane", "pattern": "$loop.pattern", "start": 0}},
 {"command": "clip.addPattern", "params": {"lane": "$lane.lane", "pattern": "$loop.pattern", "start": 7680}},
 {"command": "clip.addAutomation", "params": {"laneName": "Filter sweep", "start": 0, "length": 15360,
   "target": {"kind": "deviceParameter", "device": "$filter.device", "parameterId": "cutoff"},
   "points": [{"time": 0, "value": 0.3}, {"time": 15360, "value": 1.0}]}},
 {"command": "marker.add", "params": {"at": 0, "name": "Intro"}}
]
```

Dragonfly Hall is one of the [free plugins](free-plugins.md): get it first, or use Spinus's own effects only.

```sh
spinus --run song.json --save ~/Music/Example
spinus --check ~/Music/Example
spinus --render ~/Music/Example --out ~/Music/Example.wav --stems ~/Music/Example-stems
```

`--run` prints a line per step, such as `{"step": 4, "command": "channel.add", "ok": true, "result":
{"channel": 5, "insert": 7, "device": 6}}`, then `{"saved": "..."}`.

### Automation in the control's units

An automation point takes `text` instead of `value`, in the units the control shows: `"-6 dB"` for a
volume, `"30% L"` for a pan, `"1.2 kHz"` or `"Low cut"` for a device's setting. This works in scripts run
with `--run` (the plain commands take `value`, 0 to 1):

```json
{"command": "clip.addAutomation", "params": {"laneName": "Fade out", "start": 0, "length": 15360,
  "target": {"kind": "channelVolume", "channel": "$pad.channel"},
  "points": [{"time": 0, "text": "0 dB"}, {"time": 15360, "text": "-40 dB"}]}}
```

A pan's text is `"30% L"`, `"20% R"` or `"C"` (the middle). As `value`, a pan is 0 (left) to 1 (right),
0.5 in the middle, and a volume is 6 dB at 1, then dB = 20 × ln(value) + 6: 0 dB is 0.741, -6 dB 0.549,
-12 dB 0.407, -24 dB 0.223.

## Checking a project

`spinus --check <project>` prints the project's tempo, key, length in bars and seconds, its channels
(instrument, the Drum Machine's kit, how many samples a Sampler has), its inserts (effects, sends, where a
sidechain comes from), its patterns (bars, how many notes), lanes, clips and markers. Under `problems`
it lists what is missing here: a plugin that isn't installed, an audio file that isn't there. It ends with
code 0 when nothing is missing, 3 when something is, and 1 when the project doesn't open.

Each channel says whether anything plays it (`"used"`): a clip on the Playlist or in the launcher with
notes for it, or an audio clip on it. `unusedChannels` lists the ones nothing plays. That isn't a problem
(the exit code stays 0), but it's often a mistake in a script.

## Rendering

`spinus --render <project> --out <file>` writes the song as WAV (24-bit), FLAC, Opus or MP3, by the file's
extension. `--stems <folder>` also writes one file per mixer insert that has sound, straight into that
folder, named by the insert's number and name (`04 Bass.opus`), in the same format. The mix and its stems
come from one render. `--tail <seconds>` sets how long reverbs ring out after the end (2 by default).

It prints, for the mix and each stem:

- `seconds`: its length.
- `lufs`: its integrated loudness. This is measured only where it sounds (that's how loudness is measured),
  so a short crash can read as loud as a part playing all song.
- `soundingSeconds`: how long it actually sounds, to tell those apart.
- `truePeakDb`: its true peak. For Opus and MP3 it's the encoded file's peak, and `unencodedTruePeakDb` is
  the render's before encoding: encoding can raise peaks by about 1 dB, so for Opus a limiter ceiling of
  -2 dB is safer than -1 dB.

That is how a script can tell that a part is silent, too quiet, or clipping: see
[Loudness Meter](loudness-meter.md) for the numbers streaming services aim for.

## Settings of a device

`spinus --describe spinus.3x-osc` prints every setting of 3x Osc with its ID (the one `device.setParameters`
and automation use), its name, and what it shows at 0, 0.25, 0.5, 0.75 and 1 of its range, plus 3x Osc's
presets. For the Drum Machine it lists the pads, with each pad's note and sound, and the kits. `--describe all`
prints Spinus's own devices and the free plugins installed here, one per line.

A setting with a fixed list of values (a filter type, a mode, a switch) also has `choices`: every value by
name, as `text` takes it (`"Low cut"`, `"Bell"`).

`device.setParameters` also takes settings in their own units through `text`: `{"cutoff": "1.2 kHz",
"attack": "20 ms"}`, so a script rarely needs the 0 to 1 values.

The last line of `--describe all` lists the free plugins that aren't installed here (`notInstalled`).
Spinus's own effects have no reverb or delay: those come from the free plugins (the Dragonfly reverbs, and
Surge XT Effects for delay and chorus). Get them in Spinus with Options › Get free plugins, then scripts
can use them. Without them, Spinus's own effects still make a full mix.

## Things to know

- Plugins you installed yourself work in scripts by their name and file, as `--describe` can't list them:
  add one in Spinus, save, and look at the project with `--check` to see how it is named.
- A plugin's own presets and the sounds chosen in its window can't be set from a script yet, apart from
  3x Osc's presets and the Drum Machine's kits.
- `--run` and `--render` load Spinus's instruments and effects, and Windows plugins through Wine, as
  Spinus does. A slow plugin makes them slow too.
