# Changelog

What changed in each version of Spinus DAW. The [user manual](docs/user/README.md) explains every
feature; [Known issues](docs/user/known-issues.md) lists what doesn't work yet.

## Not released yet

### Fixes

- An automated setting can be turned by hand before its first automation clip and after its last one.
  Before, the clips held it for the whole song: their first value before them and their last value after
  them, and a knob turned there jumped back. Between two clips the value still holds, as in FL Studio.
  Holding a knob in a plugin's window takes over even during a clip, until you let go.
- A new automation clip goes where you are working: over the area selected in the Playlist's timeline if
  there is one, else four bars from the song position. Before, it always started at the beginning of the song.
- Right-click an effect in the Mixer › **Replace effect** puts another effect in its slot, as Replace
  instrument does in the Channel Rack. Its place and dry/wet stay; one Ctrl+Z brings the old one back.
- Automation of Spinus's own devices (Filter, EQ, Delay, compressor and the rest, and the 3x Osc, Sampler and
  Drum Machine) now changes their sound. Before, an automation clip moved the setting but the sound stayed where
  the knob was last turned by hand, and the device's window did not move either until it was closed and opened
  again. Its knobs now follow the automation while the song plays. Other plugins were not affected.
- A knob turned in a plugin's own window can be undone: each drag is one **Ctrl+Z** step, which puts the knob
  back where it was. Before, Ctrl+Z skipped it and undid the change before it instead.
- An agent's song summary (and `spinus --check`) now lists each Playlist lane's clips: where they start, how long
  they are and what they play. Before, it only counted them, so an agent could not move clips between lanes.
- An agent could only see a plugin's first 80 or so settings, so on big plugins (Ample Guitar's amp section, for
  one) it could not find what to change. It now pages through all of them or looks them up by name.
- The Browser scrolls sideways when a name is longer than it is wide, so long sample, plugin and project names
  can be read in full instead of being cut off.
- Deleting a channel deletes its mixer insert too, unless another channel or insert still uses it. It used to stay
  behind in the Mixer. Ctrl+Z brings both back.
- A plugin that sends out a burst far louder than any real sound (Constant Signal Generator from the SWH pack
  does as it starts) is muted for that moment instead of cracking through the speakers and pinning the mixer's
  meters. The bar at the bottom of the window says which plugin it was.
- The Piano Roll no longer crashes on the first click on a new instrument: the pattern that click creates used to
  reload the notes while the mouse was still down, taking away the note being placed. The velocity lane is safe
  from the same problem now.
- One click, one undo step: the first step, note, Swing or Triplet change in a new project, and a Playlist clip
  placed below the last track, used to need two **Ctrl+Z** (the second taking away an empty pattern or empty
  tracks).
- Clicking a Channel Rack step no longer resets the release, pan, fine pitch and volume of the other notes.
- Snap **None** in the Piano Roll: **Ctrl+U** chopped a note into thousands of one-tick notes (and could freeze the
  window), and **Shift+Left** / **Shift+Right** moved notes one tick. Both use a sixteenth now, like the other note
  tools.
- With no note selected, the arrow keys and note tools still change every note, but no longer leave all of them
  selected (as in FL Studio), so the next drag moves one note, not all.
- The velocity lane can reach each note of a chord: selected notes come first, then the bar nearest the mouse.
- The Piano Roll's buttons and lists no longer take the keys away from the notes: after choosing a tool or a snap,
  **Delete**, **Ctrl+C** and the arrow keys work again without clicking the grid (which added a note).
- The first Playlist click in a new project did nothing. It now makes **Pattern 1** and places it.
- Playlist with a folded group: a selection box picked clips on the wrong (hidden) tracks, and **Ctrl+A** then
  **Shift+arrows** did nothing. Clips folded away now move in time with the others and stay on their tracks.
- The Playlist no longer crashes if an automation point is being dragged when its clip is undone.
- Flatpak: LV2 plugins from Flathub's plugin packs (such as the SWH and TAP plugins) load again. They were listed
  in the Browser but showed as Missing when added, with "Unable to locate plugin" in the log.
- The tempo box no longer stays stuck over the tempo after a double-click: Escape, Enter or clicking elsewhere
  closes it. A typed tempo above 999 or below 20 BPM is set to 999 or 20; text that is not a number changes
  nothing.
- Scrolling the mouse wheel over the tempo is one undo step per scroll, not one per notch.
- Save as over another project (Replace) puts this project's audio files there: an audio file of the same name
  from the old project no longer stays and plays in place of yours.
- Pressing Play with no sound card open now says why nothing is heard, and where to choose one (Options › Audio
  settings..., F10).
- A crashed project opened from the file manager or the command line asks "Recover your work?" once, not twice.
- When Spinus can't save, the message says why (no permission to write there, a file in the way, a full disk) and
  what to do next.
- Opening a project that another Spinus already has open (double-clicking its .spn file) says so and points to
  that window, instead of starting a second, empty Spinus with an error.
- **Ctrl+Y** redoes, as **Ctrl+Shift+Z** does.
- An empty Channel Rack says how to add the first instrument.
- In a key written with flats (C minor, F major, ...), the Piano Roll names notes and chords with flats: Eb5 and
  Bb, not D#5 and A#.
- Recording messages name the real menu item, Options › Audio settings....
- Manual: Projects do show in the Browser (Known issues); `--no-audio`, `--snapshot`, `--version` and
  `--self-test` are on the command-line page; Backspace deletes like Delete; what clicking the Playlist's ruler
  does in pattern mode.
- **Export no longer locks Spinus.** The Export audio window stays open beside your work: you can keep editing
  and playing while it exports, as the manual said. Closing the window cancels the export.
- **Exports are named after the project** and start in the project's folder; only a project you haven't saved yet
  is called "Untitled" and starts in Music. MP3, Opus and AAC files get the project's name as their title.
- **An empty song is caught before you pick a file name**, not after.
- **The MP3 and AAC hints fit your system:** the Arch install command only on Linux, a plain sentence on
  Windows and in the Flatpak (which has neither program).
- **Mixer faders, pan and send knobs show their value** while you point at them or drag them, and the faders and
  send knobs now go all the way down to silence (-inf dB) instead of stopping at -60 dB.
- **"+ Send" no longer offers bad targets:** the insert the sound already plays into (usually the Master, where a
  send only made it louder) and inserts that would make a feedback loop are greyed out, with the reason.
- A mixer strip's right-click menu can no longer act on a strip that was deleted while the menu was open
  (scrolled out of view), which could crash Spinus.
- The manual now says that removing a mixer insert also removes the sends and sidechains to it and its automation
  clips (all undoable in one step).
