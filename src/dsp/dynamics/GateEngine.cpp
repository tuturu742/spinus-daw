#include "GateEngine.h"

#include "CompressorEngine.h"

namespace spinus::dsp
{

void GateEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    envelopeRelease = CompressorEngine::coefficient (10.0f, sampleRate); // the level detector's fall
    reset();
}

void GateEngine::reset() noexcept
{
    envelope = 0.0f;
    gain = 0.0f;
    open = false;
    holdLeft = 0;
    state.store ((int)State::closed);
    openings.store (0);
    gainNow.store (0.0f);
    levelNow.store (floorDb);
    sidechainPeak.store (0.0f);
}

void GateEngine::process (juce::AudioBuffer<float>& main, const juce::AudioBuffer<float>& sidechain,
                          const GateSettings& s) noexcept
{
    const auto numSamples = main.getNumSamples();
    const auto channels = juce::jmin (2, main.getNumChannels());
    const auto sideChannels = juce::jmin (2, sidechain.getNumChannels());
    const auto& detector = s.sidechain && sideChannels > 0 ? sidechain : main;
    const auto detectorChannels = s.sidechain && sideChannels > 0 ? sideChannels : channels;

    const auto openAt = juce::Decibels::decibelsToGain (s.thresholdDb, floorDb);
    const auto closeAt = juce::Decibels::decibelsToGain (s.thresholdDb - juce::jmax (0.0f, s.hysteresisDb), floorDb);
    const auto closedGain = s.rangeDb <= fullyClosedDb ? 0.0f : juce::Decibels::decibelsToGain (s.rangeDb);
    const auto attack = CompressorEngine::coefficient (s.attackMs, sampleRate);
    const auto release = CompressorEngine::coefficient (s.releaseMs, sampleRate);
    const auto holdSamples = (int)std::round (juce::jmax (0.0f, s.holdMs) * 0.001 * sampleRate);
    float sidePeak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        float peak = 0.0f;
        for (int c = 0; c < detectorChannels; ++c)
            peak = juce::jmax (peak, std::abs (detector.getSample (c, i)));
        for (int c = 0; c < sideChannels; ++c)
            sidePeak = juce::jmax (sidePeak, std::abs (sidechain.getSample (c, i)));
        envelope = peak >= envelope ? peak : peak + envelopeRelease * (envelope - peak);

        if (envelope >= openAt)
        {
            if (! open)
                openings.fetch_add (1, std::memory_order_relaxed);
            open = true;
            holdLeft = holdSamples;
        }
        else if (open)
        {
            if (envelope >= closeAt)
                holdLeft = holdSamples; // between the two thresholds: stays open, the hold has not begun
            else if (holdLeft > 0)
                --holdLeft;
            else
                open = false;
        }

        const auto target = open ? 1.0f : closedGain;
        gain = target + (target > gain ? attack : release) * (gain - target);
        if (std::abs (gain - target) < 1.0e-6f)
            gain = target;
        for (int c = 0; c < channels; ++c)
            main.setSample (c, i, main.getSample (c, i) * gain);
    }

    State now;
    const auto closedNow = std::abs (gain - closedGain) < 1.0e-3f;
    if (open)
        now = holdLeft < holdSamples ? State::holding : gain < 0.999f ? State::opening : State::open;
    else
        now = closedNow ? State::closed : State::closing;
    state.store ((int)now, std::memory_order_relaxed);
    gainNow.store (gain, std::memory_order_relaxed);
    levelNow.store (juce::Decibels::gainToDecibels (envelope, floorDb), std::memory_order_relaxed);
    sidechainPeak.store (sidePeak, std::memory_order_relaxed);
}

} // namespace spinus::dsp
