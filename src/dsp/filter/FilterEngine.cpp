#include "FilterEngine.h"

#include <complex>

namespace spinus::dsp
{

namespace
{
constexpr int chunk = 16; // samples between cutoff updates
constexpr float octavesPerAmount = 4.0f;
constexpr float minCutoff = 20.0f;

/** The follower's level in decibels mapped to 0..1 (-60 dB and below: 0, 0 dB: 1). */
float envelopeLevel (float linear) noexcept
{
    return juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (linear, -60.0f) + 60.0f) / 60.0f);
}
} // namespace

void FilterEngine::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    logCutoff.reset (sampleRate / chunk, 0.02);
    mix.reset (sampleRate, 0.02);
    gain.reset (sampleRate, 0.02);
    reset();
}

void FilterEngine::reset()
{
    for (auto& channel : stages)
        channel.fill ({});
    lfoPhase = 0.0;
    randomLevel = 0.0f;
    random.setSeed (randomSeed);
    envelope = 0.0f;
    smoothersStarted = false;
    modulatedCutoff.store (-1.0f);
}

float FilterEngine::lfoValue (FilterSettings::LfoShape shape, double phase, float level) noexcept
{
    const auto p = (float)(phase - std::floor (phase));
    switch (shape)
    {
    case FilterSettings::LfoShape::sine:
        return std::sin (juce::MathConstants<float>::twoPi * p);
    case FilterSettings::LfoShape::triangle:
        return p < 0.25f ? 4.0f * p : p < 0.75f ? 2.0f - 4.0f * p : 4.0f * p - 4.0f;
    case FilterSettings::LfoShape::rampUp:
        return 2.0f * p - 1.0f;
    case FilterSettings::LfoShape::rampDown:
        return 1.0f - 2.0f * p;
    case FilterSettings::LfoShape::square:
        return p < 0.5f ? 1.0f : -1.0f;
    case FilterSettings::LfoShape::random:
        return level;
    }
    return 0.0f;
}

std::array<float, 2> FilterEngine::dampingFor (const FilterSettings& s) noexcept
{
    // The resonant stage goes from Butterworth (no peak) towards a damping of 0.05 (Q 20),
    // exponentially so the knob feels even.
    auto resonant = [&] (float base) { return base * std::pow (0.05f / base, juce::jlimit (0.0f, 1.0f, s.resonance)); };
    if (s.steep)
        return { 1.8478f, resonant (0.7654f) };
    return { resonant (1.4142f), 0.0f };
}

