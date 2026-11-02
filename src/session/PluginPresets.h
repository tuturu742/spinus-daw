#pragma once

#include "session/InstrumentPresets.h"

#include <vector>

namespace spinus
{
class Session;

/** A plugin's presets for coding agents and scripts (device.loadPreset on a plugin, spinus --presets, the live
    interface's list_presets): the ones InstrumentPresets finds (its programs, the presets a CLAP plugin lists,
    Surge XT's patches, OB-Xf's and Odin 2's) and the sounds the user saved for it with Save this sound. Found on a
    copy of the plugin made for it, out of the project, so the one playing does not change. Message thread. */
namespace pluginPresets
{
/** Every preset of the device's plugin, its own first; none (and `problem` says why) when it can't be loaded. */
std::vector<InstrumentPreset> find (Session&, const model::Device&, juce::String& problem);

/** The preset `said` names: by its name (any case), as "Category/Name", else the only one whose name holds all the
    words. None when no preset or more than one fits. */
const InstrumentPreset* match (const std::vector<InstrumentPreset>&, const juce::String& said);

/** The plugin's state with the preset loaded, and the preset's name: commands::PresetLoader. */
juce::Result stateWith (Session&, const model::Device&, const juce::String& preset, juce::MemoryBlock& state,
                        juce::String& name);
} // namespace pluginPresets

} // namespace spinus
