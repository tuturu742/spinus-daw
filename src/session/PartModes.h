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

    A mode the songwriting can use is reachable (the one the instrument opens in, the first, or one a switch key
    enters) and either plays notes on known keys or is a chord mode: no playing keys, but chord keys (special keys
    whose words say "chord", an octave or more of them) that pick the chord the instrument plays, often with pattern
    keys (special keys that say "pattern", "sequence", "rhythm", "strum"...) that play it. In a chord mode Spinus
    holds each of the song's chords on the chord keys and presses the section's pattern key with it. Plain data, any
    thread but the audio thread. */
namespace partmodes
{
using Profile = InstrumentProfiles::Profile;

/** A chord mode's chord keys (lowest and highest; -1 when none) and pattern keys (what -> key, in key order). */
struct ChordKeys
{
    int low = -1, high = -1;
    std::vector<std::pair<juce::String, int>> patterns;
};
ChordKeys chordKeysOf (const InstrumentProfiles::Mode&);
bool isChordMode (const InstrumentProfiles::Mode&);

/** The modes of `profile` the songwriting can use, by index. */
std::vector<int> usable (const Profile& profile);

/** What the AI is told of the usable modes: [{name, goodFor, voices, low, high, notPlaying, switches: [{key,
    what}], first}], in that order (a chord mode: chordKeys true, low and high its chord keys, and its patterns:
    [{key, what}]); an empty array when none is usable. */
juce::var brief (const Profile& profile);

/** The usable mode whose "good for" words best match `words` (the part's role and name, the genre): its index,
    the first usable mode when none matches, -1 when none is usable. A chord mode only for a part that plays chords
    (`chords`). */
int pick (const Profile& profile, const juce::String& words, bool chords = false);

/** How a part uses its instrument: the mode (an index into the profile's modes) and an articulation (a switch
    key's "what", or a chord mode's pattern) per section id. */
struct Use
{
    int mode = -1;
    std::map<juce::String, juce::String> articulations;
    bool picked = false; // the mode was picked by its words, not named in the song
};

/** The use a song's part makes of its instrument: the song's `modes` entry for it ({mode, articulations}) where
    it names a usable mode and that mode's switches, else the mode `pick` finds for `words` and no articulation. */
Use useFor (const juce::var& song, const juce::String& part, const Profile& profile, const juce::String& words,
            bool chords = false);

/** A part's instrument as the song is written for it: its profile and the words its mode is picked by. */
struct Instrument
{
    Profile profile;
    juce::String words;
    bool chords = false; // the part plays chords (a chord mode may be picked for it)
};

/** The song with each part's notes kept to the playing keys of the mode it uses (as keepToRanges), and its mode's
    entering key and its articulations' switch keys written as short notes at the start of every section the part
    plays in: the articulation named for that section, else the one named for the section before it in the song,
    else the first named. Notes starting with a switch note move just after it, so the switch comes first. In a
    chord mode the part's notes (where it plays) become the section's chords held on the chord keys (the root at or
    above the lowest, the other tones above it) and its pattern key pressed with each chord (the pattern named for
    the section, carried as an articulation is, else one picked by the section's energy). Parts by layer id (a full
    arrangement) or part ("bass"); the song's `modes` gets the use of each. */
juce::var apply (const juce::var& song, const std::map<juce::String, Instrument>& instruments);

/** The ticks a switch note lasts, and how far after it a note starting with it moves. */
constexpr juce::int64 switchTicks = 30, switchLead = 8;

/** "Guitar: Normal mode; palm mute, open" for each part with a profile, for the song's note. */
juce::StringArray describe (const juce::var& song, const std::map<juce::String, Instrument>& instruments,
                            const std::map<juce::String, juce::String>& names);
} // namespace partmodes

} // namespace spinus
