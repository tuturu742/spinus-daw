#pragma once

#include "Session.h"

namespace spinus
{

/** Chooses one of 3x Osc's built-in presets for its device, as its window's preset menu does: the
    command device.loadPreset (one undo step). Controls turned in the window since a preset was loaded last are
    then set to the preset too: they changed the plugin, not the project, so the command alone would not move
    them back when the preset sets them to the value it set before. */
juce::Result loadBuiltInPreset (Session&, model::DeviceId, const juce::String& preset);

/** Lets a 3x Osc's window choose presets through loadBuiltInPreset (if the plugin is a 3x Osc). */
void connectBuiltInPresets (Session&, tracktion::Plugin&);

} // namespace spinus
