#pragma once

#include "InstrumentProfiles.h"

#include <juce_core/juce_core.h>

#include <map>
#include <vector>

namespace spinus
{

/** Which of an instrument's modes a song's part plays in, and the articulations it switches to: the Track
    creator's songwriting reads the instrument's profile (InstrumentProfiles), picks a mode for each part (by the
    words of what the mode is good for, or as the AI chose), keeps the part's notes on that mode's playing keys and
    writes the switch keys as short notes at the start of each section, so the project plays the same anywhere.

    A mode the songwriting can use plays notes on known keys and is reachable: the one the instrument opens in (the
    first), or one a switch key enters. Modes with no playing keys (a strummer's chord keys) are written in a later
    step. Plain data, any thread but the audio thread. */
namespace partmodes
{
using Profile = InstrumentProfiles::Profile;

/** The modes of `profile` the songwriting can use, by index. */
std::vector<int> usable (const Profile& profile);

/** What the AI is told of the usable modes: [{name, goodFor, voices, low, high, notPlaying, switches: [{key,
    what}], first}], in that order; an empty array when none is usable. */
juce::var brief (const Profile& profile);

/** The usable mode whose "good for" words best match `words` (the part's role and name, the genre): its index,
    the first usable mode when none matches, -1 when none is usable. */
int pick (const Profile& profile, const juce::String& words);

/** How a part uses its instrument: the mode (an index into the profile's modes) and an articulation (a switch
    key's "what") per section id. */
struct Use
{
    int mode = -1;
    std::map<juce::String, juce::String> articulations;
    bool picked = false; // the mode was picked by its words, not named in the song
};

/** The use a song's part makes of its instrument: the song's `modes` entry for it ({mode, articulations}) where
    it names a usable mode and that mode's switches, else the mode `pick` finds for `words` and no articulation. */
Use useFor (const juce::var& song, const juce::String& part, const Profile& profile, const juce::String& words);

/** A part's instrument as the song is written for it: its profile and the words its mode is picked by. */
struct Instrument
{
    Profile profile;
    juce::String words;
};

/** The song with each part's notes kept to the playing keys of the mode it uses (as keepToRanges), and its mode's
    entering key and its articulations' switch keys written as short notes at the start of every section the part
    plays in: the articulation named for that section, else the one named for the section before it in the song,
    else the first named. Notes starting with a switch note move just after it, so the switch comes first. Parts by
    layer id (a full arrangement) or part ("bass"); the song's `modes` gets the use of each. */
juce::var apply (const juce::var& song, const std::map<juce::String, Instrument>& instruments);

/** The ticks a switch note lasts, and how far after it a note starting with it moves. */
constexpr juce::int64 switchTicks = 30, switchLead = 8;

/** "Guitar: Normal mode; palm mute, open" for each part with a profile, for the song's note. */
juce::StringArray describe (const juce::var& song, const std::map<juce::String, Instrument>& instruments,
                            const std::map<juce::String, juce::String>& names);
} // namespace partmodes

} // namespace spinus
