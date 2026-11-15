#pragma once

#include "model/Model.h"

#include <juce_core/juce_core.h>

namespace spinus
{
class Session;
}

namespace spinus::ai
{

/** What the AI service is told about the open project: for the assistant (ai.v1.assistant.ask's context) and for
    the live interface's project tools (ai.v1.project.tool), so both read the same thing.

    `project` (project.json), `selection`, `chips`, `deviceNames` (devices as Spinus shows them), `devices` (what
    can be added, each with the device JSON the commands take) and `deviceParameters`: for each device in the
    project, its settings with the values they show now ({id, name, text}), so a tool can move "Decay" from
    "400 ms" to "180 ms" on any instrument. Message thread only. */
struct ContextOptions
{
    bool project = true; // the Project chip: without it, only the selection and the devices
    juce::StringArray chips { "project", "channel", "pattern", "insert" };
    model::ChannelId channel;
    model::PatternId pattern;
    model::InsertId insert;
    /** Each free or Spinus effect's `modes`: its settings that pick its kind by name ({"FX Type": ["Delay",
        "Chorus"...]}), read from a copy of it the first time (Session::getDeviceChoices). */
    bool deviceChoices = true;
    /** Where the instrument profiles are (the sound index's folder; SoundIndex::getDefaultFolder() when empty):
        `instrumentKeys` gives, by device id, what each learned instrument's keys do, and each instrument that can
        be added has its `keys` (InstrumentProfiles::describeForAgents). The effect profiles beside them (Learn this
        effect) give `effectGoodFor` by device id, `goodFor` on each effect that can be added, and on a learned
        effect's settings in `deviceParameters` what each does, its safe range, whether it matters and the
        plugin's own range. */
    juce::File profilesFolder;
};

juce::var projectContext (Session&, const ContextOptions&);

} // namespace spinus::ai
