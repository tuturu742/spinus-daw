#include "StemAutomation.h"

#include <juce_dsp/juce_dsp.h>
#include <tracktion_engine/tracktion_engine.h>

#include <algorithm>
#include <cmath>

namespace spinus::stemautomation
{

namespace
{
constexpr double silentBelow = 40.0; // dB under the stem's loudest beat: not playing
constexpr double volumeChange = 3.0; // dB between its quiet and loud parts, at least, for a volume curve
constexpr double panChange = 0.15;   // of the stereo width, at least, for a pan curve
constexpr double offCentre = 0.1;    // a steady pan nearer the centre than this stays centred
constexpr model::Ticks grid = model::ticksPerQuarterNote / 4;
constexpr double toneChange = 1.5;  // octaves between its dull and bright parts, at least, for a cutoff curve
constexpr size_t toneBeats = 16;    // the tone smoothed over four bars
constexpr double toneBelow = 20.0;  // dB under its loudest beat: too quiet to tell its tone (other stems' bleed)
constexpr double openWithin = 0.25; // octaves under its bright parts: the filter stays open
constexpr double closedFrom = 1.0;  // octaves under its bright parts: the cutoff at twice its brightness
constexpr double lowestCutoff = 150.0, highestCutoff = 20000.0;
constexpr int fftOrder = 11; // 2048-sample frames for the brightness

/** Spectral centroid (Hz) of a mono mix of `from` to `to`, by magnitude (as brightness is usually measured: by
    power, a rich sound's fundamental outweighs its harmonics); 0 when silent. */
double centroidOf (const juce::AudioBuffer<float>& stem, int from, int to, double sampleRate)
{
    constexpr int size = 2048; // 2 ^ fftOrder
    static thread_local juce::dsp::FFT fft (fftOrder);
    static thread_local juce::dsp::WindowingFunction<float> window (size, juce::dsp::WindowingFunction<float>::hann);
    std::vector<float> frame ((size_t)(2 * size));
    double weighted = 0.0, power = 0.0;
    const auto channels = stem.getNumChannels();
    for (auto start = from; start + size <= to; start += size)
    {
        std::fill (frame.begin(), frame.end(), 0.0f);
        for (int c = 0; c < channels; ++c)
        {
            const auto* x = stem.getReadPointer (c) + start;
            for (int i = 0; i < size; ++i)
                frame[(size_t)i] += x[i] / (float)channels;
        }
        window.multiplyWithWindowingTable (frame.data(), size);
        fft.performFrequencyOnlyForwardTransform (frame.data(), true);
        for (int bin = 1; bin < size / 2; ++bin)
        {
            const auto magnitude = (double)frame[(size_t)bin];
            weighted += magnitude * bin * sampleRate / size;
            power += magnitude;
        }
    }
    return power > 1.0e-9 ? weighted / power : 0.0;
}

/** The mean frequency of the notes sounding between two tick positions (0: none). */
double notesFrequency (const std::vector<model::Note>& notes, double from, double to)
{
    double sum = 0.0;
    int count = 0;
    for (const auto& n : notes)
        if ((double)n.start < to && (double)(n.start + n.length) > from)
        {
            sum += 440.0 * std::pow (2.0, (n.pitch - 69) / 12.0);
            ++count;
        }
    return count > 0 ? sum / count : 0.0;
}

struct Point
{
    double ticks, value;
};

/** Douglas-Peucker: the fewest points whose straight lines stay within `tolerance` of every point. */
std::vector<Point> simplify (const std::vector<Point>& points, double tolerance)
{
    if (points.size() < 3)
        return points;
    std::vector<bool> keep (points.size(), false);
    keep.front() = keep.back() = true;
    std::vector<std::pair<size_t, size_t>> spans { { 0, points.size() - 1 } };
    while (! spans.empty())
    {
        const auto [from, to] = spans.back();
        spans.pop_back();
        double worst = 0.0;
        size_t at = from;
        for (auto i = from + 1; i < to; ++i)
        {
            const auto t = (points[i].ticks - points[from].ticks) / (points[to].ticks - points[from].ticks);
            const auto line = points[from].value + t * (points[to].value - points[from].value);
            if (const auto off = std::abs (points[i].value - line); off > worst)
            {
                worst = off;
                at = i;
            }
        }
        if (worst > tolerance)
        {
            keep[at] = true;
            spans.emplace_back (from, at);
            spans.emplace_back (at, to);
        }
    }
    std::vector<Point> kept;
    for (size_t i = 0; i < points.size(); ++i)
        if (keep[i])
            kept.push_back (points[i]);
    return kept;
}

double percentile (std::vector<double> values, double share)
{
    const auto at = values.begin() + (std::ptrdiff_t)std::lround (share * (double)(values.size() - 1));
    std::nth_element (values.begin(), at, values.end());
    return *at;
}

std::vector<model::AutomationPoint> toAutomation (const std::vector<Point>& points,
                                                  const std::function<float (double)>& value)
{
    std::vector<model::AutomationPoint> result;
    for (const auto& p : points)
    {
        const auto time = (model::Ticks)std::llround (p.ticks / (double)grid) * grid;
        if (time < 0 || (! result.empty() && time <= result.back().time))
            continue;
        result.push_back ({ time, juce::jlimit (0.0f, 1.0f, value (p.value)), 0.0f });
    }
    return result;
}
} // namespace

double toTicks (double seconds, const std::vector<double>& beats, double firstDownbeat)
{
    const auto perBeat = (double)model::ticksPerQuarterNote;
    if (beats.size() < 2)
        return (seconds - firstDownbeat) * 2.0 * perBeat; // 120 BPM
    const auto first =
        (double)(std::min_element (beats.begin(), beats.end(), [firstDownbeat] (double a, double b)
                                   { return std::abs (a - firstDownbeat) < std::abs (b - firstDownbeat); }) -
                 beats.begin());
    if (seconds < beats.front())
        return (-first - (beats.front() - seconds) / (beats[1] - beats.front())) * perBeat;
    if (seconds >= beats.back())
    {
        const auto last = beats.size() - 1;
        return ((double)last - first + (seconds - beats.back()) / (beats.back() - beats[last - 1])) * perBeat;
    }
    const auto next = (size_t)(std::upper_bound (beats.begin(), beats.end(), seconds) - beats.begin());
    return ((double)next - 1.0 - first + (seconds - beats[next - 1]) / (beats[next] - beats[next - 1])) * perBeat;
}

float cutoffSetting (double hertz)
{
    juce::NormalisableRange<float> range (20.0f, 20000.0f);
    range.setSkewForCentre (1000.0f);
    return range.convertTo0to1 (juce::jlimit (20.0f, 20000.0f, (float)hertz));
}

Curves measure (const juce::AudioBuffer<float>& stem, double sampleRate, const std::vector<double>& beats,
                double firstDownbeat, float volumeDb, const std::vector<model::Note>& notes)
{
    Curves curves;
    const auto samples = stem.getNumSamples();
    if (samples == 0 || sampleRate <= 0.0 || stem.getNumChannels() == 0)
        return curves;

    // Windows: the song's beats, or half seconds without them.
    std::vector<double> edges;
    if (beats.size() >= 2)
        edges = beats;
    else
        for (int half = 0; half * 0.5 < (double)samples / sampleRate; ++half)
            edges.push_back (half * 0.5);
    struct Window
    {
        double middle, level, pan, energy;
        double tone = 0.0;       // octaves of brightness over the notes it plays
        double brightness = 0.0; // its spectral centroid, octaves over 1 Hz
        bool toneKnown = false;  // false: no notes there, or silent
    };
    std::vector<Window> windows;
    const auto right = stem.getNumChannels() > 1 ? 1 : 0;
    for (size_t i = 0; i + 1 < edges.size(); ++i)
    {
        const auto from = juce::jlimit (0, samples, (int)(edges[i] * sampleRate));
        const auto to = juce::jlimit (0, samples, (int)(edges[i + 1] * sampleRate));
        if (to - from < 16)
            continue;
        double left = 0.0, rightEnergy = 0.0;
        const auto* l = stem.getReadPointer (0);
        const auto* r = stem.getReadPointer (right);
        for (int s = from; s < to; ++s)
        {
            left += (double)l[s] * l[s];
            rightEnergy += (double)r[s] * r[s];
        }
        left /= to - from;
        rightEnergy /= to - from;
        const auto energy = 0.5 * (left + rightEnergy);
        Window w { 0.5 * (edges[i] + edges[i + 1]), 10.0 * std::log10 (energy + 1.0e-12),
                   energy > 0.0 ? (rightEnergy - left) / (rightEnergy + left) : 0.0, energy };
        if (! notes.empty())
        {
            const auto played = notesFrequency (notes, toTicks (edges[i], beats, firstDownbeat),
                                                toTicks (edges[i + 1], beats, firstDownbeat));
            if (const auto centroid = played > 0.0 ? centroidOf (stem, from, to, sampleRate) : 0.0; centroid > 0.0)
            {
                w.tone = std::log2 (centroid / played);
                w.brightness = std::log2 (centroid);
                w.toneKnown = true;
            }
        }
        windows.push_back (w);
    }
    if (windows.empty())
        return curves;
    const auto loudest = std::max_element (windows.begin(), windows.end(),
                                           [] (const Window& a, const Window& b) { return a.level < b.level; })
                             ->level;
    const auto playing = [&] (const Window& w) { return w.level > std::max (-60.0, loudest - silentBelow); };

    // Smoothed over a bar (four beats around each), in dB for the loudness, by energy for the place; the tone over
    // four bars, in octaves (a part's tone changes with every chord and phrase: only its long moves count).
    std::vector<Point> levels, places, tones, brightness;
    for (size_t i = 0; i < windows.size(); ++i)
    {
        if (! playing (windows[i]))
            continue;
        double level = 0.0, pan = 0.0, energy = 0.0, tone = 0.0, bright = 0.0;
        int count = 0, toned = 0;
        for (auto j = i >= 2 ? i - 2 : 0; j <= std::min (windows.size() - 1, i + 1); ++j)
            if (playing (windows[j]))
            {
                level += windows[j].level;
                pan += windows[j].pan * windows[j].energy;
                energy += windows[j].energy;
                ++count;
            }
        for (auto j = i >= toneBeats / 2 ? i - toneBeats / 2 : 0; j < std::min (windows.size(), i + toneBeats / 2); ++j)
            if (windows[j].toneKnown && windows[j].level > loudest - toneBelow)
            {
                tone += windows[j].tone;
                bright += windows[j].brightness;
                ++toned;
            }
        const auto ticks = toTicks (windows[i].middle, beats, firstDownbeat);
        levels.push_back ({ ticks, level / count });
        places.push_back ({ ticks, energy > 0.0 ? pan / energy : 0.0 });
        if (toned > 0)
        {
            tones.push_back ({ ticks, tone / toned });
            brightness.push_back ({ ticks, bright / toned });
        }
    }
    if (levels.size() < 4)
        return curves;

    // Loudness: the loud parts (the 90th percentile) keep the channel's volume.
    std::vector<double> values;
    values.reserve (levels.size());
    for (const auto& p : levels)
        values.push_back (p.value);
    const auto reference = percentile (values, 0.9);
    if (percentile (values, 0.95) - percentile (values, 0.1) >= volumeChange)
    {
        for (auto& p : levels)
            p.value = volumeDb + juce::jlimit (-30.0, 6.0, p.value - reference);
        curves.volume = toAutomation (simplify (levels, 1.0),
                                      [] (double db) { return tracktion::decibelsToVolumeFaderPosition ((float)db); });
    }

    // Place: a curve when it moves, else a fixed pan off the centre.
    values.clear();
    for (const auto& p : places)
        values.push_back (p.value);
    if (percentile (values, 0.9) - percentile (values, 0.1) >= panChange)
        curves.pan = toAutomation (simplify (places, 0.05), [] (double pan) { return (float)(0.5 * (pan + 1.0)); });
    else if (const auto middle = percentile (values, 0.5); std::abs (middle) >= offCentre)
        curves.steadyPan = (float)juce::jlimit (-1.0, 1.0, middle);

    // Tone: the bright parts (the 90th percentile) play with the filter open. A part an octave or more duller has a
    // low pass at about twice its brightness (a low pass leaves a rich sound's centroid near half its cutoff), and
    // between the two the cutoff glides (in octaves). A rough match: the instrument has its own tone.
    if (tones.size() >= 4)
    {
        values.clear();
        for (const auto& p : tones)
            values.push_back (p.value);
        const auto bright = percentile (values, 0.9);
        if (bright - percentile (values, 0.1) >= toneChange)
        {
            for (size_t i = 0; i < tones.size(); ++i)
            {
                const auto duller = bright - tones[i].value;
                const auto closed =
                    juce::jlimit (std::log2 (lowestCutoff), std::log2 (highestCutoff), 1.0 + brightness[i].value);
                const auto t = juce::jlimit (0.0, 1.0, (duller - openWithin) / (closedFrom - openWithin));
                const auto octaves = std::log2 (highestCutoff) + t * (closed - std::log2 (highestCutoff));
                tones[i].value = cutoffSetting (std::pow (2.0, octaves));
            }
            curves.cutoff = toAutomation (simplify (tones, 0.1), [] (double v) { return (float)v; });
        }
    }
    return curves;
}

} // namespace spinus::stemautomation
