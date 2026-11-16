#pragma once

#include "model/Model.h"

#include <map>
#include <vector>

namespace spinus
{

/** A transcribed song (the Analyzer's notes, one list per stem, on the project's bars) built the way a producer
    would (§17, the Analyzer's step 1): its sections found by comparing bars, and each stem's notes as short
    patterns: within a section, the shortest repeat (a loop of 1, 2, 4 or 8 bars) is one pattern placed again, a part
    that differs (a fill) a pattern of its own, and the same notes anywhere in the song the same pattern, so a
    section that comes back reuses its patterns and no note is lost. No AI and no model: plain data, any thread. */
namespace songsections
{
struct Section
{
    juce::String name; // "Intro", "Verse", "Chorus", "Bridge", "Outro", "Part C"...
    int firstBar = 0, bars = 0;
    bool operator== (const Section&) const = default;
};

/** The song's sections over `bars` bars: the bars are cut into phrases of `phraseBars`, phrases that sound alike
    (the same harmony and rhythm in the stems that play, a little noise allowed) are one kind, and a run of one kind
    is a section. Kinds are named by how often they come back and how busy they are: the busiest that comes back
    is the chorus, another the verse; one heard once is the intro before them, the outro after, else a bridge. */
std::vector<Section> find (const std::map<juce::String, std::vector<model::Note>>& stems, model::Ticks barLength,
                           int bars, int phraseBars = 4);

/** "Verse 1", "Chorus 2": sections of a name that comes back numbered, the others as they are. */
juce::StringArray markerNames (const std::vector<Section>&);

struct Piece
{
    juce::String name; // "Chorus loop", "Chorus fill", "Intro"
    model::Ticks length = 0;
    std::vector<model::Note> notes; // from the piece's start
};
struct Placed
{
    int piece = 0;
    model::Ticks start = 0;
    bool operator== (const Placed&) const = default;
};
struct Split
{
    std::vector<Piece> pieces;
    std::vector<Placed> placed; // in time order; silent parts are left out
};

/** One stem's notes as pieces placed over the sections (see above). Notes match when they start, last and sound the
    same; a repeat with other velocities plays the first one's. */
Split split (const std::vector<model::Note>& notes, const std::vector<Section>& sections, model::Ticks barLength);
} // namespace songsections

} // namespace spinus
