#pragma once

#include "Model.h"

namespace spinus::model
{

/** Chords for the Piano Roll's chord stamp and chord names (FL's chord tool). */
struct ChordType
{
    const char* name;           // "Minor 7"
    const char* suffix;         // "m7": after the root's name ("Cm7")
    std::vector<int> intervals; // semitones above the root, the root (0) first
};

/** The chords the stamp offers and names recognise, in menu order. */
const std::vector<ChordType>& getChordTypes();

/** The chord of `type` on `root`, inside MIDI's 0 to 127. */
std::vector<int> chordOn (int root, const ChordType&);

/** The key's own chord on `root` (moved to the nearest note of the key): every second note of the scale,
    `notes` of them (3: a triad, 4: a seventh chord). */
std::vector<int> chordInKey (int root, const Key&, int notes);

/** "Cm7", "F/A", "G": the chord the notes make, with the bass after a slash when it isn't the root; empty
    when the notes aren't a chord Spinus knows (or are fewer than three different notes). */
juce::String nameChord (const std::vector<int>& pitches);

} // namespace spinus::model
