#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace spinus::dsp
{

/** The "Spinus Synth Kit": 16 drum sounds made by simple synthesis (sine sweeps, filtered noise), so the
    Drum Machine has sounds without shipping sample files. Deterministic: the same audio every time. */
struct SynthDrum
{
    juce::String name;
    juce::AudioBuffer<float> audio; // mono
    int chokeGroup = 0;
};

std::vector<SynthDrum> makeSynthKit (double sampleRate);

} // namespace spinus::dsp
