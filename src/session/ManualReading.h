#pragma once

#include "InstrumentProfiles.h"

namespace spinus
{

/** The AI's reading of an instrument's manuals (Read the manual, the AI service's ai.v1.instrument.readManual) as
    a proposal for its profile: the modes the manuals name, how to enter each, what each suits, and what the keys of
    each do (Source::manual). Shown in Learn this instrument; nothing changes until Apply, and what the user set
    stays. Plain data: message thread. */
namespace manualreading
{
struct Reading
{
    std::vector<InstrumentProfiles::Mode> modes; // the keys the manuals name; the others unknown
    bool middleCSaid = true;    // false: the manuals don't say how they number octaves (C3 was taken as middle C)
    int octavesMoved = 0;       // by fitOctaves
    juce::String notes, by;     // the AI's one-sentence note, and which model read it
    juce::StringArray problems; // the manuals that could not be read, and why
    juce::String file;          // an imported profile's file name (Import...): what it says, not a manual
};

/** The service's answer as a reading; nullopt when it holds no mode. */
std::optional<Reading> fromVar (const juce::var&);

/** When the manuals don't say how they number octaves: the reading's keys moved by whole octaves (up to two either
    way) to fit best what Spinus heard or was told (the keys that play in the matching modes); unmoved when nothing
    is known to compare with. Sets and returns `octavesMoved`. */
int fitOctaves (Reading&, const InstrumentProfiles::Profile&);

/** Which of the profile's modes a proposed mode is: the one with the same name, else (for a mode entered by
    nothing, as the instrument opens) the first; -1 when it is a new mode. */
int matchingMode (const InstrumentProfiles::Profile&, const InstrumentProfiles::Mode& proposed);

/** Takes the reading into the profile: a mode not there is added; a mode there gets what it lacks (how to enter
    it, what it suits, chords or one note; the first mode's name while it is still "Normal") and the keys the
    manuals name, the keys the user set staying theirs (keyprobe::merge). Returns how many things changed. */
int apply (InstrumentProfiles::Profile&, const Reading&);

/** "2 modes: Normal, Strummer (entered by C#8, MIDI 97)" (keys named as the Piano roll names them, middle C being
    C5, with their MIDI numbers: manuals number octaves differently). */
juce::String describe (const Reading&);
} // namespace manualreading

} // namespace spinus
