#include "Scales.h"

namespace spinus::model
{

namespace
{
/** Semitones from the key's first note, per scale name (in the order of getScaleNames()). */
const std::vector<std::vector<int>>& stepsOfScales()
{
    static const std::vector<std::vector<int>> steps {
        { 0, 2, 4, 5, 7, 9, 11 },                 // major
        { 0, 2, 3, 5, 7, 8, 10 },                 // minor
        { 0, 2, 3, 5, 7, 8, 11 },                 // harmonic minor
        { 0, 2, 4, 7, 9 },                        // major pentatonic
        { 0, 3, 5, 7, 10 },                       // minor pentatonic
        { 0, 3, 5, 6, 7, 10 },                    // blues
        { 0, 2, 3, 5, 7, 9, 10 },                 // dorian
        { 0, 2, 4, 5, 7, 9, 10 },                 // mixolydian
        { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, // chromatic
    };
    return steps;
}
} // namespace

const juce::StringArray& getScaleNames()
{
    static const juce::StringArray names { "major", "minor",  "harmonic minor", "major pentatonic", "minor pentatonic",
                                           "blues", "dorian", "mixolydian",     "chromatic" };
    return names;
}

bool isInKey (const Key& key, int note)
{
    const auto index = juce::jmax (0, getScaleNames().indexOf (key.scale.toLowerCase()));
    const auto step = ((note - key.tonic) % 12 + 12) % 12;
    const auto& steps = stepsOfScales()[(size_t)index];
    return std::find (steps.begin(), steps.end(), step) != steps.end();
}

int nearestNoteInKey (const Key& key, float pitch)
{
    const auto centre = juce::roundToInt (pitch);
    int best = centre;
    float distance = 1000.0f;
    for (int note = centre - 6; note <= centre + 6; ++note)
        if (note >= 0 && note < 128 && isInKey (key, note) && std::abs ((float)note - pitch) < distance)
        {
            distance = std::abs ((float)note - pitch);
            best = note;
        }
    return best;
}

bool usesFlats (const Key& key)
{
    // Semitones from the key's first note up to its major key's first note.
    const auto scale = key.scale.toLowerCase();
    const int toMajor = scale == "minor" || scale == "harmonic minor" || scale == "minor pentatonic" || scale == "blues"
                            ? 3
                        : scale == "dorian"     ? 10
                        : scale == "mixolydian" ? 5
                                                : 0;
    const auto major = ((key.tonic + toMajor) % 12 + 12) % 12;
    return major == 5 || major == 10 || major == 3 || major == 8 || major == 1;
}

juce::String pitchClassName (int pitch, bool flats)
{
    static const char* sharpNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char* flatNames[] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
    const auto pc = (pitch % 12 + 12) % 12;
    return flats ? flatNames[pc] : sharpNames[pc];
}

} // namespace spinus::model
