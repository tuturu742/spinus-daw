#include "session/SongTempo.h"

#include <algorithm>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace spinus;

TEST_CASE ("A song's tempo is the average over all its beats, and its drift how far they stray", "[analyzer][tempo]")
{
    // A steady 120 BPM: no drift.
    std::vector<double> steady;
    steady.reserve (64);
    for (int i = 0; i < 64; ++i)
        steady.push_back (0.4 + i * 0.5);
    CHECK (songtempo::average (steady, 0.0) == Catch::Approx (120.0));
    CHECK (songtempo::drift (steady) == Catch::Approx (0.0).margin (1e-9));

    // A band that rushes in the middle: a little faster there, back at the end. The average puts the first and
    // last beats right; the middle strays.
    std::vector<double> live;
    live.reserve (64);
    double t = 0.0;
    for (int i = 0; i < 64; ++i)
    {
        live.push_back (t);
        t += i < 32 ? 0.48 : 0.52;
    }
    CHECK (songtempo::average (live, 0.0) == Catch::Approx (60.0 * 63 / live.back()));
    CHECK (songtempo::drift (live) > 0.5);

    // Too few beats: the fallback.
    CHECK (songtempo::average ({ 0.0, 0.5, 1.0 }, 98.0) == Catch::Approx (98.0));
    CHECK (songtempo::drift ({}) == Catch::Approx (0.0));

    // The steady beat goes through the first downbeat, a beat past both ends of the song.
    const auto grid = songtempo::steadyBeats (live, live[4], 120.0);
    CHECK (grid.front() < live.front());
    CHECK (grid.back() > live.back());
    CHECK (std::any_of (grid.begin(), grid.end(), [&] (double b) { return std::abs (b - live[4]) < 1e-9; }));
    CHECK (grid[1] - grid[0] == Catch::Approx (0.5));
}

TEST_CASE ("A song that changes tempo: its sections, each at its own tempo, and the beats notes are placed on",
           "[analyzer][tempo]")
{
    // 40 bars at 158 BPM; 4 bars where the beat finder finds twice as many beats (a blast beat, a fill); then
    // 30 bars at 145 BPM, as in a band's song that slows down for its second half.
    std::vector<double> beats;
    double t = 0.3;
    for (int i = 0; i < 160; ++i, t += 60.0 / 158.0)
        beats.push_back (t);
    for (int i = 0; i < 32; ++i, t += 30.0 / 158.0)
        beats.push_back (t);
    const auto slower = t;
    for (int i = 0; i <= 120; ++i, t += 60.0 / 145.0)
        beats.push_back (t);

    const auto found = songtempo::sections (beats, 0.3, 4);
    REQUIRE (found.size() == 2);
    CHECK (found[0].from == Catch::Approx (0.3));
    CHECK (found[0].bpm == Catch::Approx (158.0).epsilon (0.001));
    CHECK (found[0].beats == 176); // the unclear bars counted at its tempo
    CHECK (found[1].from == Catch::Approx (slower));
    CHECK (found[1].bpm == Catch::Approx (145.0).epsilon (0.001));
    CHECK (found[1].beats == 0);

    // Notes placed on these beats keep their moments: the grid goes through each section's downbeats, a beat past
    // both ends, and strays from the clear beats by nothing.
    const auto grid = songtempo::sectionBeats (found, beats);
    CHECK (grid.front() < beats.front());
    CHECK (grid.back() > beats.back());
    CHECK (std::any_of (grid.begin(), grid.end(), [&] (double b) { return std::abs (b - slower) < 1e-6; }));
    std::vector<double> clear (beats.begin(), beats.begin() + 160);
    clear.insert (clear.end(), beats.begin() + 192, beats.end());
    CHECK (songtempo::driftFrom (clear, grid) < 0.01);
    // The average over the beats found would be far off both tempos.
    CHECK (songtempo::average (beats, 0.0) > 160.0);

    // A song at one tempo, or a band that only drifts a little: one section, at the average.
    std::vector<double> live;
    t = 0.0;
    for (int i = 0; i < 128; ++i, t += i < 64 ? 0.495 : 0.505)
        live.push_back (t);
    const auto one = songtempo::sections (live, 0.0, 4);
    REQUIRE (one.size() == 1);
    CHECK (one[0].bpm == Catch::Approx (songtempo::average (live, 0.0)).epsilon (0.01));
    CHECK (songtempo::sections ({ 0.0, 0.5, 1.0 }, 0.0, 4).size() == 1);
}

