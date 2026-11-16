#include <catch2/catch_test_macros.hpp>

#include "session/StemParts.h"

using namespace spinus;

namespace
{
constexpr model::Ticks bar = 3840, beat = 960;

model::Note note (model::Ticks start, model::Ticks length, int pitch)
{
    model::Note n;
    n.start = start;
    n.length = length;
    n.pitch = pitch;
    n.velocity = 0.8f;
    return n;
}

const std::vector<songsections::Section> sections { { "Verse", 0, 4 },   { "Chorus", 4, 4 },  { "Verse", 8, 4 },
                                                    { "Chorus", 12, 4 }, { "Bridge", 16, 4 }, { "Chorus", 20, 4 } };

/** A chord (E minor, around E3) on every beat of `bars` bars from `first`. */
void strum (std::vector<model::Note>& notes, int first, int bars)
{
    for (int b = first; b < first + bars; ++b)
        for (int i = 0; i < 4; ++i)
            for (const int pitch : { 52, 55, 59 })
                notes.push_back (note (b * bar + i * beat, beat - 60, pitch));
}

/** A free line of eighths above the chords, each bar different, between the beats. */
void line (std::vector<model::Note>& notes, int first, int bars)
{
    for (int b = first; b < first + bars; ++b)
        for (int i = 0; i < 8; ++i)
            notes.push_back (note (b * bar + beat / 4 + i * beat / 2, beat / 4, 64 + (b * 5 + i * 3) % 12));
}
} // namespace

TEST_CASE ("A stem's notes split into rhythm, lead and solo", "[analyzer][parts]")
{
    // Chords all through; a lead over each chorus; the bridge only the lead (taking turns).
    std::vector<model::Note> guitar;
    strum (guitar, 0, 16);
    strum (guitar, 20, 4);
    for (const int chorus : { 4, 12, 20 })
        line (guitar, chorus, 4);
    line (guitar, 16, 4);

    const auto parts = stemparts::split (guitar, sections, bar);
    REQUIRE (parts.size() == 3);
    CHECK (parts[0].role == stemparts::Role::rhythm);
    CHECK (parts[0].notes.size() == 20 * 4 * 3);
    CHECK (parts[1].role == stemparts::Role::lead);
    CHECK (parts[1].notes.size() == 3 * 4 * 8);
    CHECK (parts[2].role == stemparts::Role::solo); // the bridge, heard once
    CHECK (parts[2].notes.size() == 4 * 8);
    CHECK (parts[2].notes.front().start == 16 * bar + beat / 4);
    CHECK (stemparts::nameOf (parts[2].role) == "Solo");

    // Only the bridge has a lead: it is the solo.
    std::vector<model::Note> turns;
    strum (turns, 0, 16);
    line (turns, 16, 4);
    strum (turns, 20, 4);
    const auto two = stemparts::split (turns, sections, bar);
    REQUIRE (two.size() == 2);
    CHECK (two[1].role == stemparts::Role::solo);
    CHECK (two[0].notes.size() + two[1].notes.size() == turns.size());
}

TEST_CASE ("A stem that holds one part stays whole", "[analyzer][parts]")
{
    // Chords only; one free line only (nothing to tell it from); a riff played again; a few notes.
    std::vector<model::Note> chords, free, riff;
    strum (chords, 0, 24);
    line (free, 0, 24);
    for (int b = 0; b < 24; ++b)
        for (int i = 0; i < 8; ++i)
            riff.push_back (note (b * bar + i * beat / 2, beat / 4, 40 + (i * 7) % 12));
    for (const auto* notes : { &chords, &free, &riff })
    {
        const auto parts = stemparts::split (*notes, sections, bar);
        REQUIRE (parts.size() == 1);
        CHECK (parts[0].role == stemparts::Role::rhythm);
        CHECK (parts[0].notes.size() == notes->size());
    }

    // A short line over the chords is too small to stand alone.
    auto few = chords;
    for (int i = 0; i < 6; ++i)
        few.push_back (note (16 * bar + beat / 4 + i * beat / 2, beat / 4, 70 + i));
    CHECK (stemparts::split (few, sections, bar).size() == 1);
    CHECK (stemparts::split ({}, sections, bar).empty());
}

TEST_CASE ("Drums, bass and vocals are not split", "[analyzer][parts]")
{
    CHECK (stemparts::splits ("guitar"));
    CHECK (stemparts::splits ("piano"));
    CHECK (stemparts::splits ("other"));
    CHECK_FALSE (stemparts::splits ("drums"));
    CHECK_FALSE (stemparts::splits ("bass"));
    CHECK_FALSE (stemparts::splits ("vocals"));
    CHECK_FALSE (stemparts::splits ("lead-vocal"));
}
