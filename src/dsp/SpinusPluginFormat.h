#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

/** Spinus's own devices (Sampler; later Drum Machine and 3x Osc) as a plugin format, so Tracktion
    hosts them like any plugin: state, windows, parameters and automation all work the same way.
    Identifiers look like "spinus:sampler". */
class SpinusPluginFormat final : public juce::AudioPluginFormat
{
public:
    static constexpr const char* formatName = "Spinus";

    /** Every device the format offers, described. */
    static juce::Array<juce::PluginDescription> getDescriptions();
    static std::optional<juce::PluginDescription> findDescription (const juce::String& identifier);

    /** A new instance without going through a format manager (tests, the standalone plugin). */
    static std::unique_ptr<juce::AudioPluginInstance> create (const juce::String& identifier);

    juce::String getName() const override { return formatName; }
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String& identifier) override;
    bool fileMightContainThisPluginType (const juce::String& identifier) override
    {
        return identifier.startsWith ("spinus:");
    }
    juce::String getNameOfPluginFromIdentifier (const juce::String&) override;
    bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
    bool doesPluginStillExist (const juce::PluginDescription& d) override
    {
        return findDescription (d.fileOrIdentifier).has_value();
    }
    bool canScanForPlugins() const override { return false; }
    bool isTrivialToScan() const override { return true; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override { return {}; }
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }

protected:
    void createPluginInstance (const juce::PluginDescription&, double initialSampleRate, int initialBufferSize,
                               PluginCreationCallback) override;
};

} // namespace spinus::dsp
