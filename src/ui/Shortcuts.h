#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** Spinus's keyboard shortcuts in every window of its own: the main window has them, and so do the
    windows that open beside it (a panel detached into its own window, a plugin's window, Spinus Tune), so
    Ctrl+S or Ctrl+Alt+1 work whichever of them is in front. A key a window's own controls don't use goes to
    the main window's commands. In a text field, Ctrl with a digit or a sign (Ctrl+Alt+1, Ctrl+=) is a shortcut
    too, not typed. */
namespace shortcuts
{
/** The main window's commands, until `forget` (when it closes; a newer main window's stay). */
void setCommandManager (juce::ApplicationCommandManager&);
void forget (juce::ApplicationCommandManager&);

/** Gives `window` the shortcuts (call once it exists, before it is shown); `removeFrom` before it goes. */
void addTo (juce::Component& window);
void removeFrom (juce::Component& window);

/** What addTo adds to a window, and what a focused text field gets (tests). */
juce::KeyListener& getWindowKeys();
juce::KeyListener& getTextFieldKeys();
} // namespace shortcuts

} // namespace spinus::ui
