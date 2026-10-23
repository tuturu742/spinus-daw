#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <vector>

namespace spinus::dsp
{

/** The Limiter's settings. */
struct LimiterSettings
{
    float inputDb = 0.0f;    // gain before limiting (drives the sound into the ceiling)
    float ceilingDb = -1.0f; // the output never goes above this
    float releaseMs = 60.0f;
    bool truePeak = true; // also catch peaks between samples (as BS.1770 measures them)
};

/** A look-ahead brickwall limiter. The audio is delayed by the look-ahead; the gain each sample needs is
    held over the look-ahead window and averaged over it, so the gain is already down when a peak arrives
    and never goes above what any sample in the window needs. With true peak on, the points between
    samples count too: the 4 points BS.1770-4's true-peak filter makes (dsp/TruePeak.h, as the Loudness Meter
    and other BS.1770 meters measure), and 8 points from a longer 32-sample interpolation, which meters that
    resample more finely (ffmpeg's ebur128) see near the top of the spectrum. The gain is held flat over the 32
    samples each point is made from. A last clip at the ceiling catches what rounding might leave. The latency
    is lookahead + 30 samples (the true-peak window), with true peak on or off.

    Real-time safe: process() does not allocate, lock or block; buffers are sized in prepare() for the
    longest look-ahead at that sample rate. */
class LimiterEngine
{
public:
    static constexpr float maxLookaheadMs = 10.0f;
    static constexpr int interpolationTaps = 32, finePhases = 8, truePeakSpan = interpolationTaps - 1;

    void prepare (double sampleRate, float lookaheadMs);
    void reset() noexcept;

    /** Asks for a new look-ahead from any thread; the audio thread takes it at its next block. */
    void setLookahead (float lookaheadMs) noexcept;
    static int lookaheadSamplesFor (float lookaheadMs, double sampleRate) noexcept;
    /** The delay through the limiter for a look-ahead. */
    static int latencyFor (float lookaheadMs, double sampleRate) noexcept;
    int getLatencySamples() const noexcept { return latencyFor (lookaheadMs.load(), sampleRate); }

    void process (juce::AudioBuffer<float>&, const LimiterSettings&) noexcept;

    float getGainReductionDb() const noexcept { return reductionNow.load(); }
    float getDeepestReductionDb() const noexcept { return reductionDeepest.load(); }
    float getInputPeakDb() const noexcept { return inputPeak.load(); }
    float getOutputPeakDb() const noexcept { return outputPeak.load(); }

private:
    struct SlidingMinimum
    {
        std::vector<juce::int64> index;
        std::vector<float> value;
        int head = 0, size = 0, capacity = 1;
        void allocate (int n);
        void clear() noexcept { head = size = 0; }
        void push (juce::int64 at, float v, int window) noexcept;
        float minimum() const noexcept { return value[(size_t)head]; }
    };

    void configure (int lookaheadSamples) noexcept;
    float interpolatedPeak (int channel) const noexcept;

    double sampleRate = 44100.0;
    std::atomic<float> lookaheadMs { 3.0f };
    int lookahead = 1, active = -1; // samples
    juce::int64 counter = 0;

    // Delay line (per channel), the recent input for the interpolation, and the gain pipeline.
    std::array<std::vector<float>, 2> delay;
    int delaySize = 1, write = 0;
    std::array<std::array<float, interpolationTaps>, 2> recent {};
    std::array<std::array<float, interpolationTaps>, finePhases> fine {};
    SlidingMinimum hold;
    std::vector<float> box;
    int boxWrite = 0;
    double boxSum = 0.0;
    float envelope = 1.0f;
    juce::SmoothedValue<float> input;
    bool started = false;

    std::atomic<float> reductionNow { 0.0f }, reductionDeepest { 0.0f }, inputPeak { -120.0f }, outputPeak { -120.0f };
};

} // namespace spinus::dsp
