# Your first beat

This walks through making a short song with Spinus. Each step links to the page with the details.

## Listen to the demo songs first

Spinus comes with demo songs made in Spinus. In the [Browser](browser.md#projects) on the left, under
**Projects**, double-click **Siskin (demo)** (or another song marked "(demo)").
Press **L** for song mode and **Space** to play it, and look around: its patterns in the Channel Rack and the
Piano Roll, its arrangement and automation in the Playlist, its effects in the Mixer.

- Spinus puts your own copy of the song in your **Music** folder (for example `Music/Siskin`) and
  opens that, so you can change it and save it freely. Opening the demo again opens the same copy, with
  your changes. For a fresh copy, rename or delete that folder first.
- Besides Spinus's own instruments and effects, the demo songs use free plugins: Surge XT, Surge XT
  Effects and Dragonfly Reverb (and OB-Xf and PeakEater in some). If they aren't downloaded yet, Spinus
  offers them as soon as a song opens (see [Get free plugins](free-plugins.md)); until then those channels
  and effects show **Missing** and stay silent. You can also get them later in **Options › Get free
  plugins...** and the song's sounds come back without opening it again. They all exist for Linux and
  Windows, so the demo songs play in full on both.
- The demo songs are free to reuse under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/): credit
  "Spinus DAW contributors".

## 1. Sound

Press **F10** (or click **⚙** in the top bar) and check that your sound card is chosen. Click **♩** to turn
on the metronome and press **Space**: you should hear clicks. Press **Space** again to stop. See
[Audio settings](audio-settings.md).

## 2. Drums

1. In the [Browser](browser.md) on the left, double-click **Drum Machine** (under Instruments › Spinus).
   A channel called "Drum Machine" appears in the [Channel Rack](channel-rack.md).
2. Click steps 1, 5, 9 and 13 on its row: a kick on every beat. Press **Space** to hear the pattern loop.
3. For snares and hi-hats, open the [Piano Roll](piano-roll.md) (**F7**) and draw notes: the
   [Drum Machine](drum-machine.md)'s pads start at C5 (kick), C#5 (snare), D5 (clap), D#5 (closed hat).

Have your own drum samples? Drag them from your file manager onto the Channel Rack: each becomes a
[Sampler](sampler.md) channel.

## 3. A bass line and a melody

1. Double-click **3x Osc** in the Browser for a simple synth, or **Surge XT** for a big one (if you
   skipped downloading it on first launch, double-clicking it offers the download; see
   [Get free plugins](free-plugins.md)).
2. Select its channel (click its name), open the Piano Roll (**F7**) and draw notes. With a MIDI keyboard,
   press **R**, then **Space**, and play; see [Recording](piano-roll.md#recording-from-a-midi-keyboard).
3. Make another pattern with the **+** next to the pattern list, and add a melody in it.

## 4. Arrange the song

Open the [Playlist](playlist.md) (**F5**). Choose a pattern in the top bar and click on a track to place it.
Press **L** to switch to song mode and **Space** to play the song. Drag audio files onto the Playlist to
add vocals or loops.

## 5. Mix

Open the [Mixer](mixer.md) (**F9**). Every channel has its own insert. Click a strip, then an empty slot on
the right to add an effect: try **Parametric EQ** and **Compressor** on the drums, and a **Hall Reverb** on a
new insert that the melody sends to. Right-click a strip's name and choose **Create automation clip** for
a volume fade.

## 6. Save and share

Press **Ctrl+S** to save the project: Spinus makes a folder with your song's name, holding everything
the song needs. Press **Ctrl+R** to [export](export.md) a WAV, MP3 or another format of the song. To
send the whole project to someone, send its folder. Everything you did can be undone
with **Ctrl+Z**, and the **History** panel (**Ctrl+H**) lists every step. See [Projects](projects.md).
