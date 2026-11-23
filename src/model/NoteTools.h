#pragma once

#include "Model.h"

namespace spinus::model::noteTools
{

/** The Piano Roll's Tools menu (FL's note tools). Each takes notes and gives the changed notes; the Piano
    Roll applies them to the selection (or every note) as one undo step. */

/** Starts on the nearest grid line (`grid` ticks); lengths stay. */
std::vector<Note> quantize (std::vector<Note>, Ticks grid);

/** Notes that start together (a chord) start one after another, `spacing` ticks apart, from the lowest
    (`up`) or the highest; each still ends where it did. */
std::vector<Note> strum (const std::vector<Note>&, Ticks spacing, bool up);

/** Each chord (notes that start together) becomes its notes one after another, `step` ticks each, going
    up and starting again, for as long as the chord lasted. Single notes stay. */
std::vector<Note> arpeggiate (const std::vector<Note>&, Ticks step);

/** Each note cut into repeats `step` ticks long, for as long as it lasted (FL's chop and note repeat). */
std::vector<Note> chop (const std::vector<Note>&, Ticks step);

/** Each note lasts until the next note starts (the last one keeps its length). */
std::vector<Note> legato (std::vector<Note>);

/** Velocities moved at random by up to `amount` (0..1), kept between 0.05 and 1. */
std::vector<Note> humanizeVelocity (std::vector<Note>, float amount, juce::Random&);

/** Each chord (notes that start together; a single note is one too) becomes the chord with these intervals
    (semitones, 0 first) on its lowest note, held as long as its longest note, at its loudest note's velocity.
    Pitches past 127 are left out. */
std::vector<Note> chordOnEach (const std::vector<Note>&, const std::vector<int>& intervals);

/** Notes that start together at the same pitch become one (the longest, the loudest velocity), and a note
    that starts where another of its pitch is still sounding cuts that one short. */
std::vector<Note> removeOverlaps (std::vector<Note>);

} // namespace spinus::model::noteTools
