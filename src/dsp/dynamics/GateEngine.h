#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>

namespace spinus::dsp
{

/** The Gate's settings. */
struct GateSettings
{
    float thresholdDb = -40.0f; // opens when the level rises above this
    float hysteresisDb = 3.0f;  // closes only when the level falls this far below the threshold
    float rangeDb = -80.0f;     // how far it turns the sound down when closed (-80: to silence)
    float attackMs = 0.5f;      // time to open
    float holdMs = 20.0f;       // stays open this long after the level falls
    float releaseMs = 100.0f;   // time to close
    bool sidechain = false;     // open on the sidechain input's level instead of the sound's own
};

/** A noise gate with hysteresis and hold. Real-time safe: process() does not allocate, lock or block. */
class GateEngine
{
public:
    enum class State
    {
        closed,
        opening,
        open,
        holding,
        closing
    };

    static constexpr float floorDb = -120.0f, fullyClosedDb = -79.9f;

    void prepare (double sampleRate);
    void reset() noexcept;

    /** Gates `main` in place. `sidechain` may have no channels (none connected). */
    void process (juce::AudioBuffer<float>& main, const juce::AudioBuffer<float>& sidechain,
                  const GateSettings&) noexcept;

    State getState() const noexcept { return (State)state.load(); }
    bool isOpen() const noexcept { return open; }
    float getGain() const noexcept { return gainNow.load(); }
    float getGainReductionDb() const noexcept { return juce::Decibels::gainToDecibels (gainNow.load(), floorDb); }
    float getLevelDb() const noexcept { return levelNow.load(); }
    float getSidechainPeak() const noexcept { return sidechainPeak.load(); }
    /** How many times it has opened since reset (tests). */
    int getNumOpenings() const noexcept { return openings.load(); }

private:
    double sampleRate = 44100.0;
    float envelope = 0.0f, envelopeRelease = 0.0f, gain = 0.0f;
    bool open = false;
    int holdLeft = 0;
    std::atomic<int> state { (int)State::closed }, openings { 0 };
    std::atomic<float> gainNow { 0.0f }, levelNow { floorDb }, sidechainPeak { 0.0f };
};

} // namespace spinus::dsp
