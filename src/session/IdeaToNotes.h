#pragma once

#include "IdeaRecorder.h"
#include "model/Model.h"

namespace spinus
{

/** Turns what the AI service heard in a take (ai.v1.listen.melody or .beats) into notes for
    the pattern the take was recorded over: in time with the beat, snapped to the grid
    and, for melodies, to the project's key when it has one. */
struct IdeaToNotes
{
    model::Ticks grid = model::ticksPerQuarterNote / 4; // a sixteenth
    bool snapToKey = true;

    /** The Drum Machine's pads for each beatbox sound (its kit's kick, snare and closed hat). */
    int kickNote = 60, snareNote = 61, hatNote = 63;

    /** A sung or hummed melody: one note at a time. */
    std::vector<model::Note> melody (const juce::var& heard, const IdeaRecorder::Take&, const model::Project&,
                                     model::PatternId) const;

    /** Beatboxed hits: a short note on the sound's pad for each. */
    std::vector<model::Note> beats (const juce::var& heard, const IdeaRecorder::Take&, const model::Project&,
                                    model::PatternId) const;
};

} // namespace spinus
