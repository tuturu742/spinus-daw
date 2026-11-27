#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

namespace spinus::engine::outputmix
{

/** Where one of an instrument's outputs goes in its channel's stereo sound. */
enum class Side
{
    left,
    right,
    centre
};

/** An instrument whose main output has more than two channels (a drum plugin that gives each microphone its own
    output, a surround synth) is heard as one stereo sound: each channel goes to its side, from what the plugin
    says the channel is. A left channel goes left, a right one right, any other one (a numbered output, a centre,
    a mono one) to the middle. Empty for two channels or fewer: those play as they are. */
std::vector<Side> sidesOf (const juce::AudioChannelSet& mainOutput);

/** The gain of a channel in the middle on each side: -3 dB, so it sounds as loud as on one side. */
inline constexpr float centreGain = 0.70710678f;

/** Mixes the first `numSides` channels into channels 0 and 1 (in place), then silences the others. Real-time
    safe: no allocation. Does nothing with no sides, or with fewer than two channels in the buffer. */
void mixToStereo (juce::AudioBuffer<float>&, int start, int numSamples, const Side* sides, int numSides) noexcept;

} // namespace spinus::engine::outputmix
