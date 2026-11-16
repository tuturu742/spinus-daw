#pragma once

#include "session/SongSections.h"

namespace spinus
{

/** One separated stem's notes split into the parts played in it (§17, the Analyzer's step 2): a stem often holds
    two players (two guitars, a piano's chords and its tune). Chords and steady playing are the rhythm part; single,
    higher, freer lines the lead; a lead heard in one section only (or in one section heard once, among choruses
    that have one too) the solo. Parts that take turns split well; parts playing at the same moment only roughly: a
    line above the chords in the same bar goes to the lead, notes struck with a chord stay in the rhythm part. No
    AI and no model: plain data, any thread. */
namespace stemparts
{
enum class Role
{
    rhythm,
    lead,
    solo
};

struct Part
{
    Role role = Role::rhythm;
    std::vector<model::Note> notes; // in time order
};

/** "Rhythm", "Lead", "Solo". */
juce::String nameOf (Role);

/** Whether a stem of this name may hold more than one part: not drums, bass or vocals. */
bool splits (const juce::String& stem);

/** The parts of one stem over the song's `sections`, in the order rhythm, lead, solo, each left out when it has
    no notes. A part too small to stand alone (fewer than 8 notes or a twentieth of the stem) goes back to the
    rhythm part, so a stem that holds one part comes back as one rhythm part with every note. */
std::vector<Part> split (const std::vector<model::Note>& notes, const std::vector<songsections::Section>& sections,
                         model::Ticks barLength);
} // namespace stemparts

} // namespace spinus
