# Changelog

What changed in each version of Spinus DAW. The [user manual](docs/user/README.md) explains every
feature; [Known issues](docs/user/known-issues.md) lists what doesn't work yet.

## Not released yet

### Fixes

- **Closing a song you only opened no longer asks to save it.** Some plugins (Ample's, for example) change
  their own settings a moment after loading; Spinus took that for a change of yours. A plugin's settings now
  count only if you opened its window; changes made with Spinus's own controls count as always.

## 0.1.0-alpha.2 (2026-10-26)

### Finding out what went wrong

- **Log files:** Spinus writes a log of each run (the sound card it opened, projects opened, saved and
  exported, plugin scans and plugin crashes, the Windows plugins setup, the AI service, and every error it
  showed you), so a problem can be found from the file you attach to a bug report. **Help › Open log folder**
  opens them; the last 10 runs are kept. Your home folder is written as `~`, and keys, tokens and email
  addresses are masked, so the logs hold no keys and no user name. After a crash, the next run's log names
  the run that ended unexpectedly. See [Troubleshooting](docs/user/troubleshooting.md#log-files).
- **Help › Report a problem...:** makes a file with the logs of the last three runs, the version and the system,
  and opens GitHub's bug report page with the version and system filled in, to drag the file into. Spinus
  sends nothing itself. The bug report and idea pages on GitHub are now forms. See
  [Manual and About](docs/user/help.md#report-a-problem).

### Making music

- **Zoom buttons and keys:** the Playlist, Piano Roll, Automation editor and Spinus Tune have **−**, **+**
  and **Fit** buttons, and **Ctrl+=**, **Ctrl+-** and **Ctrl+0** (fit) zoom them. **Ctrl**+wheel now zooms
  around the mouse everywhere, the Beat Slicer's loop too, and **Ctrl+Shift**+wheel makes the Playlist's
  tracks taller or shorter.
- **Follow:** the Playlist, Piano Roll and Automation editor turn the page while the song or pattern plays,
  so the play position stays in sight. The **Follow** button switches it off; Spinus remembers your choice.
- **FL Studio's editing keys:** in the Piano Roll, **Shift+Up/Down** moves notes a semitone, **Ctrl+Up/Down** an
  octave and **Shift+Left/Right** a snap step, and the note tools have keys (**Alt+Q** quantize, **Ctrl+L**
  legato, **Ctrl+U** chop, **Alt+A** arpeggiate, **Alt+S** strum, **Alt+R** humanize). The Playlist gets
  **Ctrl+C**, **Ctrl+X**, **Ctrl+V** and **Ctrl+B** (duplicate) for clips, and **Shift**+arrows to move them
  a grid step or a track. **Ctrl+D** selects nothing and **Page Up/Down** zoom, in both.
- **The Piano Roll's bar numbers** show across the whole width again: its buttons have a row of their own
  above the timeline (they hid the bars after the first).

### Windows and shortcuts

- **Shortcuts work everywhere:** Spinus's keys now work from a panel in its own window, a plugin's window
  and Spinus Tune too, and Ctrl+Alt+1 (or Ctrl+=) in the Browser's search box no longer types "1" instead
  of switching the layout.
- **Your own layouts get shortcuts:** Ctrl+Alt+4 to 9 open your saved layouts, in the order saved.
- **Layouts are kept while Spinus runs:** the windows come back as they were even when Spinus was ended
  without closing, and two Spinus windows open at once no longer lose each other's saved layouts.
- **Closing with a menu open:** the window's close button now closes Spinus even while one of the top menus
  is open (it did nothing before).

### Sounds

- Three more free one-click downloads, for Linux and Windows: the **Six Sines** FM synthesizer, with its
  factory patches, and **TIME-12** and **GATE-12**, which shape the delay time and the volume in time with
  the song (stutters, tape stops, trance gates), drawn with the mouse. See
  [Get free plugins](docs/user/free-plugins.md).

### Plugins

- **The Flatpak runs your system's plugins** that need newer libraries than it has (most plugins from Arch's
  packages, for example): they run in a separate process with your system's own libraries instead of being
  listed as problem plugins. The next **Scan plugins** tries the ones listed before. See
  [Install](docs/user/install.md#as-a-flatpak).

### Fixes

- **Adding an instrument to a new project no longer crashes Spinus.** With no pattern yet, pattern mode asked
  the audio engine to loop over nothing, and the engine could then divide by zero on its audio thread (most
  often seen adding a Windows plugin, since those take a moment to load). Playback now loops only while it
  plays; with no pattern, pattern mode loops one bar.

## 0.1.0-alpha.1 (2026-10-24)

The first alpha: an early, quiet release for testers before 0.1.0. Everything below is new.

### Making music

- **Channel Rack** with a step sequencer, swing, triplet steps, channel groups, and a picture of each
  channel's notes.
- **Piano Roll:** drawing, painting, slicing and selecting notes; snap to any grid; ghost notes from
  other channels; scales; chord stamps and chord names; strum, arpeggiate, chop, legato, quantize and
  humanize; a lane for velocity, release, pan, fine pitch and volume per note; slides; copy and paste
  between channels; notes sound as you place them.
- **Playlist:** patterns, audio and automation clips; markers; track groups that fold; reverse, pitch
  and speed for audio clips; takes and comping; make a clip unique. Clicking a pattern clip makes its
  pattern the current one (top bar, Channel Rack, Piano Roll), as in FL Studio.
- **Launcher:** an Ableton-style grid of scenes for jamming, recorded into the Playlist on request.
- **Mixer:** an insert per channel, effects that can be added, moved and removed while playing,
  sends, buses, sidechains, gain reduction and CPU on each strip, and a Routing view.
- **Freeze** a channel or insert to audio to save CPU.
- Undo and redo for everything, with a History panel. Tempo, key and time signature per song and
  pattern. Your own keyboard shortcuts and window layouts.

### Sounds

- Spinus's own **Sampler**, **Drum Machine**, **3x Osc** and **Beat Slicer**, also as VST3 and LV2
  plugins for other programs.
- **3x Osc presets:** 23 ready-made sounds (basses, leads, pads, keys and effects) in a menu at the top of
  its window, with **Default** to start again; choosing one can be undone.
- Spinus's own effects: **Spinus EQ** (eight bands dragged on a curve over a live spectrum),
  **Spinus Compressor** (with parallel mix and sidechain), **Spinus Limiter** (look-ahead, true peak),
  **Spinus Gate**, **Filter**, **Sidechain Ducker**, **Stereo Utility**, **Loudness Meter** and
  **Spinus Tune** (pitch correction, live or edited note by note). The basic mixing effects need no
  download, on every system Spinus runs on.
- **Surge XT** (on its own window, as its makers designed it), **LSP Plugins** (Linux) and **Dragonfly
  Reverb** as free one-click downloads from their own projects.
- More free one-click downloads: the synthesizers **Odin 2** (Linux) and **OB-Xf**, and the
  **PeakEater** clipper. **Get free plugins** lists only what each project publishes for your system,
  and only open-source builds (no installers, no account-gated or feature-limited versions).
- **Remove** a free plugin Spinus downloaded, from **Get free plugins**: it asks first, and won't remove
  one the open project is using.

### Plugins

- VST3, CLAP, LV2 and VST2 plugins, scanned safely in a separate process; each plugin listed once, in
  the format you choose.
- **Windows plugins on Linux** through Wine and yabridge, with a tested Wine that Spinus sets up for you, and
  installers run from Spinus. Remove a Windows plugin from Options › Windows plugins; a switch draws
  plugin windows without the graphics card, for windows that don't update. A Windows plugin that asks
  something in a window of its own (for example to register) has that window brought to the front, and
  Spinus's title says it is waiting for it.
- An instrument plugin that can't be loaded shows **Missing** on its channel (as effects do in the
  Mixer), and Spinus says so when you add it; a plugin file that doesn't load is listed under Problem
  plugins with the reason.
- **Crash protection:** a plugin that crashed Spinus once can run in its own process from then on, where
  a crash only silences that plugin until you reload it.
- An LV2 plugin whose own window can't open gets Spinus's sliders for its settings instead.

### Recording

- MIDI from a keyboard or the computer keyboard, with optional quantizing; MPE controllers.
- Audio from any microphone, choosing it by name, and from a sound card with many inputs: each armed
  track records its own input, all at once.

### Projects and export

- Projects are folders with a `.spn` file and their audio inside; autosave and recovery; a project
  opened twice is noticed.
- **Four demo songs come with Spinus**, listed in the Browser under **Projects**; each opens as your own
  copy in your Music folder, and all are free to reuse under CC BY 4.0. If one needs free plugins you
  haven't downloaded, Spinus offers them right away, naming each one.
  - *Siskin* (electronic, 124 BPM): patterns, automation clips, buses and sends, stems.
  - *Neon Coast* (synthwave): 3x Osc presets, a gated snare, arpeggiated bass, a 6/8 bridge and a last
    chorus a tone up.
  - *Sodium Lights* (liquid drum & bass, 174 BPM): the Beat Slicer on a breakbeat, the Launcher,
    sidechain ducking, freeze and track groups.
  - *Rainy Window* (lo-fi hip hop, 82 BPM): swing and triplets, the Sampler, chord stamps and strum,
    note lanes, takes and comping, and Spinus Tune.
- **Projects in the Browser:** every project you open or save is listed there, and folders of projects
  you add are searched (with their subfolders) for songs; double-click to open, right-click to show it
  in the file manager or remove it from the list.
- **Help › Manual** (**F1**): the whole manual inside Spinus, with links between pages, pictures and a
  search across every page that works without AI. **Help › About Spinus**: version (with a button to
  copy it for a bug report), licenses and the projects Spinus is built with.
- Export to WAV (16, 24, 32-bit), FLAC, MP3, Opus and AAC: the song, the current pattern, or stems per
  insert, optionally while it plays.
- Stems are made in the same pass as the mix, so exporting every stem takes about as long as the mix
  alone. A reverb or delay insert fed by sends has its wet sound in its stem, so the stems add up to the
  mix. Stems skip the Master, so muting the Master doesn't mute them.
- Songs with many instruments (dozens, with thousands of notes each) stay quick to edit and save.

### AI (optional, off until turned on)

- An **assistant** that knows the manual and your project, and proposes changes you apply or reject
  (each applied proposal is one undo step). Connections to Anthropic, OpenRouter or a local server
  (Ollama), with usage per month.
- **Generate** notes in the Piano Roll; **hum, sing or beatbox** an idea into notes.
- The **Analyzer** (experimental: hidden until **Show experimental features** is ticked in
  **Options › AI...**): a song's tempo, key and stems (up to 53 instruments), turned into patterns played
  by Spinus's instruments, the song's own sounds, matched or tuned Surge XT, the free open-source synths
  or your own plugins. Odin 2 and OB-Xf can be tuned to a stem too, and so can any installed instrument
  plugin, as an experiment (Spinus picks the settings to tune by their names). **Closest preset** works
  for OB-Xf (its factory patches come with its download), Odin 2 (its built-in presets) and, as an
  experiment, any installed instrument with presets Spinus can choose from (its programs, or a CLAP
  plugin's own preset list).
- The **Automixer:** suggested mix changes with reasons, applied in one step, with before and after.
- **Show experimental features** in **Options › AI...** (off at first): shows features still being
  worked on (in this version, the Analyzer), and hides them again without a restart.
- The AI service stops by itself if Spinus stops unexpectedly.

### Packages

- **Linux:** an Arch Linux package (release and newest code), an AppImage and a Flatpak. File dialogs
  (open, save, export, choosing a folder) are your desktop's own (KDE's on Plasma, GNOME's on GNOME, with
  their places and bookmarks), also in the Flatpak; without a desktop portal (a bare window manager)
  Spinus shows its own file browser. **Open** takes the project's `.spn` file.
- The Flatpak runs Windows plugins (with Spinus's tested Wine, sharing the Windows folder with the other
  Spinus; 32-bit Windows installers run with Flatpak's 32-bit support, and Spinus says how to install it
  when it is missing), lists the system's plugins it can load and says why the others can't, and keeps
  its own plugin list.
- **Windows** (10 and 11, 64-bit): an installer that needs no administrator rights, with ASIO and
  Windows Audio, native Windows plugins, and the AI service's own Python. Not on Windows in this
  version: LSP Plugins' effects, Odin 2 and song analysis (see Known issues).
