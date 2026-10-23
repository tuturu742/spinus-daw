#include "SampleSet.h"

#include <algorithm>
#include <map>

namespace spinus::dsp
{

void SampleZone::clampPoints() noexcept
{
    const auto length = getLength();
    start = juce::jlimit<juce::int64> (0, juce::jmax<juce::int64> (0, length - 1), start);
    end = end <= 0 ? length : juce::jlimit<juce::int64> (start + 1, juce::jmax<juce::int64> (start + 1, length), end);
    loopStart = juce::jlimit (start, juce::jmax (start, end - 1), loopStart);
    loopEnd = loopEnd <= loopStart ? end : juce::jlimit (loopStart + 1, end, loopEnd);
    rootNote = juce::jlimit (0, 127, rootNote);
    lowKey = juce::jlimit (0, 127, lowKey);
    highKey = juce::jlimit (lowKey, 127, highKey);
    lowVelocity = juce::jlimit (1, 127, lowVelocity);
    highVelocity = juce::jlimit (lowVelocity, 127, highVelocity);
}

const SampleZone* SampleSet::pick (int note, int velocity, uint32_t counter) const noexcept
{
    // Count the matching zones of the first matching range, then take the counter-th of them.
    const SampleZone* first = nullptr;
    uint32_t matching = 0;
    for (auto& z : zones)
        if (z.audio != nullptr && z.answers (note, velocity))
        {
            if (first == nullptr)
                first = &z;
            if (z.lowKey == first->lowKey && z.highKey == first->highKey && z.lowVelocity == first->lowVelocity &&
                z.highVelocity == first->highVelocity)
                ++matching;
        }

    if (first == nullptr)
        return nullptr;

    auto wanted = counter % matching;
    for (auto& z : zones)
        if (z.audio != nullptr && z.answers (note, velocity) && z.lowKey == first->lowKey &&
            z.highKey == first->highKey && z.lowVelocity == first->lowVelocity && z.highVelocity == first->highVelocity)
        {
            if (wanted == 0)
                return &z;
            --wanted;
        }

    return first;
}

std::optional<int> noteFromFileName (const juce::String& name)
{
    // The last thing that looks like a note: a letter A-G, an optional # or b, an octave -1..9, not
    // inside a longer word ("Bass" is not B, "Kick2" is not a note).
    std::optional<int> found;
    const auto text = name.toUpperCase();
    const int semitones[] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G

    for (int i = 0; i < text.length(); ++i)
    {
        const auto c = text[i];
        if (c < 'A' || c > 'G')
            continue;
        if (i > 0 && juce::CharacterFunctions::isLetter (text[i - 1]))
            continue;

        int j = i + 1;
        int semitone = semitones[c - 'A'];
        if (j < text.length() && (text[j] == '#' || name[j] == 'b'))
        {
            semitone += text[j] == '#' ? 1 : -1;
            ++j;
        }

        bool negative = false;
        if (j < text.length() && text[j] == '-')
        {
            negative = true;
            ++j;
        }

        if (j >= text.length() || ! juce::CharacterFunctions::isDigit (text[j]))
            continue;
        const int octave = (text[j] - '0') * (negative ? -1 : 1);
        if (j + 1 < text.length() && (juce::CharacterFunctions::isLetterOrDigit (text[j + 1])))
            continue;

        // FL: C5 = 60, so MIDI = (octave + 0) * 12 + semitone.
        const auto midi = octave * 12 + semitone;
        if (midi >= 0 && midi <= 127)
            found = midi;
    }

    return found;
}

juce::String noteName (int midiNote)
{
    static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (names[((midiNote % 12) + 12) % 12]) + juce::String (midiNote / 12 - (midiNote < 0 ? 1 : 0));
}

void mapZonesByRoot (std::vector<SampleZone>& zones)
{
    std::map<int, std::pair<int, int>> ranges; // root -> key range
    for (auto& z : zones)
        ranges[z.rootNote] = {};

    int previousRoot = -1;
    for (auto it = ranges.begin(); it != ranges.end(); ++it)
    {
        const auto next = std::next (it);
        const int low = previousRoot < 0 ? 0 : (previousRoot + it->first) / 2 + 1;
        const int high = next == ranges.end() ? 127 : (it->first + next->first) / 2;
        it->second = { low, high };
        previousRoot = it->first;
    }

    for (auto& z : zones)
    {
        z.lowKey = ranges[z.rootNote].first;
        z.highKey = ranges[z.rootNote].second;
    }
}

} // namespace spinus::dsp