- **Coding agents (the live interface):**
  - **Two agents no longer knock each other off.** A second agent (another Claude Code window, or Cursor) used to
    end the first one's session and reset **Undo the agent's changes**. Now each has its own session, the top bar
    names them all, and the undo point stays at when the first one connected.
  - **An agent's own undo_last no longer counts as your change**, so its next change isn't refused for it.
  - **Opening another project or File › New while an agent is connected:** the agent is told before its next
    change, and **Undo the agent's changes** starts from the new project instead of undoing the wrong steps.
  - **Playing a section works past the last part** (a section marked before its parts are written, or bars
    added at the end), and "Chorus 2" plays the second section named Chorus.
  - **Render measures the part before the first marker too** ("Start"), and its numbers are short (-16.9, not
    -16.899999999999999).
  - **list_presets on the Drum Machine, Sampler or another Spinus device** says they have no presets and how to
    change their sound, instead of "There is no device".
  - **With AI off, the agent is told only about the tools it has**, and the ready-made requests say how to do
    their job without the music tools. Turning AI on or off tells the agent's app that its tools changed. A
    misspelled tool name gets the right message.
  - **Tools explain their settings** to the agent (what each value means), and steps can use `"as"` labels as
    scripts do. Tools that overwrite settings or routing (shape sound, send, duck, bus) are marked as such.
  - **Safer:** the token comes from the system's secure random source, its settings file is private from the
    start, only this computer's own addresses count as local, and the protocol version is agreed properly.
  - **The Claude Code setup command adds `--scope user`**, so Spinus is there in every folder, not only the one
    where you ran it. The manual says where Cursor's file goes, what Disconnect really does (to keep an agent
    out, untick the option or make a new token), and what an agent can't do.
- **The music harness's command-line MCP server** (`python -m spinus_ai.harness mcp`) answers bad input with an
  error instead of stopping.

### Automation clips: see what a clip moves

- Click an automation clip on the Playlist (or open it in the Automation editor) and what it moves is outlined
  in yellow: the effect's slot and its insert's strip in the Mixer, the strip for an insert's volume or pan, or
  the channel in the Channel Rack for a channel's volume, pan or instrument. The outline goes when the clip is
  no longer selected.
- An automation clip's menu (**Shift+right-click**) has **Show ...**, for example **Show Filter on
  Master**: it opens the Mixer at that insert, or the Channel Rack at that channel, and the device's window.
- The clip's label names the device and where it is: **Filter (Master): Cutoff**, **3 Drums: Volume**.

### Track creator: remembered per project

- A new project starts with an empty Track creator, like the Channel Rack and the Mixer. Each saved project keeps
  its own reference songs, summary, description and parts, and opening it brings them back. Songs analyzed once
  stay ready in every project.

### Track creator: songs that sound written, not generated

- Songs with lyrics no longer get filler. Round 2's songs had long copied intros and outros (sometimes two of each)
  to leave room for instruments. Now the AI writes that room as real sections with their own music, and Spinus's
  own writer keeps intros and outros to 8 bars and puts breaks and drops between the sung parts.
- The instrument standing in for the voice is back on top of the mix.
- Taught by Spinus's demo songs: the lead is a hook that comes back and resolves, with its peak note loudest; the
  drums follow the phrases (an open hi-hat closing each 4 bars, a fill closing each 8, a crash opening the next,
  builds rolling into drops, outros fading).
- Parts come in as the song builds: in long choruses and verses some parts join halfway through instead of all at
  the first bar.
- The last chorus is the biggest: Spinus's writer plays it a little harder, doubles its lead an octave up and adds
  a crash at its middle (**Last chorus** on the Playlist).
- The AI writer is told how the demo songs do it, and **check** now tells it when a song sounds generated: a chorus
  played the same every time, a lead with no figure that comes back, a part with every note at one velocity, or
  drums looping one bar through a long section.
- No more automation fighting itself: an automation clip can't move what another moves on the same part over the
  same bars, and a transition placed twice adds no second clip.
- Starting levels: one part that measures oddly quiet (a sub-heavy bass) no longer pulls every other fader down.

### Track creator: Learn this instrument

- **Strummer modes**: an instrument that plays chords from its chord keys (a guitar strummer) can now be used
  for a part that plays chords. Spinus holds each chord of the song on the chord keys and presses one of the
  instrument's pattern keys with it, picked by how loud each section is, or named by your AI connection.
- **Export... and Import...** in Learn this instrument: share what Spinus learned of an instrument as a file.
  An imported profile is a proposal you Apply, and the keys you marked stay yours. 3x Osc, Surge XT, OB-Xf and
  Odin 2 come with a profile.
- **Songs use the instrument's modes**: for an instrument you taught Spinus, each part of a written song plays
  in one of its modes (the one whose "good for" words fit the part and genre, or the one your AI connection
  picks), and the AI can switch articulations per section (palm mute in the verse, open in the chorus). Spinus
  writes the switch keys as short notes at the start of each section, and the page lists the modes used.
- **Read the manual** in Learn this instrument: add an instrument's manuals (PDF files or web pages, as many as
  it has) and the AI proposes its modes, how to enter each, what each suits and what their keys do. Apply takes
  it, keeping what you set; each manual remembers the instrument version it was read for. Needs AI.
- **Ask the plugin** in Learn this instrument: Spinus reads the names an instrument gives its keys (a drum
  machine's "Kick", a key switch's "Palm mute") and proposes them, switches as switches. Few instruments name
  their keys; Spinus's Drum Machine names its pads.
- **Listen to every key** in Learn this instrument: Spinus plays each key of a mode and listens, then proposes
  which keys play, which switch, which do something else (a chord, another note) and which are silent. Apply
  takes it; keys you marked yourself stay yours.
- **Notes it plays** becomes **Learn this instrument...**, in the instrument menu and as **Keys...** beside Play
  in Save this sound: a keyboard where you mark what each key does (plays, switch, special, unused), with a few
  words for the switches ("palm mute"). Listen, Play and the songs play only on the keys that play, so a switch
  key in the middle of an instrument's range is skipped too. An instrument can have several modes (normal,
  strummer), each with its own keys, how to enter it and what it suits. What you set with Notes it plays carries
  over.

### Track creator: songs lean less on the vocals

- Songs with lyrics were nearly all vocals, even in genres that mostly aren't sung (drum and bass, dubstep). Each
  genre now has a range for how much of a song is sung, and the writer picks a share inside it, adding
  instrumental intros, outros, drops, breaks and solos around your lines (never cutting or moving them). A new
  slider under the lyrics, **Sung: 10% to 70%**, shows the genre's range; drag it to set your own.
