# Let a coding agent work on your project

A coding agent on your computer (Claude Code, Codex, Cursor and others that speak MCP) can work on the song open in
Spinus, while you watch and play it here:

- **Reading the song:** its sections, parts, sounds and mix.
- **Changing it:** writing parts, adding a bridge, shaping a sound, automating, mixing.
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
     `claude mcp add --transport http spinus http://127.0.0.1:47219/mcp --header "Authorization: Bearer <token>"`
   - **Codex:** add the lines to `~/.codex/config.toml`.
   - **Cursor:** put the lines in `.cursor/mcp.json`, or merge them into yours.
4. Ask your agent for something: "Look at my song in Spinus and add a bridge after the second chorus", "make the
   bass pluckier and duck it under the kick", "check the mix and fix what's muddy".

The music tools (write_part, insert_bars, shape_sound, check...) need AI turned on in Spinus (**Options › AI**).
They run in Spinus's own AI service on this computer, without any AI connection or internet. With AI off, the
agent still has Spinus's own tools: the song's summary, Spinus's commands, the devices that can be added, and
render.

## While an agent works

- **Changes apply at once,** with no proposal to accept, because an agent works in many small steps. Each change is one
  undo step, named "Agent: ...", so **Ctrl+Z** takes back its last one.
- **The top bar shows "Agent: <name>"** while an agent is connected. Click it for:
  - **Undo the agent's changes:** the song goes back to how it was when the agent connected (your own changes
    since then too).
  - **Disconnect:** the agent's session ends. It has to connect again to go on.
- Rendering to listen takes as long as the song does to render. The agent sees the numbers; you hear the song in
  Spinus as usual.

## Safety

- **On this computer only:** the interface listens on this computer alone (127.0.0.1). Other computers can't
  reach it.
- **The token:** every request needs it. **New token** makes a new one; agents set up with the old one can't
  connect any more. Keep the token to yourself, like a password.
- **No web pages:** a web page in your browser can't use it.
- **Files:** an agent can only use files in the project's folder, your sample folders (Browser › Samples) and
  Spinus's starter pack. It can't change Spinus's settings.
- **Turning it off** (untick it) disconnects the agent at once.

If you'd rather agents never touch the open project, leave it off: they can still make songs of their own with
[Spinus from the command line](command-line.md).
