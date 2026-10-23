#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace spinus::plugins
{

/** Surge XT's presets (patches, .fxp files), for closest preset: the factory and third-party
    patches that come with Surge XT's data, and the user's own. Surge XT loads them itself through
    clap.preset-load; Spinus only lists them. Users pick presets in Surge XT's own browser. */
struct SurgePreset
{
    enum class Source
    {
        factory,
        thirdParty,
        user
    };

    juce::String name;     // the file name without ".fxp"
    juce::String category; // the folder inside the source, e.g. "Bass" or "Jacky Ligon/Leads"
    Source source = Source::factory;
    juce::File file;
};

class SurgePresets
{
public:
    /** Where Surge XT's data is for this plugin file, as Surge XT looks for it: a "SurgeXTData" folder
        beside the plugin or above it, otherwise the system's share folder (the Arch package), otherwise
        ~/.local/share/surge-xt. Returns an empty file if none exists. */
    static juce::File findDataFolder (const juce::File& pluginFile);

    /** The user's own patches: Documents/Surge XT/Patches. */
    static juce::File getUserFolder();

    /** Every patch, by source, category and name. */
    static std::vector<SurgePreset> scan (const juce::File& dataFolder, const juce::File& userFolder);

    static juce::String describe (SurgePreset::Source);
};

} // namespace spinus::plugins