- Lines are phrased the way the genre does: normal, stretched over a half-time feel, or in a fast flow.
- The instrument playing the sung tune starts quieter, under the lead.
- With **Transitions and automation**, the AI can automate a part in time with the beat ("bars 17 to 24 in
  eighths, then sixteenths"): dubstep's wobble, a gate. At most 24 automation clips a song.

### Automation clips: one shortcut for the control you moved last

- **Ctrl+Shift+L** (Edit › **Create automation clip for the last moved control**) makes an automation clip for
  the knob or fader you moved last. It now counts the Mixer's and the Channel Rack's volume and pan faders too,
  not only a plugin's knobs.

### Export: no more Surge XT crash before it starts

- An export, and the Automixer's recordings of the mix, sometimes crashed in Surge XT before anything was
  recorded: Surge was played a few test notes, there only to wait for instruments that load their sounds in the
  background. Spinus's bundled instruments, Surge XT among them, are ready at once and get no test notes now.

### Export and the Automixer: Windows instruments no longer freeze them

- An export, and the Automixer's recordings of the mix, could stop for ever with Windows instruments (through
  Wine) in the song, most often on the second or third song mixed in a row: an instrument no longer used (the
  export before's, the song before's, or one the Automixer's changes replaced) was deleted while the export
  played another, and the two waited on each other. Those are deleted before an export plays now.

### Track creator: an instrument's own preset files

- Many instruments keep their presets as files Spinus couldn't list (Ample Sound's guitars and basses), so only
  their default sound was measured, and going through them meant Save this sound for each one. Now, in the
  instrument's menu, **Preset files › Add a folder of its preset files...**: Spinus gives each file to the
  instrument as its settings and the sound index measures it like any preset.
- Preset files also work for instruments that keep their settings in an older VST2 bank inside their VST3 one
  (Ample Bass P and Ample Guitar M among them): before, each of their preset files stopped answering and none
  was measured.
- An instrument whose sound as it opens was already measured (most with a saved sound) could not be measured
  again: the sound index reported it as "crashed while listing its presets", so a preset folder added to it
  measured nothing. It is measured again now.

### Coding agents: ready-made requests, your edits kept in view, playback

- **Ready-made requests:** agents connected to Spinus offer five: Start a song, Make a beat, Add a section,
  Check and fix the mix, Shape a sound (in Claude Code, `/mcp__spinus__...`).
- **Each tool says what it does:** only looks, adds, or may replace what is there, so an agent can run the ones
  that only look without asking you each time.
- **Your edits are safe while an agent works:** it's told what you changed or undid since it last looked, and
  its next change waits until it has heard.
- **The manual and your references:** agents can search Spinus's manual, and read the style your reference songs
  share in the Track creator.
- **Play for you:** an agent can play a section (looped) or bars for you to hear, and stop.
- Agents are told Spinus's conventions (bars from 1, sixteenth steps, C5 = 60, ticks, settings in the device's
  units) before their first call.

### AI: a provider that only keeps the line open is asked again

- Some providers, when busy, keep a request waiting for a long time and send only "keep-alive" signals (DeepSeek
  does, for up to half an hour). Spinus waited for them without end: writing a song could seem stuck on one
  step. After five minutes of nothing but keep-alives it now asks again, as it does when a connection drops.

### Track creator: saved drum kits load

