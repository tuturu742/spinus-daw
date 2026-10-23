# The demo song

Every project folder here is a demo song that ships with Spinus (`Siskin/` is listed first): the
Browser lists each under **Projects** as "*name* (demo)", and opening one copies it to the user's Music
folder and opens the copy (`src/session/DemoSong.h`). The whole folder (without this README)
is installed to `<prefix>/share/spinus/demo/` (Arch, AppImage, Flatpak; `cmake/SpinusInstall.cmake`) and
beside `spinus.exe` on Windows (`packaging/windows/stage.py`).

`Neon Coast/` is a second demo song, an original synthwave track (104 BPM, D minor, with a 6/8 bridge and a
last chorus in E minor) on Spinus's own devices and Surge XT, Surge XT Effects, OB-Xf, Dragonfly Reverb and
PeakEater (all free for Linux and Windows). It is made by `tools/demo-synthwave` (see
[Making Neon Coast again](#making-neon-coast-again)).

## Rainy Window

`Rainy Window/` is a second demo song: lo-fi hip hop in D minor at 82 BPM, about 2:35. It shows the
Channel Rack's swing and triplet steps, the Sampler and the Drum Machine playing sounds made for the song
(an electric piano, vinyl crackle and rain loops, a dusty kit), chord stamps, strum, arpeggiate and the
Piano Roll's per-note velocity, pan, fine pitch, volume and slides, audio clips reversed, slowed down and
pitched, three takes of a hummed line comped bar by bar, an "ooh" vocal that Spinus Tune pulls to a
melody channel, a tape wobble and filter sweeps as automation clips, and Spinus EQ, Compressor, Filter,
Stereo Utility, Ducker and Limiter. It plays on Surge XT and Dragonfly Reverb besides Spinus's own devices.
Its sounds (the samples inside the Sampler and Drum Machine states, and `audio/`) are synthesised by
`tools/demo-lofi/make_audio.py` (numpy), with no recordings.

Make it again like the showcase (`python3` with numpy for the sounds):

```sh
cmake --build --preset release --target spinus_demo_lofi
mkdir -p /tmp/lofi/home /tmp/lofi/audio
python3 tools/demo-lofi/make_audio.py /tmp/lofi/audio
HOME=/tmp/lofi/home build/release/tools/demo-lofi/spinus_demo_lofi --plugins build/release/plugins \
    --audio /tmp/lofi/audio --make "/tmp/lofi/Rainy Window"
rm -rf "demo/Rainy Window/Rainy Window.spn" "demo/Rainy Window/plugin-states" "demo/Rainy Window/audio"
cp -r "/tmp/lofi/Rainy Window/Rainy Window.spn" "/tmp/lofi/Rainy Window/plugin-states" \
      "/tmp/lofi/Rainy Window/audio" "demo/Rainy Window/"
```

`--render "<project folder>" --wav <file> [--stems <folder>]` exports it to listen to. The test "The lo-fi
demo song Rainy Window reads, names nothing of this computer and opens" (`tests/session/DemoSongTests.cpp`)
checks the shipped copy.

## License

The demo song (everything in `Siskin/`) is licensed under the
[Creative Commons Attribution 4.0 International License](https://creativecommons.org/licenses/by/4.0/)
(CC BY 4.0). Credit: "Spinus DAW contributors". *Rainy Window* (everything in `Rainy Window/`) is under the
same license and credit. Spinus's code, including the generators in `tools/showcase-song/` and
`tools/demo-lofi/`, stays AGPL-3.0-or-later.

The song uses Spinus's own instruments and effects (3x Osc, Drum Machine, EQ, Compressor, Filter, Ducker,
Gate, Stereo Utility, Loudness Meter, Limiter) and the free open-source plugins Spinus downloads on request
(Surge XT, Surge XT Effects, Dragonfly Reverb), which all exist for Linux and Windows. Its plugin states hold
only their settings; its two
sound effects (`audio/`) are synthesised by `tools/showcase-song/make_audio.py`, with no samples.

## Making it again

The song is made through the Command API by `tools/showcase-song` (a development build with the free
plugins in `build/release/plugins`). Run it with HOME set to a scratch folder, then copy the result here
without the rendered `.wav` and without `.autosave/` or `.session.lock`:

```sh
cmake --build --preset release --target spinus_showcase_song
mkdir -p /tmp/showcase/home /tmp/showcase/audio
python3 tools/showcase-song/make_audio.py /tmp/showcase/audio
HOME=/tmp/showcase/home SHOWCASE_AUDIO=/tmp/showcase/audio \
    build/release/tools/showcase-song/spinus_showcase_song --plugins build/release/plugins \
    --make "/tmp/showcase/Siskin"
rm -rf "demo/Siskin/Siskin.spn" "demo/Siskin/plugin-states" "demo/Siskin/audio"
cp -r "/tmp/showcase/Siskin/Siskin.spn" "/tmp/showcase/Siskin/plugin-states" \
      "/tmp/showcase/Siskin/audio" "demo/Siskin/"
```

Its settings are written as the devices show them ("60%", "180 ms", "-18 dB"), read by Spinus's rule for
typed settings (`src/dsp/ParameterText.h`); `SHOWCASE_SHOW_SETTINGS=1` prints what each one shows once set.
`--measure "<project folder>" --wav <file>` renders it and prints each section's level (`SHOWCASE_STEMS=<folder>`
also exports stems). The generator removes this computer's paths from the plugin states. The test "The shipped demo song
reads, and its copy opens as a project" (`tests/session/DemoSongTests.cpp`) checks that none are left.

## Making Neon Coast again

`tools/demo-synthwave` makes it the same way, reusing the showcase tool's helpers. Its three sound effects
are synthesised with numpy (`make_audio.py`). The plugin folder holds the free plugins as Spinus downloads
them: Surge XT and the Dragonfly reverbs (`build/release/plugins`), OB-Xf (`OB-Xf.clap` with its `Patches`
folder beside it) and `peakeater.clap`.

```sh
cmake --build --preset release --target spinus_demo_synthwave
mkdir -p /tmp/neon/home /tmp/neon/audio
python3 tools/demo-synthwave/make_audio.py /tmp/neon/audio
HOME=/tmp/neon/home SYNTHWAVE_AUDIO=/tmp/neon/audio \
    build/release/tools/demo-synthwave/spinus_demo_synthwave --plugins <plugin folder> --make "/tmp/neon/Neon Coast"
rm -rf "demo/Neon Coast/Neon Coast.spn" "demo/Neon Coast/plugin-states" "demo/Neon Coast/audio"
cp -r "/tmp/neon/Neon Coast/Neon Coast.spn" "/tmp/neon/Neon Coast/plugin-states" "/tmp/neon/Neon Coast/audio" \
      "demo/Neon Coast/"
```

`--render "<project folder>" --wav <file> [--stems <folder>]` exports it and prints each section's level;
`--shots "<project folder>" --out <folder>` draws the Playlist, Channel Rack, Mixer and Piano Roll to PNG
files, to check that it is tidy. The test "The bundled Neon Coast demo song reads, and names nothing of this
computer" (`tests/session/DemoSongTests.cpp`) checks the shipped folder.

## Sodium Lights

`Sodium Lights/` is a second demo song: liquid drum & bass in F minor at 174 BPM, about 2:45 long. It
shows the Beat Slicer on a breakbeat made for it (sliced at its hits and re-sequenced into new grooves),
the Drum Machine as kick and percussion layers, the Sidechain Ducker on the bass and music buses, a Reese
bass in Surge XT with filter automation, the Launcher with one scene per section, buses and sends, an
effect copied to another insert, a frozen channel (the pad), the Limiter and Loudness Meter on the Master,
markers per section and track groups in the Playlist. It is CC BY 4.0 like the showcase song
(`Sodium Lights/LICENSE.txt`). It plays on Spinus's own devices and on Surge XT, Surge XT Effects,
Dragonfly Reverb and PeakEater, which all exist for Linux and Windows.

Its four sounds (the break, vinyl crackle, an impact and a reversed cymbal) are synthesised by
`tools/demo-dnb/make_audio.py` (Python with numpy and scipy); the break is also kept inside the Beat
Slicer's state. The song is made by `tools/demo-dnb` the same way as the showcase song. The plugins folder
needs `peakeater.clap` too (from Spinus's download of PeakEater):

```sh
cmake --build --preset release --target spinus_demo_dnb
mkdir -p /tmp/dnb/home /tmp/dnb/audio
python3 tools/demo-dnb/make_audio.py /tmp/dnb/audio
HOME=/tmp/dnb/home DNB_AUDIO=/tmp/dnb/audio \
    build/release/tools/demo-dnb/spinus_demo_dnb --plugins build/release/plugins --make "/tmp/dnb/Sodium Lights"
rm -rf "demo/Sodium Lights/Sodium Lights.spn" "demo/Sodium Lights/plugin-states" "demo/Sodium Lights/audio"
cp -r "/tmp/dnb/Sodium Lights/Sodium Lights.spn" "/tmp/dnb/Sodium Lights/plugin-states" \
      "/tmp/dnb/Sodium Lights/audio" "demo/Sodium Lights/"
```

`spinus_demo_dnb --render <folder> --wav <file>` renders it and prints its level per section, and
`--shots <folder> --out <folder>` draws its panels (Playlist, Mixer, Launcher ...) to PNG files. The test
"The liquid drum & bass demo song reads, names nothing of this computer and opens"
(`tests/session/DemoSongTests.cpp`) checks the shipped folder.
