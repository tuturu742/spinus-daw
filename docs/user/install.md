# Installing Spinus

## On Arch Linux

Spinus comes with an Arch package recipe (a PKGBUILD). It builds Spinus and everything it bundles from
source, so the first build takes a while (about half an hour on a fast computer) and needs an internet
connection.

1. Install the tools for building packages, if you have not yet:

   ```sh
   sudo pacman -S --needed base-devel git
   ```

2. Get Spinus and build it:

   ```sh
   git clone https://github.com/tuturu742/spinus-daw.git
   cd spinus-daw/packaging/arch/release
   makepkg -si
   ```

   This builds the newest release. For the newest code instead (it may have more bugs), use
   `packaging/arch` instead of `packaging/arch/release`.

   `makepkg` installs the libraries Spinus needs, builds it, and asks for your password to install the
   package.

3. Start **Spinus DAW** from your application menu, or type `spinus` in a terminal.

## Recommended extras

| Package | Why |
|---|---|
| `pipewire-jack` | Low-latency audio through PipeWire. Most Arch desktops already use PipeWire. |
| `lame` | Exporting MP3 files. |

Install them with `sudo pacman -S pipewire-jack lame`.

## The free plugins (Surge XT, LSP, Dragonfly, Odin 2, OB-Xf, PeakEater)

The package does not include Surge XT, LSP Plugins, Dragonfly Reverb, Odin 2, OB-Xf or PeakEater. The
first time you start Spinus, it offers to download them from their projects' own websites; see
[Get free plugins](free-plugins.md).

If you prefer Arch's own packages, install them instead and Spinus uses them:
`sudo pacman -S surge-xt-clap lsp-plugins-clap dragonfly-reverb-clap`.

## What gets installed

- `spinus` (the program) and its plugin scanner, in `/usr/lib/spinus`.
- Spinus's own instruments as plugins for other programs: "Spinus Sampler", "Spinus Drum Machine" and
  "Spinus 3x Osc", as VST3 (`/usr/lib/vst3`) and LV2 (`/usr/lib/lv2`).
- A menu entry, an icon, and the `.spn` file type, so a project opens in Spinus when you double-click
  its `.spn` file.
- This manual, in `/usr/share/doc/spinus`.

## Updating and removing

To update, run `git pull` in the `spinus-daw` folder, then `makepkg -si` in the same folder as before.
To remove Spinus: `sudo pacman -R spinus` (or `spinus-git` for the newest code). Your projects and
settings (`~/.config/Spinus DAW`) stay, and so do the downloaded free plugins (`~/.local/share/Spinus DAW`);
delete that folder to remove them too.

## On other Linux systems: the AppImage

Download `Spinus-<version>-x86_64.AppImage` from the
[releases page](https://github.com/tuturu742/spinus-daw/releases). An AppImage is one file that runs on
most Linux systems from 2024 on (with glibc 2.39 or newer: Arch, Fedora 40 and newer, Ubuntu 24.04 and
newer, Debian 13). Nothing is installed:

```sh
chmod +x Spinus-*-x86_64.AppImage
./Spinus-*-x86_64.AppImage
```

It brings its own Python for the AI features. It uses your system's sound (ALSA, PipeWire or JACK)
and finds your plugins in the usual folders. Building one takes podman and about half an hour:

```sh
packaging/appimage/build.sh          # writes build/appimage/Spinus-<version>-x86_64.AppImage
packaging/appimage/check.sh build/appimage/Spinus-*-x86_64.AppImage   # tries it on Arch, Fedora, Ubuntu
```

## As a Flatpak

`packaging/flatpak/io.github.tuturu742.spinus.yml` builds Spinus as a Flatpak (with `flatpak-builder`).
Inside the Flatpak, Spinus sees:

- your home folder, with your plugins in `~/.vst3`, `~/.clap` and `~/.lv2`;
- plugins installed as Flatpak plugin extensions (`org.freedesktop.LinuxAudio.Plugins.*` on Flathub);
- plugins your system's package manager installed (in `/usr/lib/vst3`, `/usr/lib/clap`, `/usr/lib/lv2`,
  `/usr/lib/vst` and `/usr/local/lib/...`), read-only;
