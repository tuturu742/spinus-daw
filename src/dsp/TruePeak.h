#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace spinus::dsp::truepeak
{

/** ITU-R BS.1770-4 Annex 2's true-peak interpolation: 4x oversampling with its 48-tap filter, 12 taps for each
    of the 4 phases. Phase p's point after sample n is the sum over k of coefficients[p][k] * x[n - k]; the true
    peak is the largest magnitude of any phase's points. The Loudness Meter measures with it and the Limiter
    limits by it, so they agree with each other and with other BS.1770 meters. */
inline constexpr int phases = 4, taps = 12;

inline constexpr std::array<std::array<float, taps>, phases> coefficients { {
    { 0.0017089843750f, 0.0109863281250f, -0.0196533203125f, 0.0332031250000f, -0.0594482421875f, 0.1373291015625f,
      0.9721679687500f, -0.1022949218750f, 0.0476074218750f, -0.0266113281250f, 0.0148925781250f, -0.0083007812500f },
    { -0.0291748046875f, 0.0292968750000f, -0.0517578125000f, 0.0891113281250f, -0.1665039062500f, 0.4650878906250f,
      0.7797851562500f, -0.2003173828125f, 0.1015625000000f, -0.0582275390625f, 0.0330810546875f, -0.0189208984375f },
    { -0.0189208984375f, 0.0330810546875f, -0.0582275390625f, 0.1015625000000f, -0.2003173828125f, 0.7797851562500f,
      0.4650878906250f, -0.1665039062500f, 0.0891113281250f, -0.0517578125000f, 0.0292968750000f, -0.0291748046875f },
    { -0.0083007812500f, 0.0148925781250f, -0.0266113281250f, 0.0476074218750f, -0.1022949218750f, 0.9721679687500f,
      0.1373291015625f, -0.0594482421875f, 0.0332031250000f, -0.0196533203125f, 0.0109863281250f, 0.0017089843750f },
} };

/** The largest magnitude of the 4 points the last 12 samples make (`recent`: 12 samples, oldest first). */
inline float peakOf (const float* recent) noexcept
{
    float highest = 0.0f;
    for (const auto& phase : coefficients)
    {
        float y = 0.0f;
        for (size_t t = 0; t < (size_t)taps; ++t)
            y += recent[t] * phase[(size_t)taps - 1 - t];
        highest = std::max (highest, std::abs (y));
    }
    return highest;
}

} // namespace spinus::dsp::truepeak
