#include "PitchDetector.h"

namespace spinus::dsp
{

void PitchDetector::prepare (double rate, float minHz, float maxHz)
{
    sampleRate = rate;
    maxLag = (int)std::ceil (rate / minHz);
    minLag = juce::jmax (2, (int)std::floor (rate / maxHz));
    windowSize = maxLag;

    // The correlation of the window with the whole frame, without wrapping around.
    const auto order = juce::roundToInt (std::ceil (std::log2 ((double)(windowSize + maxLag))));
    fft = std::make_unique<juce::dsp::FFT> (order);
    windowSpectrum.assign ((size_t)fft->getSize() * 2, 0.0f);
    frameSpectrum.assign ((size_t)fft->getSize() * 2, 0.0f);
    difference.assign ((size_t)maxLag + 2, 0.0f);
}

PitchDetector::Result PitchDetector::analyse (const float* frame) noexcept
{
    const auto size = fft->getSize();
    const auto frameLength = getFrameLength();

    // d(tau) = sum (x[j] - x[j + tau])^2 over the window
    //        = energy of the window + energy of the window moved by tau - 2 * correlation(tau)
    std::fill (windowSpectrum.begin(), windowSpectrum.end(), 0.0f);
    std::fill (frameSpectrum.begin(), frameSpectrum.end(), 0.0f);
    std::copy (frame, frame + windowSize, windowSpectrum.begin());
    std::copy (frame, frame + frameLength, frameSpectrum.begin());

    fft->performRealOnlyForwardTransform (windowSpectrum.data());
    fft->performRealOnlyForwardTransform (frameSpectrum.data());

    // conj(window) * frame, then back: the correlation for every lag.
    for (size_t k = 0; k <= (size_t)size / 2; ++k)
    {
        const auto ar = windowSpectrum[2 * k], ai = windowSpectrum[2 * k + 1];
        const auto br = frameSpectrum[2 * k], bi = frameSpectrum[2 * k + 1];
        frameSpectrum[2 * k] = ar * br + ai * bi;
        frameSpectrum[2 * k + 1] = ar * bi - ai * br;
    }
    fft->performRealOnlyInverseTransform (frameSpectrum.data());
    const auto* correlation = frameSpectrum.data();

    double windowEnergy = 0.0;
    for (int j = 0; j < windowSize; ++j)
        windowEnergy += (double)frame[j] * frame[j];
    if (windowEnergy < 1.0e-6 * windowSize || correlation[0] <= 0.0f)
        return {}; // silence

    // FFT engines scale their inverse differently: the correlation at lag 0 is the window's energy.
    const auto scale = windowEnergy / (double)correlation[0];

    double movedEnergy = windowEnergy, runningSum = 0.0;
    difference[0] = 1.0f;
    for (int tau = 1; tau <= maxLag; ++tau)
    {
        movedEnergy +=
            (double)frame[tau + windowSize - 1] * frame[tau + windowSize - 1] - (double)frame[tau - 1] * frame[tau - 1];
        const auto d = juce::jmax (0.0, windowEnergy + movedEnergy - 2.0 * scale * (double)correlation[tau]);
        runningSum += d;
        // The cumulative mean normalised difference.
        difference[(size_t)tau] = runningSum > 0.0 ? (float)(d * tau / runningSum) : 1.0f;
    }

    // The first dip under the threshold, followed to the bottom of that dip.
    int best = -1;
    for (int tau = minLag; tau < maxLag; ++tau)
        if (difference[(size_t)tau] < threshold)
        {
            while (tau + 1 < maxLag && difference[(size_t)tau + 1] < difference[(size_t)tau])
                ++tau;
            best = tau;
            break;
        }

    if (best < 0)
        return { 0.0f,
                 juce::jlimit (0.0f, 1.0f,
                               1.0f - *std::min_element (difference.begin() + minLag, difference.begin() + maxLag)) };

    // Between samples: the bottom of a parabola through the three points.
    auto period = (float)best;
    if (best > 1 && best < maxLag)
    {
        const auto a = difference[(size_t)best - 1], b = difference[(size_t)best], c = difference[(size_t)best + 1];
        const auto denominator = a - 2.0f * b + c;
        if (std::abs (denominator) > 1.0e-9f)
            period += juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / denominator);
    }

    return { period, juce::jlimit (0.0f, 1.0f, 1.0f - difference[(size_t)best]) };
}

} // namespace spinus::dsp
