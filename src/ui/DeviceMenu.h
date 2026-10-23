#pragma once

#include "plugins/DeviceList.h"
#include "session/Session.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** Adds the instruments (or effects) of the device list to a menu: Spinus's own, then the bundled
    open-source ones with their project, e.g. "Hall Reverb  (Dragonfly Reverb)", then the installed
    plugins in a submenu per maker, each with its format, e.g. "Vital  (VST3)". */
void addDeviceItems (juce::PopupMenu&, const plugins::DeviceList&, bool instruments,
                     const std::function<void (const plugins::DeviceList::Entry&)>& onChoose);

/** "Create automation clip": adds the clip (Session::createAutomationClip) and opens the Playlist, where
    it appears. Returns false if the target cannot be automated. */
bool createAutomationClip (juce::Component& from, Session&, const model::ParameterTarget&);

/** A submenu of a device's parameters (in groups of 40 for big plugins); choosing one creates an
    automation clip for it. */
juce::PopupMenu makeAutomateParameterMenu (juce::Component& from, Session&, model::DeviceId);

} // namespace spinus::ui
