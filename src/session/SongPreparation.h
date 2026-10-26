#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>

namespace spinus
{

/*  A song made ready for the analysis steps (the Analyzer, the Track creator): decoded to a stereo 32-bit
    float WAV file that the AI service reads, named by the song's contents so that it is decoded once.
    Any thread (the panels decode on a thread of their own). */

/** Extensions of songs the analysis steps take: those Spinus reads itself and those ffmpeg reads. */
bool isSongFile (const juce::File&);
/** The file dialog's pattern for them. */
juce::String songFilePatterns();

/** The SHA-256 of a file's contents, in hex (reads it all). */
juce::String contentDigest (const juce::File&);

/** Where `song` is decoded to in `folder`: its name (safe characters only) and the first 16 hex digits of
    its contents' SHA-256 (`digest`). */
juce::File preparedSongFile (const juce::File& folder, const juce::File& song, const juce::String& digest);

/** Decodes `source` into `destination` (nothing to do when it is there already): with Spinus's own
    decoders, or with the system's ffmpeg for formats Spinus cannot read. `progress` goes from 0 to 1;
    `shouldStop` is asked between blocks ("Stopped." when it says yes). */
juce::Result prepareSong (const juce::File& source, const juce::File& destination,
                          const std::function<bool()>& shouldStop, std::atomic<double>& progress);

} // namespace spinus
