# The main window

![The Spinus main window](images/main-window.png)

| Area | What it is |
|---|---|
| **Menu bar** | **File**: new, open, open recent, save, save as, export, quit. **Edit**: undo, redo, History, automate the last moved control. **View**: every panel (below), the Browser, the Assistant and [layouts](#layouts). **Options**: [audio settings](audio-settings.md), [free plugins](free-plugins.md), [Windows plugins](windows-plugins.md), [AI](assistant.md), [keyboard shortcuts](keyboard-shortcuts.md), metronome, quantize recorded notes, typing keyboard to piano. **Help**: the [manual and About Spinus](help.md). |
| **Top bar** | Transport, current pattern, tempo, key, position, CPU load, and buttons that open the panels (a lit button means the panel is open). |
| **Browser** (left) | Your projects and the demo songs, your samples, instruments, effects and installed plugins: double-click or drag to use them. Show or hide it with **F8** or the **Browser** button. See [Browser](browser.md). |
| **Workspace** (centre) | Spinus's panels float here, like in FL Studio. |
| **Assistant** (right) | Ask about your song or Spinus; changes come as proposals (see [The assistant](assistant.md)). Off until you turn AI on in **Options › AI...**; until then its button in the top bar is dimmed. Show or hide it with **Ctrl+Shift+A**. |
| **Hint bar** (bottom) | Tells you what the control under the mouse does. |

## Panels

| Panel | Shortcut |
|---|---|
| Playlist | **F5** |
| Channel Rack | **F6** |
| [Audio tracks](audio-tracks.md) | **Shift+F6** |
| Piano Roll | **F7** |
| Mixer | **F9** |
| History | **Ctrl+H** |
| [Launcher](launcher.md) | View menu |
| [Routing](mixer.md#the-routing-panel) | View menu |
| [Automation editor](playlist.md#the-automation-editor) | View menu, or double-click an automation clip |
| [Analyzer](analyzer.md) (experimental) | View menu, once **Show experimental features** is ticked in **Options › AI...** |
| [Automixer](automixer.md) | View menu |

Every key is listed in [Keyboard shortcuts](keyboard-shortcuts.md).

- Pressing a panel's shortcut brings it to the front. If it is already in front, the shortcut closes it.
- Drag a panel by its title bar to move it, and drag its edges to resize it. The title bar always stays
  in the workspace, so a panel can always be grabbed again.
- Click **↗** in the title bar, or double-click the title, to open the panel in its own window, for
  example on a second monitor. Close that window to put the panel back into the workspace.
- Click **×** to close a panel. Its shortcut opens it again.

## Layouts

A layout is a set of open panels in their places, for one kind of work. **View › Layouts**:

| Layout | Shortcut | Opens |
|---|---|---|
| **Compose** | **Ctrl+Alt+1** | Channel Rack and Playlist side by side, the Piano Roll below them, and the Browser. |
| **Arrange** | **Ctrl+Alt+2** | The Playlist over the whole workspace, the Channel Rack behind it. |
| **Mix** | **Ctrl+Alt+3** | The Mixer below a short Playlist, without the Browser. |

Panels a layout doesn't list are closed. To keep your own, arrange the panels (and the Browser and
Assistant) the way you like, then **View › Layouts › Save layout...** and give it a name: it appears in
the list. Saving under the name Compose, Arrange or Mix changes that one; **Delete layout › Reset ... to
Spinus's** brings Spinus's back. Panels in their own windows are saved where they are on the screen.

Spinus also opens with the panels where they were when you last closed it. Layouts are kept for you, not
in projects (in `~/.config/Spinus DAW/layouts.xml`).

## Transport

| Control | What it does | Shortcut |
|---|---|---|
| **▶ / ■** | Play or stop. Stopping returns to where playback started. | **Space** |
| **●** | Arm recording: playing then records your MIDI keyboard into the current pattern. See [Recording](piano-roll.md#recording-from-a-midi-keyboard). | **R** |
| **PAT / SONG** | **Pattern mode** plays the current pattern on its own, over and over. **Song mode** plays the playlist. | **L** |
| Pattern list and **+** | Choose the current pattern (each is shown in its colour), or add a new one. Below the patterns, the list also has **New pattern**, **Clone** (a copy with all its notes, "Verse 2"), **Rename**, **Colour** and **Delete** for the current pattern (deleting it also removes its Playlist clips; Ctrl+Z brings both back). | |
| **♩** | Metronome on or off. The first beat of each bar has its own, higher click. | **Ctrl+M** |
| **4/4** | The time signature: the current pattern's in pattern mode, the song's in song mode. Click to set the song's (2/4, 3/4, 4/4, 5/4, 6/8, 7/8, 9/8, 12/8), or to give the current pattern its own, so its bars in the Piano Roll and Channel Rack are counted that way. The song's time signature sets the Playlist's bars and the metronome. | |
| **⌨** | Typing keyboard to piano: the computer keyboard plays the selected channel. See [Playing with the computer keyboard](piano-roll.md#playing-with-the-computer-keyboard). | **Ctrl+T** |
| Tempo | Drag up or down (hold Shift for small steps), or scroll, to change it. Double-click to type a value. Each change can be undone. | |
| **Key: ...** | The song's key, e.g. **Key: A minor**: click to choose its note and scale, or **No key**. [Spinus Tune](spinus-tune.md), the [pattern generator](piano-roll.md#generate), [Record idea](piano-roll.md#record-an-idea) and the Piano Roll's [scale highlighting](piano-roll.md#scale-highlighting) use it. It never changes notes by itself. | |
| Position | Where playback is, as bar:beat:step. | |
| CPU | How busy audio processing is. | |


## Keyboard shortcuts

**Help › Manual** (**F1**) opens this manual inside Spinus, with a search; **Help › About Spinus** shows the
version and the licenses (see [Manual and About](help.md)). The demo songs are in the
[Browser](browser.md#projects), under **Projects**.

**Options › Keyboard shortcuts...** lists every command with its keys. Click a key to change or remove
it, click **+** to give a command another key, **reset to defaults** to go back to Spinus's keys. Your
keys are kept for the next start (in `~/.config/Spinus DAW/shortcuts.xml`, only the ones you changed).
Keys inside a panel (for example **Ctrl+C** in the Piano Roll, **Delete** for the selection) are fixed.
All of them are listed in [Keyboard shortcuts](keyboard-shortcuts.md).

## Undo

**Ctrl+Z** undoes the last change and **Ctrl+Shift+Z** redoes it. The hint bar shows what was undone.

The **History** panel (**Ctrl+H**) lists every change, oldest at the top. Click any step to go back
(or forward) to it; steps you can still redo are dimmed. "Open project" is the project as it was opened.
Making a new change after going back removes the dimmed steps.

## Projects

**Ctrl+N** starts a new project, **Ctrl+O** opens one and **Ctrl+S** saves. The window title shows the
project's name, with a **\*** while there are unsaved changes. See [Projects](projects.md).

## Export

**Ctrl+R** exports the song or a pattern as WAV, FLAC, MP3 or Opus, with stems if you like. See
[Exporting audio](export.md).
