#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <set>

namespace spinus::ui
{

/** The Spinus logo, a siskin with a knob on its wing, drawn from packaging/share/spinus.svg (the same file
    the desktop uses as the app's icon). */
std::unique_ptr<juce::Drawable> createLogo();

/** The logo as a square picture of the given size, for the window's icon. */
juce::Image logoImage (int size);

/** Gives every Spinus window the logo as its icon (the taskbar and window switcher show it): the main
    window, panels in their own window, dialogs and plugin windows. Without it, a window that is not the
    main one showed the window system's own logo. Keep one while the app runs. */
class WindowIcons final : private juce::FocusChangeListener, private juce::Timer
{
public:
    WindowIcons();
    ~WindowIcons() override;

    /** Sets the icon on windows that do not have it yet. */
    void apply();

private:
    void globalFocusChanged (juce::Component*) override { apply(); }
    void timerCallback() override { apply(); }

    juce::Image icon;
    std::set<juce::ComponentPeer*> done;
};

} // namespace spinus::ui
