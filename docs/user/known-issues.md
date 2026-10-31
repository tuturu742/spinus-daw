# Known issues

What doesn't work yet, or works with a catch, in this version of Spinus. If you find something that
isn't listed here, please [report it](https://github.com/tuturu742/spinus-daw/issues): say what you did,
what happened, and attach the project if you can.

## Being worked on

- **Short crackles or clipping on some systems** when Spinus starts, when a project opens, or when
  switching **Before** and **After** in the [Automixer](automixer.md). Spinus's own output is clean in
  tests, so the cause may be the sound system switching devices. If you hear it, please report which
  sound card and sound system (PipeWire, JACK) you use.

## Systems

- Spinus runs on **Linux** and **Windows** (10 and 11, 64-bit). macOS is not planned for now (releasing
  for macOS needs a paid Apple developer account).
- The AppImage needs a Linux system from 2024 or newer (glibc 2.39: Arch, Fedora 40, Ubuntu 24.04,
  Debian 13 or newer).
- Song analysis on the graphics card has been tested on a few cards only; see
  [the analysis engine](analysis-engine.md). Every computer can use the CPU instead.

## Free plugins

- **OB-Xf's own patch menu does not list its factory patches**: Spinus downloads them (the Analyzer's
  Closest preset uses them), but OB-Xf only looks in its own shared folder, outside the folder Spinus
  downloads into. Install OB-Xf with its own installer to browse them there; see
  [Get free plugins](free-plugins.md).

## Windows

- The installer isn't signed yet, so Windows shows **"Windows protected your PC"** the first time: click
  **More info**, then **Run anyway**.
- **LSP Plugins has no Windows version**, so its effects (Parametric EQ, Compressor, Limiter, Gate,
  Multiband Compressor and Spectrum Analyzer) aren't available on Windows. Use **Spinus EQ**, **Spinus
  Compressor**, **Spinus Limiter** and **Spinus Gate** instead: they are built in and work everywhere. A
  project that uses LSP's effects opens on Windows with them shown as missing (their settings are kept,
  and they play again on Linux).
- **Odin 2 isn't offered on Windows**: its project publishes only an installer program for Windows, and
  Spinus never runs other programs' installers. A project that uses Odin 2 opens on Windows with it
  shown as missing, like LSP's effects above. You can still install Odin 2 yourself from its website:
  Spinus then finds it as an ordinary plugin.
- **The [analysis engine](analysis-engine.md) doesn't use AMD graphics cards on Windows yet**: on a
  computer with one it runs on the CPU, which works but takes longer. Nvidia and Intel Arc cards are used.
- A plugin running in a separate process doesn't show its own window on Windows yet.

## Recording

- You hear your microphone through your sound card's direct monitoring, not through Spinus's effects
  while recording.
- Several USB microphones at once (each its own device) can't be recorded together: use one sound card
  with several inputs. See [Audio tracks](audio-tracks.md).
- Recording from an MPE controller keeps the notes, not each note's bends and pressure.

## Plugins

- 32-bit Windows plugins aren't supported. Plugins with copy protection that needs a USB key or a
  background service (iLok and similar) may not work under Wine. See [Windows plugins](windows-plugins.md).
- Some Windows plugins that draw with the graphics card (for example Ample Bass P Lite) may not redraw
  their window with some graphics drivers. Turn on **Draw plugin windows without the graphics card** in
  Options › Windows plugins. See [Windows plugins](windows-plugins.md#when-a-plugins-window-doesnt-update).
- In the Flatpak version, plugins installed by your system's package manager that need newer libraries than
  the Flatpak has run in a separate process with your system's libraries, so they have that process's limits
  (below). A plugin that needs a library your system doesn't have either is listed under Problem plugins
  with the reason. The Flatpak can use only Spinus's tested Wine for Windows plugins. See [Install](install.md#as-a-flatpak).
- A plugin running in a separate process can't use sidechain inputs, send MIDI out, or have more than
  eight channels. See [Plugins](plugins.md).
- Some **LV2 plugins open with Spinus's sliders instead of their own window**: those whose window is
  made for GTK or Qt only (some Calf and older plugins), and those whose window needs something Spinus
  doesn't offer yet or fails to open. The plugin itself works as usual.

## Track creator (experimental)

- **Local models in Ollama are slow at writing whole songs:** on a laptop, the models tested (Qwen 3 30B and
  Qwen 3.8 27B) didn't finish a song in an hour and a half;
  for the AI writer, a cloud model is much faster.
- **AI models can be unreliable** when the AI writes the song: one may leave parts to Spinus's own rules,
  miss a section the description asks for, or stop with an error from its provider. Listen before you keep
  it, and write again if needed.

## Projects

- Projects saved by this version open in every later version of Spinus. A project saved by a newer
  version may not open in an older one.
- Presets and projects don't show in the Browser yet.
