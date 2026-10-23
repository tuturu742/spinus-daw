#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>

namespace spinus::dsp
{

/** The Ducker's settings: how far and for how long the sound dips each time it is
    triggered. */
struct DuckerSettings
{
    enum class Trigger
    {
        sidechain, // each hit on the sidechain input (e.g. the kick's insert)
        everyBeat, // in time with the song, without a sidechain
        everyHalfBeat,
        everyTwoBeats
    };

    Trigger trigger = Trigger::sidechain;
    float amount = 0.7f;   // 0..1: how far the sound dips (1: to silence)
    float release = 0.25f; // seconds to come back up
    float curve = 0.5f;    // 0: comes back evenly; 1: stays down longer, then comes back quickly
};

/** Dips the volume each time it is triggered, then brings it back along a curve (a "pump").
    Real-time safe: process() does not allocate, lock or block. */
class DuckerEngine
{
public:
    static constexpr float attackSeconds = 0.003f;

    void prepare (double sampleRate);
    void reset();

    /** Ducks `main` in place. `sidechain` may have no channels (no sidechain connected). `songBeats` is
        the song position in quarter notes while playing, negative when stopped. */
    void process (juce::AudioBuffer<float>& main, const juce::AudioBuffer<float>& sidechain, const DuckerSettings&,
                  double bpm, double songBeats) noexcept;

    /** The gain `seconds` after a trigger, starting from full volume (for the window's curve). */
    static float gainAt (const DuckerSettings&, float seconds) noexcept;

    /** The gain at the end of the last block, and the loudest sidechain sample in it (for the window). */
    float getGain() const noexcept { return currentGain.load(); }
    float getSidechainPeak() const noexcept { return sidechainPeak.load(); }

    /** How many times it has been triggered since reset (tests). */
    int getNumTriggers() const noexcept { return triggers.load(); }

private:
    static constexpr int longAgo = 1'000'000'000; // samples: "never" for the counters below

    void trigger() noexcept;
    float nextGain (const DuckerSettings&) noexcept;
    bool detectHit (float level) noexcept;

    double sampleRate = 44100.0;
    float fastAttack = 0.0f, fastRelease = 0.0f, slowAttack = 0.0f, slowRelease = 0.0f;
    float fast = 0.0f, slow = 0.0f;
    int samplesSinceHit = longAgo;
    int samplesSinceTrigger = longAgo; // the curve's time
    float startGain = 1.0f, gain = 1.0f;
    double lastBeatIndex = -1.0;
    std::atomic<float> currentGain { 1.0f }, sidechainPeak { 0.0f };
    std::atomic<int> triggers { 0 };
};

} // namespace spinus::dsp
