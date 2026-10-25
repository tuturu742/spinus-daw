#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::plugins::sandbox
{

/** Plugins run in a separate process: a plugin format whose instances forward everything to
    spinus-plugin-host, which runs the real plugin. A crash or hang there cannot take Spinus down: the
    instance then passes its input through (an effect) or stays silent (an instrument), and says so.

    A sandboxed plugin's description is the real one with this format's name, and the real format and
    file in `fileOrIdentifier` ("CLAP:/path/to/plugin.clap"); describe() makes one. */
class SandboxPluginFormat final : public juce::AudioPluginFormat
{
public:
    static constexpr const char* formatName = "Spinus Sandbox";

    /** `host` is spinus-plugin-host; `real` finds the real plugin's description (the engine's known
        plugins). */
    SandboxPluginFormat (juce::File host,
                         std::function<std::optional<juce::PluginDescription> (const juce::String& format,
                                                                               const juce::String& file, int uniqueId)>
                             real);

    /** In the Flatpak: `launcher` (spinus-plugin-host-system) starts spinus-plugin-host with the computer's own
        libraries, for the plugins `withHostLibraries` says need them (plugins::HostLibraries). */
    void setHostLibrariesLauncher (juce::File launcher,
                                   std::function<bool (const juce::PluginDescription&)> withHostLibraries)
    {
        hostLibrariesLauncher = std::move (launcher);
        needsHostLibraries = std::move (withHostLibraries);
    }

    /** The sandboxed twin of a plugin's description. */
    static juce::PluginDescription describe (const juce::PluginDescription& real);
    /** The real plugin's format and file, from a sandboxed description. */
    static std::pair<juce::String, juce::String> realFormatAndFile (const juce::PluginDescription&);

    juce::String getName() const override { return formatName; }
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String&) override {}
    bool fileMightContainThisPluginType (const juce::String& file) override { return file.contains (":"); }
    juce::String getNameOfPluginFromIdentifier (const juce::String& id) override { return id; }
    bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
    bool doesPluginStillExist (const juce::PluginDescription&) override { return true; }
    bool canScanForPlugins() const override { return false; }
    bool isTrivialToScan() const override { return true; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override { return {}; }
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }

private:
    void createPluginInstance (const juce::PluginDescription&, double rate, int block, PluginCreationCallback) override;

    juce::File hostExecutable, hostLibrariesLauncher;
    std::function<bool (const juce::PluginDescription&)> needsHostLibraries;
    std::function<std::optional<juce::PluginDescription> (const juce::String&, const juce::String&, int)> findReal;
};

/** What a sandboxed plugin instance can report (found through the instance Tracktion holds). */
class SandboxedInstance
{
public:
    virtual ~SandboxedInstance() = default;

    enum class Failure
    {
        none,
        crashed, // the plugin's process went away
        hung     // it stopped answering
    };
    virtual Failure getFailure() const = 0;
};

} // namespace spinus::plugins::sandbox
