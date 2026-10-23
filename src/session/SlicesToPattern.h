#pragma once

#include "Session.h"

namespace spinus
{

/** "Slices to pattern" (Beat Slicer): writes one note per slice of the channel's Beat
    Slicer, at the slice's place in the loop, so the pattern plays the loop back; the notes can then
    be moved, repeated or deleted to rearrange it.

    The notes replace the channel's notes in the current pattern when the loop fits in it. Otherwise a
    new pattern as long as the loop (whole bars) is made for them and becomes the current pattern.
    Either way it is one undo step. */
juce::Result slicesToPattern (Session&, model::ChannelId);

/** True if the channel's instrument is a Beat Slicer with a loop. */
bool hasSlices (Session&, model::ChannelId);

/** Lets the Beat Slicer's window offer "Slices to pattern" for this plugin (if it is a Beat Slicer). */
void connectSlicesToPattern (Session&, tracktion::Plugin&);

} // namespace spinus
