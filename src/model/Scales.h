#pragma once

#include "Model.h"

namespace spinus::model
{

/** The scales a project's key can name (Key::scale), in menu order: "major", "minor", ... */
const juce::StringArray& getScaleNames();

/** True if the MIDI note is in the key (an unknown scale name counts as major). */
bool isInKey (const Key&, int note);

/** The note of the key nearest to a (fractional) MIDI pitch. */
int nearestNoteInKey (const Key&, float pitch);

/** True if the key is written with flats: its major key (a minor key's relative major, a mode's parent
    major) is F, Bb, Eb, Ab or Db. So C minor names its notes Eb and Bb, not D# and A#. */
bool usesFlats (const Key&);

/** A note's name without the octave: "C#" (or "Db" with `flats`). */
juce::String pitchClassName (int pitch, bool flats = false);

} // namespace spinus::model
