#pragma once

#include "session/Session.h"

namespace spinus::ui
{

/** A device's own window: the plugin's editor, or JUCE's generic editor (a slider per parameter) for
    plugins without one. Opened from the Channel Rack and the Mixer through Session::showDeviceWindow().

    Tracktion owns the window (Plugin::windowState) and closes it when the plugin is removed, so a
    window never outlives its plugin. It stays above Spinus's main window (not above other
    applications), like FL Studio's plugin windows, and reopens where it was last closed.
*/
class PluginWindow final : public juce::DocumentWindow
{
public:
    /** owner: the main window's content, which the window stays above (nullptr: no owner). */
    PluginWindow (tracktion::Plugin&, const juce::String& title, juce::Component* owner = nullptr);
    ~PluginWindow() override;

    /** Makes Tracktion open PluginWindows for this session's devices; uninstall before the session goes. */
    static void install (Session&, juce::Component* owner = nullptr);
    static void uninstall (Session&);

    bool hasEditor() const noexcept { return editor != nullptr; }

private:
    void closeButtonPressed() override;
    void moved() override;

    tracktion::Plugin& plugin;
    std::unique_ptr<tracktion::Plugin::EditorComponent> editor;
    bool trackBounds = false;
};

} // namespace spinus::ui
