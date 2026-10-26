# The Track creator

Give the Track creator a few songs you like, or a whole album, and it tells you what they have in common:
their **tempo**, **key**, **chords**, **drum groove**, which **parts** play and how high or low, how long
their **sections** are, and how **loud** they are. Later versions will use this to make a new song in
that style, with the instruments you have installed. For now it does the first step: listening and
summing up.

![The Track creator](images/track-creator.png)

The Track creator is **experimental**: it is still being worked on, and may change, be slow or not work
well for some songs. It is hidden until you turn it on: open **Options › AI...** and tick **Show
experimental features**. **View › Track creator (experimental)** then opens it (no restart needed). Untick
it again and the Track creator closes and disappears from the menus; a saved
[window layout](main-window.md#layouts) doesn't bring it back while it's off.

It never changes your song: nothing is added to the project, so there is nothing to undo.

## What it needs

The same as the [Analyzer](analyzer.md#what-it-needs), whose steps it uses. Everything runs on your
computer; nothing is sent anywhere.

1. **AI turned on** in **Options › AI...**.
2. **The analysis engine** (PyTorch), installed once: the Track creator offers a **Set up the analysis
   engine...** button that opens [the analysis engine page](analysis-engine.md).
3. **Model files**, downloaded once: Beat This! (8 MB on the CPU, 81 MB with a graphics card), Demucs for
   4 stems (84 MB) and Basic Pitch (230 KB). The Track creator says what is missing and how big it is;
   click **Download** to agree, and it carries on. The Analyzer's **Sources and licences...** says where
   each comes from and under which licence.

## Adding songs

- **Add songs...** picks one or more audio files. **Add a folder...** adds every song in a folder, such as
  an album, and in the folders inside it (CD 1, CD 2), but not deeper. You can also drag files or folders
  onto the Track creator.
- It reads WAV, FLAC, Ogg, AIFF, MP3 and Opus, and with ffmpeg installed (`sudo pacman -S ffmpeg`) also
  M4A/AAC, WebM, WMA and more. Other files in the folder (covers, text) are left out, and a song already in
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

**A song is analyzed once.** What Spinus found is kept in its settings folder (in
`~/.config/Spinus DAW/ai/cache/track-creator`, a few kilobytes per song), recognised by the song's contents.
Add the same song again, even under another name or in another project, and it is **Done (analyzed
before)** at once. The large files made on the way (the decoded song and its stems) are deleted after
each song. Deleting that folder is safe: the songs are then analyzed again.

## The summary

Once the songs are analyzed, **What the songs have in common** says it in plain words, for example:

- **Tempo**: "120 to 128 BPM, typically 124 BPM; 4 beats per bar."
- **Key**: "mostly minor (67 % of the songs): A minor (2), C major." Chords are named from each song's
  key with Roman numerals, so songs in different keys can be compared: in A minor, A minor is **i**,
  C major is **bIII** (three semitones up, major) and F major is **bVI**.
- **Chords**: how often the chord changes, the chords used most, and the favourite progression
  ("i - bVI - bIII - bVII").
- **Drums**: on which of the 16 steps of a bar the kick, the snare (or clap) and the hi-hats usually
  play, numbered as in the [Channel Rack](channel-rack.md), and whether the beat is straight or swung.
- **Parts**: bass, vocals, other instruments: in how many of the songs, from which note to which, how
  many notes a bar, and whether they play single notes or chords.
- **Sections**: how long they usually are in bars (8 bars, say), and how many a song has.
- **Loudness**: about how loud the songs are (LUFS, as the [Loudness Meter](loudness-meter.md) shows),
  their peaks, and how much quieter the quiet parts are.

These are measurements made by a program, not a musician: a busy mix can give an odd chord or a missed
hi-hat now and then. The more songs, the steadier the summary.

**Save summary...** saves it all as a JSON file, with the exact numbers: the drums' chance on each step,
how often each progression and each chord-to-chord change comes, every song's sections and parts. This
file is what the Track creator's next steps will start from.

## What comes next

The Track creator is being built in steps:

1. **References and their style** (this version): the summary above.
2. **Your sounds**: Spinus listens once, in the background, to the presets of the instruments you have
   installed, so it knows what each can sound like.
3. **The instruments for each part**: for each part (bass, chords, lead, drums) it picks the instrument
   and preset closest to the references, then tunes it (as the Analyzer's closest preset and tuned synths
   do), chooses effects, and lets the [Automixer](automixer.md) balance the mix.
4. **A new song**: sections, chords, bass, drums and a melody written from the summary by rules that work
   without any AI connection; with a connection set up, the [assistant](assistant.md) can vary or refine
   it. It comes as one proposal you apply or reject (one undo step), and you can redo one part at a time
   ("a new bassline").

Only the style is used: the references' melodies, words and sound are never copied into your song.

## Copyright

Analyzing songs is for study and personal use. The Track creator measures the style of the songs (tempo,
chords, grooves and so on), not their melodies, words or sound; if you release music, you are responsible
for the rights. The analysis uses freely available open-source models made by others, not affiliated with
Spinus.
