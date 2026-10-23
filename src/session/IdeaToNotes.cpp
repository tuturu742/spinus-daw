#include "IdeaToNotes.h"

#include "model/Scales.h"

namespace spinus
{

using namespace spinus::model;

namespace
{
/** Seconds in the take's file to ticks from the pattern's start. */
double toTicks (double fileSeconds, const IdeaRecorder::Take& take)
{
    return (fileSeconds - take.patternStart) * take.bpm / 60.0 * (double)ticksPerQuarterNote;
}

Ticks snap (double ticks, Ticks grid) { return (Ticks)std::llround (ticks / (double)grid) * grid; }
} // namespace

std::vector<Note> IdeaToNotes::melody (const juce::var& heard, const IdeaRecorder::Take& take, const Project& project,
                                       PatternId patternId) const
{
    const auto* pattern = project.findPattern (patternId);
    if (pattern == nullptr)
        return {};

    std::vector<Note> notes;
    if (auto* list = heard["notes"].getArray())
        for (auto& n : *list)
        {
            const auto start = snap (toTicks ((double)n["start"], take), grid);
            const auto end = snap (toTicks ((double)n["end"], take), grid);
            if (start < 0 || start >= pattern->length)
                continue;

            auto pitch = (float)(double)n["pitch"];
            const auto note = snapToKey && project.settings.key ? nearestNoteInKey (*project.settings.key, pitch)
                                                                : juce::roundToInt (pitch);
            Note result;
            result.start = start;
            result.length = juce::jlimit (grid, pattern->length - start, end - start);
            result.pitch = juce::jlimit (0, 127, note);
            result.velocity = 0.8f;
            notes.push_back (result);
        }

    // One note at a time: two on the same step keep the longer; a note ends where the next starts.
    std::sort (notes.begin(), notes.end(), [] (auto& a, auto& b) { return a.start < b.start; });
    std::vector<Note> result;
    for (auto& n : notes)
    {
        if (! result.empty() && result.back().start == n.start)
        {
            if (n.length > result.back().length)
                result.back() = n;
            continue;
        }
        if (! result.empty())
            result.back().length = juce::jmin (result.back().length, n.start - result.back().start);
        result.push_back (n);
    }
    return result;
}

std::vector<Note> IdeaToNotes::beats (const juce::var& heard, const IdeaRecorder::Take& take, const Project& project,
                                      PatternId patternId) const
{
    const auto* pattern = project.findPattern (patternId);
    if (pattern == nullptr)
        return {};

    std::vector<Note> notes;
    if (auto* list = heard["hits"].getArray())
        for (auto& hit : *list)
        {
            const auto start = snap (toTicks ((double)hit["time"], take), grid);
            if (start < 0 || start >= pattern->length)
                continue;

            const auto sound = hit["sound"].toString();
            Note note;
            note.start = start;
            note.length = grid;
            note.pitch = sound == "kick" ? kickNote : sound == "snare" ? snareNote : hatNote;
            note.velocity = juce::jlimit (0.3f, 1.0f, 0.4f + 0.6f * (float)(double)hit["strength"]);

            // The same sound twice on one step: once.
            if (std::none_of (notes.begin(), notes.end(),
                              [&] (auto& n) { return n.start == note.start && n.pitch == note.pitch; }))
                notes.push_back (note);
        }

    std::sort (notes.begin(), notes.end(),
               [] (auto& a, auto& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
    return notes;
}

} // namespace spinus
