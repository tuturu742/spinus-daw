# Project file format (`SongName.spn`, schema version 1)

A Spinus project is a folder `SongName/`. This page describes `SongName.spn`, the
model written by `src/model/ModelJson.cpp`. A complete example is
[`tests/model/files/example-v1.json`](../../tests/model/files/example-v1.json); a test keeps loading it, so
the format cannot change by accident.

## Folder layout

| Path | Contents |
|---|---|
| `SongName.spn` | the model (this page). Found as `<folder name>.spn`, else any `.spn` in the folder (a renamed folder), else `project.json` (an older layout; the next save replaces it) |
| `plugin-states/<device id>.bin` | each instrument's and effect's state (a Tracktion plugin state tree; third-party plugins keep their own chunk inside) |
| `audio/` | audio files used by the project, copied in on import (`kick.wav`, `kick(2).wav`, …) |
| `cache/` | AI results and waveform peaks; never required, safe to delete |
| `cache/freeze/<key>.wav` | audio of frozen channels and inserts, named by `model::freezeKey`; rebuilt when missing |
| `.autosave/<YYYYMMDD-HHMMSS-mmm>/` | the newest 10 autosaves (`project.spn` + `plugin-states/`) |
| `.session.lock` | exists while Spinus has the project open; names the process (`pid N`). Left behind by a crash: the process is gone |

Saving is crash-safe: each file is written to a temporary file and renamed into place, plugin states
before the `.spn` file, stale state files last. Code: `src/project/ProjectFiles.*`.

## Rules

- `schemaVersion` is required. Spinus refuses files from a newer schema and upgrades older ones through
  `src/model/Migrations.cpp`, one version at a time. Changing the format means: bump
  `Project::currentSchemaVersion`, add a migration step, and add an `example-v<N>.json` test file.
- Every object has a numeric `id`, unique across the whole project and below `nextId`. IDs are never
  reused, so references survive reordering and deletion.
- Times (`start`, `length`, `offset`, `fadeIn`, …) are **ticks at 960 per quarter note**.
- Levels are decibels (`volumeDb`, `gainDb`, `levelDb`); `pan` is -1 (left) to +1 (right); automation
  values and `velocity` are 0 to 1; colours are `#AARRGGBB` strings.
- Missing optional fields take their defaults; wrong types and broken references are load errors.
- Plugin and device state is not in this file: it lives in `plugin-states/<device id>.bin`.

## Top level

| Key | Contents |
|---|---|
| `schemaVersion` | `1` |
| `settings` | `tempoBpm`, `timeSignature {numerator, denominator}`, optional `key {tonic 0–11, scale}`, `sampleRate`, `title`, `author`, `comments` |
| `channels` | Channel rack, in order: `id`, `name`, `colour`, optional `instrument` (device), `volumeDb`, `pan`, `pitchCents`, `muted`, `solo`, optional `frozen` (true when frozen; left out otherwise), optional `audioTrack` (true for a channel that audio clips play through, listed under Audio tracks instead of the Channel Rack; when loading, a channel without an instrument that an audio clip plays through is one even without the flag, as in files saved before it existed), `targetInsert`, optional `group` (its Channel Rack filter group, e.g. "Drums"; left out when none) |
| `patterns` | `id`, `name`, `colour`, `length`, optional `timeSignature {numerator, denominator}` (the pattern's own; left out when it follows the song's), optional `tripletSteps: true` (Channel Rack steps are sixteenth-note triplets), optional `swing` (0 to 1: every second step plays up to half a step later), `channels: [{channel, notes: [...]}]` |
| `playlist` | `lanes: [{id, name, colour, muted, group?, collapsed?}]` (`group`: the lane ID it is grouped under, left out when none; `collapsed`: true for a folded group, left out otherwise), `clips: [...]`, and only when there are some, `markers: [{id, at, name}]` (named points on the ruler, `at` in ticks, in time order) |
| `mixer` | `master` and `inserts` (display order) |
| `nextId` | next free ID |
| `launcher` | the clip launcher, left out when it has no tracks or scenes: `tracks: [{id, name, colour}]`, `scenes: [{id, name}]` (both in display order), `clips: [...]` (launcher clips), `quantize` (`none`, `beat` or `bar`) |

**Device** (instrument or effect): `id`, `kind` (`stock` or `plugin`), `stockType` (e.g.
`spinus.sampler`, `bundled.surge-xt`) or `plugin {format, name, manufacturer, fileOrIdentifier,
uniqueId}`, `bypassed`, optional `parameters {parameterId: value}` (settings set by commands,
`device.setParameters`, each 0–1 over the setting's whole range; left out when empty). `parameterId` here
and in automation targets is Spinus's parameter ID: a Spinus device's own setting ID
(`cutoff`), for any other plugin the setting's position in the plugin's list (`"0"`, `"1"`...). Older
files may hold a plugin's own parameter IDs in `parameters`; a key that is no setting's position is read as
one. A 3x Osc
preset loaded with `device.loadPreset` is stored the same way: every 3x Osc setting.

**Note**: `start`, `length`, `pitch` (0–127), `velocity`, `releaseVelocity`, `pan`, `finePitchCents`,
optional `expressions {name: value}` (per-note modulation, named like CLAP note expressions). The Piano
Roll writes `volume` there (0–1, linear gain; left out at the default of 1) and keeps other names as they are.

**Clip**: `id`, `lane`, `start`, `length`, `muted`, `type` and per-type fields:
- `pattern`: `pattern`, `offset`
- `audio`: `file` (relative to `audio/`, or absolute until the project is first saved), `channel` (the channel it plays through; 0 = none), `offset`, `gainDb`, `pitchSemitones` (-24..24), `speed` (0.25..4, 1 = as recorded), `reversed`, `stretchMode` (`off`: as recorded, pitch and speed ignored; `stretch`/`stretch-drums`: pitch and speed apart, Rubber Band melodic/percussive; `resample`: speed and pitch together, like a record; `tempo-sync`: not yet, plays as `off`), `fadeIn`, `fadeOut`, optional `tune [{start, end, pitch, shift, drift, vibrato}]` (Spinus Tune's notes, seconds in the file)
- `automation`: `target {kind, channel | insert | device + parameterId}`, `points [{time, value, curve}]`

**Launcher clip**: as a clip of type `pattern` or `audio`, placed by `track` and `scene` instead of `lane`,
`start` and `muted`, with `length` (one pass) and `loop` (false: plays once). At most one per track and
scene.

**Insert**: `id`, `name`, `colour`, `icon`, `volumeDb`, `pan`, `stereoWidth`, `muted`, `solo`, optional `frozen`,
`output` (absent for master), `effects [{device, mix}]` (any number), `sends [{target, levelDb}]`,
`sidechainSources [insert ids]`, optional `notesFrom` (a channel whose notes its effects receive). Routing through outputs and sends must not form a cycle.
