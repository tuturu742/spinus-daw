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

} // namespace spinus::model