TEST_CASE ("A song that speeds up for a part and slows down after it: three sections", "[analyzer][tempo]")
{
    // 160 BPM, 12 bars at 210 (about 2/3 of it back to the 145 after: a tempo change, not beats counted another way),
    // then 145.
    std::vector<double> beats;
    double t = 0.2;
    for (int i = 0; i < 128; ++i, t += 60.0 / 160.0)
        beats.push_back (t);
    const auto faster = t;
    for (int i = 0; i < 48; ++i, t += 60.0 / 210.0)
        beats.push_back (t);
    const auto slower = t;
    for (int i = 0; i <= 80; ++i, t += 60.0 / 145.0)
        beats.push_back (t);
    const auto found = songtempo::sections (beats, 0.2, 4);
    REQUIRE (found.size() == 3);
    CHECK (found[0].bpm == Catch::Approx (160.0).epsilon (0.001));
    CHECK (found[1].from == Catch::Approx (faster));
    CHECK (found[1].bpm == Catch::Approx (210.0).epsilon (0.001));
    CHECK (found[2].from == Catch::Approx (slower));
    CHECK (found[2].bpm == Catch::Approx (145.0).epsilon (0.001));
}

TEST_CASE ("Beats found after a song ends don't change its tempo", "[analyzer][tempo]")
{
    // 16 bars at 160 BPM, then the ring after the last chord, where the beat finder still finds a few beats, late.
    std::vector<double> beats;
    beats.reserve (67);
    for (int i = 0; i < 64; ++i)
        beats.push_back (i * 0.375);
    for (auto late : { 24.58, 24.96, 25.34 })
        beats.push_back (late);
    const auto found = songtempo::sections (beats, 0.0, 4);
    REQUIRE (found.size() == 1);
    CHECK (found[0].bpm == Catch::Approx (160.0).epsilon (0.001));
}

TEST_CASE ("The last section's tempo fits all its bars, not only its last downbeat", "[analyzer][tempo]")
{
    // 16 bars at 160 BPM, found on 20 ms frames: every beat a little off, the last downbeat 40 ms late.
    std::vector<double> beats;
    beats.reserve (65);
    for (int i = 0; i <= 64; ++i)
        beats.push_back (std::round ((i * 0.375 + (i % 3 == 0 ? 0.008 : -0.006)) / 0.02) * 0.02);
    beats.back() = 24.04;
    const auto found = songtempo::sections (beats, 0.0, 4);
    REQUIRE (found.size() == 1);
    CHECK (found[0].bpm == Catch::Approx (160.0).epsilon (0.0015));
}

TEST_CASE ("Following a band bar by bar puts every downbeat where the band played it", "[analyzer][tempo]")
{
    // 32 bars of a band drifting between 91 and 95 BPM (within 4%: one steady section), no click.
    std::vector<double> beats;
    double t = 0.5;
    for (int i = 0; i <= 128; ++i)
    {
        beats.push_back (t);
        t += 60.0 / (93.0 + 1.8 * std::sin (i / 16.0));
    }
    REQUIRE (songtempo::sections (beats, 0.5, 4).size() == 1);
    const auto steady = songtempo::sectionBeats (songtempo::sections (beats, 0.5, 4), beats);
    CHECK (songtempo::driftFrom (beats, steady) > 0.25); // a steady grid wanders a quarter beat or more off

    const auto bars = songtempo::barSections (beats, 0.5, 4);
    REQUIRE (bars.size() == 32);
    CHECK (bars.back().beats == 0);
    const auto grid = songtempo::sectionBeats (bars, beats);
    for (size_t bar = 0; bar < 32; ++bar)
    {
        const auto it = std::min_element (grid.begin(), grid.end(), [&] (double a, double b)
                                          { return std::abs (a - beats[bar * 4]) < std::abs (b - beats[bar * 4]); });
        CHECK (*it == Catch::Approx (beats[bar * 4]).margin (1.0e-9));
    }
    CHECK (songtempo::driftFrom (beats, grid) < 0.02);
}
