#include "StemParts.h"

#include <algorithm>
#include <set>

namespace spinus::stemparts
{

namespace
{
constexpr model::Ticks together = model::ticksPerQuarterNote / 8; // notes struck within a 32nd are one chord
constexpr int above = 3;                                          // semitones a lead sits above the rhythm, at least
constexpr double alike = 0.7;                                     // bars at least this alike repeat (a riff)

int median (std::vector<int> values)
{
    const auto middle = values.begin() + (std::ptrdiff_t)(values.size() / 2);
    std::nth_element (values.begin(), middle, values.end());
    return *middle;
}

/** One bar of the stem: its notes struck alone (by index), its chords' notes, and what it plays alone as
    (sixteenth, pitch) pairs, to tell a riff played again from a free line. */
struct Bar
{
    std::vector<size_t> singles;
    std::vector<int> chordPitches;
    std::set<std::pair<int, int>> played;
};

double overlap (const std::set<std::pair<int, int>>& a, const std::set<std::pair<int, int>>& b)
{
    if (a.empty() || b.empty())
        return 0.0;
    size_t shared = 0;
    for (const auto& p : a)
        shared += b.count (p);
    return (double)shared / (double)(a.size() + b.size() - shared);
}
} // namespace

juce::String nameOf (Role role)
{
    switch (role)
    {
    case Role::lead:
        return "Lead";
    case Role::solo:
        return "Solo";
    case Role::rhythm:
        break;
    }
    return "Rhythm";
}

bool splits (const juce::String& stem)
{
    return ! (stem == "drums" || stem == "bass" || stem.containsIgnoreCase ("vocal"));
}

std::vector<Part> split (const std::vector<model::Note>& input, const std::vector<songsections::Section>& sections,
                         model::Ticks barLength)
{
    auto notes = input;
    std::stable_sort (notes.begin(), notes.end(),
                      [] (const model::Note& a, const model::Note& b) { return a.start < b.start; });
    const auto whole = [&notes] { return std::vector<Part> { { Role::rhythm, notes } }; };
    if (notes.empty())
        return {};
    if (notes.size() < 2 || barLength <= 0)
        return whole();

    // Chords: two notes or more struck together, not just octaves of one (a transcription often hears a single
    // note's overtone as a note an octave up). Each note's bar.
    std::vector<bool> inChord (notes.size(), false);
    for (size_t first = 0; first < notes.size();)
    {
        auto end = first + 1;
        std::set<int> pitchClasses { notes[first].pitch % 12 };
        while (end < notes.size() && notes[end].start - notes[first].start < together)
            pitchClasses.insert (notes[end++].pitch % 12);
        if (pitchClasses.size() > 1)
            std::fill (inChord.begin() + (std::ptrdiff_t)first, inChord.begin() + (std::ptrdiff_t)end, true);
        first = end;
    }
    const auto barOf = [barLength] (const model::Note& n)
    { return (int)(std::max<model::Ticks> (0, n.start) / barLength); };
    std::map<int, Bar> bars;
    for (size_t i = 0; i < notes.size(); ++i)
    {
        auto& bar = bars[barOf (notes[i])];
        if (inChord[i])
            bar.chordPitches.push_back (notes[i].pitch);
        else
        {
            bar.singles.push_back (i);
            const auto within = std::max<model::Ticks> (0, notes[i].start) % barLength;
            bar.played.emplace ((int)(within * 16 / barLength), notes[i].pitch);
        }
    }

    // A bar's single notes are steady when they use few pitches or repeat a bar nearby (a riff); else free.
    std::map<int, bool> steady;
    for (const auto& [index, bar] : bars)
    {
        std::set<int> pitches;
        for (const auto i : bar.singles)
            pitches.insert (notes[i].pitch);
        bool repeats = pitches.size() < 3;
        for (const int near : { -2, -1, 1, 2 })
            if (const auto other = bars.find (index + near); ! repeats && other != bars.end())
                repeats = overlap (bar.played, other->second.played) >= alike;
        steady[index] = repeats;
    }

    // The rhythm part's register: its chords and steady notes. With none, the stem is one free line.
    std::vector<int> rhythmPitches;
    for (size_t i = 0; i < notes.size(); ++i)
        if (inChord[i] || steady[barOf (notes[i])])
            rhythmPitches.push_back (notes[i].pitch);
    if (rhythmPitches.empty())
        return whole();
    const auto rhythmMiddle = median (rhythmPitches);

    // The lead: a free bar's single notes, above the rhythm part and above the middle of that bar's chords, in a
    // phrase of two bars or more (a lone bar of them is more often a fill or the transcription's noise).
    std::set<int> leadBars;
    for (const auto& [index, bar] : bars)
    {
        if (steady[index] || bar.singles.size() < 2)
            continue;
        std::vector<int> pitches;
        for (const auto i : bar.singles)
            pitches.push_back (notes[i].pitch);
        if (const auto middle = median (pitches);
            middle >= rhythmMiddle + above && (bar.chordPitches.empty() || middle >= median (bar.chordPitches) + above))
            leadBars.insert (index);
    }
    std::vector<Role> roles (notes.size(), Role::rhythm);
    for (const auto index : leadBars)
        if (leadBars.contains (index - 1) || leadBars.contains (index + 1))
            for (const auto i : bars[index].singles)
                roles[i] = Role::lead;

    // The solo: the lead of the one section that has one, or of the one section heard once among several.
    std::map<juce::String, int> heard;
    for (const auto& s : sections)
        ++heard[s.name];
    std::vector<size_t> withLead;
    for (size_t s = 0; s < sections.size(); ++s)
    {
        const auto& section = sections[s];
        int count = 0;
        for (size_t i = 0; i < notes.size(); ++i)
            if (const auto bar = barOf (notes[i]);
                roles[i] == Role::lead && bar >= section.firstBar && bar < section.firstBar + section.bars)
                ++count;
        if (count >= std::max (4, section.bars))
            withLead.push_back (s);
    }
    std::vector<size_t> once;
    for (const auto s : withLead)
        if (const auto& name = sections[s].name; heard[name] == 1 && name != "Intro" && name != "Outro")
            once.push_back (s);
    const auto solo = withLead.size() == 1 ? withLead : once.size() == 1 ? once : std::vector<size_t> {};
    for (const auto s : solo)
        for (size_t i = 0; i < notes.size(); ++i)
            if (const auto bar = barOf (notes[i]);
                roles[i] == Role::lead && bar >= sections[s].firstBar && bar < sections[s].firstBar + sections[s].bars)
                roles[i] = Role::solo;

    // Parts too small to stand alone go back to the rhythm part; a rhythm part too small, everything is one part.
    const auto smallest = std::max<size_t> (8, notes.size() / 20);
    const auto count = [&roles] (Role role) { return (size_t)std::count (roles.begin(), roles.end(), role); };
    for (const auto role : { Role::lead, Role::solo })
        if (count (role) < smallest)
            std::replace (roles.begin(), roles.end(), role, Role::rhythm);
    if (count (Role::rhythm) < smallest)
        return whole();
    std::vector<Part> parts;
    for (const auto role : { Role::rhythm, Role::lead, Role::solo })
    {
        Part part { role, {} };
        for (size_t i = 0; i < notes.size(); ++i)
            if (roles[i] == role)
                part.notes.push_back (notes[i]);
        if (! part.notes.empty())
            parts.push_back (std::move (part));
    }
    return parts;
}

} // namespace spinus::stemparts
