#include "LimiterEngine.h"

#include "CompressorEngine.h"
#include "dsp/TruePeak.h"

namespace spinus::dsp
{

namespace
{
constexpr float floorDb = -120.0f;
}

void LimiterEngine::SlidingMinimum::allocate (int n)
{
    capacity = juce::jmax (2, n);
    index.assign ((size_t)capacity, 0);
    value.assign ((size_t)capacity, 1.0f);
    clear();
}

void LimiterEngine::SlidingMinimum::push (juce::int64 at, float v, int window) noexcept
{
    // A monotonic queue: each value stays only while nothing newer is smaller.
    while (size > 0 && value[(size_t)((head + size - 1) % capacity)] >= v)
        --size;
    const auto back = (size_t)((head + size) % capacity);
    index[back] = at;
    value[back] = v;
    ++size;
    while (index[(size_t)head] <= at - window)
    {
        head = (head + 1) % capacity;
        --size;
    }
}

int LimiterEngine::lookaheadSamplesFor (float ms, double rate) noexcept
{
    const auto most = (int)std::ceil (maxLookaheadMs * 0.001 * rate);
    return juce::jlimit (1, most, (int)std::round (juce::jlimit (0.0f, maxLookaheadMs, ms) * 0.001 * rate));
}

int LimiterEngine::latencyFor (float ms, double rate) noexcept
{
    return lookaheadSamplesFor (ms, rate) + truePeakSpan - 1;
}

void LimiterEngine::setLookahead (float ms) noexcept { lookaheadMs.store (ms); }

void LimiterEngine::prepare (double newSampleRate, float ms)
{
    sampleRate = newSampleRate;
    lookaheadMs.store (ms);
    const auto most = lookaheadSamplesFor (maxLookaheadMs, sampleRate);
    delaySize = most + truePeakSpan + 2;
    for (auto& d : delay)
        d.assign ((size_t)delaySize, 0.0f);
    hold.allocate (most + truePeakSpan + 2);
    box.assign ((size_t)most, 1.0f);
    input.reset (sampleRate, 0.02);

    // The finer interpolation: a Kaiser-windowed sinc (beta 8) over interpolationTaps samples, finePhases points
    // between the middle two. Closer to the sound between the samples than BS.1770's short filter near the top of
    // the spectrum, so meters with a longer interpolation (ffmpeg's ebur128, which resamples to 192 kHz) agree.
    auto bessel0 = [] (double v)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 30; ++k)
        {
            term *= (v / (2.0 * k)) * (v / (2.0 * k));
            sum += term;
        }
        return sum;
    };
    constexpr double beta = 8.0, half = interpolationTaps / 2.0;
    for (int p = 0; p < finePhases; ++p)
        for (int t = 0; t < interpolationTaps; ++t)
        {
            const auto d = half - 1.0 + (double)p / finePhases - (double)t;
            const auto sinc = std::abs (d) < 1.0e-9 ? 1.0
                                                    : std::sin (juce::MathConstants<double>::pi * d) /
                                                          (juce::MathConstants<double>::pi * d);
            const auto r = d / half;
            const auto window = std::abs (r) >= 1.0 ? 0.0 : bessel0 (beta * std::sqrt (1.0 - r * r)) / bessel0 (beta);
            fine[(size_t)p][(size_t)t] = (float)(sinc * window);
        }

    reset();
}

void LimiterEngine::reset() noexcept
{
    for (auto& d : delay)
        std::fill (d.begin(), d.end(), 0.0f);
    recent = {};
    write = 0;
    counter = 0;
    envelope = 1.0f;
    active = -1;
    started = false;
    reductionNow.store (0.0f);
    reductionDeepest.store (0.0f);
    inputPeak.store (floorDb);
    outputPeak.store (floorDb);
}

void LimiterEngine::configure (int lookaheadSamples) noexcept
{
    lookahead = juce::jlimit (1, (int)box.size(), lookaheadSamples);
    hold.clear();
    std::fill (box.begin(), box.begin() + lookahead, envelope);
    boxSum = (double)envelope * lookahead;
    boxWrite = 0;
    active = lookaheadSamples;
}

