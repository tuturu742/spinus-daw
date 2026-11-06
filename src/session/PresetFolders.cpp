#include "PresetFolders.h"

namespace spinus
{

PresetFolders::PresetFolders (const juce::File& indexFolder) : file (fileIn (indexFolder)) {}

void PresetFolders::load()
{
    folders.clear();
    const auto saved = juce::JSON::parse (file);
    if (auto* instruments = saved["folders"].getDynamicObject())
        for (const auto& [instrument, list] : instruments->getProperties())
            if (auto* paths = list.getArray())
                for (const auto& path : *paths)
                    if (juce::File::isAbsolutePath (path.toString()))
                        folders[instrument.toString()].addIfNotAlreadyThere (juce::File (path.toString()));
}

bool PresetFolders::save() const
{
    auto* all = new juce::DynamicObject();
    for (const auto& [instrument, list] : folders)
    {
        juce::Array<juce::var> paths;
        for (const auto& folder : list)
            paths.add (folder.getFullPathName());
        all->setProperty (instrument, paths);
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 1);
    root->setProperty ("folders", juce::var (all));
    file.getParentDirectory().createDirectory();
    return file.replaceWithText (juce::JSON::toString (juce::var (root)) + "\n");
}

juce::Array<juce::File> PresetFolders::get (const juce::String& instrument) const
{
    if (const auto found = folders.find (instrument); found != folders.end())
        return found->second;
    return {};
}

bool PresetFolders::add (const juce::String& instrument, const juce::File& folder)
{
    if (instrument.isEmpty() || ! folder.isDirectory())
        return false;
    folders[instrument].addIfNotAlreadyThere (folder);
    return true;
}

void PresetFolders::remove (const juce::String& instrument, const juce::File& folder)
{
    if (const auto found = folders.find (instrument); found != folders.end())
    {
        found->second.removeAllInstancesOf (folder);
        if (found->second.isEmpty())
            folders.erase (found);
    }
}

} // namespace spinus
