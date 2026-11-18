#pragma once

#include "model/Model.h"

#include <optional>

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

/** The default for an older audio clip: "time" for long audio (over 20 s: vocals, stems, songs), else "beat"
    (one-shots, loops, chops). Recorded takes are "time" too, set where they are made. A newly placed file whose
    name gives its tempo is "stretch" (at that tempo), chosen where it is placed. */
juce::String defaultOnTempoChange (const Project&, const Clip&);

/** Why an audio clip's onTempoChange cannot be (empty if it can): time, beat, or stretch from a sourceBpm. */
juce::String onTempoChangeProblem (const AudioClip&);

/** The tempo a file was made at, when its name says: "drums_120bpm.wav", "Bass Loop 92 BPM.flac", "bpm140 hats". */
std::optional<double> bpmFromFileName (const juce::String& file);

/** Seconds into its file that an audio clip plays `ticksIntoClip` after its start (its offset and speed
    included; not reversed). A stretched clip plays its file's beats as the song's; the others play through the
    song's tempo from where they start. */
double secondsInFile (const TempoMap&, const Clip&, const AudioClip&, double ticksIntoClip);

/** How long a clip at `start` is that plays `seconds` of its file from `offset` (a stretched clip at its source
    tempo, the others through the song's tempo); at least one tick. */
Ticks audioClipLength (const TempoMap&, Ticks start, const AudioClip&, double seconds);

/** After a clip's onTempoChange or sourceBpm changed from `before`: sets its offset, fades and length so
    it plays the same part of its file as before. */
void keepSameAudio (const TempoMap&, Clip&, const AudioClip& before);

/** After the tempo changed from `before` to the project's: moves and resizes every audio clip as its
    onTempoChange says, and keeps its offset and fades as long in seconds. A stretched clip keeps its ticks. */
void retimeAudioClips (Project&, const ProjectSettings& before);

} // namespace spinus::model
