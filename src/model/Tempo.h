#pragma once

#include "model/Model.h"

namespace spinus::model
{

/** The song's tempo over time: ProjectSettings::tempoBpm from the start, then each TempoChange from its tick
    (a jump; glides come later). Ticks count quarter notes, as the tempo does. */
class TempoMap
{
public:
    explicit TempoMap (const ProjectSettings&);

    double bpmAt (double ticks) const;
    double seconds (double ticks) const;
    double ticks (double seconds) const;

    /** Seconds per tick at that tempo. */
    static double secondsPerTick (double bpm) { return 60.0 / (bpm * (double)ticksPerQuarterNote); }

private:
    struct Segment
    {
        double fromTicks, fromSeconds, bpm;
    };
    std::vector<Segment> segments; // the first from 0
};

inline constexpr double minimumBpm = 20.0, maximumBpm = 999.0;

/** Why these tempo changes cannot be (empty if they can): each after the start, in order, one per tick, and a
    tempo from minimumBpm to maximumBpm. */
juce::String tempoChangesProblem (const std::vector<TempoChange>&);

/** The default for a new or older audio clip: "time" for long audio (over 20 s: vocals, stems, songs), else
    "beat" (one-shots, loops, chops). Recorded takes are "time" too, set where they are made. */
juce::String defaultOnTempoChange (const Project&, const Clip&);

/** After the tempo changed from `before` to the project's: moves and resizes every audio clip as its
    onTempoChange says, and keeps its offset and fades as long in seconds. */
void retimeAudioClips (Project&, const ProjectSettings& before);

} // namespace spinus::model
