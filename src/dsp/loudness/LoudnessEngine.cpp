#include "LoudnessEngine.h"

#include "dsp/TruePeak.h"

namespace spinus::dsp
{

void LoudnessEngine::prepare (double rate)
{
    sampleRate = rate;

    // K-weighting for any sample rate (the BS.1770 filters, as derived by libebur128): a high shelf for the
    // head's effect, then a high pass.
    {
        const double f0 = 1681.974450955533, gainDb = 3.999843853973347, q = 0.7071752369554196;
        const auto k = std::tan (juce::MathConstants<double>::pi * f0 / rate);
        const auto vh = std::pow (10.0, gainDb / 20.0), vb = std::pow (vh, 0.4996667741545416);
        const auto a0 = 1.0 + k / q + k * k;
        shelf.b0 = (vh + vb * k / q + k * k) / a0;
        shelf.b1 = 2.0 * (k * k - vh) / a0;
        shelf.b2 = (vh - vb * k / q + k * k) / a0;
        shelf.a1 = 2.0 * (k * k - 1.0) / a0;
        shelf.a2 = (1.0 - k / q + k * k) / a0;
    }
    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const auto k = std::tan (juce::MathConstants<double>::pi * f0 / rate);
        const auto a0 = 1.0 + k / q + k * k;
        highPass.b0 = 1.0;
        highPass.b1 = -2.0;
        highPass.b2 = 1.0;
        highPass.a1 = 2.0 * (k * k - 1.0) / a0;
        highPass.a2 = (1.0 - k / q + k * k) / a0;
    }

    stepLength = juce::jmax (1, juce::roundToInt (rate / 10.0));
    reset();
}

void LoudnessEngine::reset() noexcept
{
    for (auto* f : { &shelf, &highPass })
        f->z1 = f->z2 = {};
    stepFill = 0;
    stepSum = 0.0;
    steps = {};
    stepsWritten = 0;
    histogram = {};
    binEnergy = {};
    recent = {};
    peak = 0.0f;
    for (auto* reading : { &momentary, &shortTerm, &integrated, &truePeakDb })
        reading->store (silence);
    measuredSeconds.store (0.0);
    historyWritten.store (0);
}

float LoudnessEngine::toLufs (double meanSquare) noexcept
{
    return meanSquare <= 1.0e-12 ? silence
                                 : (float)juce::jmax ((double)silence, -0.691 + 10.0 * std::log10 (meanSquare));
}

float LoudnessEngine::truePeakOf (float sample, size_t channel) noexcept
{
    // BS.1770-4's 4x oversampling with its own filter (dsp/TruePeak.h), the samples themselves too.
    auto& r = recent[channel];
    std::copy (r.begin() + 1, r.end(), r.begin());
    r.back() = sample;
    return juce::jmax (std::abs (sample), truepeak::peakOf (r.data()));
}

void LoudnessEngine::process (const juce::AudioBuffer<float>& buffer) noexcept
{
    if (resetRequested.exchange (false))
        reset();

    const auto channels = (size_t)juce::jmin (2, buffer.getNumChannels());
    if (channels == 0)
        return;

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        double sum = 0.0;
        for (size_t c = 0; c < channels; ++c)
        {
            const auto x = buffer.getSample ((int)c, i);
            const auto weighted = highPass.process (shelf.process ((double)x, c), c);
            sum += weighted * weighted;
            peak = juce::jmax (peak, truePeakOf (x, c));
        }
        stepSum += channels == 1 ? sum * 2.0 : sum; // mono plays from both speakers: counted twice (BS.1770)
        if (++stepFill == stepLength)
            endStep();
    }
    truePeakDb.store (peak > 0.0f ? juce::Decibels::gainToDecibels (peak, silence) : silence);
}

void LoudnessEngine::endStep() noexcept
{
    const auto meanSquare = stepSum / stepLength;
    stepSum = 0.0;
    stepFill = 0;
    std::copy (steps.begin() + 1, steps.end(), steps.begin());
    steps.back() = meanSquare;
    ++stepsWritten;
    measuredSeconds.store (stepsWritten / 10.0);

    auto meanOfLast = [this] (int count)
    {
        count = juce::jmin (count, stepsWritten);
        double sum = 0.0;
        for (int s = stepsPerShortTerm - count; s < stepsPerShortTerm; ++s)
            sum += steps[(size_t)s];
        return sum / count;
    };

    const auto block = meanOfLast (stepsPerMomentary);
    momentary.store (toLufs (block));
    shortTerm.store (toLufs (meanOfLast (stepsPerShortTerm)));
    if (stepsWritten >= stepsPerMomentary)
    {
        addGatingBlock (block); // 400 ms blocks overlapping by 75%
        updateIntegrated();
    }

    const auto written = historyWritten.load();
    history[(size_t)(written % historySteps)].store (shortTerm.load());
    historyWritten.store (written + 1);
}

void LoudnessEngine::addGatingBlock (double meanSquare) noexcept
{
    const auto lufs = toLufs (meanSquare);
    if (lufs <= histogramFloor) // the absolute gate
        return;
    const auto bin = juce::jlimit (0, histogramBins - 1, (int)((lufs - histogramFloor) / histogramStep));
    ++histogram[(size_t)bin];
    binEnergy[(size_t)bin] += meanSquare;
}

void LoudnessEngine::updateIntegrated() noexcept
{
    double sum = 0.0;
    juce::int64 count = 0;
    for (int b = 0; b < histogramBins; ++b)
    {
        sum += binEnergy[(size_t)b];
        count += histogram[(size_t)b];
    }
    if (count == 0)
        return;

    // The relative gate: blocks more than 10 LU below the mean so far don't count.
    const auto gate = toLufs (sum / (double)count) - 10.0f;
    const auto first =
        juce::jlimit (0, histogramBins - 1, (int)std::ceil ((gate - histogramFloor) / histogramStep - 0.5f));
    sum = 0.0;
    count = 0;
    for (int b = first; b < histogramBins; ++b)
    {
        sum += binEnergy[(size_t)b];
        count += histogram[(size_t)b];
    }
    if (count > 0)
        integrated.store (toLufs (sum / (double)count));
}

int LoudnessEngine::copyHistory (std::array<float, historySteps>& out) const noexcept
{
    const auto written = historyWritten.load();
    const auto count = juce::jmin (written, historySteps);
    for (int i = 0; i < count; ++i)
        out[(size_t)i] = history[(size_t)((written - count + i) % historySteps)].load();
    return count;
}

} // namespace spinus::dsp
