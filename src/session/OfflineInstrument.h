#pragma once

#include "InstrumentPresets.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

namespace spinus
{

/** Plays an instrument plugin offline, driven directly (never by an audio device): loads a preset and waits
    until it is in place, then plays notes from silence and returns the sound. Shared by HiddenInstrument
    (inside Spinus) and the sound index's helper process (session/SoundRender), so both record presets the
    same way.

    Prepares the plugin when made (stereo out, at sampleRate and blockSize). loadPreset() runs on the message
    thread; render() may then be used from one other thread at a time (it is processed like an audio thread
    would, but never on Spinus's audio thread). */
class OfflineInstrument
{
public:
    static constexpr double sampleRate = 48000.0;
    static constexpr int blockSize = 512;

    struct Note
    {
        int note = 60;
        double start = 0.0, length = 1.0; // seconds
        float velocity = 0.8f;
    };

    explicit OfflineInstrument (juce::AudioPluginInstance&);
    ~OfflineInstrument();

    juce::AudioPluginInstance& getInstance() const noexcept { return instance; }

    /** Loads any preset (a file, a state, a program or a discovered one) and waits until it is in place. */
    juce::Result loadPreset (const InstrumentPreset&);

    /** Plays the notes from silence and returns the sound (mono, at sampleRate), `tail` seconds after the
        last note ends. */
    juce::AudioBuffer<float> render (const std::vector<Note>&, double tail = 0.4);

private:
    /** Stops every note, cuts what still rings, and waits a moment. */
    void silence();
    void play (int samples, const juce::MidiBuffer&, juce::AudioBuffer<float>* into);

    juce::AudioPluginInstance& instance;
};

} // namespace spinus
