#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>

namespace spinus::dsp
{

/** The Stereo Utility's settings. */
struct StereoUtilitySettings
{
    float gainDb = 0.0f; // -48..+24
    float pan = 0.0f;    // -1 (left) .. 1 (right), balance
    float width = 1.0f;  // 0 mono .. 1 unchanged .. 2 extra wide
    bool mono = false;
    bool invertLeft = false, invertRight = false;
};

/** Gain, balance, stereo width (mid/side), mono and polarity per side. Real-time safe: process() does
    not allocate, lock or block. */
class StereoUtilityEngine
{
public:
    void prepare (double sampleRate);
    void reset();

    void process (juce::AudioBuffer<float>&, const StereoUtilitySettings&) noexcept;

    /** Levels after processing in the last block (peak, linear) and how alike left and right are
        (1: the same, 0: unrelated, -1: opposite), for the window. */
    float getPeak (int channel) const noexcept { return peaks[(size_t)juce::jlimit (0, 1, channel)].load(); }
    float getCorrelation() const noexcept { return correlation.load(); }

private:
    double sampleRate = 44100.0;
    juce::SmoothedValue<float> gain, leftOfPan, rightOfPan, sideGain, leftSign, rightSign;
    bool started = false;
    std::array<std::atomic<float>, 2> peaks {};
    std::atomic<float> correlation { 1.0f };
};

} // namespace spinus::dsp
