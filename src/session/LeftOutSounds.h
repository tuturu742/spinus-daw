#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace spinus
{

/** The sounds the user took out of the sound index (Options › Sound index › Leave out): an instrument's presets
    (by the index's instrument key and the preset's id) and sample files (by path). An update measures none of
    them again and drops them from the index, so the Track creator and the Analyzer never choose them. Kept
    beside the index's folder (`sound-left-out.json` next to it, like UserRoles), so they stay left out after the
    index is deleted. Plain data: any thread but the audio thread, one at a time. */
class LeftOutSounds
{
public:
    explicit LeftOutSounds (const juce::File& indexFolder);
    static juce::File fileIn (const juce::File& indexFolder)
    {
        return indexFolder.getSiblingFile ("sound-left-out.json");
    }

    struct Entry
    {
        juce::String instrument, preset; // a preset ("" preset: the instrument's sound as it opens)
        juce::String sample;             // or a sample file
        juce::String name, source;       // as the Sounds tab showed it
        bool isSample() const { return sample.isNotEmpty(); }
        bool sameSound (const Entry& o) const
        {
            return isSample() ? sample == o.sample : ! o.isSample() && instrument == o.instrument && preset == o.preset;
        }
    };

    void load();
    bool save() const;

    bool isLeftOut (const juce::String& instrument, const juce::String& preset) const;
    bool isSampleLeftOut (const juce::String& path) const;
    /** The left-out presets of one instrument. */
    juce::StringArray presetsOf (const juce::String& instrument) const;
    /** Adds one (once). Not saved until save(). */
    void add (const Entry&);
    /** Takes one back; false when it was not left out. Not saved until save(). */
    bool remove (const Entry&);
    const std::vector<Entry>& getEntries() const noexcept { return entries; }

private:
    juce::File file;
    std::vector<Entry> entries;
};

} // namespace spinus
