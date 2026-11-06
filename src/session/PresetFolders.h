#pragma once

#include <juce_core/juce_core.h>

#include <map>

namespace spinus
{

/** Folders of an instrument's own preset files that the user chose, by the sound index's instrument key. Many
    instruments keep their presets as files in a folder of their own that no host can list, and some of those files
    are the instrument's saved state itself (Ample Sound's guitars and basses): given to the instrument as its
    state, such a file is the preset. Spinus does not know where each maker keeps them, so the user points to the
    folder once (the instrument's menu in the Track creator's parts list); InstrumentPresets then lists every file
    there as one of the instrument's presets, and the sound index measures them like any other. Kept beside the
    index's folder (`sound-preset-folders.json` next to it), like the user's roles and note ranges, so they survive a
    new scan. Plain data: any thread but the audio thread, one at a time. */
class PresetFolders
{
public:
    explicit PresetFolders (const juce::File& indexFolder);
    static juce::File fileIn (const juce::File& indexFolder)
    {
        return indexFolder.getSiblingFile ("sound-preset-folders.json");
    }

    void load();
    bool save() const;

    /** The instrument's folders, in the order they were added. */
    juce::Array<juce::File> get (const juce::String& instrument) const;
    /** Adds a folder (once); false when it is not a folder. Not saved until save(). */
    bool add (const juce::String& instrument, const juce::File& folder);
    void remove (const juce::String& instrument, const juce::File& folder);

private:
    juce::File file;
    std::map<juce::String, juce::Array<juce::File>> folders;
};

} // namespace spinus
