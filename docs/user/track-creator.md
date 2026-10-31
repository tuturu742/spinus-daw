# The Track creator

The Track creator writes a new song with the instruments you have, in one path of four steps (the tabs at
the top):

1. **References** (optional): a few songs you like, or a whole album. Spinus studies what they have in
   common: their **tempo**, **key**, **chords**, **drum groove**, which **parts** play and how high or low,
   how long their **sections** are, and how **loud** they are. See [Adding songs](#adding-songs).
2. **Description**: the song in words. With references, it starts as their style in words ("Like your songs:
   about 128 BPM, in A minor, four-on-the-floor drums; with drums, bass, synth chords and a lead melody"),
   which you change as you like ("make it 140 BPM", "in D minor", "add a guitar"). Without references, you
   describe the song yourself. See [Describe a song](#describe-a-song).
3. **Parts**: the song's parts and the sound each plays, chosen from [your sounds](#your-sounds). See
   [Sounds for each part](#sounds-for-each-part).
4. **Write a song**: puts it all together and writes the song, to apply to your project. See
   [Write a song](#write-a-song).

![The Track creator](images/track-creator.png)

The Track creator is **experimental**: it is still being worked on, and may change, be slow or not work
well for some songs. It is hidden until you turn it on: open **Options › AI...** and tick **Show
experimental features**. **View › Track creator (experimental)** then opens it (no restart needed). Untick
it again and the Track creator closes and disappears from the menus; a saved
[window layout](main-window.md#layouts) doesn't bring it back while it's off.

It changes your song only when you click **Add these sounds to the project** (step 3) or **Apply** (step
4), and each of those is one step you can undo. Everything else only listens, measures and suggests. The
[sound index](#your-sounds) controls are under every step.

## What it needs

The same as the [Analyzer](analyzer.md#what-it-needs), whose steps it uses. Everything runs on your
computer; nothing is sent anywhere.

1. **AI turned on** in **Options › AI...**.
2. **The analysis engine** (PyTorch), installed once: the Track creator offers a **Set up the analysis
   engine...** button that opens [the analysis engine page](analysis-engine.md).
3. **Model files**, downloaded once: Beat This! (8 MB on the CPU, 81 MB with a graphics card), Demucs for
   6 stems (55 MB: drums, bass, vocals, guitar, piano and the rest) and Basic Pitch (230 KB). The Track creator says what is missing and how big it is;
   click **Download** to agree, and it carries on. The Analyzer's **Sources and licences...** says where
   each comes from and under which licence, and can download one from another link its authors give (the
   models are shared, so the Track creator then has it too).

   Songs analyzed by an older Spinus (with 4 stems, before guitars were told apart) are analyzed again the
   next time you click **Analyze**: their tempo and key are kept, and only the stems and what follows are
   found again.

## Adding songs

Step 1, **References**, is optional: songs you like, whose style the description (step 2) starts from.

- **Add songs...** picks one or more audio files. **Add a folder...** adds every song in a folder, such as
  an album, and in the folders inside it (CD 1, CD 2), but not deeper. You can also drag files or folders
  onto the Track creator.
- It reads WAV, FLAC, Ogg, AIFF, MP3 and Opus, and with ffmpeg installed (`sudo pacman -S ffmpeg`; on
  Windows `winget install ffmpeg`) also M4A/AAC, WebM, WMA and more. Other files in the folder (covers, text) are left out, and a song already in
  the list isn't added twice.
- **Remove** takes out the song you selected; **Clear** empties the list.

Each song is analyzed in turn, and its row says how it is going: **Waiting**, **Preparing...**, the step
it is on ("Finding the beat...", "Separating...", "Listening for the notes of the bass...") with its
progress, then **Done**, or **Problem:** and what went wrong. A song that can't be read doesn't stop the
others. **Cancel** stops the song being analyzed; **Analyze again** carries on with the songs that are
waiting or had a problem (for example after turning AI on or downloading the models).

For each song Spinus finds the beat and the key, measures its loudness, splits it into four stems
(vocals, drums, bass and the rest), listens for the drum hits and for the notes of the other stems, and
then measures its style. Separating takes the longest, as in the Analyzer: much quicker on a graphics card
than on the CPU, so an album takes a while. The first time, Spinus spends a few seconds learning its own
drum sounds.

**A song is analyzed once.** Everything Spinus finds (tempo, key, loudness, notes, drum hits, the style and
the stems) goes into its [saved analyses](#saved-analyses), shared with the [Analyzer](analyzer.md). Add the
same song again, even under another name or in another project, and it is **Done (analyzed before)** at
once. A song you already took apart in the Analyzer starts from what the Analyzer found: its beat, key and
stems are used as they are, which saves most of the time.

**Your list is remembered.** Close Spinus and open the Track creator again: your songs are still there, the
analyzed ones done, with the summary as it was, and so are your description and your
[parts](#sounds-for-each-part) with their sounds. Songs that were not finished wait until you click
**Analyze again** (Spinus doesn't start analyzing by itself when it starts).

## Saved analyses

**Saved analyses...** (in the Track creator and in the Analyzer) shows every song Spinus has analyzed, how
much space each takes and how much of that is stems, and the total. Stems take most of the space (a
four-minute song's four stems take in the order of 100 MB); what was measured takes a few kilobytes.

- **Delete its stems**: the selected song's stems go; what was measured stays. If you need them again, the
  song is split again.
- **Delete**: everything kept of the selected song.
- **Delete all stems**: every song's stems; the measurements stay.
- **Delete everything**: the whole list.

Deleting never touches your own audio files, only what Spinus kept. The saved analyses are in Spinus's
settings folder (`~/.config/Spinus DAW/ai/library`), not in your projects. A song kept by an older version
of Spinus is shown as not used, and is analyzed again when you add it.

The window also shows how much space the [sound index](#your-sounds) takes, with **Delete the sound
index** (it is made again the next time you click **Update sound index**). If you have
[saved sounds](#save-this-sound), it asks first: **Keep my saved sounds** (they are listened to again at the
next update) or **Delete them too**.

## The summary

Once the songs are analyzed, **What the songs have in common** says it in plain words, for example:

- **Tempo**: "120 to 128 BPM, typically 124 BPM; 4 beats per bar."
- **Tempo, second thoughts**: when a fast song has its snare on every beat, Spinus adds that it "may be
  felt at half that speed" (and the other way round for a slow one). The tempo itself is left as found.
- **Key**: "mostly minor (67 % of the songs): A minor (2), C major." Spinus asks several independent
  estimates (the whole mix, the instruments without drums and voice, the notes it heard, with two kinds of
  key profile) and takes the one most agree on. When they disagree, it says so: "Not sure of the key of:
  ...". Chords are named from each song's key with Roman numerals, so songs in different keys can be
  compared: in A minor, A minor is **i**, C major is **bIII** (three semitones up, major) and F major is
  **bVI**.
- **Chords**: how often the chord changes, the chords used most, and a progression the songs share
  ("Shared progression: i - bVI - bIII - bVII (in 2 songs)"). A loop counts the same whichever chord you
  start it from, and it is called shared only when at least two songs play it three times or more;
  otherwise Spinus says no progression is shared and names the one repeated most, in one song.
- **Drums**: on which of the 16 steps of a bar the kick, the snare (or clap) and the hi-hats usually
  play, numbered as in the [Channel Rack](channel-rack.md), and whether the beat is straight or swung.
  Spinus listens to the drum part itself for this. Songs with different grooves are not mixed into one:
  you get the main groove (and in how many songs), and the other grooves beside it.
- **Parts**: bass, vocals, guitar, piano, other instruments: in how many of the songs, from which note to
  which, how many notes a bar, and whether they play single notes or chords.
- **Guitars**: in how many songs, whether they sound distorted or clean, whether they play power chords,
  and whether there is a lead guitar: "Guitars: in 3 of 4 songs, distorted, mostly power chords, with a lead
  guitar in 2." Spinus listens to the guitar part on its own for this: a distorted guitar fills its sound
  with harmonics and noise, a clean one rings with clear notes. A guitar too quiet in the mix, or one that
  is really a synth (pure tones), is not counted. When at least half of your songs have guitars, the
  description and **Suggest parts** name them (a rhythm guitar, a clean guitar, a lead guitar), and the
  guitar parts follow your songs' guitar: its notes, and the sound **Tune toward the references** aims
  for.
- **Sections**: how long they usually are in bars (8 or 16, say), and how many a song has. Sections
  follow the song's 4-bar phrases, and a short fill does not count as a section of its own.
- **Loudness**: about how loud the songs are (LUFS, as the [Loudness Meter](loudness-meter.md) shows),
  their peaks, and how much quieter the quiet parts are.

These are measurements made by a program, not a musician: a busy mix can give an odd chord or a missed
hi-hat now and then. The more songs, the steadier the summary.

**Save summary...** saves it all as a JSON file, with the exact numbers: the drums' chance on each step,
how often each progression and each chord-to-chord change comes, every song's sections and parts. This
file is what the Track creator's next steps will start from.

## Describe a song

Step 2, **Description**, holds the song in words; the next steps work from it. Type, and click **Read**.

**With reference songs**, it starts as their style in words, already read:

> Like your songs: about 128 BPM, in A minor, four-on-the-floor drums; with drums, bass, synth chords and a
> lead melody.

The tempo is your songs' typical one, in whole BPM, and the key is their commonest, named as musicians
usually write it (Bb minor rather than A# minor). The song is written in the key the text names: leave it,
or change it.

Change what you want and **Read** again: "make it 140 BPM", "in D minor", "darker", "add a guitar", "make it
trance". What you change wins; everything you don't change stays as your songs have it (their chords,
groove, parts and sections, measured in detail). Spinus says what changed: "Understood: like your songs,
changed: 140 BPM, D minor, darker; parts: drums, bass, synth, melody, guitar". **Your songs again** puts
their description back.

**Without references**, describe the song yourself:

![Describe a song](images/track-creator-describe.png)

> A sunny, upbeat indie pop song at 118 BPM in G major, with acoustic guitar, handclaps and a catchy
> whistled lead

Spinus reads it on your computer, with no AI connection, and says what it understood:
"Understood: 118 BPM, G major, sunny, upbeat, catchy; genre: indie pop; sounds: acoustic guitar, whistled".
If something is missing or wrong, change the words and **Read** again. What it understands:

| You write | What it does |
|---|---|
| A **genre**, by any of its usual names: pop, indie pop, K-pop, J-pop, city pop, hyperpop, synthpop, new wave, synthwave, darksynth, darkwave, coldwave, post-punk, dream pop, shoegaze, chillwave, vaporwave, rock, indie rock, punk, pop punk, emo, metal, thrash, doom, power metal, black metal, DSBM, EDM, house, disco, nu-disco, techno, trance, hardstyle, drum and bass, jungle, breakbeat, UK garage, dubstep, future bass, hip hop (boom bap), trap, phonk, lo-fi hip hop, trip-hop, downtempo, reggae, dub, ska, reggaeton, latin, bossa nova, afrobeats, funk, soul, R&B, gospel, jazz, blues, folk, country, bluegrass, cinematic (orchestral), chiptune (video game music), ambient... Adjectives count too: funky, jazzy, bluesy, punky, poppy, rocky, dubby, soulful, folky ("funky disco" is still disco). | That genre's usual tempo, chord loops, drum groove, parts, section lengths, loudness and sound palette (a cold synth palette for darkwave, acoustic guitar for folk...). |
| No genre Spinus knows | It says "genre: not recognized (using a neutral style)" and leans on your mood words for the tempo and for minor or major. |
| A **tempo**: "72 BPM", "tempo 90" | That tempo. |
| A **key**: "D minor", "F#m", "Bb major" | That key. Without one, a usual key of the mode, not always the same. |
| **minor** or **major** on its own: "slow and chill, minor" | That mode, in a key Spinus picks (or your songs' key). |
| **Pace**: slow, slowburn, laid-back, fast, uptempo, driving... | A slower or faster tempo than the genre's usual one (when no tempo is given). |
| **Mood**: dark, brooding, melancholic, cold... or happy, sunny, euphoric, uplifting... | Minor or major more often (when no key is given). |
| **Energy**: calm, chill, dreamy... or energetic, aggressive, heavy... | Fewer or more notes and drum hits. |
| **Length**: short, long, epic | A shorter or longer song. |
| **Instruments**: guitar, acoustic guitar, lead guitar, bass, drums, piano, organ, strings, brass, flute, choir, handclaps, pad, synth, whistle, 808... | The song's parts: "a song with guitar, bass and drums" gives a rhythm guitar, a bass and drums on [Sounds for each part](#sounds-for-each-part), each looking for a sound of that name in your [sound index](#your-sounds) ("Grand Piano" for a piano). Without instruments, the parts list starts empty: add parts, or **Suggest parts**. |
| **Leaving out**: no drums, no melody, "no synth leads", "without strings", "don't use organ" | That part is left out. Ambient has no drums anyway. Words that say "not" count: "not too fast" is not fast. |

**Let the AI read it** (off at first): with an AI connection set up in Options › AI, the AI reads the
description instead. It answers in the same terms, choosing from the same genres, and Spinus checks its
answer. It also names the parts the description asks for ("a wall of fuzzy guitars" becomes a Fuzz guitar
part), which start step 3; when it names none, the parts come from the instruments your words name. If the
AI isn't reachable or answers something unusable, Spinus's own reading stays, and the page
says so. The page names the model that read it; when your first choice didn't answer and another
connection or model did, it says why: "Read by the AI (claude-sonnet-5-5, because Claude
(claude-opus-5-5) did not answer: ...)". This sends your description, and nothing else, to your AI provider.

Then go on to step 3, [Parts](#sounds-for-each-part), and step 4, [Write a song](#write-a-song), which
says "From your description" at the top. Reading a description (or your songs being summed up again)
starts the parts list again, and the page says so. Spinus remembers the description and what it understood, like
your list of songs.

## Your sounds

To pick sounds for a new song later, the Track creator first gets to know the sounds you have. Under
**Sounds you have**, click **Update sound index**. Spinus then listens to:

- **Your instruments' presets**: 3x Osc and the Drum Machine, and the free instruments you downloaded
  through Spinus (Surge XT, Odin 2, OB-Xf, Six Sines). Your other instrument plugins (CLAP, VST3, LV2,
  VST2, and Windows plugins added through Spinus) are listened to only if you tick them: click **Your
  plugins...** and tick the ones you want (click again to untick). This keeps the first update quick, and
  Spinus loads none of your own plugins unless you ask. Each preset plays a few short test notes, which
  nobody hears.
- **Your samples**: the files in the folders you added in the **Browser › Samples** list ("Add a folder of
  samples"), and the folders inside them. Nothing else on your disk is looked at.

For each sound Spinus notes how it starts and fades, how bright it is, whether it has a clear pitch, which
notes it plays, whether it plays chords, and what it is probably for: **bass, lead, pad, pluck, keys,
drums** or **fx**. A sample gets a kind (**kick, snare, clap, hat, cymbal, tom, perc, bass** or
**other**), and a loop its tempo, length in bars and key where Spinus can tell. A preset or file whose name
says what it is ("Deep Sub Bass", a "Kicks" folder, "128 BPM", "D#min") is taken at its word. Files that are
not audio (MIDI files, project files, text, zip archives) are left out; an audio file named with the wrong
extension (an AIFF called .wav) is still read.

- It runs **in the background**, so you can keep working; the line under **Sounds you have** shows how far
  it is. **Stop** stops it: what was done is kept, and the next update carries on from there.
- The first time takes a while (Surge XT alone has about 3,000 presets: about 20 minutes on a fast
  computer). After that, **only what is new or changed** is listened to: a new plugin, an updated one, new
  preset files, new or changed samples. Removed ones leave the index.
- **Plugins can't crash Spinus here**: the listening happens in a separate helper program. A preset that
  makes it crash or freeze is skipped and counted as a problem; so is a plugin that doesn't load (it is
  tried again once it's updated).
- **Measured again by a newer Spinus:** when a newer version of Spinus listens better (it now waits for
  instruments that load their sounds slowly, as big sampled guitars and basses do), the plugins an older
  version found silent, or that failed, are listened to again at the next update. The ones that worked are
  kept as they were, so the update stays quick.
- **Measure again**: to listen to a plugin anew yourself, click **Your plugins...**, then **Measure again**
  and the plugin, or **Measure again every silent or failed one**. Spinus forgets what it heard from it and
  updates the index at once (an installed plugin you hadn't ticked is ticked for it).
- Some plugins stay silent until you choose a sound in their own window: a drum player that needs a kit
  picked first, for example. Set it up once and keep it with [Save this sound](#save-this-sound).
- **Show sounds...** lists everything Spinus measured, with its guess and a few details ("attack 5 ms,
  sustained, brightness 820 Hz, C1-C7, chords"), and a search box: type "bass", "pad", "Surge" or "kick
  sample" to check its guesses. When a name says one thing and the sound another, both are shown ("sounds
  like pad").
- It needs no AI connection and no analysis engine, and nothing is sent anywhere. The index is kept in
  Spinus's settings folder (`~/.config/Spinus DAW/ai/sounds`) and can be deleted in
  [Saved analyses](#saved-analyses).

**How Spinus guesses a role.** When a preset's name or category says its role, that decides. For the others,
Spinus learns from your own presets: the ones whose names say "bass", "pad" and so on show it how each role
sounds, and a preset without such a name gets the role of the presets it sounds most like. (With fewer than
about 60 named presets, simple rules decide instead.) On Surge XT's own presets, each guessed without its
name, this gets the role right for 57 % of them, against 44 % with the rules alone; for basses 69 % instead
of 14 %. Plucks are still often taken for keys or pads.

The guesses are a program's, made from a few test notes: a bass preset that sounds bright can be called a
lead, and an unusual drum sample "perc" or "other". The next step uses them only as a first sorting.

## Sounds for each part

Step 3, **Parts**, holds the song's **parts**: what the new song will have, and the sound
each plays. The song is written with exactly these parts and these sounds.

![Sounds for each part](images/track-creator-sounds.png)

**The parts are yours.** The list starts with the parts your description names ("a song with guitar, bass
and drums": those three), or empty. Then:

- **Add part...** adds a part of a role:

  | Role | How it plays |
  |---|---|
  | **Drums** | Kick, snare and hats on one drum instrument, on your songs' (or the genre's) steps; a double kick in metal's choruses |
  | **Percussion** | A shaker on the sixteenths, congas, a clap with the backbeat |
  | **Bass**, **Sub bass** | The chords' roots where the kick plays; a sub holds them low in the choruses |
  | **Rhythm guitar** | Power chords (root, fifth, octave): in rock and metal, palm-muted chugs on the sixteenths or gallops, the chord ringing on the accents; otherwise strummed on the eighths |
  | **Acoustic guitar** | Open chords strummed down and up (D D U U D U), each string a moment after the last |
  | **Clean guitar (picked)** | The chord's strings picked one by one on the eighths |
  | **Lead guitar** | A riff on the key's pentatonic scale, on the chord's notes on the strong beats; busier in the bridge, like a solo |
  | **Synth chords**, **Piano / keys** | Chords; the piano with two hands, broken into eighths in the verses and struck on the beats in the choruses |
  | **Organ**, **Strings**, **Choir / vocal pad**, **Pad** | Held chords: the organ over its root, the strings spread wide, the choir in the voices' range |
  | **Brass** | Short stabs on the bar's first beat and with the snare, in the choruses |
  | **Woodwinds**, **Counter-melody** | A slower line answering the lead |
  | **Arpeggio**, **Lead melody** | The chord's notes one after another; the tune |
  | **Riser**, **Impacts** | Effects before and where each chorus starts |

  Two parts may share a role, such as two guitars: the second then plays something else (a second guitar or
  synth chords part an octave up or broken into an arpeggio, a second lead harmonises the first).
- **Add part... › From an instrument** adds a part playing an instrument you pick. Its role is the one you
  marked it with, or what its name says in plain words ("Steel Guitar" an acoustic guitar, "Fingered Bass" a
  bass, "Grand Piano" a piano); otherwise a lead, to change.
- **Your own roles for your sounds.** Spinus guesses each sound's role; you can set it yourself. In the
  instrument menu (**Other sound...** or **From an instrument**), **Mark it as** gives an instrument a role
  (for example a plugin whose default sound is a rhythm guitar); in **Show sounds...**, right-click a preset
  and **Use as** gives that preset one. Your roles come first wherever roles are used (choosing sounds,
  parts from an instrument), are kept beside the sound index, and stay when the index is updated or deleted.
- **Suggest parts...** fills the list with the parts a song like this usually has: your genre's (rock: drums,
  bass, two rhythm guitars and a lead guitar), a **sketch** (drums, bass, chords, lead) or a **full**
  arrangement (see [A full arrangement](#a-full-arrangement)).
- The list beside a part's name changes its **role**; **x** removes it. A new description or a new summary
  of your songs starts the list again; the page says so.

Then click **Choose sounds**. For each part, Spinus suggests sounds you have: the closest first, and as
alternatives other instruments and other kinds of presets, so the parts don't all get the same synth.
**Choose sounds** again suggests others. A part you already chose a sound for keeps it:

1. **Which sounds fit**: a preset of the part's role (a bass for the bass, a pad for a pad, a pluck for an
   arpeggio...) that plays the notes the part needs, and plays chords when the part does. A guitar, piano,
   organ, strings, brass, woodwind or choir part first looks for sounds whose name says so ("Guitar",
   "Metal", "Grand", "Strings"...).
2. **Drums**, from any of three kinds: Spinus's own **Drum Machine** with its kit (always there); another
   **drum instrument** (a plugin with a kit on its keys); or a **sample set**: a kick, a snare and a hat
   from your samples (or the starter pack) on the Drum Machine's pads. A drum loop near the songs' tempo
   is offered too. With songs to compare with, the closest sample set comes first; otherwise the Drum
   Machine. Whichever you pick is what the written drums play.
3. **Which is closest**: for the first drums, bass, chords and lead, Spinus measures the songs' own parts
   (their stems, kept in your [saved analyses](#saved-analyses)) note by note, the same way it measured
   your presets: how fast a note starts, whether it holds or fades, how bright it is at the part's own
   notes, how noisy. The closest come first. Other parts are chosen by role and range.
4. **Listen closer** (needs AI turned on, the analysis engine and its CLAP model, which the Analyzer's
   closest preset downloads): the first five presets are also compared with the songs by listening, as
   closest preset does, and put in order again.
5. **Tune toward the references** (only with reference songs: each part whose sound is a synth's preset has
   it; slow, a minute or more): the part's synth is tuned toward that part in your songs, as the Analyzer's
   tuned synths are. Tick it before **Choose sounds**, or after to tune that part at once. It runs in a
   separate program, so a synth that crashes only stops its own tuning; **Stop** keeps what was found.
6. **Effects and mix** (needs AI turned on, but no AI connection): each chosen sound plays a few seconds of
   the part, and Spinus compares it with the songs' part: an EQ moves its tone toward theirs, a compressor
   evens it out when it is much peakier, its fader makes the parts as loud as each other as in the songs,
   and a limiter on the master aims at the songs' loudness. These are estimates until the song has notes;
   the [Automixer](automixer.md) can balance it again later.

Each part's line says what was chosen and **why**, in plain words and numbers, for example "a bass (its
name says); plays C1-C6 (the part: F#1-A#2); attack 6 ms (the songs' 10 ms); brightness 2.1 harmonics at
D2 (the songs' 2.4)". Compare the numbers: the closer they are, the closer the sound.

- **Listen** plays the chosen sound playing a short phrase in the songs' key. **The songs'** (only with
  reference songs) plays a few seconds of that part in one of your songs (its stem), to compare.
- The list beside each part holds the **alternatives**, closest first: pick another to swap. Its tuning
  and effects are dropped; **Set effects again** sets them for the sounds chosen now.
- **Other sound...** (at the end of that list) picks **any** instrument you have, of any format (Windows
  plugins too), and any of its presets, or its **Default sound** when it has none or lists none. Instruments
  the sound index hasn't measured (or measured as silent, or couldn't load) are there too, marked "not
  measured". Your pick stays first on the list, marked "(your choice)", when you choose sounds again.
- **Save this sound...** (the button beside **Listen**, and also in that list) opens the part's instrument
  in its own window: see [Save this sound](#save-this-sound).
- **Add these sounds to the project** makes, for each part, a channel named after it ("Bass (Track
  creator)") in the Channel Rack group **Track creator**, with its instrument, preset, tuning, effects and
  fader, and a pattern **Track creator: sounds** with a short phrase on each, so you can hear them
  together. A drum loop goes on a Playlist track of its own. It is all **one undo step**. The song you
  write later plays on these channels.

**What you choose here wins.** The written song uses each part's sound from this page; a description's
instrument words and the sound index only fill in the parts you didn't choose a sound for (each another
instrument when one fits as well, and another song other ones).

Without AI turned on, the sounds are still chosen, by their measurements alone, and can be added; the line
at the bottom says what the AI would add. Nothing is sent anywhere: all of this runs on your computer.

These are a program's choices, made from measurements: treat them as a starting point and change them by
ear.

**Your parts are remembered.** Close Spinus and open the Track creator again: the parts are as you left them,
with their roles and the sounds you chose. Reading a new description starts the list again, and the page
says so.

### Save this sound

Some instruments need you: a drum player that stays silent until you choose a kit in its own window, a
synth whose best sound is one you made yourself. **Save this sound...** (the button beside a part's
**Listen**) opens the part's instrument in its own window, outside your project, with the sound the part has
now.

The button works as soon as the part has an instrument: one **Choose sounds** found, or one you picked with
**Other sound...**, even one the sound index couldn't measure ("not measured", or "silent until a sound is
chosen in its window"). It is greyed out while the part has no sound yet, or plays samples or a loop (not an
instrument plugin's own sound); hold the mouse over it to see why.

![Save this sound: the instrument's own window, with a name and Save](images/track-creator-save-sound.png)

1. Set the sound up in the instrument's window: choose a kit, a patch, turn its knobs.
2. Give it a name and click **Save**.
3. Spinus keeps it as **your saved sound**, listens to it like any preset (in the separate helper program,
   a few seconds; longer for a big instrument) and chooses it for the part.

From then on it is one of your sounds: **Choose sounds** can suggest it ("your saved sound"), the AI writer
can pick it, and **Other sound...** lists all of them first, under **Your saved sounds**. A song that uses it
plays the instrument exactly as you saved it. It works with any instrument, Windows plugins too. Saved sounds
are kept beside the sound index (`~/.config/Spinus DAW/ai/saved-sounds`), so they stay when the index is
updated or deleted. Nothing in your project changes until you add the sounds or write the song.

If a saved sound made no sound when Spinus played it, the page says so: open it again and check the
instrument's window.

## Write a song

The last page, **Write a song**, writes a new song in the style of your songs (or of your description): its
sections, chords and every part on the [Sounds for each part](#sounds-for-each-part) page, as notes in
patterns you can edit like any other. Apply it to hear it.

![Write a song](images/track-creator-song.png)

1. Make the parts on [Sounds for each part](#sounds-for-each-part) (or **Suggest parts** there). Then click
   **Write**. In a moment the page says what it wrote: how long the song is, its tempo and key, its sections
   ("Intro 8, Verse 16, Chorus 16, ...") and each section's chords, its parts, and how all this compares with
   your songs in numbers. Without parts, it says to add some first.
2. Not quite right? **Another version** writes a different song in the same style. **Redo part...** writes
   one part again (**New drums**, **New guitar**...) or one section again (**The chorus again**, with new
   chords), and keeps everything else exactly as it was.
3. **Apply** puts the song into your project, which is how you hear it: the tempo and key, a channel per
   part (or the one the Parts step added for it), a pattern per part and section, Playlist tracks and a
   marker where each section starts. Each part starts at a sensible level for its role (the bass near the
   drums, a pad well under them): after **Write**, Spinus plays each part's busiest bars with its sound,
   measures how loud it is and sets its fader, so no sound that happens to be loud drowns the rest. The
   page lists the levels. With reference songs and **Effects and mix**, the level toward your songs is
   used instead. It is **one undo step**: Ctrl+Z takes it all away. After that,
   **Redo part...** and **Apply** change only that part or section (one more undo step each).
4. **Writing again replaces the song.** When the project has a song the Track creator wrote, Apply asks
   "Replace Song 1? It will be overwritten (Ctrl+Z brings it back)": **Replace** takes the old song out
   (its channels, patterns, Playlist tracks and markers; the Parts step's channels stay) and puts the new
   one in its place, still one undo step; **Keep both** adds it as "Song 2" after the first; **Cancel**
   leaves the project as it is.
5. **Mix it...** opens the [Automixer](automixer.md) with your first song as its reference: it measures the
   parts now that they play notes, and suggests levels, EQ and compression toward your songs. Tick what you
   want and apply (one undo step).

**How it is written.** Rules on your computer turn the summary's numbers into the song; nothing is sent
anywhere and no AI connection is needed.

- **Tempo and key**: your songs' usual tempo, moved a little (up to 3 %); the key of one of your songs,
  sometimes a step higher or lower; minor or major as often as your songs are.
- **Sections**: an intro, verses, choruses, a bridge and an outro, about as many as your songs have and
  about as long, each 4, 8, 12, 16... bars as your songs' sections are. A chorus is the same each time
  (one pattern), so you can change it once.
  When your AI connection writes the song, a section can be any of the kinds songwriters and song
  generators name: **Intro, Verse, Pre-Chorus, Chorus, Post-Chorus, Hook, Bridge, Break, Breakdown, Build,
  Drop, Interlude, Instrumental, Solo, Outro, End** and **Fade Out**. Each has its usual energy (a drop is as
  big as a chorus, a break and a breakdown are quiet, a build rises, an end and a fade out fall away) and
  can carry a short cue, as in "Breakdown: drums fall away", shown in the song's summary on Write a song.
  Markers on the Playlist use these names.
- **Chords**: loops of four chords, chosen from the chords your songs use and the way they move from one
  chord to the next: new combinations, not a copy of one of your songs (a loop that one song plays as its
  own is used at most once).
- **Drums**: the kick, snare and hi-hats on the steps your songs play them, on one drums channel, a few
  extra hits by chance (fewer in quiet sections), a fill at the end of each section and a crash at the
  start of the next.
- **Bass**: the chords' roots, sometimes the fifth or the octave and a step leading into the next chord,
  where the kick plays most, as low and as busy as your songs' bass.
- **Chords part**: the chords played as high and as busy as your songs' other instruments, each chord
  close to the one before.
- **Lead**: short phrases that answer each other, in your songs' vocal range and about as many notes a bar,
  with the chord's own notes on the strong beats and in the key in between. Only the style is used: your
  songs' melodies are never read (the summary has none).
- **The other roles** play as their instruments do (see the table on
  [Sounds for each part](#sounds-for-each-part)): power-chord chugs for a metal rhythm guitar, strummed
  open chords for an acoustic one, two hands for the piano, held chords for strings and organ, stabs for
  brass. Two parts of one role play different things.

### A full arrangement

**Suggest parts... › Full** proposes a song laid out as a producer would:

![A full arrangement on the Playlist](images/track-creator-full.png)

| Family (colour) | Parts |
|---|---|
| **Drums** (pink) | **Drums**: kick, snare and hats on one drum instrument (percussion beside it) |
| **Bass** (cyan) | **Bass**; **Sub bass** when your songs' bass sits high enough to leave room for it |
| **Synths** (purple) | **Chords**, **Pad**, **Arpeggio** |
| **Leads** (orange) | **Lead**, **Counter-melody** |
| **FX** (grey) | **Riser**, **Impacts** |
| **Guitars** (red), **Keys** (green), **Orchestra** (gold) | Guitars, piano and organ, strings, brass, woodwinds and choir, when the list has them |

- **Who plays when**: verses stay thin (drums, bass, chords, lead); choruses have everything; the bridge
  breaks down to the pad and builds up again with the arpeggio; the intro starts with the pad and chords.
- **The layout** (for any list of parts): each part's channel is coloured by its family and in the Channel
  Rack group of it, on a mixer insert of its own with the same name and colour, and a family's inserts play
  into a bus named after it (FX...), so one fader sets the family's level. There is a pattern per part and
  section ("Song 1: Chorus - Pad", only where the part plays) and one per effect ("Song 1: Riser"). On the
  Playlist, a track per family ("Song 1: Drums") holds a track per part, so you see the parts come and go;
  fold a family to hide its tracks.
- **Sounds**: each part plays the sound chosen for it on the Parts step. A part without one gets the
  closest sound of its role from your [sound index](#your-sounds) (a pad for the pad, a pluck for the
  arpeggio, the instruments a description names); without one, Spinus's own (3x Osc's "Warm Pad",
  "Pluck", "Sub Bass", "Bell", "Noise Riser", the Drum Machine). The page lists which.
- **Redo part...**: **New chords** writes the chords again and with them the pad and the arpeggio,
  **New lead** the lead and what follows it; a second part of a role is written again on its own.

**Let the AI vary the plan** (off at first): with an AI connection set up in Options › AI, the AI may
change the order and length of the sections, the chord loops and the shape of the melody. Spinus checks
what it says against your songs' style (lengths on 4-bar phrases, only your songs' chords) and writes every
note itself; if the AI isn't reachable or answers something unusable, the rules' song stays, and the page
says so. It names the model that answered, and why, when it wasn't your first choice. This sends the
summary's numbers (not your songs) to your AI provider.

### Let your AI connection write it

![Write a song with "Your AI connection" chosen: the reasoning level, the fix rounds, the AI tools set to Custom with
their groups ticked, and after the run the tokens each round used and what Spinus's checks still
find](images/track-creator-ai-writer.png)

**Who writes the notes** (at the top of the page) is **Spinus (offline, nothing is sent)** at first. Choose
**Your AI connection** (shown with its name; greyed out until you set one up in Options › AI) and the AI
composes the whole song: the form, the chords, every part's notes, and a sound for each part you didn't
choose yourself in step 3 (from a few that fit in your sound index). Your own choices in step 3 always win.

It works like a programmer with tools, which Spinus calls the **music harness**. The song is a working copy
kept on your computer; the AI never sends it whole. It reads the brief (your style and your parts), looks at
the form, writes each part (a loop once, repeated), names a riff and places it again, copies a section that
repeats, lets Spinus's own rules write a part it doesn't need to shape by hand, and runs Spinus's checks, as a
programmer runs tests:

- notes outside the key, or rubbing against the chord on the beat (with the bar and step);
- two parts in the same register;
- notes a bar against your songs' parts;
- the drums' groove against your songs';
- a lead repeating one bar, a part with no notes, sections off the 4-bar phrases;
- sections your description asks for that the form doesn't have, by the kinds above (a pre-chorus, a solo, a
  breakdown, a build-up, a drop, an interlude, a double chorus, a fade out...); "a breakdown or bridge": either
  will do.

It fixes what the checks find with small edits of a few bars, then says it's finished. If it tries to finish while the checks still find something, it is told what is open, once; asked again, it finishes as it is. While it works, the
page shows each step in plain words ("Writing the bass for the chorus", "Checking: 2 issues") and the tokens
used so far. A note it can't read, outside its instrument's range or off the grid is fixed or left out,
and the page says so. A part it never wrote is written by Spinus's rules, and the page says which.

When it's done, Spinus plays each part with its sound to set its level (as for its own songs). **Then fix
what Spinus hears** (once, at first; no, twice or 3 times) sends what it heard back to the AI: a part that
made no sound (its notes may be outside the instrument's range) and the balance. The AI fixes it the same way,
on the same working copy. If no part could be played (Spinus's sound helper isn't there), there's nothing new to
tell the AI, so no fix round runs.

**AI tools** is how deep the AI may go: which groups of the harness's tools it gets.

- **Basic**: the form, chords, every part's notes, motifs, sounds, the checks and finish. Always on.
- **Arrangement**: parts and layers of its own (a channel each, grouped and coloured by family, or the family
  and colour it chooses), a motif varied instead of repeated (its rhythm or notes, upside down, or moved
  later), drum fills at phrase ends with a crash after, layers entering and leaving per section, and a check
  of the song's energy section by section against your songs' sections. Parts it adds can be removed by it;
  yours always stay.
- **Transitions and automation**: automation clips on the Playlist (a part's volume, pan, or a filter sweep:
  Spinus adds a Spinus Filter to that part's insert), and transitions into and out of a section wherever it
  plays: a riser before it, a downlifter after it starts, a swell into its first beat (a noise swell, or a reversed
  cymbal: the starter pack's crash played backwards), an impact on it (the
  kit's kick and crash, or the starter pack's crash and 808). They go on tracks of their own, under the song's
  **Automation** and **Samples** tracks.
- **Effects and mix**: Spinus's own effects on a part's insert, with their controls set by name (EQ,
  compressor, gate, limiter, filter, ducker, stereo), and on the song's master, whose chain ends with a limiter
  (the check reminds the AI; writing again doesn't add a second one); sends to a reverb bus and a delay bus;
  ducking a part under another (the bass under the kick); buses of its own. Spinus has no reverb or delay of its
  own, so the AI uses the ones you have, found by the words in their names ("reverb", "hall", "room", "plate";
  "delay", "echo"), leaving out plugins that take no audio in (a MIDI delay). **Get free plugins** (in Options) offers some, and without one the AI is told there's none. If
  a device won't take one of the AI's settings, Apply puts the song in with those effects as they come, and the
  page says so.

Choose **Basic**, **Basic + arrangement** (at first), **Basic + arrangement + transitions** or **Full**, or
**Custom...** to tick the groups yourself. Spinus remembers your choice, and the song's note says which tools
wrote it. Only the tools you allow are sent to the AI, so tools you leave off cost nothing. Each group lets
the AI do more, and costs more: more steps and more tokens per song. Full can use several times the tokens
of Basic.

**Reasoning** is how hard the model thinks before each step: low at first, or the model's own, medium, high,
or extra high where the model offers it. Higher levels use many more tokens, because the AI thinks again
before every step. A model that can't reason at the level you chose uses the lowest it offers, and the page
says so.

**AI models can be unreliable.** A song may not match your references, and the tokens a song uses vary from
one run to the next. Spinus checks the notes, but listen before you keep it.

**How many tokens**: Spinus doesn't guess in advance, since nobody knows how long a model will think. While the
AI works, the page shows the tokens used so far. When it's done, it lists each round's tokens as your provider
reported them:

- input, and how much of it was read from the provider's cache, which costs less;
- output, and of that the reasoning.

Nothing about it is kept beyond the song itself. In a long session the oldest tool calls are cleared from what
the model reads, a batch at a time (the working copy on your computer keeps everything): Anthropic does this on its
side; for OpenAI-compatible services such as DeepSeek or OpenRouter, Spinus does it before sending. The page counts
what was cleared.

There's no token limit that could cut a song short when it's almost finished. If an answer still reaches
the model's own length limit, Spinus asks it to go on and joins the pieces. You control the cost with the
reasoning level and the number of fix rounds. **Stop now, keep the best** stops the AI after its current step and
keeps its best checked version so far. **Cancel** stops at once and keeps the song shown before.

**What is sent** to your AI provider: your style's numbers (tempo, keys, chords, groove, the parts'
ranges), your description, your parts' names and roles, and the names of the sounds it may choose from. Not
your songs or their audio, and nothing else from your computer.

**For developers**: the same tools run outside Spinus as an MCP server (`python -m spinus_ai.harness mcp
--brief brief.json --out song.json`, after `python -m spinus_ai.harness brief --describe "..." -o
brief.json`), so a coding agent can compose through the identical harness. `python -m spinus_ai.harness
eval` writes the same brief once in one answer and once with the harness, at each reasoning level, and
prints the tokens each way used and the checks' score of each song.

The same click of **Write** with the same summary can give a different song; each song keeps its own
random numbers, so **Redo part...** changes only what you asked for.

**What it needs**: AI turned on in Options › AI (the writing runs in Spinus's AI service, on your
computer), and a summary from the first page. Without the AI service the page says so; the rest of Spinus
works as always.

These are a program's songs, made from measurements: a sketch to change by ear, not a finished song.

## How it was built

The Track creator was built in four steps: the summary of your songs' style, the
[sound index](#your-sounds), [sounds for each part](#sounds-for-each-part), and [writing a
song](#write-a-song). Then came [full arrangements](#a-full-arrangement) and
[describing a song](#describe-a-song) in words.

Only the style is used: the references' melodies, words and sound are never copied into your song.

## Copyright

Analyzing songs is for study and personal use. The Track creator measures the style of the songs (tempo,
chords, grooves and so on), not their melodies, words or sound; if you release music, you are responsible
for the rights. The analysis uses freely available open-source models made by others, not affiliated with
Spinus.
