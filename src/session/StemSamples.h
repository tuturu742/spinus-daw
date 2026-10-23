#pragma once

#include "Session.h"

namespace spinus
{

/** Samples from a song's stem (the Analyzer's "the song's own sounds"): pieces of the stem's
    audio put into a new channel's Drum Machine pads or Sampler keys, chosen by the AI service
    (ai.v1.samples.make). The audio goes into the device's own saved state, like a sample
    dropped onto the Channel Rack. */
struct StemPiece
{
    int note = 60;                 // the Drum Machine pad's or the Sampler key's note
    double start = 0.0, end = 0.0; // seconds in the stem
    juce::String name;
};

/** Cuts a piece of an audio file, with short fades so it starts and stops without a click. Empty when the
    file cannot be read or the piece is outside it. */
juce::AudioBuffer<float> cutPiece (juce::AudioFormatReader&, double start, double end);

/** Puts the pieces on the Drum Machine pads that play their notes, keeping the other pads' sounds. */
juce::Result loadDrumPieces (Session&, model::ChannelId, const juce::File& stem, const std::vector<StemPiece>&);

/** Loads an audio file onto the Drum Machine pad that plays `note` (a suggested drum sample). */
juce::Result loadDrumPadFile (Session&, model::ChannelId, int note, const juce::File&);

/** Gives the pad that plays `to` the sound of the pad that plays `from`, named `name` (a second group of a
    drum before a better sample is chosen). */
juce::Result copyDrumPad (Session&, model::ChannelId, int from, int to, const juce::String& name);

/** Replaces the channel's Sampler zones with the pieces, one key each, played at their own pitch. */
juce::Result loadSamplerPieces (Session&, model::ChannelId, const juce::File& stem, const std::vector<StemPiece>&);

} // namespace spinus
