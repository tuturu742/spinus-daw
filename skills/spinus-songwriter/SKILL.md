---
name: spinus-songwriter
description: Write, arrange, mix and render a complete original song as a Spinus DAW project, using only an installed Spinus (no source code). Use when asked to compose, produce or arrange music in Spinus, or to make a demo song, a beat or a track with it.
---

# Writing a song in Spinus

You are going to produce a finished song: arranged, mixed and rendered. It should not be a loop with four
instruments. You drive Spinus from its command line: a script of the same commands a user's edits are
made of. Then you check the project and render it, and you listen to it through numbers. Treat the song
like code: plan it, write it in small reviewable pieces, test it, and fix what the tests show.

You need a Spinus with the command line for agents: `spinus --commands` prints a JSON Schema. If it opens
the window instead, that Spinus is too old for this skill: tell the user to update it.
**Never let Spinus open its window.** Run every command as `env -u DISPLAY -u WAYLAND_DISPLAY spinus ...`
(on Windows no change is needed). `spinus --help` lists the options. Starting Spinus with a project path,
or with nothing, opens the full app on the user's screen: don't.

## 0. If Spinus's live interface is on

If the user has Spinus open with **Options › AI › Coding agents** turned on, and has set you up with its MCP
server (`spinus`, `http://127.0.0.1:47219/mcp`), work on their open song through it instead of `--run`. Its
tools:

- **Spinus's own:** `song_summary`, `run_commands` (the same commands, with `"$1.channel"` references), `command_help`,
  `list_devices`, `render` (loudness of the mix, each section and each part), `undo_last`.
- **With AI on in Spinus,** the music tools: `song_overview`, `read_part`, `write_part`, `motif`, `place_motif`,
  `add_part`, `insert_bars`, `copy_bars`, `automate`, `add_effect`, `send`, `duck`, `bus`, `set_sound`,
  `shape_sound`, `check`.

Every change applies at once as one undo step, and the user hears it in Spinus. The craft below still applies;
check and render after each big change. Tell the user what you changed.

## 1. Find Spinus and its reference material

