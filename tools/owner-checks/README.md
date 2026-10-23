# Owner checks

Scripts for checks that need real hardware (GPU, audio latency, Windows plugins) and cannot run
in CI. Each script prints a report; paste the output into the issue or pull request that asked for it.

| Script | Checks |
|---|---|
| `gpu.sh` | The analysis engine on your graphics card |
| `analysis.sh <song>` | The Analyzer's and Automixer's AI with the real models |
| `packages.sh` | The AppImage and the Flatpak with your sound card and plugins |
| `windows-plugins-setup.sh`, `windows-plugins.sh`, `windows-plugin-redraw.sh` | Windows plugins through Wine |
| `flatpak-32bit-windows.sh` | 32-bit Windows installers in the Flatpak (needs Flatpak's 32-bit support) |
| `ai-gpu-benchmark.sh` | Local AI models' speed |
| `audio-latency.sh` | Audio latency |
| `file-dialog.sh` | File dialogs are your desktop's own (open, save, folder), for this build and the installed Flatpak |
| `spinus-clicks.sh` | Whether a click is in Spinus's own sound (records only Spinus while you make it happen) |
