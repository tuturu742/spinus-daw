#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** Asks the window manager to keep `window` above `owner`'s window (and minimise with it), without
    putting it above other applications: X11's WM_TRANSIENT_FOR, as used for dialogs. Both must be on
    the desktop. Does nothing on other systems or when there is no window yet.

    Not Component::setAlwaysOnTop(): on Linux JUCE implements that by destroying and recreating the
    window, which destroys any plugin window embedded in it. */
void keepAboveOwner (juce::Component& window, juce::Component& owner);

} // namespace spinus::ui