| Install | Run Spinus as (`spinus` below) | Demo songs, samples and the manual |
|---|---|---|
| Flatpak | `flatpak run io.github.tuturu742.spinus` | `$(flatpak info --show-location io.github.tuturu742.spinus)/files/share/spinus/{demo,samples}`; manual in `.../files/lib/spinus/ai/docs/user` |
| AppImage | the AppImage file itself | inside the image (see below): `<mount>/usr/share/spinus` and `<mount>/usr/lib/spinus/ai/docs/user` |
| Arch package | `spinus` | `/usr/share/spinus/{demo,samples}`; manual in `/usr/share/doc/spinus` |
| Windows | `& "$env:LOCALAPPDATA\Programs\Spinus DAW\spinus.exe" ... \| Out-Host` (PowerShell) | beside `spinus.exe`: `demo\`, `samples\`, `docs\` |

If a path doesn't match, search near the program for `commands.schema.json` and a `demo` folder.

For the AppImage, `Spinus.AppImage --appimage-mount` prints the mount folder and then keeps running until
it's stopped. Start it in the background, read the folder from its first line, and stop that one process
(by the PID you started, not by name) when you're done; the folder goes away with it.

Read these before you write anything:

- `spinus --commands`: every command, with its parameters, as a JSON Schema.
- `spinus --describe all` (or `--describe spinus.3x-osc`, `--describe spinus.drum-machine`...): every
  instrument and effect, with each setting's ID, what it shows at 0, 0.25, 0.5, 0.75 and 1, and the full
  `choices` of settings that have a list. It also lists 3x Osc's presets, and the Drum Machine's pads (note
  and sound) and kits. Its last line, `notInstalled`, names the free plugins that can be downloaded.
  `device.setParameters` takes real units as text (`{"cutoff": "1.2 kHz", "attack": "20 ms"}`).
- The manual's `command-line.md`: the script format, the `loadKit` and `loadSamples` actions, and a
  complete small example. Then read the pages of the devices you use (`3x-osc.md`, `drum-machine.md`,
  `sampler.md`, `mixer.md`, `playlist.md`, `ducker.md`...).
- The demo songs (`demo/*/*.spn`, plain JSON): run `spinus --check` on one to see how a finished song
  is laid out: channels, groups, buses, sends, sidechains, automation and markers. They are the standard
  to reach.
- Free samples: `samples/Spinus Starter Pack/` (drums, loops with tempo and key in the name, vocal stabs,
  textures; see its README.md). Load them with `loadSamples`, or use them as audio clips.

Every option prints JSON, one object per line. `--run` stops at the first failing step and saves
nothing, unless you add `--keep-going`. Run it without `--save` first: that's a dry run of every step.

Use only what ships with Spinus, unless the user tells you which of their own plugins or samples to use.
Spinus's own instruments and effects, plus the bundled ones (`bundled.*`), work on every computer the song
is opened on.

## 2. Plan before notes

Write a short plan first, and show it to the user if they are around:

- **Brief:** genre, mood, tempo, key, length, and what the song is about.
- **Form,** bar by bar: for example Intro 8, Verse 16, Pre 8, Chorus 16, Break 8, Verse 16, Chorus 16,
  Bridge 8, Final chorus 16, Outro 8. Put sections on 4- or 8-bar phrases.
- **Energy curve:** a number from 1 to 10 per section. The song breathes: it builds, drops back and
  peaks late. Most of the arrangement follows from this curve.
- **Parts:** 10–20 channels for a full song, grouped by family (Drums, Bass, Keys or Synths, Leads, FX,
  Vocals or Textures), each with its role, its sound and where it plays. Each section adds or takes
  away something.
- **Harmony:** a progression per section, and the chorus differs from the verse. Plan the bass line's
  motion and where the tension and release are.
- **Motifs:** the hook (2–4 bars), its variations (rhythm, inversion, octave, a fill), and where each
  comes back.

## 3. Write it as a script, in pieces

Generate the script with a small program in any language. Don't write thousands of JSON lines by hand.
Build it in this order, and run `spinus --run` plus `spinus --check` after each piece, so a mistake shows
up while it's small:

1. **Project:** tempo, key, time signature.
2. **Mixer skeleton:** a bus per family, a reverb bus and a delay bus (sends), and the master chain:
   EQ, then compressor, then limiter, with the loudness meter last, so it measures what's heard (some demos
   put the meter before the limiter; that only changes what the meter shows).
   The delay bus uses Spinus's own `spinus.delay`, always there: `{"note": "1/8 dotted", "mix": "100%"}`
   (its time follows the song's tempo; `"mode": "Ping-pong"` bounces the echoes, `"high_cut": "5 kHz"`
   darkens the repeats). Spinus's own effects have no reverb. That comes from the free plugins
   (`bundled.dragonfly.hall`, `.room`, `.plate`, `.early`; `bundled.surge-xt-effects`), and only if
   `--describe all` lists them. If it doesn't, ask the user to get them in Spinus (Get free plugins).
   Otherwise, fake the space: longer releases, the delay bus, and a widened bus with the Stereo Utility.
3. **Channels:** each with its instrument and sound (a factory preset, then parameter changes), its own
   insert routed to its family bus, a colour and a group.
4. **Patterns:** one per part and section, named "Chorus - Bass". Ticks are 960 per quarter note (3840 per 4/4 bar,
   240 per 16th). Notes are MIDI numbers: the Piano Roll's C5 is 60, and so is the Drum Machine's pad 1.
   `pattern.setNotes` fits the pattern's length to its notes, so call `pattern.setLength` after it when a
   pattern must be a whole number of bars.
5. **The Playlist:** a lane per part, clips per section, and markers at section starts.
6. **Transitions:**
   - drum fills on the last bar of phrases;
   - risers (a noise sweep or a sample) into big sections;
   - an impact or crash on the downbeat;
   - filter sweeps and volume fades as automation clips;
   - a reversed swell or silence before a drop.
7. **Mix:**
   - levels by role: kick and bass loudest; then the lead or vocal; chords and pads under them;
     textures low;
   - sidechain the bass and pads to the kick with the Ducker;
   - cut the lows of everything but the kick and bass;
   - sends to reverb and delay, more for pads and leads, little or none for kick and bass;
   - pan the doubles and percussion apart;
   - master limiter ceiling about -1 dBTP for WAV or FLAC, and -2.5 dB for Opus or MP3. Encoding adds about
     1–1.5 dB of peak, so judge the encoded file's `truePeakDb`, not `unencodedTruePeakDb`.
   - **The sidechain needs the kick on its own insert.** A Drum Machine is one channel with one insert, so
     ducking from it ducks on every drum. Put the kick on its own Drum Machine channel (same kit, kick
     notes only) and duck from that insert.
   - velocity changes loudness only as much as the instrument's own velocity setting allows (3x Osc's
     `velocity`): set it if accents must be heard.
   - presets have their own level: a 3x Osc preset's `gain` can leave a part very quiet (the Noise Riser).
     Check every stem and raise `gain` before reaching for the fader.
   - stems are taken before the master, so a stem's true peak can be above 0 dBTP. Only the mix's peak
     matters.

## 4. Musical craft that separates a song from a loop

- **Repetition with change:** repeat a section, but change something every time it comes back: a new
  layer, a fill, a variation of the hook, a counter-melody, or a different drum pattern in its last bar.
- **Rhythm:**
  - vary velocities (accents on the beat, softer ghost notes);
  - swing where the style has it;
  - hi-hats are rarely all the same velocity;
  - leave space: rests are part of the groove.
- **Bass and kick** lock together. Bass notes mostly land on chord roots and fifths, and move by step
  into the next chord.
- **Voicings:** keep chords in the middle register (around C4–C5). Use inversions so the voices move
  little between chords. Don't stack a pad and keys in the same octave on the same chord.
- **Registers:** give every part its own range. Sub and bass go low, chords in the middle, the lead
  above them, sparkle on top. Two parts in one register mask each other.
- **Melody:**
  - mostly steps, with a few leaps, followed by a step back the other way;
  - phrases answer each other (question, then answer);
  - the hook is singable and rhythmic;
  - on strong beats, use chord tones.
- **Dynamics across the song:** the first chorus should not be the loudest moment. Keep something back
  for the last one: an octave up, an extra layer, double drums. Mono presets (3x Osc's leads and basses)
  play one note at a time: set `voices` to "Poly" before doubling a melody in octaves or playing chords.
  The master limiter flattens contrast, so make the quiet sections really thinner (fewer parts, a filter
  closed), not just lower.
- **Endings:** an outro that takes parts away, or a final hit with a reverb tail, never just a stop.

## 5. Test it: check, render, listen by numbers

After every big piece:

- `spinus --check <project>`: it must open cleanly, with nothing missing (exit code 0). Compare its counts
  with your plan (channels, patterns, clips, markers). Every channel has `"used"`, and `unusedChannels`
  lists any that never play: fix those.
- `spinus --render <project> --out mix.wav --stems stems/`: it prints each file's length, loudness (LUFS)
  and true peak (dBTP). Read them:
  - **Loudness:** about -14 to -9 LUFS for pop or electronic, quieter for ambient. True peak no more than
    -1 dBTP.
  - **Stems:** no stem is silent unless it should be, and none is far louder than its role. Integrated
    LUFS ignores silence, so a part that plays for a few seconds (a riser, a crash) reads loud: look at
    its `soundingSeconds` and true peak too.
  - For Opus or MP3, `unencodedTruePeakDb` is the mix's peak before encoding.
  - **Energy:** the energy per section (render sections, or measure the stems over time) follows your
    curve.
- With `--stems`, one render writes the mix and a file per mixer insert, `NN <insert>.<ext>`, into the
  stems folder. Every file reports `soundingSeconds`, the time it actually plays.
- If you can analyze audio (ffmpeg, Python with numpy), also check:
  - the loudness per section: Spinus has no per-section meter, so measure the render, for example
    `ffmpeg -i mix.opus -ss <start> -to <end> -af ebur128 -f null -` per section, using your markers'
    times. It should follow your energy curve;
  - the low end: the kick and bass together and nothing else below about 120 Hz;
  - clipping;
  - long silences you didn't intend.
- Fix, re-render, compare. Two or three rounds are normal.

## 6. Hand it over

Tell the user, in plain words:

- where the project and the render are;
- the form and the energy plan;
- the sounds;
- what you couldn't do (for example "I used the Drum Machine's own kit, since your drum plugin needs
  a kit chosen in its window").

They open the project in Spinus with `spinus "<folder>/<name>.spn"`, listen, and change anything:
everything you did is ordinary Spinus channels, patterns and clips.

## Things that are easy to get wrong

- Settings with a list (an EQ band's type, a filter's mode) take one of their `choices` exactly ("Low cut"
  for a high-pass). A wrong name is refused with the list in the error.
- Automation targets: `{"kind": "deviceParameter", "device": <id>, "parameterId": "<id>"}`,
  `{"kind": "channelVolume", "channel": <id>}`, `{"kind": "channelPan", ...}`,
  `{"kind": "insertVolume", "insert": <id>}` (`spinus --commands` documents each). Give automation points
  as text in real units, `{"time": 0, "text": "-12 dB"}`, `"1.2 kHz"`, `"30% L"`, rather than 0–1 values.
- Instruments and effects in commands are `{"kind": "stock", "stockType": "spinus.3x-osc"}`.
- Audio files for `loadSamples` or audio clips: give absolute paths. They're copied into the project's
  `audio` folder when it's saved. A single file in a Sampler plays at its own pitch on C5 (60).
- `text` in automation points works in `--run` scripts only (the schema itself lists `value`).
- An automation value stays where its clip left it after the clip ends: add a point back to the resting
  value if the sweep shouldn't last.
- The Drum Machine's factory kits all use the Synth Kit's layout (`--describe spinus.drum-machine` shows
  it): kick on pad 1 (60), snare on pad 2 (61), closed and open hi-hat on 4 and 5, and so on.

## Limits

- Plugins the user installed: add one in Spinus by hand, save, and run `--check` to see how it's named in a
  project. Then a script can add it by that name and set its parameters. Its own presets, and sounds chosen
  in its window, can't be set from a script. Prefer Spinus's own instruments (3x Osc presets, Drum Machine
  kits, the Sampler with the starter pack) unless the user asks.
- Free plugins (`bundled.*`, such as Dragonfly reverbs and Surge XT) work only once the user has downloaded
  them in Spinus (`--describe all` lists the ones that are there). Spinus's own effects always work.
- Don't use copyrighted audio, and don't imitate a real artist's recording note for note. Write
  originals.
