#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

namespace spinus::dsp::slicing
{

/** Finding slices in a loop (Beat Slicer). Slice points are sample positions where a
    slice starts; the first is always 0, and each slice ends where the next starts. */

constexpr int maxSlices = 64; // played from C5 (MIDI 60) up

/** Slices at the loop's hits: where the level jumps up. `sensitivity` 0..1: higher finds quieter
    hits. Each point is moved back to the nearest zero crossing (no clicks). */
std::vector<juce::int64> byTransients (const juce::AudioBuffer<float>&, double sampleRate, float sensitivity);

/** Equal slices: the loop is `loopBeats` quarter notes long, each slice `beatsPerSlice` long. */
std::vector<juce::int64> byGrid (juce::int64 length, double loopBeats, double beatsPerSlice);

/** How many quarter notes a loop of this length probably is: from a tempo in its file name
    ("Amen 136bpm"), otherwise the power of two that puts it closest to `tempo`. */
double estimateBeats (double seconds, double tempo, const juce::String& name);

/** Sorted, unique, inside the audio, starting at 0, at most maxSlices. */
std::vector<juce::int64> tidy (std::vector<juce::int64> points, juce::int64 length);

} // namespace spinus::dsp::slicing
