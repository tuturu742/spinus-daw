#pragma once

#include "SampleSet.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>

#include <atomic>

namespace spinus::dsp
{

/** A zone as a ValueTree for a device's saved state, with its audio FLAC-compressed inside. */
juce::ValueTree zoneToState (const SampleZone&);
SampleZone zoneFromState (const juce::ValueTree&);

/** Reads an audio file for a zone: stereo at most, up to `maxSeconds`. */
juce::Result readAudioFile (const juce::File&, SampleZone& result, double maxSeconds = 600.0);

/** The audio file formats Spinus reads (WAV, AIFF, FLAC, Ogg, Opus, MP3): one shared list. */
juce::AudioFormatManager& getAudioFormats();

/** The SampleSet a device's audio thread plays, swapped safely (message thread publishes, audio thread
    reads once per block). Old sets are freed once the audio thread cannot be using them any more. */
class SampleSetHolder
{
public:
    SampleSetHolder() { publish ({}); }

    /** Message thread. */
    void publish (std::vector<SampleZone>);
    const SampleSet& getCurrent() const noexcept { return *current; }

    /** Audio thread: the set to play this block, and the call at the end of each block. */
    const SampleSet* getPlaying() const noexcept { return playing.load(); }
    void blockDone() noexcept { blocksDone.fetch_add (1); }

private:
    std::unique_ptr<SampleSet> current;
    std::atomic<const SampleSet*> playing { nullptr };
    std::atomic<uint64_t> blocksDone { 0 };
    std::vector<std::pair<std::unique_ptr<SampleSet>, uint64_t>> retired; // set, blocksDone when retired
};

} // namespace spinus::dsp