- the Windows plugins you set up in the other Spinus ([Windows plugins](windows-plugins.md#in-the-flatpak)).

**Plugins from your system's package manager often can't run inside the Flatpak.** They were built for
your system's own libraries, and the Flatpak brings its own, older ones; a plugin that needs a newer
library, or one the Flatpak doesn't have, doesn't load. Spinus lists such a plugin under **Problem
plugins** in the Browser with the reason ("... was installed by your system and cannot run inside the
Flatpak version of Spinus"), and a project that uses it shows it as **Missing**. For those plugins use
Spinus installed from a package or the AppImage; or install the plugin's Flatpak extension from Flathub,
or its own Linux download into `~/.vst3`, `~/.clap` or `~/.lv2`.

**32-bit Windows installers** (many plugin installers are 32-bit programs) need Flatpak's 32-bit
support, `org.freedesktop.Platform.Compat.i386`. Installing Spinus from Flathub brings it along; with the
`Spinus.flatpak` file it may be missing, and then Spinus says so when you choose such an installer, with a
**Copy the command** button. Paste the command in a terminal, answer its questions, then restart Spinus:

```sh
flatpak install flathub org.freedesktop.Platform.Compat.i386//25.08
```

The Flatpak keeps its own plugin list (in `~/.var/app/io.github.tuturu742.spinus/config/Spinus DAW`), so
it shows only plugins it can load; your other settings, recent projects and layouts are shared with the
other Spinus. A project saved in one opens in the other with the same plugins, where both can load them.

## On Windows

Spinus runs on Windows 10 and 11 (64-bit).

1. Download `Spinus-<version>-Setup.exe` from the [releases page](https://github.com/tuturu742/spinus-daw/releases).
2. Run it. Windows may say **"Windows protected your PC"**: Spinus isn't signed with a paid certificate
   yet. Click **More info**, then **Run anyway**.
3. Click through the installer. It needs no administrator rights: Spinus goes into your own programs
   folder (`%LOCALAPPDATA%\Programs\Spinus DAW`), with a Start menu entry, and `.spn` projects open in
   Spinus when you double-click them.
4. Start **Spinus DAW** from the Start menu.

Spinus brings its own Python for the AI features; nothing else needs installing.

**Sound.** Spinus uses your sound card's **ASIO** driver when one is installed (most audio interfaces
come with one; it gives the lowest delay). Otherwise it uses Windows's own sound (WASAPI). You can switch
in Options › Audio (F10).

**Plugins.** Spinus finds VST3, CLAP and VST2 plugins in the usual Windows folders
(`C:\Program Files\Common Files\VST3`, `...\Common Files\CLAP`, and your VST2 folder). Windows plugins
load directly; the Wine setup Spinus has on Linux isn't needed.

**The free plugins.** On first start Spinus offers Surge XT, Dragonfly Reverb, OB-Xf and PeakEater,
downloaded from their projects. LSP Plugins publishes no Windows version, and Odin 2 only an installer
program, so they aren't offered on Windows; Spinus's own EQ, Compressor, Limiter and Gate are built in
instead of LSP's. See [Get free plugins](free-plugins.md) and [Known issues](known-issues.md).

**Your files.** Settings are in `%APPDATA%\Spinus DAW`, downloads (free plugins, models) in
`%LOCALAPPDATA%\Spinus DAW`. To remove Spinus, use Settings › Apps › Spinus DAW › Uninstall; your settings,
downloads and projects stay. Delete those folders to remove them too.

## Checking that Spinus works

In a terminal:

```sh
spinus --self-test ~/spinus-check
```

On Windows, in PowerShell:

```powershell
& "$env:LOCALAPPDATA\Programs\Spinus DAW\spinus.exe" --self-test "$HOME\spinus-check" | Out-Host
```

Spinus makes a short song, saves it, opens it again and exports it, then checks its helper programs
and the Python its AI features use. It needs no sound card or window, and prints a line per check,
ending with "Spinus works here." The song and the exported WAV are left in `~/spinus-check` to look at;
delete the folder afterwards.
