#include "SurgePresets.h"

#include "session/ProgramFolder.h"

namespace spinus::plugins
{

juce::File SurgePresets::findDataFolder (const juce::File& pluginFile)
{
    for (auto folder = pluginFile.getParentDirectory(); ! folder.isRoot(); folder = folder.getParentDirectory())
        if (auto data = folder.getChildFile ("SurgeXTData"); data.isDirectory())
            return data;

#if JUCE_WINDOWS
    // Surge's own Windows installer puts its sounds in C:\ProgramData\Surge XT.
    if (auto folder =
            juce::File::getSpecialLocation (juce::File::commonApplicationDataDirectory).getChildFile ("Surge XT");
        folder.isDirectory())
        return folder;
#else
    for (auto* path : { "/usr/share/surge-xt", "/usr/local/share/surge-xt" })
        if (juce::File folder (path); folder.isDirectory())
            return folder;
#endif

    if (auto folder = getUserDataRoot().getChildFile ("surge-xt"); folder.isDirectory())
        return folder;
    return {};
}

juce::File SurgePresets::getUserFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Surge XT/Patches");
}

std::vector<SurgePreset> SurgePresets::scan (const juce::File& dataFolder, const juce::File& userFolder)
{
    std::vector<SurgePreset> result;
    auto add = [&result] (const juce::File& root, SurgePreset::Source source)
    {
        if (! root.isDirectory())
            return;
        std::vector<SurgePreset> found;
        for (const auto& entry : juce::RangedDirectoryIterator (root, true, "*.fxp", juce::File::findFiles))
        {
            const auto& file = entry.getFile();
            const auto folder = file.getParentDirectory();
            found.push_back ({ file.getFileNameWithoutExtension(),
                               folder == root ? juce::String() : folder.getRelativePathFrom (root).replace ("\\", "/"),
                               source, file });
        }
        std::sort (found.begin(), found.end(),
                   [] (const SurgePreset& a, const SurgePreset& b)
                   {
                       if (a.category != b.category)
                           return a.category.compareNatural (b.category) < 0;
                       return a.name.compareNatural (b.name) < 0;
                   });
        result.insert (result.end(), found.begin(), found.end());
    };

    if (dataFolder != juce::File())
    {
        add (dataFolder.getChildFile ("patches_factory"), SurgePreset::Source::factory);
        add (dataFolder.getChildFile ("patches_3rdparty"), SurgePreset::Source::thirdParty);
    }
    add (userFolder, SurgePreset::Source::user);
    return result;
}

juce::String SurgePresets::describe (SurgePreset::Source source)
{
    switch (source)
    {
    case SurgePreset::Source::factory:
        return "Factory";
    case SurgePreset::Source::thirdParty:
        return "Third party";
    case SurgePreset::Source::user:
        return "Yours";
    }
    return {};
}

} // namespace spinus::plugins
