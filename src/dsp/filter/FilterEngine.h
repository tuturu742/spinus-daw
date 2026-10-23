#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>

namespace spinus::dsp
{

/** The Filter's settings: a resonant filter whose cutoff an LFO and an envelope follower
    can move. */
struct FilterSettings
{
    enum class Type
    {
        lowPass,
        highPass,
        bandPass,
        notch
    };
    enum class LfoShape
    {
        sine,
        triangle,
        rampUp,
        rampDown,
        square,
        random // a new random level each cycle (sample and hold)
    };

    Type type = Type::lowPass;
    bool steep = false;       // 24 dB per octave instead of 12
    float cutoffHz = 1000.0f; // 20..20000
    float resonance = 0.2f;   // 0..1 (near 1: rings strongly)
    float lfoAmount = 0.0f;   // -1..1: moves the cutoff by up to 4 octaves each way
    LfoShape lfoShape = LfoShape::sine;
    bool lfoSync = true;          // rate in note lengths, locked to the song position while playing
    float lfoRateHz = 1.0f;       // when not synced
    double lfoBeats = 1.0;        // when synced: one cycle per this many quarter notes
    float envelopeAmount = 0.0f;  // -1..1: the input's level moves the cutoff by up to 4 octaves
    float envelopeAttack = 0.01f; // seconds
    float envelopeRelease = 0.2f; // seconds
    float mix = 1.0f;             // 0 dry .. 1 filtered
    float gainDb = 0.0f;          // output
};

/** A state-variable filter (Simper's trapezoidal SVF, two stages for 24 dB) with its cutoff modulated
    per 16 samples. Real-time safe: process() does not allocate, lock or block. */
class FilterEngine
{
public:
    static constexpr int maxChannels = 2;

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    /** Filters the buffer in place. `songBeats` is the playing song position in quarter notes, or a
        negative number when stopped (a synced LFO then runs freely at `bpm`). */
    void process (juce::AudioBuffer<float>&, const FilterSettings&, double bpm, double songBeats) noexcept;

    /** The cutoff after modulation at the end of the last block (for the window's display); negative
        before the first block. */
    float getModulatedCutoff() const noexcept { return modulatedCutoff.load(); }

    /** The LFO's value at a phase (0..1), from -1 to 1. */
    static float lfoValue (FilterSettings::LfoShape, double phase, float randomLevel) noexcept;

    /** The response of the filter at a frequency, as gain (for the display and tests). */
    static float magnitudeAt (const FilterSettings&, float cutoffHz, float frequencyHz) noexcept;

private:
    struct Stage
    {
        float ic1 = 0.0f, ic2 = 0.0f;
    };
    struct Coefficients
    {
        float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, k = 2.0f;
    };

    static Coefficients coefficientsFor (float cutoffHz, float k, double sampleRate) noexcept;
    static float tick (Stage&, const Coefficients&, FilterSettings::Type, float input) noexcept;
    /** The damping (1/Q) of each stage: Butterworth at no resonance, near self-oscillation at full. */
    static std::array<float, 2> dampingFor (const FilterSettings&) noexcept;

    double sampleRate = 44100.0;
    std::array<std::array<Stage, 2>, maxChannels> stages {};
    double lfoPhase = 0.0;
    float randomLevel = 0.0f;
    static constexpr juce::int64 randomSeed = 0x5f17e4; // the same input always gives the same output
    juce::Random random { randomSeed };
    float envelope = 0.0f;
    juce::SmoothedValue<float> logCutoff, mix, gain;
    bool smoothersStarted = false;
    std::atomic<float> modulatedCutoff { -1.0f };
};

} // namespace spinus::dsp
