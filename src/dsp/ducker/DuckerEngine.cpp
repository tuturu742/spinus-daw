#include "DuckerEngine.h"

namespace spinus::dsp
{

namespace
{
constexpr float minimumLevel = 0.003f; // -50 dB: quieter hits are ignored
constexpr float riseRatio = 2.0f;      // a hit: the level jumps 6 dB above its recent average
constexpr double minimumGap = 0.06;    // seconds between hits

float coefficient (double seconds, double sampleRate) { return (float)std::exp (-1.0 / (seconds * sampleRate)); }
} // namespace

void DuckerEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    fastAttack = coefficient (0.0005, sampleRate);
    fastRelease = coefficient (0.01, sampleRate);
    slowAttack = coefficient (0.02, sampleRate);
    slowRelease = coefficient (0.2, sampleRate);
    reset();
}

void DuckerEngine::reset()
{
    fast = slow = 0.0f;
    samplesSinceHit = samplesSinceTrigger = longAgo;
    startGain = gain = 1.0f;
    lastBeatIndex = -1.0;
    currentGain.store (1.0f);
    sidechainPeak.store (0.0f);
    triggers.store (0);
}

float DuckerEngine::gainAt (const DuckerSettings& s, float seconds) noexcept
{
    const auto amount = juce::jlimit (0.0f, 1.0f, s.amount);
    if (seconds < attackSeconds)
        return 1.0f - amount * juce::jmax (0.0f, seconds) / attackSeconds;

    const auto x = juce::jlimit (0.0f, 1.0f, (seconds - attackSeconds) / juce::jmax (0.001f, s.release));
    const auto recovered = std::pow (x, 1.0f + 3.0f * juce::jlimit (0.0f, 1.0f, s.curve));
    return 1.0f - amount * (1.0f - recovered);
}

void DuckerEngine::trigger() noexcept
{
    startGain = gain;
    samplesSinceTrigger = 0;
    triggers.fetch_add (1, std::memory_order_relaxed);
}

float DuckerEngine::nextGain (const DuckerSettings& s) noexcept
{
    const auto t = (float)((double)samplesSinceTrigger / sampleRate);
    if (samplesSinceTrigger < (longAgo))
        ++samplesSinceTrigger;

    if (t < attackSeconds)
    {
        // Down from wherever the last dip had got to, so a new hit never jumps up.
        const auto bottom = 1.0f - juce::jlimit (0.0f, 1.0f, s.amount);
        gain = startGain + (bottom - startGain) * t / attackSeconds;
    }
    else
    {
        gain = gainAt (s, t);
    }
    return gain;
}

bool DuckerEngine::detectHit (float level) noexcept
{
    fast = level + (level > fast ? fastAttack : fastRelease) * (fast - level);
    slow = level + (level > slow ? slowAttack : slowRelease) * (slow - level);
    if (samplesSinceHit < (longAgo))
        ++samplesSinceHit;

    if (fast > minimumLevel && fast > riseRatio * slow && samplesSinceHit > (int)(minimumGap * sampleRate))
    {
        samplesSinceHit = 0;
        return true;
    }
    return false;
}

void DuckerEngine::process (juce::AudioBuffer<float>& main, const juce::AudioBuffer<float>& sidechain,
                            const DuckerSettings& s, double bpm, double songBeats) noexcept
{
    const auto numSamples = main.getNumSamples();
    const auto beatsPerSample = juce::jmax (1.0, bpm) / 60.0 / sampleRate;
    const auto period = s.trigger == DuckerSettings::Trigger::everyHalfBeat   ? 0.5
                        : s.trigger == DuckerSettings::Trigger::everyTwoBeats ? 2.0
                                                                              : 1.0;
    if (songBeats < 0.0)
        lastBeatIndex = -1.0;

    float peak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        float level = 0.0f;
        for (int c = 0; c < sidechain.getNumChannels(); ++c)
            level = juce::jmax (level, std::abs (sidechain.getSample (c, i)));
        peak = juce::jmax (peak, level);

        bool hit = false;
        if (s.trigger == DuckerSettings::Trigger::sidechain)
        {
            hit = detectHit (level);
        }
        else if (songBeats >= 0.0)
        {
            // A new beat (or half beat, or two) has started. Starting playback on one counts too.
            const auto position = (songBeats + i * beatsPerSample) / period;
            const auto index = std::floor (position);
            hit = lastBeatIndex < 0.0 ? position - index < 0.02 : index > lastBeatIndex;
            lastBeatIndex = index;
        }

        if (hit)
            trigger();

        const auto g = nextGain (s);
        for (int c = 0; c < main.getNumChannels(); ++c)
            main.setSample (c, i, main.getSample (c, i) * g);
    }

    currentGain.store (gain, std::memory_order_relaxed);
    sidechainPeak.store (peak, std::memory_order_relaxed);
}

} // namespace spinus::dsp
