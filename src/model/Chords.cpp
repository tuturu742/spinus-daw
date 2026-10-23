#include "Chords.h"

#include "Scales.h"

#include <set>

namespace spinus::model
{

const std::vector<ChordType>& getChordTypes()
{
    static const std::vector<ChordType> types {
        { "Major", "", { 0, 4, 7 } },
        { "Minor", "m", { 0, 3, 7 } },
        { "Diminished", "dim", { 0, 3, 6 } },
        { "Augmented", "aug", { 0, 4, 8 } },
        { "Suspended 2", "sus2", { 0, 2, 7 } },
        { "Suspended 4", "sus4", { 0, 5, 7 } },
        { "Seventh", "7", { 0, 4, 7, 10 } },
        { "Major 7", "maj7", { 0, 4, 7, 11 } },
        { "Minor 7", "m7", { 0, 3, 7, 10 } },
        { "Half-diminished", "m7b5", { 0, 3, 6, 10 } },
        { "Diminished 7", "dim7", { 0, 3, 6, 9 } },
        { "Sixth", "6", { 0, 4, 7, 9 } },
        { "Minor 6", "m6", { 0, 3, 7, 9 } },
        { "Add 9", "add9", { 0, 4, 7, 14 } },
    };
    return types;
}

std::vector<int> chordOn (int root, const ChordType& type)
{
    std::vector<int> pitches;
    for (auto interval : type.intervals)
        if (juce::isPositiveAndBelow (root + interval, 128))
            pitches.push_back (root + interval);
    return pitches;
}

std::vector<int> chordInKey (int root, const Key& key, int notes)
{
    // The key's notes from the root upwards; the chord takes every second one.
    auto note = nearestNoteInKey (key, (float)root);
    std::vector<int> scale;
    for (int p = note; p < 128 && (int)scale.size() < 2 * notes; ++p)
        if (isInKey (key, p))
            scale.push_back (p);
    std::vector<int> chord;
    for (size_t i = 0; i < scale.size() && (int)chord.size() < notes; i += 2)
        chord.push_back (scale[i]);
    return chord;
}

juce::String nameChord (const std::vector<int>& pitches)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    std::set<int> classes;
    int bass = 128;
    for (auto p : pitches)
    {
        classes.insert (((p % 12) + 12) % 12);
        bass = std::min (bass, p);
    }
    if (classes.size() < 3)
        return {};

    // The root that makes the notes exactly one of the chords; the bass as the root first.
    std::vector<int> roots (classes.begin(), classes.end());
    std::stable_partition (roots.begin(), roots.end(), [bass] (int r) { return r == bass % 12; });
    for (auto root : roots)
        for (const auto& type : getChordTypes())
        {
            std::set<int> want;
            for (auto interval : type.intervals)
                want.insert ((root + interval) % 12);
            if (want == classes)
                return juce::String (names[root]) + type.suffix +
                       (bass % 12 != root ? "/" + juce::String (names[bass % 12]) : "");
        }
    return {};
}

} // namespace spinus::model