FilterEngine::Coefficients FilterEngine::coefficientsFor (float cutoffHz, float k, double sampleRate) noexcept
{
    const auto g = (float)std::tan (juce::MathConstants<double>::pi * cutoffHz / sampleRate);
    Coefficients c;
    c.k = k;
    c.a1 = 1.0f / (1.0f + g * (g + k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
}

float FilterEngine::tick (Stage& s, const Coefficients& c, FilterSettings::Type type, float v0) noexcept
{
    const auto v3 = v0 - s.ic2;
    const auto v1 = c.a1 * s.ic1 + c.a2 * v3;
    const auto v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
    s.ic1 = 2.0f * v1 - s.ic1;
    s.ic2 = 2.0f * v2 - s.ic2;

    switch (type)
    {
    case FilterSettings::Type::lowPass:
        return v2;
    case FilterSettings::Type::highPass:
        return v0 - c.k * v1 - v2;
    case FilterSettings::Type::bandPass:
        return c.k * v1; // peak gain 1 at any resonance
    case FilterSettings::Type::notch:
        return v0 - c.k * v1;
    }
    return v2;
}

float FilterEngine::magnitudeAt (const FilterSettings& s, float cutoffHz, float frequencyHz) noexcept
{
    const std::complex<float> z (0.0f, frequencyHz / juce::jmax (1.0f, cutoffHz)); // s = jw, normalised
    const auto damping = dampingFor (s);
    float result = 1.0f;
    for (int stage = 0; stage < (s.steep ? 2 : 1); ++stage)
    {
        const auto k = damping[(size_t)stage];
        const auto denominator = z * z + k * z + 1.0f;
        std::complex<float> numerator;
        switch (s.type)
        {
        case FilterSettings::Type::lowPass:
            numerator = 1.0f;
            break;
        case FilterSettings::Type::highPass:
            numerator = z * z;
            break;
        case FilterSettings::Type::bandPass:
            numerator = k * z;
            break;
        case FilterSettings::Type::notch:
            numerator = z * z + 1.0f;
            break;
        }
        result *= std::abs (numerator / denominator);
    }
    return result;
}

void FilterEngine::process (juce::AudioBuffer<float>& buffer, const FilterSettings& s, double bpm,
                            double songBeats) noexcept
{
    const auto numChannels = juce::jmin (maxChannels, buffer.getNumChannels());
    const auto numSamples = buffer.getNumSamples();
    if (numChannels == 0 || numSamples == 0)
        return;

    const auto targetLogCutoff = std::log2 (juce::jlimit (minCutoff, 20000.0f, s.cutoffHz));
    if (! smoothersStarted)
    {
        logCutoff.setCurrentAndTargetValue (targetLogCutoff);
        mix.setCurrentAndTargetValue (s.mix);
        gain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (s.gainDb));
        smoothersStarted = true;
    }
    logCutoff.setTargetValue (targetLogCutoff);
    mix.setTargetValue (juce::jlimit (0.0f, 1.0f, s.mix));
    gain.setTargetValue (juce::Decibels::decibelsToGain (s.gainDb));

    // The LFO: synced to the song while it plays, free-running otherwise.
    const auto beatsPerCycle = juce::jmax (1.0 / 64.0, s.lfoBeats);
    const auto cyclesPerSecond = s.lfoSync ? juce::jmax (1.0, bpm) / 60.0 / beatsPerCycle : (double)s.lfoRateHz;
    const auto phaseStep = cyclesPerSecond / sampleRate;
    if (s.lfoSync && songBeats >= 0.0)
    {
        const auto phase = songBeats / beatsPerCycle;
        if (phase - std::floor (phase) < lfoPhase - std::floor (lfoPhase))
            randomLevel = random.nextFloat() * 2.0f - 1.0f;
        lfoPhase = phase - std::floor (phase);
    }

    const auto attack = std::exp (-1.0f / (juce::jmax (0.0001f, s.envelopeAttack) * (float)sampleRate));
    const auto release = std::exp (-1.0f / (juce::jmax (0.001f, s.envelopeRelease) * (float)sampleRate));
    const auto damping = dampingFor (s);
    const auto nyquistSafe = (float)(sampleRate * 0.45);
    float cutoff = s.cutoffHz;

    for (int start = 0; start < numSamples; start += chunk)
    {
        const auto count = juce::jmin (chunk, numSamples - start);

        const auto octaves = octavesPerAmount * (s.lfoAmount * lfoValue (s.lfoShape, lfoPhase, randomLevel) +
                                                 s.envelopeAmount * envelopeLevel (envelope));
        cutoff = juce::jlimit (minCutoff, nyquistSafe, std::exp2 (logCutoff.skip (1) + octaves));
        const Coefficients first = coefficientsFor (cutoff, damping[0], sampleRate);
        const Coefficients second = coefficientsFor (cutoff, damping[1], sampleRate);

        for (int i = start; i < start + count; ++i)
        {
            float level = 0.0f;
            for (int c = 0; c < numChannels; ++c)
                level = juce::jmax (level, std::abs (buffer.getSample (c, i)));
            envelope = level + (level > envelope ? attack : release) * (envelope - level);

            const auto wet = mix.getNextValue();
            const auto out = gain.getNextValue();
            for (int c = 0; c < numChannels; ++c)
            {
                auto& channelStages = stages[(size_t)c];
                const auto dry = buffer.getSample (c, i);
                auto y = tick (channelStages[0], first, s.type, dry);
                if (s.steep)
                    y = tick (channelStages[1], second, s.type, y);
                buffer.setSample (c, i, (dry + wet * (y - dry)) * out);
            }
        }

        lfoPhase += phaseStep * count;
        if (lfoPhase >= 1.0)
        {
            lfoPhase -= std::floor (lfoPhase);
            randomLevel = random.nextFloat() * 2.0f - 1.0f;
        }
    }

    modulatedCutoff.store (cutoff, std::memory_order_relaxed);
}

} // namespace spinus::dsp
