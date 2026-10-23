#pragma once

#include <tracktion_engine/tracktion_engine.h>

namespace spinus
{

class PipeWireInput;

/** Owns the single tracktion::Engine instance for the process.

    The Engine is the playback backend only. The Spinus Model is the source of
    truth; the Engine Adapter will compile it into a Tracktion Edit.
*/
class EngineHost
{
public:
    struct Options
    {
        /** When false, no audio device is opened (headless tests, render server). */
        bool openAudioDevice = true;
        /** spinus-plugin-host, for plugins that run in a separate process; by default the one
            next to the running program. */
        juce::File pluginHost;
    };

    explicit EngineHost (const Options&);

    /** Tracktion keeps settings (audio device, its plugin list) in ~/.config/<name>/Settings.xml, by
        default the app's own. Tests call this once with another name so they never touch the user's
        settings. Affects EngineHosts created afterwards. */
    static void setSettingsName (const juce::String&);
    ~EngineHost();

    tracktion::Engine& getEngine() noexcept { return *engine; }

    /** A one-line, human-readable description of the current audio device. */
    juce::String describeAudioDevice() const;

    /** Makes plugin windows: called when Tracktion opens a plugin's window (Plugin::showWindowExplicitly).
        Without one, plugin windows do not open (headless use). The engine must come from an EngineHost. */
    using PluginWindowFactory = std::function<std::unique_ptr<juce::Component> (tracktion::Plugin&)>;
    static void setPluginWindowFactory (tracktion::Engine&, PluginWindowFactory);

    /** On the very first start: what AudioSetup::applyDefaults() chose (empty otherwise). */
    const juce::String& getFirstStartAudioChoice() const noexcept { return firstStartAudioChoice; }

    /** The microphone chosen by name when recording through PipeWire; nullptr without an audio device (or
        for an engine not made by an EngineHost). */
    static PipeWireInput* getPipeWireInput (tracktion::Engine&);

private:
    std::unique_ptr<tracktion::Engine> engine;
    juce::String firstStartAudioChoice;
    std::unique_ptr<PipeWireInput> pipeWireInput;

    JUCE_DECLARE_NON_COPYABLE (EngineHost)
};

} // namespace spinus
