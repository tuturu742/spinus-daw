# Freeze

Heavy synths and effect chains use a lot of your computer's power (CPU). **Freeze** turns a channel or a
mixer insert into audio for a while: Spinus records what it plays, then plays that recording instead and
switches the plugins off. It sounds the same and uses almost no CPU. **Unfreeze** brings the plugins
back when you want to change their sound.

## Freeze a channel

Right-click the channel's name in the [Channel Rack](channel-rack.md) and choose **Freeze**.

- What is recorded: the instrument with all its notes, its audio clips and the automation of its settings.
- What stays live: the channel's own volume, pan and mute, and everything after it in the mixer.
- You can still write and change notes: Spinus records the channel again by itself a moment later.
  Until it is done you hear the previous recording.
- The instrument's window cannot open while it is frozen. Right-click the name and **Unfreeze** first.

## Freeze a mixer insert

Right-click the insert in the [Mixer](mixer.md) and choose **Freeze**.

- What is recorded: everything that plays into the insert (its channels, and inserts routed or sent into
  it) through its effects. For example, freeze a "Drums" insert to switch off all the drum channels and
  their effects at once.
- What stays live: the insert's fader, pan, mute, solo, output and sends.
- The channels and inserts inside a frozen insert are silent on their own; their plugins are off too.
- A [Sidechain Ducker](ducker.md) inside keeps following the kick even when the kick is not inside.
- An insert cannot be frozen when something inside it also plays somewhere else, for example a channel's
  insert that plays into it and also sends to a reverb outside. Its sound would be lost there. The menu
  says what is in the way; change the routing, or freeze something smaller.

## What you see

| Sign | Meaning |
|---|---|
| Blue snowflake by the name | Frozen: playing its recording. |
| Faint snowflake with a bar under the name | Recording (just frozen, or you changed what it plays). The bar shows how far it is. |
| Red snowflake | It could not be frozen. Right-click to see why (for example, a plugin is missing on this computer). It plays live meanwhile. |

## Good to know

- Freezing and unfreezing are each one undo step (**Ctrl+Z**).
- The project remembers what is frozen. The recordings are kept in the project's `cache/freeze` folder,
  so a frozen project opens without loading those plugins. The folder is safe to delete: Spinus records
  them again when needed (the plugins must be installed for that).
- In pattern mode the current pattern is recorded on its own. Sounds that ring past the pattern's end
  are cut when the pattern loops.
- [Exporting](export.md) always uses the plugins themselves, so exports are exact whether or not
  something is frozen.
- Changing an instrument's settings needs Unfreeze, because its plugin is switched off while frozen.
