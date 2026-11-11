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

## A damaged project file

A project file can be damaged, for example cut off by a crash or a full disk while it was saved. Spinus
then opens as much of your work as it can, and tells you what happened:

- **Parts of the file are damaged:** Spinus opens everything it can read and lists what it left out, for
  example "a clip at bar 5 on 'Drums' (its pattern is missing)" or "channel 'Bass'".
- **The file can't be read at all** (it is empty or cut off): Spinus opens the project's newest autosave
  instead and says so: "Song.spn was damaged and could not be read; Spinus opened the autosave from
  12 Nov 2026 14:05 instead."
- **Nothing can be read and there is no autosave:** Spinus says why it can't open the project, and your
  current project stays open.

The title shows a \* after a damaged project opens. **Save** to repair it. Nothing is written over the damaged
file until you save, and when you do, Spinus keeps a copy of it beside the project as `Song.spn.damaged`, in
case something you need was in it. You can delete that copy once the song is as it should be.

A project can be open in only one Spinus window at a time; Spinus tells you if it is already open
elsewhere.