- A drum kit kept with **Save this sound** (a plugin's kit, set up as you like it) is chosen for a drum part again
  and now loads as you saved it. Before, adding the chosen sounds said the plugin "no longer has" it, and the drums
  played the plugin's default sound.

### Track creator: one lyrics format, square-bracket tags

- Lyrics take one format, the one song generators take: an English section tag in square brackets on its own
  line ("[Verse 1]", "[Chorus x2]", "[Guitar Solo]") before each part; the lines can be in any language. The
  manual's Track creator page lists the tags. Anything before the first tag is not sung, and a line in
  parentheses is a note. "[Pre-Chorus]" and "[Build-up]" keep their whole names.
- Lyrics without tags are no longer guessed at. Without the AI, Read says to add the tags. With **Let the AI read
  it**, the AI tags them (in any language and layout) and shows its version under the lyrics box: **Use these
  tags** puts it in the box (Ctrl+Z takes it back) and reads it, **Keep mine** leaves your text alone. The AI
  also counts the syllables, and Spinus checks that each line it gives is one of yours.

### Track creator: the variety sets how closely the song follows the lyrics' form

- With lyrics, **Close to my references** keeps exactly the form of your tags. **Balanced** now and then sings
  the last chorus twice or adds an instrumental bridge (or a solo) before it; **Adventurous** does so more often.
  With the AI as the writer, Balanced allows it up to 2 such changes and Adventurous up to 5. Every line is
  always sung, in your order.

### Track creator: the song follows the lyrics

- With lyrics, Spinus's writer takes the song's form from their tags, in order (a repeated chorus, "[Chorus
  x2]", instrumental sections), makes each sung section as long as its lines need, and plays the tune one note
  a syllable, a line at a time, stressed syllables on the beats. The Vocals part plays where words are sung, the
  Lead the tune of the instrumental sections. The Write page's plan says which lines go in which section.
  Songs without lyrics are written exactly as before.
- With the AI as the writer the lyrics' form holds too: the AI may add instrumental sections and choose the
  chords, but the sung sections keep their length and order, and Spinus checks that its tune plays one note a syllable.

### Track creator: a Vocals part for a song with lyrics

- With lyrics on the Description page, the Parts page suggests a **Vocals** part: an instrument plays the sung
  tune in the voice's place (Spinus never sings). Its sound is suggested like any part's, in the singers' range,
  and you can change it; a Lead beside it harmonises. Without lyrics, or with **This song has no vocals**
  ticked, there is no Vocals part.

### Track creator: several sounds from one window, and the notes an instrument may play

- **Save this sound** has **Save and keep open**: save one preset or kit, set up the next and save it too, from
  the same window; **Close** keeps all of them. Going through an instrument's presets no longer means reopening
  the window for each.
- Many sampled guitars and basses switch what they play with keys outside their range, so Play and Listen could
  change the instrument instead of playing it. **Notes it plays** (in the instrument menu, and **Notes** beside
  Play in Save this sound) sets the lowest and highest note an instrument may play; Listen, Play, the sounds
  added to the project and the written song keep to them, moving notes by octaves.

### Track creator: lyrics (first step)

- The Description page has a box for the song's lyrics, tagged by section the way song generators take them
  ("[Verse 1]", "[Chorus]", "[Guitar Solo]"), and a **This song has no vocals** tick. **Read** reads the sections
  and counts each line's syllables, on your computer (roughly) or, with **Let the AI read it**, by the AI.

### Browser: folders of projects show their subfolders, and the Browser can be made wider

- A folder of projects listed every project in one long list, with its subfolder after the name. Now its
  subfolders show as a tree you open and close with a click, each with how many projects are inside; a
  project's own folder shows only as the project. Searching opens the subfolders the projects found are in.
- Drag the thin bar on the Browser's right edge to make it wider or narrower (up to half the window); the
  width is kept with the window layout.

### Automixer: the master is always limited when you choose a loudness

- When the mix already measured about the chosen loudness, the plan had no limiter: its EQ cuts then left it
  quieter (-15.8 LUFS for -14) and its peaks were not held (one went over 0 dB). With a loudness chosen, the plan
  now always ends with the master's limiter, peaks held at -1 dB, and Spinus corrects its gain after Apply.

### Track creator: a sound you marked stays in its role

- A sound marked **Mark it as › Clean guitar** was only ranked lower for other parts, so with few good choices it
  could still be picked for, say, the choir. A sound marked as something a part is not is no longer offered for
  that part; for a part its role fits it still comes first.

### Track creator: Suggest parts keeps the instruments you name

- **Suggest parts › For <genre>** gave the genre's usual band and dropped the instruments the description names:
  "neo-soul with drums, bass, a clean guitar, a lead guitar and electric piano" got soul's piano, organ and brass
  and no guitar at all. Now, when the description names three or more parts, those are the parts; fewer ("add a
  guitar") are added to the genre's list.

### Automixer: the mix reaches the loudness you chose

- With **Streaming (-14 LUFS)** or **Club (-9 LUFS)**, the master's limiter was set from the mix as it was
  before the other changes. When the same plan also turned loud parts down (often 4 to 6 dB for guitars), the
  mix ended 3 to 5 dB too quiet (-17 to -19 LUFS for -14). After Apply, Spinus now measures the result and
  corrects the limiter (still one undo step), so the mix lands within about half a dB of the target.

### Automixer: no gigabytes left behind

- **Mix it...** records the mix and every insert to measure them, often a gigabyte, in the computer's temporary
  folder. Those recordings were never deleted: each mix left one more folder behind, and on many Linux systems
  that folder is in memory. Now only the latest recording is kept while the automixer is open, and it is deleted
  when the automixer closes.

### Track creator: "a melodic bass" is an instrument

- A description such as "gothic rock with drums and a melodic bass" was read as future bass, because "melodic
  bass" is also that genre's other name and the longer name won. "Dark synth" and "80s synth" had the same
  problem ("synthpop with a dark synth lead" became darksynth). These names now count as the genre only when no
  other genre is named.

### Track creator: gothic rock, gothic metal and symphonic metal

- **Gothic rock** ("gothic rock", "goth rock", "goth", "deathrock") is its own style now, not post-punk's cold
  synths: chorus-drenched clean guitars, a driving melodic bass, a steady drum machine, dark pads and a low lead,
  about 132 BPM, minor. **Suggest parts** gives drums, bass, a clean guitar, a rhythm guitar, a pad and a lead.
- **Gothic metal** ("gothic metal", "goth metal", "gothic doom") was plain metal: now slower (about 92 BPM),
  almost always minor, a chord every two bars, with piano, strings and organ over the guitars.
- **Symphonic metal** ("symphonic metal", "orchestral metal", "operatic metal") was power metal: now metal's
  guitars and double kick under strings, choir, brass and piano, about 138 BPM, mostly minor.
- "Gothic" or "goth" beside another genre darkens it ("gothic darkwave" is darkwave, in minor).

### Command line: rendering over an earlier file

- **Fixed:** `spinus --render` to an Opus, FLAC or MP3 file that was already there wrote the new song after the
  old one (the file got twice as long). Now it replaces the file, as a WAV already did.
### Track creator: the writer you see is the writer that writes

- **Fixed:** with Your AI connection chosen, Spinus could write the song with its own rules after it checked the
  connection again (the choice was quietly lost while the box still showed it). Now the choice stays, Write
  waits while the connection is being checked, and a connection that doesn't work switches the box back to
  Spinus (offline) with a note.
### Automixer: the loudness target with the master fader moved

- **Fixed:** with the master fader away from 0 dB, the loudness step missed its target and could push the
  peaks past the limiter's ceiling (the fader comes after the limiter). Now that step sets the master fader to
  0 dB and gives the limiter the difference.
### Track creator: Variety

- **Variety on Write a song:** choose **Close to my references**, **Balanced** (the default, the same as before)
  or **Adventurous**. It sets how far the new song may move from your songs: its tempo, key, chords (Adventurous
  may borrow chords from the other mode), form and length, and how busy each part is. It works for Spinus's own
  rules and for your AI connection, and Spinus remembers your choice.
- **With your AI connection, the checks follow it too:** Close reports a tempo or chords that aren't your songs',
  and Adventurous lets the song go further without pulling it back toward them.

### Coding agents and scripts: any plugin's presets

- **`device.loadPreset` loads a preset on any plugin** by its name, not only on 3x Osc: its programs, the presets
  a CLAP plugin lists, Surge XT's patches, OB-Xf's and Odin 2's, and the sounds you saved for it with Save this
  sound. It is one undo step: Ctrl+Z (or Undo the agent's changes) gives the plugin back its sound from before.
- **Listing them:** `spinus --presets <project>` and, for agents connected to Spinus, `list_presets`.

### Coding agents: songs laid out like by hand

- **Short patterns repeated on the Playlist:** the music tool `write_part` with a loop makes one short pattern
  and repeats it as clips, instead of one long pattern of copies, and plays the same loop again from its pattern.
- **Family groups and colours:** a new part goes in its family's group, in one colour per family (all the
  guitars the same), which its patterns and Playlist tracks share.
- **Automation in the part's group:** on a track right under the part's own.
- The agent skills (`skills/spinus`, `skills/spinus-songwriter`) teach the same for agents that use commands
  directly, and the new Playlist commands: solo and moving tracks.

### Playlist: move and solo tracks

- **Move a track up or down:** drag it by its name, or right-click it › **Move up** / **Move down**. Its clips
  go with it, and a group moves with the tracks in it.
- **Solo a track:** **Ctrl+click** its light (or right-click › **Solo**). Only soloed tracks play; soloing a
  group plays everything in it. Saved with the project.

### Spinus Delay

- **Spinus Delay**, Spinus's own delay ([manual](docs/user/delay.md)): echoes at a note length locked to the
  song's tempo (1/32 to 1 bar, with dotted and triplet lengths) or a time in milliseconds, feedback up to 95%, a
  low cut and a high cut on the echoes so each repeat gets thinner and darker, stereo, ping-pong or mono, width
  and mix. A new time or a tempo change fades to the new echoes without a click. Its window draws the echoes of
  a sound, side by side, and says the time ("1/8 dotted at 120 BPM: 375 ms"). It needs no download, and is also
  a VST3 and LV2 plugin for other programs.
- **Delay sends work out of the box:** the Track creator's AI writer and the assistant make the song's **Delay**
  bus with the Spinus Delay at 100% mix, in time with the song; the AI may pick its note length, ping-pong and
  filters. Before, a delay send needed a delay plugin of yours, and without one it was left out. A delay plugin
  of yours is still used when the AI names it.
- Scripts and coding agents: `spinus.delay` in `--describe`, with settings as text (`"note": "1/8 dotted"`,
  `"time": "350 ms"`, `"feedback": "40%"`, `"high_cut": "4.5 kHz"`).

### Track creator: saved sounds

- **Listen plays a saved sound:** after **Save this sound**, **Listen** (and the Track creator playing each part
  to set its level, and Tune it) played nothing, because Spinus looked for the saved sound among the
  instrument's own presets. A drum player such as BFD Player, silent until a kit is chosen, now plays the kit you
  saved.
- **Play in the Save this sound window:** hear the part's phrase with the sound as you set it up, before you
  save it.
- **The Save this sound window is heard:** the instrument plays through your speakers while its window is open,
  so clicking a drum or a key in it is heard (before, nothing in the window made a sound).
- **Drums on an instrument picked with Other sound...** play on its drum notes (General MIDI's, or the ones the
  sound index found), not the Drum Machine's pads: Play in BFD Player's window was silent.
- The saved sound is listened to once its window has closed (some plugins take seconds to close), and the log
  says how the helper program ended when it stops early.

### Effect types from words

- **A chorus, phaser, flanger or distortion from words:** Surge XT Effects' type (`"FX Type": "Chorus"`) and
  that type's own settings ("Modulation Rate", "Output Mix"...) can be set in one go by scripts, the assistant
  and coding agents. Before, the settings kept showing "-" after the type was set, and a saved project lost the
  type: Surge (like some other plugins) takes in a change only when it plays audio, and nothing played while a
  script or the AI built the project. Now Spinus lets a plugin take its changes in while nothing plays (a moment
  of silence, never while the song plays), sets a type before the settings it renames, and saves the type.
- The assistant's "add a chorus to the pad" finds the chorus among the free effects' types (Surge XT Effects,
  once downloaded) and sets it up.
- Scripts: `--describe bundled.surge-xt-effects` lists the types as `"FX Type"`'s `choices`; a setting can be
  named by a few words of its name (`"Rate"`, `"Mix"`) when only one setting has them, and an unknown name's
  error lists the settings the effect has now.

### Export

- **Exports wait for plugins that run in a separate process, too**, while they load their sounds: one that was
  slow to answer while loading was taken for stuck during the wait and switched off, so the export had none
  of its sound (an export right after the Track creator's Apply, for example).

## 0.1.0-alpha.4 (2026-10-31)

### Channel Rack: replace instrument

- **Replace a channel's instrument** (FL's Replace): right-click a channel's name › **Replace instrument**. The new
  instrument plays the channel's notes; its Playlist clips, mixer insert, volume, pan, colour and group stay.
  Automation of the old instrument's settings is removed; undo brings it all back. Scripts and agents use the
  `channel.replaceInstrument` command, and the assistant's `set_sound` can change a part's instrument.

### Coding agents and the assistant on your open song

- **A guide for any coding agent:** `skills/spinus/SKILL.md` explains how to work with Spinus through the live
  interface or the command line, the rules to keep (never open the window, don't overwrite your work) and the
  details that are easy to get wrong. It is plain Markdown, so agents that don't use skill folders can read it too.
- **Let a coding agent work on your project:** turn on **Options › AI › Coding agents** and Claude Code, Codex,
  Cursor or another MCP agent on your computer can read the open song, change it and render it to measure it,
  while you watch and play it in Spinus. Its changes apply at once, each one an undo step; while it is connected the
  top bar shows it, with **Disconnect** and **Undo the agent's changes** (back to the song as it was when it
  connected). The interface listens on this computer only, needs the token shown there, refuses web pages, and is
  off until you turn it on. The music tools need AI on in Spinus but no AI connection or internet; with AI off the
  agent still has Spinus's own tools (the song's summary, commands, devices, render).
- **The assistant changes your song with Spinus's music tools:** "add pads to the chorus", "add a bridge after
  bar 20", "make the bass pluckier", "duck the pad under the kick". It writes parts over bars, places riffs, adds
  parts and sections, automates, adds effects and sends, and shapes sounds in words (pluckier, brighter, warmer,
  wider...), then checks the song: notes outside the key (a section that changes key on purpose is named once),
  parts held in one register masking each other, a muddy low end, sections off 4-bar phrases, a missing limiter.
  Everything still comes as one proposal you Apply.
- **The skill for coding agents** (`skills/spinus-songwriter`) says to use the live interface when it is on.

### Track creator: guitars in your songs

- **The Track creator hears guitars in your reference songs.** It now splits songs into 6 stems (guitar and
  piano apart from the other instruments) and measures the guitar: whether it is distorted or clean, plays
  power chords, or a lead line. When at least half of your songs have guitars, the description, **Suggest
  parts** and the songs Spinus writes have guitar parts instead of synth chords, and they follow your songs'
  guitar (its range, and the sound **Tune toward the references** aims for). Songs analyzed before are
  analyzed again once, as the summary changed; the Demucs 6-stem model (55 MB) is downloaded once.

### Track creator

- **Save this sound:** the **Save this sound...** button beside each part's **Listen** (also in the list beside
  the part) opens the part's instrument in its own window, outside the project. Set it up (a kit in a drum player that is silent until one is chosen, a
  patch, your own knob settings), name it and Save: Spinus keeps it as your saved sound, listens to it like a
  preset and chooses it for the part. Choose sounds and the AI writer can use it from then on, and **Other
  sound...** lists your saved sounds first. It works with any instrument, Windows plugins too, and saved
  sounds stay when the sound index is updated or deleted (Delete the sound index asks about them first). The
  button works whenever the part has an instrument, also one picked with Other sound... or one the sound index
  couldn't measure; when it is greyed out, its tooltip says why.
