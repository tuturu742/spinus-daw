# Let a coding agent work on your project

A coding agent on your computer (Claude Code, Codex, Cursor and others that speak MCP) can work on the song open in
Spinus, while you watch and play it here:

- **Reading the song:** its sections, parts, sounds and mix.
- **Changing it:** writing parts, adding a bridge, shaping a sound, automating, mixing.
- **Looking things up:** Spinus's manual, and the style of your reference songs in the
  [Track creator](track-creator.md) (what they share: tempo, key, chords, drums, parts), so "make it like my
  references" works.
- **Listening by numbers:** rendering it to measure the mix's loudness and peaks, each section's loudness and
  each part's.

It uses the same music tools as Spinus's [assistant](assistant.md), plus Spinus's commands, through
**Spinus's live interface**. It's off until you turn it on.

## Turning it on

1. Open **Options › AI** and click **Coding agents...**.
2. Tick **Let coding agents work on the open project**. Spinus shows the address
   (`http://127.0.0.1:47219/mcp`) and a **token**, which works like a password.
3. Set your agent up with the text shown for it, using **Copy**:
   - **Claude Code:** run the command in a terminal:
     `claude mcp add --scope user --transport http spinus http://127.0.0.1:47219/mcp --header "Authorization: Bearer <token>"`.
     `--scope user` makes Spinus available in every folder you start Claude Code in. Without it, Claude Code
     only has Spinus in the folder where you ran the command.
   - **Codex:** add the lines to `~/.codex/config.toml`.
   - **Cursor:** put the lines in `~/.cursor/mcp.json` (the `.cursor` folder in your home folder) to have Spinus
     in every project, or in `.cursor/mcp.json` inside one project's folder to have it there only. If the file
     is already there, merge the `spinus` entry into its `mcpServers`.
   - On **Windows** the same text works: run the Claude Code command in PowerShell. Codex's file is
     `%USERPROFILE%\.codex\config.toml` and Cursor's is `%USERPROFILE%\.cursor\mcp.json`.
4. Ask your agent for something: "Look at my song in Spinus and add a bridge after the second chorus", "make the
   bass pluckier and duck it under the kick", "check the mix and fix what's muddy".

The music tools (write_part, insert_bars, shape_sound, check...) need AI turned on in Spinus (**Options › AI**).
They run in Spinus's own AI service on this computer, without any AI connection or internet. With AI off, the
agent still has Spinus's own tools: the song's summary, Spinus's commands, the devices that can be added, a
device's presets (plugins too: an agent can load them, and Ctrl+Z undoes it), and render. It's told so when it
connects, and the ready-made requests tell it how to do their job with those tools.

If you turn AI on or off while an agent is connected, Spinus tells the agent's app that its tools changed, the
next time the agent asks Spinus anything. Agents that listen for this pick the music tools up then. If your agent
doesn't show them, reconnect it: in Claude Code, type
`/mcp`, choose **spinus** and **Reconnect**; in other agents, restart them.

**A Flatpak Spinus and a native Spinus** on the same computer share these settings, including the port and the
token. Only one of them can have the interface on at a time: the other one says the port is in use. Close one,
or give one another port.

What the music tools make looks like a song built by hand:
- **Short patterns:** a part that repeats is one short pattern (1, 2 or 4 bars) placed again and again on the
  Playlist, not one long pattern.
- **Family colours:** each new part goes in its family's Channel Rack group (Drums, Bass, Guitars...). All its
  family's parts share one colour, which their patterns and Playlist tracks take too.
- **Automation:** a part's automation goes on a track right under the part's own track, in its group, so folding
  the part's track hides its automation too.

## Ready-made requests

Agents that show an MCP server's prompts (in Claude Code, type `/mcp__spinus__` to see them) offer five
ready-made requests. Each starts the agent on a common job, with steps that work well in Spinus:

| Request | What you give it | What the agent does |
|---|---|---|
| **Start a song** | A description: genre, mood, tempo, instruments | Sets tempo and key, marks the sections, adds and writes the parts, checks and renders |
| **Make a beat** | The groove, and where it plays | Writes a drum loop with a fill at the end of each section |
| **Add a section** | What to add, and where | Makes room for it, fills it, builds into what follows |
| **Check and fix the mix** | A goal, if you have one | Measures, fixes one thing at a time, measures again, tells you the numbers before and after |
| **Shape a sound** | The part, and how it should sound | Changes its settings, a preset or adds an effect |

