# Plugins

Spinus loads VST3, CLAP, LV2 and VST2 plugins made for Linux. It can also download a set of free,
open-source plugins for you (Surge XT, Surge XT Effects, LSP Plugins, Dragonfly Reverb, Odin 2, OB-Xf,
PeakEater, Six Sines, TIME-12 and GATE-12; on Windows, those published for Windows): see [Get free plugins](free-plugins.md).

## Where Spinus looks

| Format | Folders |
|---|---|
| VST3 | `~/.vst3`, `/usr/lib/vst3`, `/usr/local/lib/vst3` |
| CLAP | `~/.clap`, `/usr/lib/clap`, `/usr/local/lib/clap`, and folders in `$CLAP_PATH` |
| LV2 | `~/.lv2`, `/usr/lib/lv2`, `/usr/local/lib/lv2` |
| VST2 | `~/.vst`, `/usr/lib/vst`, `/usr/local/lib/vst` |

The free plugins Spinus downloads are kept in `~/.local/share/Spinus DAW/plugins` and are found
automatically; they are not scanned.

## Scanning

Spinus checks each new or changed plugin file in a separate helper program. If a plugin crashes or
does not finish loading within 60 seconds, Spinus is not affected: the plugin goes on the
**Problem plugins** list with the reason, and later scans skip it until you right-click it in the
[Browser](browser.md) and choose **Try again on the next scan**.

Click **Scan plugins** at the bottom of the [Browser](browser.md) to scan. You can also scan from a
terminal:

```sh
./build/release/spinus --scan-plugins
```

It prints the plugins found and any problem plugins. The results are saved in
`~/.config/Spinus DAW/` (`plugin-list.xml`, `problem-plugins.xml`). The next scan only checks new or
changed files.

## When a plugin crashes

If Spinus closes unexpectedly because of a plugin, the next start says which one and offers to **run it
in a separate process**. There, if it crashes or stops responding again, only that plugin stops:

- Spinus and the rest of the song keep playing. An effect lets the sound through unchanged; an
  instrument goes silent.
- A message says which plugin stopped. **Reload** starts it again with its settings.
- Until it is reloaded, the Mixer shows the effect as **Stopped** in red (an instrument's name turns red in
  the Channel Rack), and its window closes.

Running separately costs a little more CPU and adds one audio block of delay (Spinus lines up the
rest of the song with it). Playing that instrument live from a keyboard feels that block. Its window
looks and works as before.

You can switch any plugin in or out of a separate process yourself. Right-click it in the
[Channel Rack](channel-rack.md) (an instrument) or the [Mixer](mixer.md) (an effect) and tick **Run in a
separate process (safe from crashes)**. The choice is for the plugin on this computer: it applies in
every project.

Not yet supported in a separate process: sidechain inputs, MIDI a plugin sends out, and more than eight
channels. Plugins needing those run inside Spinus.

## Plugin windows

Double-click a channel's name in the Channel Rack, or an effect in the Mixer, to open its window.

- The window shows the plugin's own controls. A plugin without its own window gets a simple one with a
  slider for each setting.
- Plugin windows stay above the Spinus main window, but not above other programs.
- A window reopens where you last closed it.
- Removing the instrument or effect closes its window. Reordering effects keeps it open.
- The title says what it is and where: for example, "Hall Reverb - 4 Reverb".