- **The parts list is remembered** between runs: the parts, their roles, the sounds you chose and Tune
  toward the references come back when you open the Track creator again. A new description starts it again.

### Track creator: the AI writer

- **More kinds of section, as songwriters and song generators name them:** besides intro, verse, chorus,
  bridge and outro, a song the AI writes can have a **Pre-Chorus, Post-Chorus, Hook, Break, Breakdown, Build,
  Drop, Interlude, Instrumental, Solo, End** or **Fade Out**. Each has its usual energy (a drop as big as a
  chorus, a break or breakdown quiet, a build rising, an end and a fade out falling away), and Playlist markers
  use these names. Each section can carry a short cue ("drums fall away"), shown in the song's summary. The
  checks name a section your description asks for that the song doesn't have, by these kinds.

### Fixes

- **An export right after opening a project or applying a song could leave big sample instruments silent:** they
  load their sounds in the background (Windows plugins through Wine, most of all) and the export renders faster
  than real time. It now lets every plugin instrument load at once, then plays each a few test notes until it
  sounds (at most 45 seconds each, a minute and a half in all) before rendering.
- **A Windows drum player (BFD Player) crashed the sound index's helper**, so it was never listed as measured
  ("crashed before its first preset", then "crashed while listing its presets"). Spinus played instruments
  there with their audio input switched off; through the Wine bridge, a plugin with an input and many outputs
  then crashed on its first sound. Spinus now plays every instrument with the inputs and outputs it has, as
  in a song, there, in Listen, and in Save this sound's window (which could crash Spinus itself for such a
  plugin with a saved sound). Instruments that were silent or failed are measured again at the next sound
  index update. An instrument that is silent until a sound is chosen in its window is listed as "Default
  sound (silent until a sound is chosen in its window: Save this sound)" under Other sound....
