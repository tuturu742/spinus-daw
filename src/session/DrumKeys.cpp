#include "DrumKeys.h"

#include "SoundIndex.h"

#include <optional>
#include <set>

namespace spinus::drumkeys
{
namespace
{
struct Drum
{
    const char* name;
    int pad, generalMidi;
    juce::StringArray fits; // the sound index's classes that are this drum, the first the drum itself
};

const std::vector<Drum>& drums()
{
    static const std::vector<Drum> list {
        { "kick", 60, 36, { "kick" } },
        { "snare", 61, 38, { "snare", "clap" } },
        { "clap", 62, 39, { "clap", "snare" } },
        { "closed_hat", 63, 42, { "hat" } },
        { "open_hat", 64, 46, { "hat", "cymbal" } },
        // A low tom rings long and deep: measured, it can read as a bass note.
        { "low_tom", 65, 45, { "tom", "bass", "perc" } },
        { "mid_tom", 66, 47, { "tom", "perc" } },
        { "high_tom", 67, 50, { "tom", "perc" } },
        { "rim", 68, 37, { "perc", "snare", "clap" } },
        { "crash", 70, 49, { "cymbal" } },
        { "ride", 74, 51, { "cymbal", "hat" } },
    };
    return list;
}
} // namespace

std::map<juce::String, int> fromMap (const juce::var& drumMap)
{
    std::map<int, juce::String> classOf;
    std::vector<std::pair<int, juce::String>> inOrder;
    if (const auto* list = drumMap.getArray())
        for (const auto& entry : *list)
        {
            const auto note = (int)entry["note"];
            classOf[note] = entry["class"].toString();
            inOrder.emplace_back (note, classOf[note]);
        }
    std::map<juce::String, int> keys;
    std::set<int> taken;
    for (const auto& drum : drums())
    {
        auto key = drum.generalMidi;
        const auto there = classOf.find (drum.generalMidi);
        if (! classOf.empty() && (there == classOf.end() || ! drum.fits.contains (there->second)))
        {
            // Its own class first (a snare's), then those like it; a key no drum before took first (a kit's
            // second hat key for the open hat, its toms in turn), else the same key again.
            std::optional<int> found;
            for (const auto unused : { true, false })
                for (const auto& cls : drum.fits)
                    for (const auto& [note, c] : inOrder)
                        if (! found && c == cls && (! unused || ! taken.contains (note)))
                            found = note;
            key = found.value_or (drum.generalMidi);
        }
        keys[drum.name] = key;
        taken.insert (key);
    }
    return keys;
}

juce::Array<juce::var> measuredKits (const juce::File& indexFolder, const juce::String& key)
{
    const auto file = SoundIndex (indexFolder).instrumentFile (key);
    struct Kept
    {
        juce::Time modified;
        juce::int64 size = 0;
        juce::Array<juce::var> kits;
    };
    static std::map<juce::String, Kept> kept;
    if (! file.existsAsFile())
        return {};
    auto& entry = kept[file.getFullPathName()];
    if (entry.modified == file.getLastModificationTime() && entry.size == file.getSize())
        return entry.kits;
    entry = { file.getLastModificationTime(), file.getSize(), {} };
    const auto parsed = juce::JSON::parse (file);
    if (const auto* sounds = parsed["sounds"].getArray())
        for (const auto& sound : *sounds)
        {
            const auto* map = sound["drumMap"].getArray();
            if (map == nullptr || sound.hasProperty ("problem"))
                continue;
            juce::StringArray classes;
            for (const auto& e : *map)
                classes.add (e["class"].toString());
            if (classes.contains ("kick") && (classes.contains ("snare") || classes.contains ("clap")) &&
                classes.contains ("hat"))
                entry.kits.add (sound);
        }
    return entry.kits;
}

juce::String drumOfPad (int pad, const std::map<int, juce::String>& extra)
{
    if (const auto found = extra.find (pad); found != extra.end())
        return found->second;
    for (const auto& drum : drums())
        if (drum.pad == pad)
            return drum.name;
    return {};
}

} // namespace spinus::drumkeys
