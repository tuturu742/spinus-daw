#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace spinus::test::checks
{

/** Finds samples where a steady sine wave deviates from its own recurrence
    x[n] = 2 cos(w) x[n-1] - x[n-2]. Any dropout, click or discontinuity shows up as a
    large residual. Returns the indices of offending samples (at most maxReported). */
inline std::vector<int64_t> findSineDiscontinuities (const std::vector<float>& x, int64_t begin, int64_t end,
                                                     double frequency, double sampleRate, float tolerance = 0.01f,
                                                     size_t maxReported = 20)
{
    std::vector<int64_t> bad;
    const auto k = (float)(2.0 * std::cos (2.0 * M_PI * frequency / sampleRate));

    for (auto n = std::max<int64_t> (begin, 2); n < end; ++n)
    {
        const auto predicted = k * x[(size_t)n - 1] - x[(size_t)n - 2];

        if (std::abs (x[(size_t)n] - predicted) > tolerance)
        {
            bad.push_back (n);

            if (bad.size() >= maxReported)
                break;
        }
    }

    return bad;
}

inline float rms (const std::vector<float>& x, int64_t begin, int64_t end)
{
    double sum = 0.0;

    for (auto n = begin; n < end; ++n)
        sum += (double)x[(size_t)n] * x[(size_t)n];

    return end > begin ? (float)std::sqrt (sum / (double)(end - begin)) : 0.0f;
}

/** First index at or after begin where |x| exceeds threshold, or -1. */
inline int64_t findOnset (const std::vector<float>& x, int64_t begin, int64_t end, float threshold = 0.001f)
{
    for (auto n = begin; n < end; ++n)
        if (std::abs (x[(size_t)n]) > threshold)
            return n;

    return -1;
}

} // namespace spinus::test::checks
