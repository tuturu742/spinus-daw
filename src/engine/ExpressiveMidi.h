#pragma once

#include "model/Model.h"

#include <tracktion_engine/tracktion_engine.h>

namespace spinus::engine
{

/** Per-note fine pitch, pan and volume on their way to instruments.

    EngineAdapter writes them onto Tracktion's notes with setNoteExpressions() and switches the clip to
    MPE mode when any note has them. When Tracktion builds the clip's playback MIDI, it calls
    createSequence(), which puts each such note on its own channel with the values just before it
    (dsp/NoteExpression.h). Only instruments that read them get them: plugins that expect every note on
    channel 1 would drop the rest. Release velocity is an ordinary note-off velocity and goes to every
    instrument. */
namespace expressiveMidi
{
/** True if the instrument reads per-note values: Spinus's own instruments and CLAP plugins (as
    CLAP note expressions). */
bool canPlay (const model::Device& instrument);

/** Stores the note's fine pitch, pan and volume on the Tracktion note. Returns true if any differs
    from its default. */
bool setNoteExpressions (tracktion::MidiNote&, const model::Note&);
/** The same on a note's state (a NOTE tree), before it is added to a list. */
bool setNoteExpressions (juce::ValueTree& noteState, const model::Note&);

/** True if the list has a note with values from setNoteExpressions(). */
bool hasNoteExpressions (const tracktion::MidiList&);

/** The playback MIDI for an MPE-mode clip whose notes have values (see EngineBehaviour). */
juce::MidiMessageSequence createSequence (const tracktion::MidiList&, tracktion::MidiClip&,
                                          tracktion::MidiList::TimeBase);
} // namespace expressiveMidi

} // namespace spinus::engine
