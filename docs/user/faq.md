# Questions people ask

## Is Spinus free?

Yes, and open source (AGPL-3.0-or-later). The free plugins it offers (Surge XT, LSP Plugins, Dragonfly
Reverb, Odin 2, OB-Xf, PeakEater, Six Sines, TIME-12, GATE-12) are open source too, and so are the exact builds it downloads. Only the AI connections you choose to add (Anthropic, OpenRouter) may cost
money, paid to that provider; a [local model](assistant.md#a-local-model-with-ollama) costs nothing.

## Do I need AI? What does it send?

No. Spinus starts with AI turned off: nothing runs, nothing is downloaded and nothing is sent anywhere.
When you turn it on and ask the assistant something, your question and a summary of your song (its
channels, patterns and mixer, not its audio) go to the connection you chose. The Analyzer, Automixer,
Spinus Tune and Record idea work on your computer. See [The assistant and AI settings](assistant.md).

## Where are my projects, settings and plugins?

| What | Where |
|---|---|
| Projects | Where you save them: a folder per project, with its `.spn` file and audio inside |
| Settings, layouts, keyboard shortcuts | `~/.config/Spinus DAW` |
| Downloaded free plugins, Windows plugins | `~/.local/share/Spinus DAW` |
| Downloaded AI models | `~/.local/share/spinus/models` |

## How do I share a project?

Send the whole project folder (zip it first). Recordings are kept in its `audio` folder, and **File ›
Save as...** copies every audio file the project uses into its new folder, so it plays the same
elsewhere, as long as the other computer has the same plugins. Missing plugins show as **Missing**, keep
their settings, and come back once installed.

## Can I open FL Studio projects?

No. Export stems from FL Studio and drop them on the Playlist. See
[Coming from FL Studio](coming-from-fl-studio.md).

## Does it run on Windows or macOS?

Spinus runs on Linux and on Windows 10 and 11 (64-bit); see [Installing Spinus](install.md). macOS isn't
planned for now. On Linux, Windows plugins work through Wine: see [Windows plugins](windows-plugins.md).

## Which plugins work?

VST3, CLAP, LV2 and VST2 plugins for Linux, and Windows VST2, VST3 and CLAP plugins through Wine. See
[Plugins](plugins.md).

## Something doesn't work

See [Troubleshooting](troubleshooting.md) and [Known issues](known-issues.md), and please
[report it](https://github.com/tuturu742/spinus-daw/issues).