- **Settings as text now work for every plugin**, not just Spinus's own devices: `device.setParameters`'
  `text` (in `--run`, the live interface for coding agents, and the assistant's set_sound, add_effect and
  shape_sound) and automation points' `text` read "-12 dB", "2.5 s", "1.2 kHz" or "Hall" for the free reverbs,
  the limiter, and any VST3, CLAP, LV2 or VST plugin, by what the plugin shows over the setting's range. When a
  plugin can't be read so, the error says what it shows at a few points.
- **Local models in Ollama no longer lose the start of long conversations.** Spinus asked Ollama through its
  OpenAI-compatible API, which can't set the context length, so Ollama kept only its default and silently
  dropped the rest of the conversation: the assistant's longer chats and the Track creator's AI writer (whose
  tools alone are a few thousand tokens) were cut short. Spinus now uses Ollama's own API and sets the context
  for each request from its size (at least 16k tokens, up to the model's own limit). Reasoning levels are passed
  on as Ollama's `think`.

## 0.1.0-alpha.3 (2026-10-29)

### Track creator: your AI connection writes the song (experimental)

- **Who writes the notes** on Write a song: Spinus (offline, as before and the default) or, if you choose,
  your AI connection. It composes the whole song with the **music harness**: tools that read and edit a
  working copy of the song, Spinus's checks run as its tests (notes outside the key or the chords, parts in
  one register, the groove and density against your songs), fixes with small edits, and finish. You see each
  step and the tokens so far; nothing changes in the project until you Apply (one undo step).
- **Then fix what Spinus hears**: after the AI finishes, each part is played with its sound, and what was heard
  (a silent part, the balance) goes back to the AI to fix (once by default, up to 3 times).
- **Tokens, as your provider reports them**: no guess before you send; while it works, the tokens so far;
  after, each round's input (and the part read from the provider's cache), output and reasoning. **Reasoning**
  sets how hard the model thinks before each step: low by default; higher levels (up to extra high, where the
  model offers it) use many more tokens; a model without the level chosen uses the lowest it offers. AI models
  can be unreliable: listen before you keep a song. No token cap: an answer cut off at the model's own limit is
  continued and joined. **Stop now, keep the best** keeps the best checked version; **Cancel** keeps the song
  shown before.
- **AI tools**: how deep the AI may go, as groups of tools: Basic (always), Arrangement, Transitions and
  automation, Effects and mix. Pick Basic, Basic + arrangement (at first), Basic + arrangement + transitions,
  Full or Custom (a checkbox per group). More groups let it do more, and cost more. Only the allowed tools are
  sent; the choice is remembered, and the song's note says which tools wrote it.
- **Arrangement tools** for the AI: it adds parts and layers of its own (a channel each, grouped and coloured by
  family, or a family and colour it chooses), varies a motif (its rhythm or notes, upside down, or moved later)
  instead of repeating it, puts drum fills at phrase ends with a crash after, lets layers enter and leave so
  sections build and drop, and checks the song's energy section by section against your songs' sections.
