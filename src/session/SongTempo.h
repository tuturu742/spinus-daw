#pragma once

#include <algorithm>
#include <cmath>
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

} // namespace spinus::songtempo
