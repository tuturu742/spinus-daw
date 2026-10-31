# Coming from FL Studio

Spinus works the way FL Studio does: channels in a Channel Rack, patterns, a Playlist to arrange them
and a Mixer with inserts. Most habits carry over. This page shows where FL's things are in Spinus, what
is different, and which features FL Studio users often ask for that Spinus already has.

## Where things are

| FL Studio | Spinus |
|---|---|
| Channel Rack and step sequencer | The [Channel Rack](channel-rack.md) (**F6**), with a step sequencer per channel |
| Piano Roll | The [Piano Roll](piano-roll.md) (**F7**) |
| Playlist | The [Playlist](playlist.md) (**F5**): patterns, audio and automation clips on tracks that are not tied to channels |
| Mixer | The [Mixer](mixer.md) (**F9**): inserts, effect slots, sends, sidechain |
| Browser | The [Browser](browser.md) (**F8**): instruments, effects, installed plugins |
| Pattern and song mode | **PAT / SONG** in the top bar, or **L** |
| Pattern list, Clone pattern | The pattern list in the top bar, with **Clone** ([The main window](main-window.md#transport)) |
| Each channel on its own mixer insert | Automatic: every new channel gets an insert of its own |
| Draw, Paint, Slice, Select tools | The same, on **P**, **B**, **C**, **E** |
| Make unique | Shift+right-click a Playlist clip › **Make unique** |
| Last tweaked parameter | **Edit › Automate the last moved control** ([Automation clips](playlist.md#automation-clips)) |
| Filter channels by group | The list next to **Swing** in the Channel Rack ([Groups](channel-rack.md#groups)) |
| Note repeat / chop, strum, arpeggiate, legato, quantize | The Piano Roll's **Tools** menu ([Note tools](piano-roll.md#note-tools)) |
| Chord stamps | The Piano Roll's chord list ([Chords](piano-roll.md#chords)) |
| Ghost notes | The Piano Roll's **Ghosts** ([Ghost notes](piano-roll.md#ghost-notes)) |
| Slide notes | 3x Osc in **Mono (slides)** ([Note tools](piano-roll.md#note-tools)) |
| Typing keyboard to piano | **Ctrl+T** ([Playing with the computer keyboard](piano-roll.md#playing-with-the-computer-keyboard)) |
| Fruity Slicer / Slicex | [Beat Slicer](beat-slicer.md) |
| Fruity Parametric EQ 2 | [Spinus EQ](eq.md) |
| Fruity Compressor, Fruity Limiter in compressor mode | [Spinus Compressor](compressor.md) |
| Fruity Limiter on the master | [Spinus Limiter](limiter.md) |
| Fruity Limiter in gate mode | [Spinus Gate](gate.md) |
| Fruity Delay 3 | [Spinus Delay](delay.md) |
| Fruity Limiter sidechain, Peak Controller ducking | [Sidechain Ducker](ducker.md), or the [Spinus Compressor](compressor.md) with a sidechain |
| Fruity Stereo Shaper, stereo widening | [Stereo Utility](stereo-utility.md) |
| 3x Osc | [3x Osc](3x-osc.md), Spinus's own |
| Sytrus, Harmor and other big synths | [Surge XT](surge-xt.md), a free download, and other [free plugins](free-plugins.md) |
| Newtone and Pitcher | [Spinus Tune](spinus-tune.md): live and note by note |
| Smart disable, freezing | [Freeze](freeze.md) a channel or insert |
| Performance mode | The [Launcher](launcher.md), Ableton-style |

Note names follow FL Studio: middle C is **C5**.

## Keys

Spinus uses FL Studio's keys where it has the same thing:

| Key | Does | Same as in FL Studio |
|---|---|---|
| **F5**, **F6**, **F7**, **F9** | Playlist, Channel Rack, Piano Roll, Mixer | Yes |
| **F8** | Browser | No: in FL Studio, F8 opens the plugin picker |
| **F10** | Audio settings | Close: FL Studio's F10 opens its settings |
| **Space**, **L**, **R** | Play or stop, pattern or song mode, arm recording | Yes |
| **P**, **B**, **C**, **E** | Draw, Paint, Slice, Select | Yes |
| **Ctrl+R** | Export audio | Yes |
| **Ctrl+B** | Duplicate the selected notes (Piano Roll) | Yes |
| **Ctrl+T**, **Ctrl+M** | Typing keyboard to piano, metronome | Yes |
| **Ctrl+Z**, **Ctrl+Shift+Z** | Undo, redo | Undo yes; FL Studio steps back through its history with Ctrl+Alt+Z instead |
| **Ctrl+H** | The History panel, every change in a list | No |

Every key in the first tables of [Keyboard shortcuts](keyboard-shortcuts.md) can be changed in
**Options › Keyboard shortcuts...**.

## Files

- **FL Studio projects (.flp) don't open in Spinus**, and Spinus 0.1.0 doesn't read MIDI files either. To
  bring a song over, export its parts (stems) from FL Studio as audio and drop them onto the Playlist:
  each file becomes an audio clip on an [audio track](audio-tracks.md) with its own mixer insert.
- **Samples:** WAV, AIFF, FLAC, Ogg, MP3 and Opus work as they are; with ffmpeg installed also M4A/AAC,
  WebM, WMA and more. Drop them onto the Channel Rack for a [Sampler](sampler.md) channel, or onto the
  Playlist ([Audio clips](playlist.md#audio-clips)).
- **Projects are folders.** A project "My Song" is a folder with "My Song.spn" and an `audio` folder
  inside, so samples and recordings travel with it. See [Projects](projects.md).
- **Export:** WAV, FLAC, MP3, Opus and AAC: the song, a pattern or stems ([Exporting audio](export.md)).
- **Plugins and their presets:** FL Studio's own plugins (Sytrus, Harmor, FPC and the Fruity effects)
  and its preset files (.fst) are FL Studio's and don't come along. Third-party plugins you own work in
  Spinus, with their own presets: Linux builds directly, Windows builds through
  [Windows plugins](windows-plugins.md).

## Different in Spinus

- **Audio tracks.** Long audio (vocals, stems) lives on [audio tracks](audio-tracks.md) in their own
  panel (**Shift+F6**), not as Sampler channels.
- **Plugins.** Linux plugins (VST3, CLAP, LV2, VST2) load directly; Windows plugins run through Wine,
  which Spinus sets up for you ([Windows plugins](windows-plugins.md)). A plugin that crashed once can run
  in its own process ([When a plugin crashes](plugins.md#when-a-plugin-crashes)).
- **Free instruments to download.** Instead of FL's stock plugins, Spinus offers Surge XT, Odin 2,
  OB-Xf, Six Sines, LSP Plugins, Dragonfly Reverb, PeakEater, and TIME-12 and GATE-12 (Gross Beat-style
  time and volume shapers) as one-click downloads ([Get free plugins](free-plugins.md)).
  Its own Sampler, Drum Machine, 3x Osc, Beat Slicer and mixing effects are built in.
- **AI help, off by default.** An [assistant](assistant.md) that knows this manual and proposes changes
  you approve, a [pattern generator](piano-roll.md#generate), and the experimental [Analyzer](analyzer.md)
  for turning a song into stems and patterns (tick **Show experimental features** in **Options › AI...**).
- **Free software.** No editions, no subscription, nothing to buy: every feature is in every copy
  ([Is Spinus free?](faq.md#is-spinus-free)).

## Features FL Studio users asked for

FL Studio users often ask for these in FL Studio itself. Spinus 0.1.0 has them:

| Asked for | In Spinus |
|---|---|
| A native Linux version, without Wine | Spinus runs on Linux natively, and on Windows ([Installing Spinus](install.md)) |
| Windows plugins on Linux without setting up Wine | Spinus sets up Wine and yabridge for you ([Windows plugins](windows-plugins.md)) |
| More than 10 effect slots per mixer track, and no limit on mixer tracks | No limit on either ([Mixer](mixer.md#effects-output-and-sends)) |
| Track freeze | [Freeze](freeze.md) a channel or a mixer insert, and unfreeze it to edit |
| Folders and groups for big projects, also groups inside groups | Playlist track groups that fold, mute and nest ([Track groups](playlist.md#track-groups)); Channel Rack groups, which can play through a new mixer bus ([Groups](channel-rack.md#groups)) |
| Shape and LFO automation instead of drawing points | **Shapes** in the Automation editor: ramps, sine, triangle, square and saw ([The Automation editor](playlist.md#the-automation-editor)) |
| Automation that goes when what it controls goes | Removing a channel or an effect removes its automation too (Ctrl+Z brings both back) |
| Ghost notes from other channels, and editing them | Ghost notes in the Piano Roll; double-click one to edit its channel ([Ghost notes](piano-roll.md#ghost-notes)) |
| Recordings that land in the right place | A take goes on a new audio track, where it was played, with its own mixer insert ([Recording audio](audio-tracks.md#recording-audio)) |
| Comping: several takes, keeping the best parts | Take lanes, and Alt+click to choose a take ([Takes and comping](audio-tracks.md#takes-and-comping)) |
| Time-stretch, pitch and reverse for audio clips | Shift+right-click an audio clip, or Alt+drag its end ([Reverse, pitch and speed](playlist.md#reverse-pitch-and-speed)) |
| Melodyne-style editing of a vocal (ARA) | [Spinus Tune](spinus-tune.md#editing-a-vocal-note-by-note), note by note |
| Make Unique | Shift+right-click a pattern clip › **Make unique** ([Playlist](playlist.md)) |
| Copying an effect, with its settings, to another track | Right-click an effect › **Copy to insert** ([Mixer](mixer.md#effects-output-and-sends)) |
| Markers on the Playlist | Double-click the ruler ([Playlist](playlist.md)) |
| Swing that is easy to reach | The **Swing** knob in the Channel Rack ([Channel Rack](channel-rack.md)) |
| Chord tools and chord names | Chord stamps and chord names in the Piano Roll ([Chords](piano-roll.md#chords)) |
| Note repeat, strum and arpeggiate | The Piano Roll's **Tools** ([Note tools](piano-roll.md#note-tools)) |
| Fully custom keyboard shortcuts | **Options › Keyboard shortcuts...** ([Keyboard shortcuts](main-window.md#keyboard-shortcuts)) |
| Saved window layouts | **View › Layouts**: Compose, Arrange, Mix and your own ([Layouts](main-window.md#layouts)) |
| Plugin windows that don't hide behind the Playlist | They stay above the main window ([Plugin windows](plugins.md#plugin-windows)) |
| Sidechain and routing you can see | The routing line on each mixer strip, and **View › Routing** ([The Routing panel](mixer.md#the-routing-panel)) |
| Gain-reduction meters on mixer strips | An orange bar beside the meter ([Strips](mixer.md#strips)) |
| Finding the plugin that uses the CPU | CPU use on every mixer strip and every effect slot ([Strips](mixer.md#strips)) |
| A loudness (LUFS) meter | The [Loudness Meter](loudness-meter.md) |
| A stereo widener | The [Stereo Utility](stereo-utility.md) |
| Hearing the song while it exports | **Play while exporting** ([Exporting audio](export.md#play-while-exporting)) |
| MPE controllers (ROLI, LinnStrument) | Per-note bend, pressure and slide while playing ([MPE controllers](piano-roll.md#mpe-controllers)) |
| Plugin scans that don't have to be repeated | Only new or changed plugin files are scanned ([Scanning](plugins.md#scanning)) |
| Stability: a plugin crash shouldn't take everything down | Scanning in a helper program, plugins in a separate process after a crash ([When a plugin crashes](plugins.md#when-a-plugin-crashes)), autosave and recovery ([Projects](projects.md#autosave-and-recovery)) |
| AI that is optional, with a clear off switch | AI is off until you turn it on, with one switch; the manual says what is sent ([AI is off until you turn it on](assistant.md#ai-is-off-until-you-turn-it-on)) |
| Stem separation | The [Analyzer](analyzer.md) (experimental) |
| Free updates, no subscription | Spinus is free and open source ([Is Spinus free?](faq.md#is-spinus-free)) |

---

FL Studio is a trademark of Image-Line. Spinus is not affiliated with Image-Line.
