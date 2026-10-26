# Troubleshooting

## First: is Spinus itself working?

In a terminal:

```sh
spinus --self-test ~/spinus-check
```

It makes, saves, reopens and exports a short song without sound card or window, and ends with "Spinus
works here." If a line fails, it says what is wrong. (From the AppImage: `./Spinus-*.AppImage
--self-test ~/spinus-check`.) Delete `~/spinus-check` afterwards.

## The window has no text

Spinus draws its text with a font from your system. Install one, for example DejaVu Sans (on Arch:
`sudo pacman -S ttf-dejavu`), and start Spinus again. The self-test above says which font it found.

## No sound

- Open [Audio settings](audio-settings.md) (**F10**) and click **Test** beside **Play through**. No beep:
  choose another output, or **PipeWire Sound Server**, which follows your desktop's speakers.
- Check the Mixer: the Master fader and the insert's fader are up, nothing is muted (**M**) or soloed
  (**S**) elsewhere, and the channel's green light in the Channel Rack is on.
- Is the song in pattern mode with an empty pattern? Press **L** for song mode, or choose another pattern.
- After switching speakers or headphones on the desktop, give Spinus a moment: it follows the change.

## Crackles and dropouts

- The line at the bottom of **F10** counts **dropouts**. If it rises while playing, choose a larger
  **Buffer size** (256 or 512).
- Install `pipewire-jack` and choose **JACK** in **F10**: it is steadier and has less delay.
- The Mixer shows each insert's CPU share at the top right of its strip, and each effect's in its slot.
  Find the heavy one and [freeze](freeze.md) its channel or insert.
- Windows plugins play best with real-time privileges: see the line at the bottom of **Options ›
  Windows plugins...**.
- Every program crackles, not only Spinus: something may be setting your system's sound clock, often a
  **webcam microphone** that is the system's default input while a program (or an echo canceller, a
  "Noise Suppressed Microphone") listens to it. Run `pw-top` in a terminal: the line at the top of the
  list that has no **+** before its name is the clock. If it is a webcam (or other USB microphone), choose
  the laptop's own microphone as the default input in your desktop's sound settings, and try again.

## The microphone isn't heard

- In **F10**, choose your microphone by name under **Record from**, then the input under **Microphone
  on**. Talk: the level bar should move.
- A USB microphone listed under **Directly through ALSA** usually fails, because PipeWire is using it:
  choose it by name at the top of the list instead.
- Spinus records your microphone but doesn't play it back while recording: listen through your sound
  card's direct monitoring. See [Audio tracks](audio-tracks.md#recording-audio).

## The MIDI keyboard does nothing

- Plug it in before starting Spinus, or restart Spinus after plugging it in.
- It plays the **selected** channel: click a channel's name in the Channel Rack first.
- If the hint bar says "no MIDI keyboard is connected" when you arm recording, the keyboard isn't seen
  by the system: try another USB port or cable. **Ctrl+T** plays notes from the computer keyboard
  meanwhile.

## A plugin is missing or crashes

- Click **Scan plugins** at the bottom of the [Browser](browser.md). Plugins that failed are listed under
  **Problem plugins**, with the reason.
- A plugin that crashed Spinus can run in its own process from then on, so it can't take Spinus down
  again: Spinus offers this after the crash. See [Plugins](plugins.md).
- Windows plugins: see [Windows plugins](windows-plugins.md).

## The file dialog doesn't look like my desktop's

On Linux, Spinus's Open, Save and Export dialogs are your desktop's own (KDE's on Plasma, GNOME's on
GNOME), shown by the desktop portal. When the portal isn't running, Spinus shows its own file browser
instead. To get the desktop's dialog, install the portal for your desktop and log in again, on Arch:

- KDE Plasma: `sudo pacman -S xdg-desktop-portal xdg-desktop-portal-kde`
- GNOME: `sudo pacman -S xdg-desktop-portal xdg-desktop-portal-gnome`
- other desktops: `sudo pacman -S xdg-desktop-portal xdg-desktop-portal-gtk`

The Windows plugins dialog's **Add** always uses Spinus's own browser: it takes an installer, a plugin
file or a plugin folder, and the desktop's dialog can't offer files and folders at once.

## Spinus closed unexpectedly

Start it again: it offers to **Recover** your work from the last autosave (every two minutes). See
[Projects](projects.md). Please report the crash with what you were doing: **Help › Report a problem...**
makes a file with the logs of the last runs and opens the bug report page (see
[Report a problem](help.md#report-a-problem)). The last lines of the crashed run's log usually say what went
wrong.

## Log files

Spinus writes a log of each run: when it started, the sound card it opened, projects opened, saved and
exported, plugin scans and plugins that failed to load or crashed, the Windows plugins setup, the AI
service, and the errors it showed you. **Help › Report a problem...** puts the logs of the last three runs in
one file to attach to a bug report; or attach the log of the run where it happened yourself.

**Help › Open log folder** opens the folder. It is:

| Spinus | Log folder |
|---|---|
| Linux (Arch package, AppImage, built yourself) | `~/.config/Spinus DAW/logs` |
| Flatpak | `~/.var/app/io.github.tuturu742.spinus/config/Spinus DAW/logs` |
| Windows | `%APPDATA%\Spinus DAW\logs` (type it into the address bar of File Explorer) |

- There is one file per run, named by the time Spinus started: `spinus-2026-10-24_14-03-12.log`. The AI
  service, when AI is on, writes its own beside it: `spinus-2026-10-24_14-03-12-ai.log`. Attach both.
- Spinus keeps the logs of its last 10 runs and deletes older ones. A log stops growing at 4 MB.
- If Spinus closed unexpectedly, the next run's log says so near its top ("The previous run ended
  unexpectedly"), and names the file of the run that crashed.
- **Nothing private is in them:** your home folder is written as `~` (so your user name isn't), API keys
  and tokens are replaced by `***`, email addresses by `<email>`, and what you ask the assistant or the AI
  features is never written down. The logs do hold the names of your projects, plugins and sound card,
  and the folders your projects are in (below your home folder). They are plain text: open one in a text
  editor to see for yourself before you send it.
- `spinus --self-test` prints where the log folder is.

## Still stuck

Look at [Known issues](known-issues.md), or ask the [assistant](assistant.md) (with AI turned on): it
knows this manual.
