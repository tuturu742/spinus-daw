#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <memory>
#include <optional>
#include <vector>

namespace spinus::dsp
{

/** One sample in the Sampler and where it plays: the keys and velocities it answers,
    the note it plays at its own pitch, which part of it plays, and its loop. Audio is shared between
    copies, so changing a zone's settings never copies the audio. */
struct SampleZone
{
    juce::String name; // shown in the editor: usually the file name without extension
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    double sampleRate = 44100.0;

    int rootNote = 60; // MIDI note; C5 in FL's naming
    int lowKey = 0, highKey = 127;
    int lowVelocity = 1, highVelocity = 127;

    juce::int64 start = 0, end = 0; // in samples; end is exclusive
    bool loop = false;
    juce::int64 loopStart = 0, loopEnd = 0;

    int settingsIndex = 0;  // which SamplerSettings play this zone (the Drum Machine has one per pad)
    int chokeGroup = 0;     // 1..8: starting a note stops other zones' notes in the same group (open/closed hat)
    int fadeOutSamples = 0; // fades out over this many samples before `end` (the Beat Slicer's slices)

    juce::int64 getLength() const noexcept { return audio != nullptr ? audio->getNumSamples() : 0; }
    bool answers (int note, int velocity) const noexcept
    {
        return note >= lowKey && note <= highKey && velocity >= lowVelocity && velocity <= highVelocity;
    }

    /** Keeps start/end/loop points inside the audio and in order. */
    void clampPoints() noexcept;

    bool operator== (const SampleZone&) const = default;
};

/** All of a Sampler's zones. Immutable once handed to the audio thread: an edit makes a new set. */
struct SampleSet
{
    std::vector<SampleZone> zones;

    /** Zones that answer this note and velocity. Zones with the same key and velocity range are
        round-robin variations: `counter` picks one of them each time (the audio thread advances it). */
    const SampleZone* pick (int note, int velocity, uint32_t counter) const noexcept;
};

/** The note in a file name such as "Piano_C#4.wav" or "bass-a2", in FL's naming (C5 = MIDI 60). */
std::optional<int> noteFromFileName (const juce::String& fileNameWithoutExtension);

/** "C5", "C#4"... for a MIDI note, in FL's naming. */
juce::String noteName (int midiNote);

/** Spreads zones over the keyboard by their root notes (each zone covers the keys from halfway to the
    previous root to halfway to the next), keeping their velocity ranges. Zones with the same root
    keep the same range (round-robin or velocity layers). */
void mapZonesByRoot (std::vector<SampleZone>&);

} // namespace spinus::dsp
