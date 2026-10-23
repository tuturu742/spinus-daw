#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <vector>

/** Reference true-peak meters for tests, independent of Spinus's own DSP. */
namespace spinus::test
{

/** The true peak as ITU-R BS.1770-4 Annex 2 measures it: 4x oversampling with the standard's 48-tap
    interpolation filter (12 taps per phase), the largest magnitude of any phase. Linear. */
inline double bs1770TruePeak (const std::vector<float>& x)
{
    static constexpr std::array<std::array<double, 12>, 4> h { {
        { 0.0017089843750, 0.0109863281250, -0.0196533203125, 0.0332031250000, -0.0594482421875, 0.1373291015625,
          0.9721679687500, -0.1022949218750, 0.0476074218750, -0.0266113281250, 0.0148925781250, -0.0083007812500 },
        { -0.0291748046875, 0.0292968750000, -0.0517578125000, 0.0891113281250, -0.1665039062500, 0.4650878906250,
          0.7797851562500, -0.2003173828125, 0.1015625000000, -0.0582275390625, 0.0330810546875, -0.0189208984375 },
        { -0.0189208984375, 0.0330810546875, -0.0582275390625, 0.1015625000000, -0.2003173828125, 0.7797851562500,
          0.4650878906250, -0.1665039062500, 0.0891113281250, -0.0517578125000, 0.0292968750000, -0.0291748046875 },
        { -0.0083007812500, 0.0148925781250, -0.0266113281250, 0.0476074218750, -0.1022949218750, 0.9721679687500,
          0.1373291015625, -0.0594482421875, 0.0332031250000, -0.0196533203125, 0.0109863281250, 0.0017089843750 },
    } };
    double peak = 0.0;
    const auto n = (int)x.size();
    for (int i = 0; i < n + 12; ++i) // and 12 zeros after, so the end rings out as a meter would show it
        for (auto& phase : h)
        {
            double y = 0.0;
            for (int k = 0; k < 12; ++k)
                if (const auto j = i - k; j >= 0 && j < n)
                    y += phase[(size_t)k] * (double)x[(size_t)j];
            peak = std::max (peak, std::abs (y));
        }
    return peak;
}

/** A true peak through a resampler's interpolation, as meters that resample to 192 kHz measure it (ffmpeg's
    ebur128): `oversampling` points per sample from a Kaiser-windowed sinc (beta 9, `halfLength` samples each
    side) passing up to `cutoff` of the Nyquist frequency. Linear. */
inline double resampledTruePeak (const std::vector<float>& x, int oversampling = 4, double cutoff = 0.97,
                                 int halfLength = 32)
{
    const auto n = (int)x.size();
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
    constexpr double beta = 9.0;
    const auto norm = bessel0 (beta);
    std::vector<std::vector<double>> taps ((size_t)oversampling, std::vector<double> ((size_t)(2 * halfLength)));
    for (int p = 0; p < oversampling; ++p)
        for (int k = 0; k < 2 * halfLength; ++k)
        {
            const auto d = (double)p / oversampling - (double)(k - halfLength + 1); // distance from sample k
            const auto sinc = std::abs (d) < 1.0e-12 ? cutoff : std::sin (M_PI * cutoff * d) / (M_PI * d);
            const auto r = d / (double)halfLength;
            const auto w = std::abs (r) >= 1.0 ? 0.0 : bessel0 (beta * std::sqrt (1.0 - r * r)) / norm;
            taps[(size_t)p][(size_t)k] = sinc * w;
        }
    double peak = 0.0;
    for (int i = -halfLength; i < n + halfLength; ++i) // the point between sample i and i + 1
        for (int p = 0; p < oversampling; ++p)
        {
            double y = 0.0;
            for (int k = 0; k < 2 * halfLength; ++k)
                if (const auto j = i + k - halfLength + 1; j >= 0 && j < n)
                    y += taps[(size_t)p][(size_t)k] * (double)x[(size_t)j];
            peak = std::max (peak, std::abs (y));
        }
    return peak;
}

inline double toDb (double gain) { return gain > 0.0 ? 20.0 * std::log10 (gain) : -200.0; }

} // namespace spinus::test
