#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::plugins
{

/** Hosts CLAP plugins through JUCE's plugin-format interface.

    JUCE 9 and Tracktion Engine cannot host CLAP, so Spinus provides its own format. Registering it
    with a juce::AudioPluginFormatManager (e.g. Tracktion's PluginManager) makes CLAP plugins
    scannable and loadable like VST3/LV2 plugins.

    Supported: scanning, instruments and effects (main audio ports; extra ports such as sidechains
    get silent inputs and discarded outputs), note and MIDI input,
    parameters (with automation in both directions), state save/restore, latency reporting,
    transport info, the thread-check, log, params, state, latency and audio-ports host extensions.

    Plugin windows: embedded X11 windows (clap.gui), with the timer and POSIX fd host extensions they
    need on Linux.

    Per-note values arrive as CLAP note expressions. Presets in the plugin's own file
    format load through clap.preset-load, and parameter renames (e.g. Surge XT's macros) are followed.
    A plugin's presets can be listed through its preset-discovery providers (for closest preset).

    Not yet supported: routing audio to/from non-main ports, watching preset folders for changes, and
    parameter-list changes after creation.
*/
class ClapPluginFormat : public juce::AudioPluginFormat
{
public:
    static constexpr const char* formatName = "CLAP";

    ClapPluginFormat() = default;

    juce::String getName() const override { return formatName; }

    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&,
                              const juce::String& fileOrIdentifier) override;
    bool fileMightContainThisPluginType (const juce::String& fileOrIdentifier) override;
    juce::String getNameOfPluginFromIdentifier (const juce::String& fileOrIdentifier) override;
    bool pluginNeedsRescanning (const juce::PluginDescription&) override;
    bool doesPluginStillExist (const juce::PluginDescription&) override;
    bool canScanForPlugins() const override { return true; }
    bool isTrivialToScan() const override { return false; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool recursive, bool allowAsync) override;
    juce::FileSearchPath getDefaultLocationsToSearch() override;
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }

    /** True if the instance is a CLAP plugin that can load preset files (clap.preset-load). */
    static bool canLoadPresets (const juce::AudioPluginInstance&);

    /** Loads a preset file in the plugin's own format into a CLAP plugin instance (message thread). */
    static juce::Result loadPreset (juce::AudioPluginInstance&, const juce::File&);

    /** A preset as the plugin's preset-discovery providers describe it (clap.preset-discovery-factory). */
    struct DiscoveredPreset
    {
        static constexpr int inFile = 0, inPlugin = 1; // CLAP_PRESET_DISCOVERY_LOCATION_FILE / _PLUGIN

        juce::String name;
        juce::String category; // the folder inside the location, or the location's name
        int kind = inFile;
        juce::String location; // the file (inFile)
        juce::String loadKey;  // which preset inside the file or the plugin; may be empty
        bool factory = false;  // the plugin's factory content
    };

    /** The presets a CLAP plugin instance lists for itself through preset discovery, at most `most`, in
        the order its providers give them (files by path). Empty for a plugin without preset discovery or
        preset-load, and for other formats. Message thread; it reads the preset files. */
    static std::vector<DiscoveredPreset> discoverPresets (juce::AudioPluginInstance&, int most = 2000);

    /** Loads a discovered preset (clap.preset-load from_location; message thread). */
    static juce::Result loadPreset (juce::AudioPluginInstance&, const DiscoveredPreset&);

    /** The paths of the CLAP files loaded in this process now (by any instance, of any edit, hidden ones
        too), e.g. so a plugin is not deleted while it runs. */
    static juce::StringArray getLoadedFiles();

    /** True if `editor` is a CLAP plugin window that has been embedded and shown (tests, diagnostics). */
    static bool isEditorAttached (const juce::AudioProcessorEditor* editor);

protected:
    void createPluginInstance (const juce::PluginDescription&, double initialSampleRate, int initialBufferSize,
                               PluginCreationCallback) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClapPluginFormat)
};

} // namespace spinus::plugins
