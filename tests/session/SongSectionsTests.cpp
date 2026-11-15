#include <catch2/catch_test_macros.hpp>

#include "session/SongSections.h"

using namespace spinus;

namespace
{
constexpr model::Ticks bar = 3840, beat = 960;

model::Note note (model::Ticks start, model::Ticks length, int pitch, float velocity = 0.8f)
{
    model::Note n;
    n.start = start;
    n.length = length;
    n.pitch = pitch;
    n.velocity = velocity;
    return n;
}

/** Intro (drums), verse, chorus (a busier bass), verse, chorus, outro (one held bass note): 40 bars. The first
    verse's last bar has a drum fill. */
std::map<juce::String, std::vector<model::Note>> song()
{
    std::vector<model::Note> drums, bass;
    for (int b = 0; b < 36; ++b)
    {
        const auto at = b * bar;
        if (b == 7)
            for (int i = 0; i < 8; ++i)
                drums.push_back (note (at + i * beat / 2, 120, 45 + i % 3)); // toms
        else
            for (int i = 0; i < 4; ++i)
                drums.push_back (note (at + i * beat, 120, i % 2 == 0 ? 36 : 38));
        const bool chorus = (b >= 12 && b < 20) || b >= 28;
        if (b >= 4)
            for (int i = 0; i < (chorus ? 8 : 4); ++i)
                bass.push_back (note (at + i * (chorus ? beat / 2 : beat), chorus ? 400 : 900, chorus ? 41 : 33));
    }
    bass.push_back (note (36 * bar, 4 * bar - 10, 45));
    return { { "drums", drums }, { "bass", bass } };
}

int notesPlaced (const songsections::Split& split)
{
    int count = 0;
    for (const auto& p : split.placed)
        count += (int)split.pieces[(size_t)p.piece].notes.size();
    return count;
}
} // namespace

TEST_CASE ("A transcribed song's sections are found and named", "[analyzer][sections]")
{
    const auto sections = songsections::find (song(), bar, 40);
    juce::StringArray found;
    for (const auto& s : sections)
        found.add (s.name + " " + juce::String (s.firstBar) + "+" + juce::String (s.bars));
    CHECK (found.joinIntoString (", ") == "Intro 0+4, Verse 4+8, Chorus 12+8, Verse 20+8, Chorus 28+8, Outro 36+4");
    CHECK (songsections::markerNames (sections).joinIntoString (", ") ==
           "Intro, Verse 1, Chorus 1, Verse 2, Chorus 2, Outro");

    // One kind all through: one section; nothing: none.
    std::vector<model::Note> steady;
    steady.reserve (8);
    for (int b = 0; b < 8; ++b)
        steady.push_back (note (b * bar, beat, 60));
    const auto one = songsections::find ({ { "keys", steady } }, bar, 8);
    REQUIRE (one.size() == 1);
    CHECK (one[0] == songsections::Section { "Song", 0, 8 });
    CHECK (songsections::find ({}, bar, 0).empty());
}

TEST_CASE ("A stem's notes become loops placed again, fills of their own, and nothing is lost", "[analyzer][sections]")
{
    const auto stems = song();
    const auto sections = songsections::find (stems, bar, 40);

    const auto drums = songsections::split (stems.at ("drums"), sections, bar);
    REQUIRE (drums.pieces.size() == 2); // the beat (one bar, the same everywhere) and the fill
    CHECK (drums.pieces[0].name == "Intro loop");
    CHECK (drums.pieces[0].length == bar);
    CHECK (drums.pieces[1].name == "Verse fill");
    CHECK (drums.placed.size() == 36); // every bar that plays, none in the outro
    CHECK (drums.placed[7] == songsections::Placed { 1, 7 * bar });
    CHECK (notesPlaced (drums) == (int)stems.at ("drums").size());

    const auto bass = songsections::split (stems.at ("bass"), sections, bar);
    juce::StringArray names;
    for (const auto& p : bass.pieces)
        names.add (p.name);
    CHECK (names.joinIntoString (", ") == "Verse loop, Chorus loop, Outro"); // the second verse and chorus reuse
    CHECK (bass.pieces[2].length == 4 * bar);                                // the held note: no loop in the outro
    CHECK (bass.placed.front().start == 4 * bar);
    CHECK (notesPlaced (bass) == (int)stems.at ("bass").size());

    // A note before the first downbeat starts it; velocities of a repeat are the first one's.
    const auto early =
        songsections::split ({ note (-30, 100, 60), note (bar, 100, 60, 0.2f) }, { { "Song", 0, 2 } }, bar);
    REQUIRE (early.pieces.size() == 1);
    CHECK (early.pieces[0].notes[0].start == 0);
    CHECK (early.placed.size() == 2);
}
