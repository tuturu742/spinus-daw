#include "CompressorEngine.h"

namespace spinus::dsp
{

float CompressorEngine::coefficient (float milliseconds, double sampleRate) noexcept
{
    return (float)std::exp (-1.0 / (juce::jmax (0.001, (double)milliseconds * 0.001) * sampleRate));
}

float CompressorEngine::staticCurve (const CompressorSettings& s, float x) noexcept
{
    const auto t = s.thresholdDb, r = juce::jmax (1.0f, s.ratio), w = juce::jmax (0.0f, s.kneeDb);
    const auto over = x - t;
    if (2.0f * over < -w)
        return x;
    if (w > 0.0f && 2.0f * std::abs (over) <= w)
        return x + (1.0f / r - 1.0f) * juce::square (over + w / 2.0f) / (2.0f * w);
    return t + over / r;
}

float CompressorEngine::autoMakeupDb (const CompressorSettings& s) noexcept { return -0.5f * staticCurve (s, 0.0f); }

void CompressorEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    rmsCoefficient = coefficient (10.0f, sampleRate);
    makeup.reset (sampleRate, 0.02);
    mix.reset (sampleRate, 0.02);
    reset();
}

void CompressorEngine::reset() noexcept
{
    envelopeDb = 0.0f;
    meanSquare = 0.0f;
    started = false;
    reductionNow.store (0.0f);
    reductionDeepest.store (0.0f);
    inputLevel.store (floorDb);
    sidechainPeak.store (0.0f);
}

void CompressorEngine::process (juce::AudioBuffer<float>& main, const juce::AudioBuffer<float>& sidechain,
                                const CompressorSettings& s) noexcept
{
    const auto numSamples = main.getNumSamples();
    const auto channels = juce::jmin (2, main.getNumChannels());
    const auto sideChannels = juce::jmin (2, sidechain.getNumChannels());
    const auto& detector = s.sidechain && sideChannels > 0 ? sidechain : main;
    const auto detectorChannels = s.sidechain && sideChannels > 0 ? sideChannels : channels;

    const auto makeupGain = juce::Decibels::decibelsToGain (s.makeupDb + (s.autoMakeup ? autoMakeupDb (s) : 0.0f));
    const auto wet = juce::jlimit (0.0f, 1.0f, s.mix);
    if (! started)
    {
        makeup.setCurrentAndTargetValue (makeupGain);
        mix.setCurrentAndTargetValue (wet);
        started = true;
    }
    makeup.setTargetValue (makeupGain);
    mix.setTargetValue (wet);

    const auto attack = coefficient (s.attackMs, sampleRate), release = coefficient (s.releaseMs, sampleRate);
    float deepest = 0.0f, level = floorDb, sidePeak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        float peak = 0.0f, squares = 0.0f;
        for (int c = 0; c < detectorChannels; ++c)
        {
            const auto x = detector.getSample (c, i);
            peak = juce::jmax (peak, std::abs (x));
            squares += x * x;
        }
        for (int c = 0; c < sideChannels; ++c)
            sidePeak = juce::jmax (sidePeak, std::abs (sidechain.getSample (c, i)));

        float detected = peak;
        if (s.rms)
        {
            meanSquare = squares / (float)juce::jmax (1, detectorChannels) +
                         rmsCoefficient * (meanSquare - squares / (float)juce::jmax (1, detectorChannels));
            detected = std::sqrt (meanSquare) * juce::MathConstants<float>::sqrt2; // a sine reads its peak
        }
        const auto inDb = juce::Decibels::gainToDecibels (detected, floorDb);
        level = inDb;
        const auto target = juce::jmin (0.0f, staticCurve (s, inDb) - inDb);
        envelopeDb = target + (target < envelopeDb ? attack : release) * (envelopeDb - target);
        deepest = juce::jmin (deepest, envelopeDb);

        const auto g = juce::Decibels::decibelsToGain (envelopeDb) * makeup.getNextValue();
        const auto m = mix.getNextValue();
        const auto gain = 1.0f - m + m * g;
        for (int c = 0; c < channels; ++c)
            main.setSample (c, i, main.getSample (c, i) * gain);
    }

    reductionNow.store (envelopeDb, std::memory_order_relaxed);
    reductionDeepest.store (deepest, std::memory_order_relaxed);
    inputLevel.store (level, std::memory_order_relaxed);
    sidechainPeak.store (sidePeak, std::memory_order_relaxed);
}

} // namespace spinus::dsp