A plugin with hundreds of settings (a sampled guitar, a big synth) is read 80 settings at a time: the agent pages
through them or looks them up by name ("amp gain"), so it reaches every section of the plugin, not only the
first.

## Tools that only look

Each tool tells the agent's client what it does: **only looks** (the song's summary, a part's notes, a sound's
settings, the device and preset lists, the manual, your references' style, check, render), **adds** (a part, an effect, a send, bars) or **may
replace** what is there (writing notes over a part's, copying bars, automation, a new sound, Spinus's
commands). Clients that use this can run the ones that only look without asking you each time and ask before
the rest. Every change is still one undo step.

## While an agent works

- **Changes apply at once,** with no proposal to accept, because an agent works in many small steps. Each change is one
  undo step, named "Agent: ...", so **Ctrl+Z** takes back its last one.
- **The top bar shows "Agent: <name>"** while an agent is connected. Click it for:
  - **Undo the agent's changes:** the song goes back to how it was when the agent connected (your own changes
    since then too).
  - **Disconnect:** the agent's session ends, and a new starting point for **Undo the agent's changes** begins.
    This doesn't lock the agent out: most agents connect again by themselves the next time they do something.
    To stop an agent from working on the song, untick **Let coding agents work on the open project**, or click
    **New token** (agents set up with the old token can't connect any more).
- **More than one agent** can be connected at once (two Claude Code windows, or Claude Code and Cursor). The top
  bar names them all. Each is told about the others' changes as it is told about yours. **Undo the agent's
  changes** goes back to when the first of them connected.
- **You can keep working on the song.** The agent is told what you changed (or undid) since it last looked. If
  you changed something, its next change isn't made: it's told what you did, so it can check its change still
  fits, and then makes it.
- **An agent's change while you drag a note** in the Piano Roll: both stay (your note moves, the agent's notes
  stay). If the agent removed or changed the very note you're holding, your drag isn't made, and the hint bar
  says so: drag it again on the notes as they are now.
- **Opening another project or starting a new one** (File › Open, File › New) while an agent is connected: the
  agent stays connected and is told that the song changed, before its next change. **Undo the agent's changes**
  then only goes back to how the new project was when you opened it.
- **It can play a part of the song for you:** "play the new chorus" plays that section, looped, in Spinus. Use
  "Chorus 2" for the second section named Chorus. The agent doesn't hear it; you do. Press Stop as usual, or ask
  the agent to stop. Playing doesn't change the song, but it switches Spinus to song mode and sets the loop
  region to the section, and it doesn't put them back afterwards.
- Rendering to listen takes as long as the song does to render. The agent sees the numbers; you hear the song in
  Spinus as usual.
- **It can save the project, start a new one, and open one of yours:** "save it", "start a new song", "open
  Summer Demo". It opens only projects listed in the Browser's **Projects** (the folders of projects you added).
  A new project it saves goes into the first of those folders, under the name it gives, and shows up there. It
  can also save into another of those folders, or into a new folder inside one ("save it as Cue 1 in Ashen
  Vigil" makes the folder Ashen Vigil for an album's songs), but never anywhere else. It never throws away
  unsaved work: while the open project has unsaved changes, it can't open another project or start a new one
  until the project is saved. Opening and starting aren't undo steps, like File › Open and
  File › New.

## What an agent can't do

- **Load a Drum Machine kit or a Sampler's samples.** Load them in the device's window, then let the agent work
  with them. A preset that is a kit (such as "Magpie") can't be loaded by an agent either.
- **Load presets on Spinus's own devices other than 3x Osc.** They have none: the agent changes their settings
  one by one instead.
- **Change Spinus's settings** or use files outside the places listed under Safety.

## Safety

- **On this computer only:** the interface listens on this computer alone (127.0.0.1). Other computers can't
  reach it.
- **The token:** every request needs it. **New token** makes a new one; agents set up with the old one can't
  connect any more. Keep the token to yourself, like a password.
- **No web pages:** a web page in your browser can't use it.
- **Files:** an agent can only use files in the project's folder, your sample folders (Browser › Samples) and
  Spinus's starter pack. It can't change Spinus's settings.
- **Turning it off** (untick it) disconnects every agent at once, and none can connect until you turn it on
  again.

If you'd rather agents never touch the open project, leave it off: they can still make songs of their own with
[Spinus from the command line](command-line.md).
