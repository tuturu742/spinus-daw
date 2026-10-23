#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <vector>

namespace spinus::dsp
{

/** One band of the EQ. */
struct EqBand
{
    enum class Type
    {
        bell,
        lowShelf,
        highShelf,
        lowCut,  // high-pass
        highCut, // low-pass
        notch
    };

    bool on = false;
    Type type = Type::bell;
    float frequencyHz = 1000.0f; // 20..20000
    float gainDb = 0.0f;         // -24..24 (bells and shelves)
    float q = 1.0f;              // 0.1..18
    int slope = 0;               // cuts: 0 = 12, 1 = 24, 2 = 48 dB per octave

    bool usesGain() const noexcept { return type == Type::bell || type == Type::lowShelf || type == Type::highShelf; }
};

struct EqSettings
{
    static constexpr int numBands = 8;
    std::array<EqBand, numBands> bands {};
    float outputDb = 0.0f;
};

/** Spinus EQ: eight parametric bands of second-order sections, each matched to its analog prototype
    (Vicanek, "Matched Second Order Digital Filters", 2016: the poles by impulse invariance, the zeros
    fitted to the analog magnitude at DC, at the Nyquist frequency and at the band's frequency), so a
    band near the top of the spectrum keeps its analog shape instead of being squeezed towards Nyquist
    as the plain bilinear transform does. Cuts of 24 and 48 dB per octave are Butterworth cascades.

    Real-time safe: process() does not allocate, lock or block. Settings glide (frequency on a log
    scale) and the sections are recomputed every 16 samples while they move. The output also goes,
    mixed to mono, into a lock-free FIFO for the window's spectrum. */
class EqEngine
{
public:
    static constexpr int maxChannels = 2, maxStages = 4, spectrumFifoSize = 16384;

    /** A second-order section: b0 + b1 z^-1 + b2 z^-2 over 1 + a1 z^-1 + a2 z^-2. */
    struct Coefficients
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    };
    struct Design
    {
        std::array<Coefficients, maxStages> stages {};
        int numStages = 0;
    };

    /** The band's sections at a sample rate (none when it is off or has nothing to do). */
    static Design design (const EqBand&, double sampleRate) noexcept;
    /** The analog prototype's gain at a frequency: what the matched sections aim for. */
    static double analogMagnitude (const EqBand&, double hz) noexcept;
    /** The gain of a design at a frequency. */
    static double magnitude (const Design&, double hz, double sampleRate) noexcept;
    /** The plain bilinear-transform (RBJ cookbook) version of a band (for comparison in tests). */
    static Design designBilinear (const EqBand&, double sampleRate) noexcept;

    /** The whole EQ's gain at a frequency, output gain included (for the window's curve). */
    static double responseAt (const EqSettings&, double hz, double sampleRate) noexcept;

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;
    void process (juce::AudioBuffer<float>&, const EqSettings&) noexcept;

    double getSampleRate() const noexcept { return sampleRate; }

    /** The window asks for the spectrum while it is open; then up to `max` of the newest output samples
        (mono) are read here, from the message thread. */
    void setSpectrumWanted (bool wanted) noexcept { spectrumWanted.store (wanted); }
    int readSpectrumSamples (float* destination, int max) noexcept;

private:
    struct BandState
    {
        juce::SmoothedValue<float> logFrequency, gainDb, q;
        juce::SmoothedValue<float> wet; // 0 off .. 1 on: switching a band fades it
        EqBand designed;                // what `current` was made for
        Design current;
        std::array<std::array<std::array<double, 2>, maxStages>, maxChannels> z {}; // per channel and stage
        bool needsDesign = true;
    };

    void updateBand (BandState&, const EqBand& target, int samples) noexcept;

    double sampleRate = 44100.0;
    std::array<BandState, EqSettings::numBands> bands;
    juce::SmoothedValue<float> output;
    bool started = false;

    std::atomic<bool> spectrumWanted { false };
    juce::AbstractFifo spectrumFifo { spectrumFifoSize };
    std::vector<float> spectrumBuffer = std::vector<float> ((size_t)spectrumFifoSize);
};

} // namespace spinus::dsp
