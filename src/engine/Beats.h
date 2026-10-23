#pragma once

#include "model/Model.h"

#include <tracktion_engine/tracktion_engine.h>

namespace spinus::engine
{

/** Spinus's ticks count quarter notes (960 each). Tracktion counts beats in the time signature's note
    value: in 6/8 a Tracktion beat is an eighth (the tempo stays in quarter notes). Every conversion between
    the two goes through here. The song has one time signature (ProjectSettings), at the edit's start. */
inline double beatsPerQuarterNote (const tracktion::Edit& edit)
{
    const auto* sig = edit.tempoSequence.getTimeSig (0);
    return sig != nullptr && sig->denominator > 0 ? (double)sig->denominator.get() / 4.0 : 1.0;
}

inline tracktion::BeatPosition toBeats (const tracktion::Edit& edit, double ticks)
{
    return tracktion::BeatPosition::fromBeats (ticks / (double)model::ticksPerQuarterNote * beatsPerQuarterNote (edit));
}

inline tracktion::BeatDuration toBeatDuration (const tracktion::Edit& edit, double ticks)
{
    return tracktion::BeatDuration::fromBeats (ticks / (double)model::ticksPerQuarterNote * beatsPerQuarterNote (edit));
}

inline double toTicks (const tracktion::Edit& edit, tracktion::BeatPosition beats)
{
    return beats.inBeats() / beatsPerQuarterNote (edit) * (double)model::ticksPerQuarterNote;
}

inline tracktion::TimePosition ticksToTime (const tracktion::Edit& edit, double ticks)
{
    return edit.tempoSequence.toTime (toBeats (edit, ticks));
}

inline double timeToTicks (const tracktion::Edit& edit, tracktion::TimePosition time)
{
    return toTicks (edit, edit.tempoSequence.toBeats (time));
}

} // namespace spinus::engine
