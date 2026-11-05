#include "NoteRanges.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

namespace spinus
{

NoteRanges::NoteRanges (const juce::File& indexFolder) : file (fileIn (indexFolder)) {}

void NoteRanges::load()
{
    ranges.clear();
    const auto saved = juce::JSON::parse (file);
    if (auto* instruments = saved["ranges"].getDynamicObject())
        for (const auto& [instrument, range] : instruments->getProperties())
        {
            const auto low = juce::jlimit (0, 127, (int)range["low"]);
            const auto high = juce::jlimit (0, 127, (int)range["high"]);
            if (range.hasProperty ("low") && range.hasProperty ("high") && low <= high)
                ranges[instrument.toString()] = { low, high };
        }
}

bool NoteRanges::save() const
{
    auto* all = new juce::DynamicObject();
    for (const auto& [instrument, range] : ranges)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("low", range.low);
        o->setProperty ("high", range.high);
        all->setProperty (instrument, juce::var (o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 1);
    root->setProperty ("ranges", juce::var (all));
    file.getParentDirectory().createDirectory();
    return file.replaceWithText (juce::JSON::toString (juce::var (root)) + "\n");
}

std::optional<NoteRanges::Range> NoteRanges::get (const juce::String& instrument) const
{
    if (const auto found = ranges.find (instrument); found != ranges.end())
        return found->second;
    return std::nullopt;
}

void NoteRanges::set (const juce::String& instrument, std::optional<Range> range)
{
    if (range.has_value() && range->low <= range->high)
        ranges[instrument] = *range;
    else
        ranges.erase (instrument);
}

int NoteRanges::fit (int note, Range range)
{
    // As few octaves as it takes: below the range up, above it down.
    auto moved = note;
    while (moved < range.low)
        moved += 12;
    while (moved > range.high)
        moved -= 12;
    if (moved >= range.low && moved <= range.high)
        return moved;
    return juce::jlimit (range.low, range.high, note); // narrower than an octave, and no octave fits
}

juce::String NoteRanges::noteName (int note)
{
    return juce::MidiMessage::getMidiNoteName (note, true, true, 5); // middle C is C5, as in the Piano roll
}

juce::String NoteRanges::describe (Range range) { return noteName (range.low) + " to " + noteName (range.high); }

} // namespace spinus
