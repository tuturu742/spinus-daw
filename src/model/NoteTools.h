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

} // namespace spinus::model::noteTools
