#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>

namespace spinus::dsp
{

/** Loudness as streaming services and broadcasters measure it (ITU-R BS.1770-4, EBU R128): K-weighted
    momentary (400 ms), short-term (3 s) and integrated (gated, since the last reset) loudness in LUFS, and
    the true peak (4x oversampled with BS.1770-4's filter, dsp/TruePeak.h) in dBTP. Mono or stereo.

    Real-time safe: process() does not allocate, lock or block; the integrated value comes from a fixed
    histogram (0.1 LU steps). Readings are atomics, for the window. */
class LoudnessEngine
{
public:
    static constexpr float silence = -100.0f; // what a reading is before there is any sound
    static constexpr int historySteps = 600;  // short-term loudness every 100 ms: one minute

    void prepare (double sampleRate);
    void reset() noexcept;

    /** Asks the audio thread to start measuring again at its next block (from any thread). */
    void requestReset() noexcept { resetRequested.store (true); }

    /** Measures; the audio passes unchanged. */
    void process (const juce::AudioBuffer<float>&) noexcept;

    float getMomentary() const noexcept { return momentary.load(); }
    float getShortTerm() const noexcept { return shortTerm.load(); }
    float getIntegrated() const noexcept { return integrated.load(); }
    float getTruePeakDb() const noexcept { return truePeakDb.load(); } // highest since the reset
    double getMeasuredSeconds() const noexcept { return measuredSeconds.load(); }

    /** The short-term loudness every 100 ms, oldest first, up to historySteps values; returns how many. */
    int copyHistory (std::array<float, historySteps>&) const noexcept;

    /** Loudness of a mean square (already K-weighted and summed over channels). */
    static float toLufs (double meanSquare) noexcept;

private:
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        std::array<double, 2> z1 {}, z2 {}; // per channel
        double process (double x, size_t channel) noexcept
        {
            const auto y = b0 * x + z1[channel];
            z1[channel] = b1 * x - a1 * y + z2[channel];
            z2[channel] = b2 * x - a2 * y;
            return y;
        }
    };

    static constexpr int stepsPerShortTerm = 30, stepsPerMomentary = 4;
    static constexpr int interpolationTaps = 12; // BS.1770-4's true-peak filter, per phase
    static constexpr float histogramFloor = -70.0f, histogramStep = 0.1f;
    static constexpr int histogramBins = 800; // -70 to +10 LUFS

    void endStep() noexcept;
    void addGatingBlock (double meanSquare) noexcept;
    void updateIntegrated() noexcept;
    float truePeakOf (float sample, size_t channel) noexcept;

    double sampleRate = 48000.0;
    Biquad shelf, highPass;
    int stepLength = 4800, stepFill = 0;
    double stepSum = 0.0;
    std::array<double, stepsPerShortTerm> steps {}; // mean squares of the last 100 ms steps
    int stepsWritten = 0;

    std::array<juce::int64, histogramBins> histogram {};
    std::array<double, histogramBins> binEnergy {};                // the blocks' mean squares added up, per bin
    std::array<std::array<float, interpolationTaps>, 2> recent {}; // per channel, newest last
    float peak = 0.0f;

    std::atomic<bool> resetRequested { false };
    std::atomic<float> momentary { silence }, shortTerm { silence }, integrated { silence }, truePeakDb { silence };
    std::atomic<double> measuredSeconds { 0.0 };
    std::array<std::atomic<float>, historySteps> history {};
    std::atomic<int> historyWritten { 0 };
};

} // namespace spinus::dsp
