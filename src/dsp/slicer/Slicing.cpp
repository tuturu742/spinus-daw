#include "Slicing.h"

namespace spinus::dsp::slicing
{

namespace
{
constexpr int hop = 256;            // samples between level measurements
constexpr double minimumGap = 0.06; // seconds between two hits
} // namespace

std::vector<juce::int64> tidy (std::vector<juce::int64> points, juce::int64 length)
{
    std::erase_if (points, [length] (juce::int64 p) { return p <= 0 || p >= length; });
    points.push_back (0);
    std::sort (points.begin(), points.end());
    points.erase (std::unique (points.begin(), points.end()), points.end());
    if ((int)points.size() > maxSlices)
        points.resize ((size_t)maxSlices);
    return points;
}

std::vector<juce::int64> byGrid (juce::int64 length, double loopBeats, double beatsPerSlice)
{
    const auto count = juce::jlimit (1, maxSlices, (int)std::round (loopBeats / juce::jmax (1.0e-6, beatsPerSlice)));
    std::vector<juce::int64> points;
    points.reserve ((size_t)count);
    for (int i = 0; i < count; ++i)
        points.push_back ((juce::int64)std::round ((double)length * i / count));
    return tidy (std::move (points), length);
}

std::vector<juce::int64> byTransients (const juce::AudioBuffer<float>& audio, double sampleRate, float sensitivity)
{
    const auto length = (juce::int64)audio.getNumSamples();
    const auto channels = audio.getNumChannels();
    if (length < (juce::int64)hop * 4 || channels == 0)
        return { 0 };

    // The level (dB) of each hop.
    const auto frames = (int)(length / hop);
    std::vector<float> level ((size_t)frames);
    float loudest = -200.0f;
    for (int f = 0; f < frames; ++f)
    {
        double sum = 0.0;
        for (int c = 0; c < channels; ++c)
        {
            const auto* x = audio.getReadPointer (c, f * hop);
            for (int i = 0; i < hop; ++i)
                sum += (double)x[i] * x[i];
        }
        level[(size_t)f] = (float)(10.0 * std::log10 (sum / (hop * channels) + 1.0e-12));
        loudest = juce::jmax (loudest, level[(size_t)f]);
    }

    // A hit: the level jumps above its recent average by the threshold, at a local peak of the jump.
    const auto threshold = juce::jmap (juce::jlimit (0.0f, 1.0f, sensitivity), 14.0f, 3.0f);
    auto rise = [&] (int f)
    {
        if (f <= 0 || f >= frames)
            return 0.0f;
        float average = 0.0f;
        int n = 0;
        for (int k = juce::jmax (0, f - 8); k < f; ++k, ++n)
            average += level[(size_t)k];
        return level[(size_t)f] - average / (float)juce::jmax (1, n);
    };

    std::vector<juce::int64> points { 0 };
    const auto gapFrames = (int)(minimumGap * sampleRate / hop);
    int last = -gapFrames;
    for (int f = 1; f < frames; ++f)
    {
        const auto r = rise (f);
        if (r < threshold || r < rise (f - 1) || r < rise (f + 1) || level[(size_t)f] < loudest - 50.0f ||
            f - last < gapFrames)
            continue;
        last = f;

        // The hit starts in this hop or the one before: at the first sample that reaches a quarter of
        // their loudest, moved back to the last zero crossing (at most 1 ms) so the slice starts cleanly.
        const auto from = (juce::int64)(f - 1) * hop, to = std::min<juce::int64> (length, (juce::int64)(f + 1) * hop);
        auto magnitude = [&] (juce::int64 n)
        {
            float m = 0.0f;
            for (int c = 0; c < channels; ++c)
                m = juce::jmax (m, std::abs (audio.getSample (c, (int)n)));
            return m;
        };
        float peak = 0.0f;
        for (auto n = from; n < to; ++n)
            peak = juce::jmax (peak, magnitude (n));
        auto at = from;
        while (at < to - 1 && magnitude (at) < 0.25f * peak)
            ++at;

        const auto* x = audio.getReadPointer (0);
        const auto limit = std::max<juce::int64> (1, at - (juce::int64)(0.001 * sampleRate));
        while (at > limit && ! (x[at - 1] <= 0.0f && x[at] >= 0.0f) && ! (x[at - 1] >= 0.0f && x[at] <= 0.0f))
            --at;
        if (at > (juce::int64)(0.03 * sampleRate))
            points.push_back (at);
    }

    return tidy (std::move (points), length);
}

double estimateBeats (double seconds, double tempo, const juce::String& name)
{
    if (seconds <= 0.0)
        return 4.0;

    // "136bpm", "136 BPM", "136_bpm" in the name.
    const auto lower = name.toLowerCase();
    if (const auto at = lower.indexOf ("bpm"); at > 0)
    {
        auto digits = lower.substring (0, at).trimEnd().trimCharactersAtEnd ("_- ");
        int start = digits.length();
        while (start > 0 && juce::CharacterFunctions::isDigit (digits[start - 1]))
            --start;
        if (const auto bpm = digits.substring (start).getIntValue(); bpm >= 40 && bpm <= 300)
            return juce::jmax (1.0, std::round (seconds * bpm / 60.0));
    }

    double best = 4.0, bestDistance = 1.0e9;
    for (int power = 0; power <= 6; ++power)
    {
        const auto beats = std::exp2 ((double)power);
        const auto distance = std::abs (std::log2 (60.0 * beats / seconds / juce::jmax (1.0, tempo)));
        if (distance < bestDistance)
        {
            best = beats;
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace spinus::dsp::slicing
