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
