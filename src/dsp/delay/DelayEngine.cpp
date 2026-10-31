#include "DelayEngine.h"

#include <cmath>

namespace spinus::dsp
{

void DelayEngine::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    size = (int)std::ceil (maxSeconds * sampleRate) + 8;
    for (auto& line : lines)
        line.assign ((size_t)size, 0.0f);
    fadeLength = juce::jmax (1, juce::roundToInt (crossfadeSeconds * sampleRate));
    for (auto* s : { &feedback, &dryGain, &wetGain, &width })
        s->reset (sampleRate, 0.05);
    reset();
}

void DelayEngine::reset() noexcept
{
    for (auto& line : lines)
        std::fill (line.begin(), line.end(), 0.0f);
    write = 0;
    lowCut.fill ({});
    highCut.fill ({});
    delayNow = delayNext = -1.0;
    fadeLeft = 0;
    smoothersStarted = false;
    timeNow = -1.0f;
}

double DelayEngine::timeMsFor (const DelaySettings& s, double bpm) noexcept
{
    if (! std::isfinite (bpm) || bpm <= 0.0)
        bpm = 120.0;
    const auto ms = s.sync ? s.beats * 60000.0 / bpm : (double)s.timeMs;
    return juce::jlimit (1.0, maxSeconds * 1000.0, std::isfinite (ms) ? ms : 1.0);
}

float DelayEngine::coefficientFor (float hz, double rate) noexcept
{
    const auto g = std::tan (juce::MathConstants<double>::pi * juce::jlimit (1.0, 0.45 * rate, (double)hz) / rate);
    return (float)(g / (1.0 + g));
}

float DelayEngine::read (int channel, double delaySamples) const noexcept
{
    const auto& line = lines[(size_t)channel];
    const auto at = (double)write - delaySamples;
    const auto whole = std::floor (at);
    const auto fraction = (float)(at - whole);
    auto i = (int)whole % size;
    if (i < 0)
        i += size;
    const auto j = i + 1 == size ? 0 : i + 1;
    return line[(size_t)i] + (line[(size_t)j] - line[(size_t)i]) * fraction;
}

void DelayEngine::process (juce::AudioBuffer<float>& buffer, const DelaySettings& s, double bpm) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    const auto channels = juce::jmin (maxChannels, buffer.getNumChannels());
    const auto n = buffer.getNumSamples();
    if (size == 0 || channels == 0 || n == 0)
        return;

    const auto target = juce::jlimit (1.0, maxSeconds * sampleRate, timeMsFor (s, bpm) * sampleRate / 1000.0);
    if (delayNow < 0.0)
        delayNow = target;

    const auto mix = juce::jlimit (0.0f, 1.0f, s.mix);
    const auto dry = juce::jmin (1.0f, 2.0f * (1.0f - mix)), wet = juce::jmin (1.0f, 2.0f * mix);
    const auto amount = juce::jlimit (0.0f, 0.95f, s.feedback), spread = juce::jlimit (0.0f, 1.0f, s.width);
    if (! smoothersStarted)
    {
        feedback.setCurrentAndTargetValue (amount);
        dryGain.setCurrentAndTargetValue (dry);
        wetGain.setCurrentAndTargetValue (wet);
        width.setCurrentAndTargetValue (spread);
        smoothersStarted = true;
    }
    else
    {
        feedback.setTargetValue (amount);
        dryGain.setTargetValue (dry);
        wetGain.setTargetValue (wet);
        width.setTargetValue (spread);
    }
    const auto gLow = coefficientFor (s.lowCutHz, sampleRate), gHigh = coefficientFor (s.highCutHz, sampleRate);
    const auto useLowCut = s.lowCutHz > DelaySettings::lowCutOff, useHighCut = s.highCutHz < DelaySettings::highCutOff;
    if (! useLowCut)
        lowCut.fill ({});
    if (! useHighCut)
        highCut.fill ({});

    auto* left = buffer.getWritePointer (0);
    auto* right = channels > 1 ? buffer.getWritePointer (1) : nullptr;
    const auto mode = right == nullptr ? DelaySettings::Mode::stereo : s.mode;

    for (int k = 0; k < n; ++k)
    {
        // A new time: fade from the echo at the old one to the echo at the new one.
        if (fadeLeft == 0 && ! juce::exactlyEqual (target, delayNow))
        {
            delayNext = target;
            fadeLeft = fadeLength;
        }

        std::array<float, maxChannels> echo {};
        for (int c = 0; c < channels; ++c)
        {
            auto tap = read (c, delayNow);
            if (fadeLeft > 0)
            {
                const auto toNew = 1.0f - (float)fadeLeft / (float)fadeLength;
                tap += (read (c, delayNext) - tap) * toNew;
            }
            const auto cs = (size_t)c;
            if (useLowCut)
                tap -= lowCut[cs].lowPass (tap, gLow);
            echo[cs] = useHighCut ? highCut[cs].lowPass (tap, gHigh) : tap;
        }
        if (fadeLeft > 0)
        {
            --fadeLeft;
            if (fadeLeft == 0)
                delayNow = delayNext;
        }

        const auto fb = feedback.getNextValue();
        const auto inL = left[k], inR = right != nullptr ? right[k] : 0.0f;
        switch (mode)
        {
        case DelaySettings::Mode::stereo:
            lines[0][(size_t)write] = inL + fb * echo[0];
            if (right != nullptr)
                lines[1][(size_t)write] = inR + fb * echo[1];
            break;
        case DelaySettings::Mode::mono:
        {
            const auto sum = 0.5f * (inL + inR);
            lines[0][(size_t)write] = sum + fb * echo[0];
            lines[1][(size_t)write] = sum + fb * echo[1];
            break;
        }
        case DelaySettings::Mode::pingPong:
            // Into the left line only; each line feeds the other, so the echoes alternate sides.
            lines[0][(size_t)write] = 0.5f * (inL + inR) + fb * echo[1];
            lines[1][(size_t)write] = fb * echo[0];
            break;
        }
        write = write + 1 == size ? 0 : write + 1;

        const auto d = dryGain.getNextValue(), w = wetGain.getNextValue(), wd = width.getNextValue();
        if (right == nullptr)
        {
            left[k] = d * inL + w * echo[0];
            continue;
        }
        const auto mid = 0.5f * (echo[0] + echo[1]), side = 0.5f * (echo[0] - echo[1]) * wd;
        left[k] = d * inL + w * (mid + side);
        right[k] = d * inR + w * (mid - side);
    }

    // No denormals left in the filters when the echoes have died away.
    for (auto* f : { &lowCut, &highCut })
        for (auto& stage : *f)
            if (std::abs (stage.s) < 1.0e-20f)
                stage.s = 0.0f;

    timeNow = (float)(delayNow * 1000.0 / sampleRate);
    bpmNow = (float)(std::isfinite (bpm) && bpm > 0.0 ? bpm : 120.0);
}

} // namespace spinus::dsp
