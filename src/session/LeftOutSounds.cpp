#include "LeftOutSounds.h"

#include <algorithm>

namespace spinus
{

LeftOutSounds::LeftOutSounds (const juce::File& indexFolder) : file (fileIn (indexFolder)) {}

void LeftOutSounds::load()
{
    entries.clear();
    const auto saved = juce::JSON::parse (file);
    if (const auto* list = saved["sounds"].getArray())
        for (const auto& s : *list)
        {
            Entry e { s["instrument"].toString(), s["preset"].toString(), s["sample"].toString(), s["name"].toString(),
                      s["source"].toString() };
            if (e.isSample() || e.instrument.isNotEmpty())
                add (e);
        }
}

bool LeftOutSounds::save() const
{
    juce::Array<juce::var> list;
    for (const auto& e : entries)
    {
        auto* o = new juce::DynamicObject();
        if (e.isSample())
            o->setProperty ("sample", e.sample);
        else
        {
            o->setProperty ("instrument", e.instrument);
            o->setProperty ("preset", e.preset);
        }
        o->setProperty ("name", e.name);
        o->setProperty ("source", e.source);
        list.add (juce::var (o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 1);
    root->setProperty ("sounds", list);
    file.getParentDirectory().createDirectory();
    return file.replaceWithText (juce::JSON::toString (juce::var (root)) + "\n");
}

bool LeftOutSounds::isLeftOut (const juce::String& instrument, const juce::String& preset) const
{
    return std::any_of (entries.begin(), entries.end(), [&] (const Entry& e)
                        { return ! e.isSample() && e.instrument == instrument && e.preset == preset; });
}

bool LeftOutSounds::isSampleLeftOut (const juce::String& path) const
{
    return std::any_of (entries.begin(), entries.end(), [&] (const Entry& e) { return e.sample == path; });
}

juce::StringArray LeftOutSounds::presetsOf (const juce::String& instrument) const
{
    juce::StringArray presets;
    for (const auto& e : entries)
        if (! e.isSample() && e.instrument == instrument)
            presets.add (e.preset);
    return presets;
}

void LeftOutSounds::add (const Entry& entry)
{
    if (std::none_of (entries.begin(), entries.end(), [&] (const Entry& e) { return e.sameSound (entry); }))
        entries.push_back (entry);
}

bool LeftOutSounds::remove (const Entry& entry)
{
    return std::erase_if (entries, [&] (const Entry& e) { return e.sameSound (entry); }) > 0;
}

} // namespace spinus