- **Transition and automation tools** for the AI: automation clips on the Playlist for a part's volume, pan or a
  filter sweep (Spinus adds a Spinus Filter to the part's insert), and transitions into and out of a section
  wherever it plays: a riser (noise rising and opening over the bars before it), a downlifter, a swell into its
  first beat (a noise swell, or a reversed cymbal from the starter pack's crash), an impact on it (the kit's kick and crash, or the starter pack's crash and 808). They go on tracks
  of their own under the song's Automation and Samples tracks, in the same undo step.
- **Effects and mix tools** for the AI: Spinus's own effects on a part's insert with their controls set by name
  (EQ, compressor, gate, limiter, filter, ducker, stereo), sends to a reverb and a delay bus made with the reverb
  and delay you have (Spinus has none of its own; Get free plugins offers some), sidechain ducking (the bass and
  pads under the kick), buses of its own, and measure giving each part's level and each section's loudness. If
  a device won't take one of the AI's settings, Apply puts the song in with those effects as they come, and says so.
- For developers: the same tools as an MCP server, and a comparison of one-answer and harness writing on the
  tokens each really used and the checks' scores (`python -m spinus_ai.harness`).

### A skill for AI coding agents

- **`skills/spinus-songwriter/SKILL.md`** teaches a coding agent to write, arrange, mix, check and render a complete
  song with Spinus's command line, from an installed Spinus alone. Two agents that had never seen Spinus's code each
  made a full song with it. How to use it: Spinus from the command line, "For AI coding agents".

### Spinus from the command line

- **Scripts and AI agents can make songs with Spinus, without its window:** `spinus --run script.json --save
  <folder>` runs a list of steps (the commands Spinus's own buttons use, plus loading a Drum Machine kit or
  samples), and saves an ordinary project. `spinus --check <project>` says what a project holds and what is
  missing, `spinus --render <project> --out song.wav` renders it and prints its loudness, `spinus --describe
  <device>` lists a device's settings, presets, pads and kits, and `spinus --commands` lists every command.
  None of them need a sound card, and they leave your settings alone. See
  [Spinus from the command line](docs/user/command-line.md).

### Command line, after a first test by an AI agent

- **`spinus --help`** lists the command-line options, and an option Spinus doesn't know is refused with
  "unknown option": neither opens Spinus's window any more.
- **`--describe`** lists every value of a choice setting (`choices`), and a wrong one says which there are.
  Before, "Low cut" was missing from Spinus EQ's band types.
- **`--describe all`** ends with the free plugins that aren't installed here, such as the reverbs, and
  how to get them.
- **`--commands`** describes instrument and effect devices (Spinus's own, bundled, or a plugin by name)
  and what each kind of automation needs.
- **Automation points in scripts** can be given in the control's own units: `"text": "-6 dB"`, `"30% L"`,
  `"1.2 kHz"`.
- **`--check`** says which channels nothing plays (`used`, `unusedChannels`).
- **`--render --stems`** writes the stems straight into the folder (`04 Bass.opus`), from the same render
  as the mix. Each file reports how long it sounds, and Opus or MP3 the true peak before encoding too.

### Starter pack

- **Spinus now comes with free samples:** the **Spinus Starter Pack**, 97 sounds from Signature Sounds
  (free for any use): drum one-shots, drum and melodic loops with their tempo and key in the name, a
  steel tongue drum, vocal stabs and textures. It is the first folder under **Samples** in the Browser;
  it cannot be removed (it comes with Spinus) but can be hidden.
- **Four more factory kits for the Drum Machine:** **Shrike** (hard and deep, with 808 bass),
  **Starling** (dusty and warm), **Magpie** (pans, coins, cutlery and other collected sounds) and
  **Raven** (dark and heavy metal hits), listed with the Spinus Synth Kit under **Kit**. They use the
  Synth Kit's layout (kick on pad 1, snare on pad 2, hi-hats on pads 4 and 5...), so your patterns keep
  working when you change kits.
- **The Drum Machine's Kit button shows the kit's name** (a factory kit or your `.spinuskit` file), with a
  `*` once you change a pad; it is saved with the project.
- Projects that use these sounds keep them, so they open the same on any computer.
- `spinus --self-test` checks the pack is installed and reads.

### Fixes

- **Track creator: plugins measured as silent, or that failed, can be measured again:** the sound index kept
  what an older Spinus heard forever, so big sampled instruments it measured too soon (before it waited for
  their sounds to load) stayed silent, and a plugin that crashed once was never tried again. Now the next
  **Update sound index** listens again to those an older version found silent or failed (the ones that
  worked are kept), and **Your plugins... › Measure again** measures one plugin, or every silent or failed
  one, anew. A plugin that stays silent until a sound is chosen in its own window (a drum player that needs a
  kit, say) still can't be heard; saving the sound you set up there is planned for alpha 4.

- **OpenAI's newest models (GPT-5.5 and later) work with the assistant and the pattern generator:** they
  refused every request that offered Spinus's tools ("Function tools with reasoning_effort are not
  supported"), because they think before answering and OpenAI's older chat API doesn't allow tools while they
  do. Spinus now talks to OpenAI's own service through its newer Responses API, which does; the model's
  reasoning carries on between its tool calls, and nothing is kept on OpenAI's side. Other OpenAI-compatible
  services (OpenRouter, local servers, your own address) keep the chat API; if one gives the same refusal,
  Spinus asks again once with the model's reasoning turned off, and the assistant says so. A model that
  can't turn its reasoning off (gpt-6.1-sol) gets a clear message there instead: use it through OpenAI's own
  address. The highest reasoning level some models offer, "xhigh", is passed on as it is.

- **Track creator's sound index and big sample instruments:** instruments that load their samples in the
  background after opening (big sampled guitars and basses, for example) were measured before they made a
  sound, filed as silent and never suggested; Spinus now waits for them (up to 40 seconds). One that crashes
  while its presets are listed is tried again with only its sound as it opens, so it can still be chosen.
- **Track creator: choosing which plugins to scan for the sound index crashed Spinus** once some had been
  ticked before: it read the saved list from memory already freed.
- **Spinus froze, and every program crackled, when a webcam microphone was chosen to record from:** Spinus
  kept the microphone open all the time, and through PipeWire a webcam's microphone can become the clock
  of the whole system's sound, with small buffers for every program. Spinus now opens the microphone only
  while a track is armed, something records, or **Options › Audio** is open.
- **First start with music playing:** Spinus could choose the program playing (a browser, say) as what to
  record from. It now chooses a microphone, or nothing.
- **Closing a song you only opened no longer asks to save it.** Some plugins (Ample's, for example) change
  their own settings a moment after loading; Spinus took that for a change of yours. A plugin's settings now
  count only if you opened its window; changes made with Spinus's own controls count as always.
- **3x Osc in Mono no longer hangs on or slides from a key held when playback stopped:** stopping lets go
  of every key, so the next note starts on its own pitch and ends when you let go of it. "All sound off"
  now silences 3x Osc at once.
- **OpenAI's newer models (GPT-5 and later) work with the assistant and the pattern generator:** they
  refused every request with "Unsupported parameter: 'max_tokens'" (error 400), because they take the
  answer-length limit under a new name. Spinus now uses the name each service asks for.
- **A key for the wrong provider is caught:** an OpenAI key with the Provider left at Anthropic (the first
  choice) was saved anyway and then refused with a puzzling 401 from Anthropic. Spinus now tells Anthropic,
  OpenRouter and OpenAI keys apart, doesn't save a mismatched one, and says which Provider to choose. After
  **Save**, the new connection is the one selected, so **Test** no longer tests an older one by mistake.

### The Browser

- **Projects** no longer lists **Opened**: the projects you opened are under **File › Open recent**. Add
  the folder you keep your songs in to see them all; a song you save there shows up at once.

### Windows and shortcuts

- **Spinus opens as you left it:** maximised or full screen if it was, otherwise at its size and place, with
  your panels laid out at that size (they were squeezed into a small window before).

### Experimental

- **Track creator** (experimental, **View › Track creator** once **Show experimental features** is ticked
  in **Options › AI...**): add a few songs you like, or an album folder, and Spinus sums up what they have
  in common: tempo, key and mode, the chords they use and their favourite progressions, the drum groove
  step by step, which parts play and how high or low, how long the sections are, and how loud the songs
  are. Each song is analyzed once and remembered, so adding it again is instant. The summary is shown in
  plain words and can be saved as a file. Your list of songs and the summary are remembered. See
  [the Track creator](docs/user/track-creator.md).
- **Saved analyses** (Analyzer and Track creator): what Spinus finds about a song (tempo, key, stems,
  notes, style) is kept and shared by both, so a song is never split or analyzed twice. Dropping a song
  the Analyzer has seen before offers **Continue from the existing analysis** or **Analyze anew**.
  **Saved analyses...** shows the space it takes and deletes stems or songs (never your own audio files).
  See [Saved analyses](docs/user/track-creator.md#saved-analyses).
- **Track creator, better summaries** (checked on 23 real songs in five styles): the drum groove comes from
  the drum part itself, so straight songs no longer read as swung; songs with different grooves are no
  longer averaged into one; sections follow 4- and 8-bar phrases; a chord loop counts the same whichever
  chord it starts on, and is called shared only when two songs really play it; the key comes from several
  estimates, with a note when they disagree; a fast song that may be felt at half speed is pointed out.
- **Track creator, your sounds:** **Update sound index** listens once to the presets of your instruments
  (3x Osc, the Drum Machine, the free downloads and your installed instrument plugins) and to the samples in
  your Browser sample folders, and notes how each sounds and what it is probably for (bass, lead, pad,
  pluck, keys, drums, fx; kick, snare, hat...; a loop's tempo and key). It runs in the background in a
  separate program, so a plugin that crashes or freezes is only skipped; later updates listen only to what
  is new or changed. **Show sounds...** lists and searches the results, and **Saved analyses...** shows the
  space they take and deletes them. No AI is needed. See
  [Your sounds](docs/user/track-creator.md#your-sounds).
- **Track creator, sounds for each part:** on its new **Sounds for each part** page, **Choose sounds** picks
  the closest sounds you have for the songs' bass, chords and melody, lead and drums: the right kind of
  sound, covering the part's notes, playing chords when the part does, and closest to the songs' own parts,
  measured note by note; each with the reasons in plain words. With AI on it also compares the closest
  presets by listening, and sets an EQ, a compressor, the levels and the master's loudness toward the songs;
  **Tune toward the references**, on each synth part, tunes it toward the songs in a separate program (a
  crashing synth cannot take Spinus down). **Listen** and **The songs'** let you compare, the list beside each part swaps
  it, and **Add these sounds to the project** makes the channels, effects and a pattern with a short phrase
  on each, as one undo step. See [Sounds for each part](docs/user/track-creator.md#sounds-for-each-part).
- **Track creator, write a song:** on its new **Write a song** page, **Write** writes a new song in the style
  of your songs: sections as long as theirs, chord loops from the chords they use (new combinations, not a
  copy of one song), the drums on the steps they play with a fill at each section's end, a bass on the
  chords' roots, a chords part and a lead melody in the songs' ranges and about as busy. Rules on your
  computer write it; no AI connection is needed. **Another version** writes another, **Redo part...** writes one part or one section
  again and keeps the rest, and **Apply** puts it in the project (tempo, key, channels, a pattern per
  section, a Playlist track and markers) as one undo step. **Mix it...** opens the Automixer toward your
  first song. With **Let the AI vary the plan** and an AI connection, the AI may change the sections, the
  chord loops and the melody's shape, checked against the style. See
  [Write a song](docs/user/track-creator.md#write-a-song).
- **Track creator, one path in four steps:** **1. References** (optional: songs you like), **2. Description**,
  **3. Parts**, **4. Write a song**. With references, the description starts as their style in words ("Like
  your songs: about 128 BPM, in A minor, four-on-the-floor drums; with drums, bass, synth chords and a lead
  melody"), which you change like a prompt ("make it 140 BPM", "in D minor", "add a guitar"): what you change
  wins, and the songs' measured style stays for the rest. Without references, you describe the song
  yourself ("a sunny, upbeat indie pop song at 118 BPM in G major, with acoustic guitar") and Spinus says what
  it understood. It knows about seventy genres by their usual names (darkwave, shoegaze, city pop, trip-hop,
  bossa nova, phonk, chiptune...) with each one's tempo, chords, groove and sound palette, and words for
  tempo, key, pace, mood and energy (slowburn, brooding, euphoric, laid-back, darker...), length,
  instruments and what to leave out; a genre it doesn't know is said ("not recognized: a neutral style").
  It reads on your computer; with **Let the AI read it** and an AI connection, the AI reads it instead,
  checked. See [Describe a song](docs/user/track-creator.md#describe-a-song).
- **Track creator, your parts:** step 3 holds the song's parts, and they are yours: it starts with the
  instruments your description names ("guitar, bass and drums") or empty; **Suggest parts...** proposes your
  genre's (rock: drums, bass, two rhythm guitars and a lead guitar), a sketch or a full arrangement; **Add
  part...** adds any of 21 roles (rhythm, acoustic, picked and lead guitar, piano, organ, strings, brass,
  woodwinds, choir, percussion, drums, bass, pads, arpeggio, lead, counter-melody...), each written as that
  instrument plays: power-chord chugs and gallops for a metal rhythm guitar (with the drums' double kick),
  strummed open chords for an acoustic one, two hands for the piano, held chords for strings and organ,
  stabs for brass. Two parts of one role play different things. **Other sound...** picks any instrument you
  have (every format, measured or not) and any preset, or its default sound; what you choose wins over
  every suggestion. Suggestions are varied (other instruments for other parts, other kinds of presets as
  alternatives, others when you choose again). You can give an instrument or a preset a role of your own
  (**Mark it as**, or **Use as** in Show sounds), kept beside the sound index. With references, each part
  whose sound is a synth can be **tuned toward the references**. Drums can be Spinus's Drum Machine, another
  drum instrument or a set of your samples (or the starter pack's).
- **Track creator, a full arrangement:** **Suggest parts... › Full** lays the song out like the demo songs:
  the drums (kick, snare and hats on one drum instrument), a sub bass when the bass leaves room for one, a
  pad, an arpeggio and a counter-melody, a riser before each chorus and an impact where it starts. Verses
  stay thin and choruses get everything. Each part's channel is coloured and grouped by family (Drums, Bass,
  Guitars, Keys, Synths, Orchestra, Leads, FX) with its own mixer insert, a family's inserts play into a bus
  of its name, and each part has its own Playlist track under its family's. Still one undo step.
- **Track creator, writing again replaces the song:** Apply asks "Replace Song 1? It will be overwritten
  (Ctrl+Z brings it back)" and puts the new song in the old one's place (its channels, patterns, tracks and
  markers go; the Parts step's channels stay), as one undo step; or keeps both. Apply is how you hear it:
  the Write a song page no longer has a Listen button.
- **Track creator, your sounds:** the sound index now measures Spinus's own instruments and the free
  downloads; your other plugins only when you tick them under **Your plugins...**. The role of a preset
  whose name does not say it is learnt from your presets that do (on Surge XT's presets: right for 57 %
  instead of 44 %, basses 69 % instead of 14 %). The index is measured again once after this update.
- **3x Osc:** in Mono, a key still held when playback stopped is let go, so the next note no longer slides
  from it or hangs on after it ends; "all sound off" now silences it at once.
- **Analyzer:** the tempo it finds is more precise (it could be off by a few BPM, jumping between values
  such as 130.4, 136.4 and 142.9).
- **On Windows too:** the analysis engine now installs and runs on Windows (CPU, Nvidia and Intel Arc;
  AMD cards use the CPU there for now), so the Analyzer's stems, tempo and key, and the Track creator,
  work on Windows. The Windows installer brings the `uv` tool it needs. See
  [the analysis engine](docs/user/analysis-engine.md#on-windows).
- **Model downloads try again** after a server error or a dropped connection (three more tries, a few
  seconds apart), instead of failing at the first hiccup. If the model's own server is still down, Spinus
  says so ("its authors' server isn't answering... please try again later"): each model comes only from
  its authors. If they give a new link for a model, **Sources and licences... › Download from another
  link** in the Analyzer takes it (the file must match the model's checksum).

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
