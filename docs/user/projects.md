# Projects: new, open, save

A Spinus project is a **folder**, for example `My Song`. Inside it, `My Song.spn` is the project
file: double-click it in your file manager, or pick it in **Open**, to open the project. The folder
also holds every plugin's settings and the audio files the song uses. Keep the whole folder together
when you copy, share or back it up (to send it as one file, zip the folder in your file manager).

## The File menu

| Item | What it does | Shortcut |
|---|---|---|
| **New** | Starts an empty project. | **Ctrl+N** |
| **Open...** | Opens a project: pick its `.spn` file. | **Ctrl+O** |
| **Open recent** | The last 10 projects you opened or saved. | |
| **Save** | Saves the project. A project that was never saved asks for a name and a place first. | **Ctrl+S** |
| **Save as...** | Saves the project in a new folder with the name you type. The audio files it uses are copied along. Spinus will not save into a folder that already holds other files. | **Ctrl+Shift+S** |
| **Export audio...** | See [Exporting audio](export.md). | **Ctrl+R** |
| **Quit** | Closes Spinus. | **Ctrl+Q** |

In the **Open** window, go into the project's folder, pick the `.spn` file and click **Open**. On Linux
the Open and Save windows are your desktop's own file dialog (KDE's, GNOME's), with its places and
bookmarks; see [Troubleshooting](troubleshooting.md#the-file-dialog-doesnt-look-like-my-desktops) if
they aren't.

You can also open a project from a terminal: `spinus "My Song/My Song.spn"`.

While a project opens, Spinus shows **Opening <name>** with a bar and the plugin it is loading, for example
"Loading Surge XT on Lead (3 of 12)". Big plugins can take a few seconds each, so a project with many of them
takes a while to open; the bar shows it is still working.

Every project you open or save is also listed in the [Browser](browser.md#projects) under **Projects**,
with the demo songs and folders of projects you add: double-click one there to open it.

## Missing plugins and audio files

A project opens even when something it uses isn't on this computer, and Spinus shows what is missing:

- A plugin that isn't installed shows as **Missing: <name>** in red, in the [Mixer](mixer.md) or the
  Channel Rack. It stays silent, keeps its settings, and plays again once the plugin is installed.
- An audio file that was moved or deleted: its clip in the [Playlist](playlist.md) turns red with a flat
  line instead of the waveform and **Missing:** before its name, and its track in
  [Audio tracks](audio-tracks.md) shows **Missing:** in red with the file's name. The clip stays silent.

Saving keeps the missing things in the project. Put the audio file back where it was (usually the
project's `audio` folder) and it plays again; reopen the project if it stays silent.

The Sampler, the Drum Machine and the Beat Slicer keep a copy of their sounds inside the project, so their
channels never miss a file, even after the original sample is deleted.

## Unsaved changes

The window title shows the project's name. A **\*** after the name means there are changes you have not
saved. Before New, Open or Quit, Spinus asks whether to save them:

- **Save** saves first (asking for a name if the project is new).
- **Don't save** throws the changes away.
- **Cancel** goes back to your project.

Changing a setting inside a plugin's own window does not add a \*, but Spinus still notices it and asks
before closing.

If a save fails (the disk is full, or the folder is one you may not write in), Spinus says why and the
project on disk stays exactly as you last saved it: nothing is half-written. Free some space, or use
**Save as...** to save somewhere else.

## Autosave and recovery

Every 2 minutes, if anything changed, Spinus saves a copy of your work in the project's `.autosave`
folder and keeps the newest 10 copies. A project that was never saved is autosaved too.

If Spinus closes unexpectedly (a crash, a power cut), it offers your autosaved work the next time you
start it, or when you open that project again:

- **Recover** opens the autosave. The title shows a \*: save to keep it.
- **Discard** keeps the last version you saved. For a saved project the autosave copies stay in its
  `.autosave` folder. For a project you never saved, Discard deletes them.

If a project file is damaged (for example cut off by a crash while it was saved), Spinus says so when you
open it, and where the project's newest autosave is. Spinus can't open that autosave for you yet; see
[Known issues](known-issues.md#projects).

A project can be open in only one Spinus window at a time; Spinus tells you if it is already open
elsewhere.
