#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>

namespace spinus::dsp
{

/** The Compressor's settings. */
struct CompressorSettings
{
    float thresholdDb = -18.0f;
    float ratio = 4.0f;       // 1..20 (20: close to a limiter)
    float attackMs = 10.0f;   // time to reach 63% of a new reduction
    float releaseMs = 120.0f; // time to recover 63% of it
    float kneeDb = 6.0f;      // width of the soft knee around the threshold
    float makeupDb = 0.0f;
    bool autoMakeup = false; // adds autoMakeupDb() to the makeup
    float mix = 1.0f;        // 0 dry .. 1 compressed (parallel compression in between)
    bool rms = false;        // level by RMS (10 ms) instead of by peak
    bool sidechain = false;  // follow the sidechain input instead of the sound itself
};

/** A feed-forward compressor: the level (peak or RMS, the channels linked) through a soft-knee static
    curve, the gain reduction smoothed in decibels with separate attack and release, then makeup gain and
    a dry/wet mix. Real-time safe: process() does not allocate, lock or block. */
class CompressorEngine
{
public:
    static constexpr float floorDb = -120.0f;

    /** The output level for an input level, before makeup (the static curve, in dB). */
    static float staticCurve (const CompressorSettings&, float inputDb) noexcept;
    /** Automatic makeup: half the reduction a full-scale sound would get. */
    static float autoMakeupDb (const CompressorSettings&) noexcept;
    /** Seconds to one-pole coefficient. */
    static float coefficient (float milliseconds, double sampleRate) noexcept;

    void prepare (double sampleRate);
    void reset() noexcept;

    /** Compresses `main` in place. `sidechain` may have no channels (none connected). */
    void process (juce::AudioBuffer<float>& main, const juce::AudioBuffer<float>& sidechain,
                  const CompressorSettings&) noexcept;

    /** The gain reduction (<= 0 dB) now, the deepest in the last block, and the detected level. */
    float getGainReductionDb() const noexcept { return reductionNow.load(); }
    float getDeepestReductionDb() const noexcept { return reductionDeepest.load(); }
    float getInputLevelDb() const noexcept { return inputLevel.load(); }
    float getSidechainPeak() const noexcept { return sidechainPeak.load(); }

private:
    double sampleRate = 44100.0;
    float envelopeDb = 0.0f, meanSquare = 0.0f, rmsCoefficient = 0.0f;
    juce::SmoothedValue<float> makeup, mix;
    bool started = false;
    std::atomic<float> reductionNow { 0.0f }, reductionDeepest { 0.0f }, inputLevel { floorDb }, sidechainPeak { 0.0f };
};

} // namespace spinus::dsp
