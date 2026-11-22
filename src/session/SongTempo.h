#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace spinus::songtempo
{

/** The tempo that puts the song's first and last beats where they are: the average over the whole song. Notes
    placed on the song's beats then line up with its recording at both ends. Fewer than 8 beats: `fallback`. */
inline double average (const std::vector<double>& beats, double fallback)
{
    if (beats.size() < 8 || beats.back() <= beats.front())
        return fallback;
    return 60.0 * (double)(beats.size() - 1) / (beats.back() - beats.front());
}

/** How far, in seconds, the song's beats stray from a steady beat at average(): near 0 for a song made to a
    click, up to a second for a band that speeds up and slows down. Notes placed on the beats are that far
    from the recording in places, since a project has one tempo. */
inline double drift (const std::vector<double>& beats)
{
    const auto tempo = average (beats, 0.0);
    if (tempo <= 0.0)
        return 0.0;
    double most = 0.0;
    for (size_t i = 0; i < beats.size(); ++i)
        most = std::max (most, std::abs (beats[i] - (beats.front() + (double)i * 60.0 / tempo)));
    return most;
}

/** A steady beat at `tempo` through `firstDownbeat`, from a beat before the song's first to one after its last:
    notes placed on it keep their moments in the recording (the project has one tempo), so they line up with
    the song played as audio however much the band speeds up or slows down. */
inline std::vector<double> steadyBeats (const std::vector<double>& beats, double firstDownbeat, double tempo)
{
    if (beats.empty() || tempo <= 0.0)
        return beats;
    const auto period = 60.0 / tempo;
    const auto before = std::ceil ((firstDownbeat - beats.front()) / period) + 1.0;
    const auto first = firstDownbeat - before * period;
    const auto count = (int)std::floor ((beats.back() + period - first) / period) + 1;
    std::vector<double> steady;
    steady.reserve ((size_t)count);
    for (int i = 0; i < count; ++i)
        steady.push_back (first + i * period);
    return steady;
}

/** A stretch of the song at one tempo, from a bar where it starts. */
struct Section
{
    double from = 0.0; // seconds into the song: the bar's downbeat
    double bpm = 0.0;
    int beats = 0; // whole bars of beats to the next section's start (0 for the last: to the end)
};

/** The song's tempos, section by section, for a band that changes tempo (a slower second half, a faster chorus):
    a new section where at least 4 bars in a row keep a tempo of their own, more than 4% from the section before.
    Bars that keep no tempo (a fill, a break) or twice or half of it (beats found twice as often in a blast beat,
    half time) stay in the section they are in, and are counted at its tempo, so the bars after them land
   where the song's are. A band that only drifts (within 4%) is one section at its average, as average() gives it. Each
   section's tempo puts its first and last downbeats where they are in the recording; the last section's (which
   has no next section to reach) is the one that fits all its beats best. */
inline std::vector<Section> sections (const std::vector<double>& beats, double firstDownbeat, int beatsPerBar)
{
    const auto bar = std::max (1, beatsPerBar);
    // The downbeats: the beat nearest the first downbeat, then every `bar` beats.
    size_t first = 0;
    for (size_t i = 0; i < beats.size(); ++i)
        if (std::abs (beats[i] - firstDownbeat) < std::abs (beats[first] - firstDownbeat))
            first = i;
    std::vector<double> downbeats;
    for (auto i = first; i < beats.size(); i += (size_t)bar)
        downbeats.push_back (beats[i]);
    if (downbeats.size() < 9)
        return { { firstDownbeat, average (beats, 0.0), 0 } };

    // Each bar's tempo, and runs of bars that keep one.
    std::vector<double> barBpm;
    for (size_t k = 0; k + 1 < downbeats.size(); ++k)
        barBpm.push_back (60.0 * bar / std::max (1.0e-6, downbeats[k + 1] - downbeats[k]));
    const auto median = [] (std::vector<double> v)
    {
        std::nth_element (v.begin(), v.begin() + (long)(v.size() / 2), v.end());
        return v[v.size() / 2];
    };
    const auto near = [] (double a, double b) { return std::abs (a / b - 1.0) <= 0.04; };
    constexpr size_t run = 4;
    const auto steadyFrom = [&] (size_t k) -> std::optional<double>
    {
        if (k + run > barBpm.size())
            return std::nullopt;
        const auto m = median ({ barBpm.begin() + (long)k, barBpm.begin() + (long)(k + run) });
        for (auto i = k; i < k + run; ++i)
            if (! near (barBpm[i], m))
                return std::nullopt;
        return m;
    };

    // Section starts (bar numbers) and the tempo each keeps.
    std::vector<std::pair<size_t, double>> starts;
    size_t k = 0;
    std::optional<double> opening;
    while (k < barBpm.size() && ! (opening = steadyFrom (k)))
        ++k;
    if (! opening)
        return { { firstDownbeat, average (beats, 0.0), 0 } };
    starts.push_back ({ 0, opening.value_or (0.0) });
    std::vector<double> kept;
    for (; k < barBpm.size(); ++k)
    {
        const auto tempo = kept.empty() ? starts.back().second : median (kept);
        if (near (barBpm[k], tempo))
        {
            kept.push_back (barBpm[k]);
            continue;
        }
        // Twice or half the tempo: the beats counted another way (a blast beat, half time), not the band changing
        // tempo. (A band does go from 210 to 145, about 2/3: the AI service mends the stretches where the beat
        // finder loses the beat, so a new tempo there is the band's.)
        const auto recounted = [&] (double other)
        {
            for (auto ratio : { 2.0, 0.5 })
                if (std::abs (other / tempo / ratio - 1.0) <= 0.06)
                    return true;
            return false;
        };
        if (const auto other = steadyFrom (k); other && ! near (*other, tempo) && ! recounted (*other))
        {
            starts.back().second = tempo;
            starts.push_back ({ k, *other });
            kept = { barBpm[k] };
        }
    }
    if (! kept.empty())
        starts.back().second = median (kept);

    // Next to one that keeps nearly the same tempo (within 2%): one section.
    for (size_t s = 1; s < starts.size();)
        if (std::abs (starts[s].second / starts[s - 1].second - 1.0) < 0.02)
            starts.erase (starts.begin() + (long)s);
        else
            ++s;

    // Each section a whole number of bars at its tempo, up to the next one's downbeat; the last to the end.
    std::vector<Section> found;
    for (size_t s = 0; s < starts.size(); ++s)
    {
        const auto from = downbeats[starts[s].first];
        auto to = s + 1 < starts.size() ? starts[s + 1].first : downbeats.size() - 1;
        // The last section ends at its last bar that keeps its tempo: beats found in the ring after the song
        // ends (or a closing slow-down) would otherwise stretch every bar (a 160 BPM song came out at 156).
        if (s + 1 == starts.size())
            while (to > starts[s].first + 1 && ! near (barBpm[to - 1], starts[s].second))
                --to;
        Section section { from, starts[s].second, 0 };
        if (to > starts[s].first)
        {
            const auto span = downbeats[to] - from;
            const auto bars = std::max (1.0, std::round (span * section.bpm / 60.0 / bar));
            section.bpm = 60.0 * bars * bar / span;
            if (s + 1 < starts.size())
                section.beats = (int)bars * bar;
            else if (to >= starts[s].first + 4 && std::abs (bars - (double)(to - starts[s].first)) < 0.5)
            {
                // The last section: no next downbeat to reach, so its tempo is fitted to all its beats (least
                // squares). Found on 20 ms frames, the last downbeat alone can be 40 ms off, which made a 160 BPM
                // song 159.7 and moved its last notes off the recording.
                const auto firstBeat = first + starts[s].first * (size_t)bar;
                const auto lastBeat = first + to * (size_t)bar;
                const auto n = (double)(lastBeat - firstBeat + 1);
                double meanK = 0.0, meanT = 0.0;
                for (auto i = firstBeat; i <= lastBeat; ++i)
                {
                    meanK += (double)(i - firstBeat) / n;
                    meanT += beats[i] / n;
                }
                double covariance = 0.0, variance = 0.0;
                for (auto i = firstBeat; i <= lastBeat; ++i)
                {
                    const auto di = (double)(i - firstBeat) - meanK;
                    covariance += di * (beats[i] - meanT);
                    variance += di * di;
                }
                if (covariance > 0.0)
                    section.bpm = 60.0 * variance / covariance;
            }
        }
        found.push_back (section);
    }
    return found;
}

/** The beats notes are placed on for a song in sections(): each section's steady beat from its downbeat, the first
    reaching back a beat before the song's first beat and the last a beat past its last, so notes keep their
    moments in the recording and the bars follow its tempo. */
inline std::vector<double> sectionBeats (const std::vector<Section>& sections, const std::vector<double>& beats)
{
    if (sections.empty() || beats.empty())
        return beats;
    std::vector<double> grid;
    const auto firstPeriod = 60.0 / sections.front().bpm;
    const auto before = std::ceil ((sections.front().from - beats.front()) / firstPeriod) + 1.0;
    for (int i = (int)before; i > 0; --i)
        grid.push_back (sections.front().from - i * firstPeriod);
    for (size_t s = 0; s < sections.size(); ++s)
    {
        const auto period = 60.0 / sections[s].bpm;
        if (s + 1 < sections.size())
            for (int i = 0; i < sections[s].beats; ++i)
                grid.push_back (sections[s].from + i * period);
        else
            for (int i = 0; sections[s].from + (i - 1) * period <= beats.back(); ++i)
                grid.push_back (sections[s].from + i * period);
    }
    return grid;
}

/** How far, in seconds, the song's beats stray from the nearest beat of `grid`. */
inline double driftFrom (const std::vector<double>& beats, const std::vector<double>& grid)
{
    double most = 0.0;
    for (auto b : beats)
    {
        const auto it = std::lower_bound (grid.begin(), grid.end(), b);
        auto nearest = 1.0e9;
        if (it != grid.end())
            nearest = std::min (nearest, std::abs (*it - b));
        if (it != grid.begin())
            nearest = std::min (nearest, std::abs (*(it - 1) - b));
        most = std::max (most, nearest);
    }
    return most;
}

} // namespace spinus::songtempo
