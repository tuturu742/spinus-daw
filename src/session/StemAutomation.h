#pragma once

#include "model/Model.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <optional>

namespace spinus
{

/** How loud a separated stem is and where it sits left to right, over time, as volume and pan automation for the
    channel that replays it (§17, the Analyzer's step 3): the fades, swells, drops and moves of the recording, not
    each note's loudness (the notes' velocities have that). Measured beat by beat, smoothed over a bar, and kept only
    where it really changes: a stem whose loudness stays within 3 dB gets no volume curve, one that stays in one
    place a fixed pan instead of a pan curve. No AI and no model: plain data, any thread. */
namespace stemautomation
{
/** Ticks from the first downbeat at `seconds` into the song, on the song's own beats (as the Analyzer's notes are
    placed: past the ends, at the first and last beat's length; with fewer than two beats, 120 BPM). */
double toTicks (double seconds, const std::vector<double>& beats, double firstDownbeat);

struct Curves
{
    std::vector<model::AutomationPoint> volume; // fader positions (0..1) around `volumeDb`; empty: none needed
    std::vector<model::AutomationPoint> pan;    // 0 left, 0.5 centre, 1 right; empty: none needed
    std::optional<float> steadyPan;             // -1..1, when the stem stays in one place off the centre
};

/** The curves for one stem, in ticks from the first downbeat. `volumeDb` is the channel's volume, which the stem's
    loud parts keep; quieter parts go below it. */
Curves measure (const juce::AudioBuffer<float>& stem, double sampleRate, const std::vector<double>& beats,
                double firstDownbeat, float volumeDb = 0.0f);
} // namespace stemautomation

} // namespace spinus
