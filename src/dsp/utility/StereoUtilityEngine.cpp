#include "StereoUtilityEngine.h"

namespace spinus::dsp
{

void StereoUtilityEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    for (auto* s : { &gain, &leftOfPan, &rightOfPan, &sideGain, &leftSign, &rightSign })
        s->reset (sampleRate, 0.02);
    reset();
}

void StereoUtilityEngine::reset()
{
    started = false;
    for (auto& p : peaks)
        p.store (0.0f);
    correlation.store (1.0f);
}

void StereoUtilityEngine::process (juce::AudioBuffer<float>& buffer, const StereoUtilitySettings& s) noexcept
{
    const auto targets = std::array {
        juce::Decibels::decibelsToGain (s.gainDb, -100.0f),
        juce::jmin (1.0f, 1.0f - s.pan), // balance: the far side goes down
        juce::jmin (1.0f, 1.0f + s.pan),
        s.mono ? 0.0f : juce::jlimit (0.0f, 2.0f, s.width),
        s.invertLeft ? -1.0f : 1.0f,
        s.invertRight ? -1.0f : 1.0f,
    };
    const auto smoothers = std::array { &gain, &leftOfPan, &rightOfPan, &sideGain, &leftSign, &rightSign };
    for (size_t i = 0; i < smoothers.size(); ++i)
    {
        if (! started)
            smoothers[i]->setCurrentAndTargetValue (targets[i]);
        smoothers[i]->setTargetValue (targets[i]);
    }
    started = true;

    const auto numSamples = buffer.getNumSamples();
    float peakL = 0.0f, peakR = 0.0f;
    double lr = 0.0, ll = 0.0, rr = 0.0;

    if (buffer.getNumChannels() < 2)
    {
        // Mono: only the gain and the polarity apply.
        auto* x = buffer.getNumChannels() > 0 ? buffer.getWritePointer (0) : nullptr;
        for (int i = 0; x != nullptr && i < numSamples; ++i)
        {
            leftOfPan.getNextValue();
            rightOfPan.getNextValue();
            sideGain.getNextValue();
            rightSign.getNextValue();
            x[i] *= gain.getNextValue() * leftSign.getNextValue();
            peakL = peakR = juce::jmax (peakL, std::abs (x[i]));
        }
        peaks[0].store (peakL);
        peaks[1].store (peakR);
        correlation.store (1.0f);
        return;
    }

    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getWritePointer (1);
    for (int i = 0; i < numSamples; ++i)
    {
        const auto l = left[i] * leftSign.getNextValue();
        const auto r = right[i] * rightSign.getNextValue();
        const auto mid = 0.5f * (l + r);
        const auto side = 0.5f * (l - r) * sideGain.getNextValue();
        const auto g = gain.getNextValue();
        left[i] = (mid + side) * leftOfPan.getNextValue() * g;
        right[i] = (mid - side) * rightOfPan.getNextValue() * g;

        peakL = juce::jmax (peakL, std::abs (left[i]));
        peakR = juce::jmax (peakR, std::abs (right[i]));
        lr += (double)left[i] * right[i];
        ll += (double)left[i] * left[i];
        rr += (double)right[i] * right[i];
    }

    peaks[0].store (peakL);
    peaks[1].store (peakR);
    if (ll > 1.0e-12 && rr > 1.0e-12)
        correlation.store ((float)(lr / std::sqrt (ll * rr)));
}

} // namespace spinus::dsp