float LimiterEngine::interpolatedPeak (int channel) const noexcept
{
    const auto& r = recent[(size_t)channel];
    float highest = truepeak::peakOf (r.data() + interpolationTaps / 2 - truepeak::taps / 2);
    for (size_t p = 1; p < (size_t)finePhases; ++p)
    {
        float y = 0.0f;
        for (size_t t = 0; t < (size_t)interpolationTaps; ++t)
            y += r[t] * fine[p][t];
        highest = juce::jmax (highest, std::abs (y));
    }
    return highest;
}

void LimiterEngine::process (juce::AudioBuffer<float>& buffer, const LimiterSettings& s) noexcept
{
    if (delay[0].empty())
        return;
    if (const auto wanted = lookaheadSamplesFor (lookaheadMs.load (std::memory_order_relaxed), sampleRate);
        wanted != active)
        configure (wanted);

    const auto numSamples = buffer.getNumSamples();
    const auto channels = juce::jmin (2, buffer.getNumChannels());
    const auto ceiling = juce::Decibels::decibelsToGain (juce::jlimit (-60.0f, 0.0f, s.ceilingDb));
    const auto inputGain = juce::Decibels::decibelsToGain (s.inputDb);
    if (! started)
        input.setCurrentAndTargetValue (inputGain);
    input.setTargetValue (inputGain);
    started = true;
    const auto release = CompressorEngine::coefficient (s.releaseMs, sampleRate);
    const auto window = lookahead + truePeakSpan;
    const auto outputDelay = lookahead + truePeakSpan - 1;

    float lowest = 1.0f, inPeak = 0.0f, outPeak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const auto g = input.getNextValue();
        float pointPeak = 0.0f;
        for (int c = 0; c < channels; ++c)
        {
            const auto x = buffer.getSample (c, i) * g;
            inPeak = juce::jmax (inPeak, std::abs (x));
            delay[(size_t)c][(size_t)write] = x;
            auto& r = recent[(size_t)c];
            std::copy (r.begin() + 1, r.end(), r.begin());
            r.back() = x;
            if (s.truePeak)
                pointPeak = juce::jmax (pointPeak, interpolatedPeak (c));
        }

        // What the sample truePeakSpan back needs, with the points between samples it helps make.
        const auto older = (write - truePeakSpan + delaySize) % delaySize;
        float level = pointPeak;
        for (int c = 0; c < channels; ++c)
            level = juce::jmax (level, std::abs (delay[(size_t)c][(size_t)older]));
        const auto need = level > ceiling ? ceiling / level : 1.0f;

        hold.push (counter, need, window);
        const auto held = hold.minimum();
        envelope = held < envelope ? held : envelope + (held - envelope) * (1.0f - release);

        boxSum += (double)envelope - (double)box[(size_t)boxWrite];
        box[(size_t)boxWrite] = envelope;
        if (++boxWrite == lookahead)
        {
            boxWrite = 0;
            boxSum = 0.0; // added up again once per window, so rounding cannot build up
            for (int k = 0; k < lookahead; ++k)
                boxSum += (double)box[(size_t)k];
        }
        const auto gain = (float)juce::jmin (1.0, boxSum / lookahead);
        lowest = juce::jmin (lowest, gain);

        const auto from = (write - outputDelay + delaySize) % delaySize;
        for (int c = 0; c < channels; ++c)
        {
            const auto y = juce::jlimit (-ceiling, ceiling, delay[(size_t)c][(size_t)from] * gain);
            outPeak = juce::jmax (outPeak, std::abs (y));
            buffer.setSample (c, i, y);
        }

        write = (write + 1) % delaySize;
        ++counter;
    }

    const auto reduction = juce::Decibels::gainToDecibels (lowest, floorDb);
    reductionNow.store (juce::Decibels::gainToDecibels ((float)(boxSum / lookahead), floorDb),
                        std::memory_order_relaxed);
    reductionDeepest.store (reduction, std::memory_order_relaxed);
    inputPeak.store (juce::Decibels::gainToDecibels (inPeak, floorDb), std::memory_order_relaxed);
    outputPeak.store (juce::Decibels::gainToDecibels (outPeak, floorDb), std::memory_order_relaxed);
}

} // namespace spinus::dsp
