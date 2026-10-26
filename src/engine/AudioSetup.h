#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace spinus::engine
{

/** Audio device choices. */
struct AudioSetup
{
    static constexpr double defaultSampleRate = 48000.0;
    static constexpr int defaultBufferSize = 256;

    /** First start: JACK (normally PipeWire's) if a server is reachable, otherwise ALSA on the
        "pipewire" or default device; 48 kHz, 256 samples. Spinus never starts a JACK server itself.
        Returns a description of what was chosen. */
    static juce::String applyDefaults (juce::AudioDeviceManager&);

    /** JACK lists every program that plays sound as an input, and JUCE takes the first: a music player
        playing when Spinus first starts would be what it records. The first of `jackInputs` that is one of
        PipeWire's `microphones` (their descriptions or names, PipeWireInput::listSources), else none. */
    static juce::String pickJackInput (const juce::StringArray& jackInputs, const juce::StringArray& microphones);

    /** What the settings page shows. */
    struct Status
    {
        juce::String type, device;
        double sampleRate = 0.0;
        int bufferSize = 0, outputLatencySamples = 0, xruns = 0;

        double getBufferMs() const { return sampleRate > 0 ? 1000.0 * bufferSize / sampleRate : 0.0; }
        double getOutputLatencyMs() const { return sampleRate > 0 ? 1000.0 * outputLatencySamples / sampleRate : 0.0; }
        juce::String describe() const;
    };

    static Status getStatus (juce::AudioDeviceManager&);
};

} // namespace spinus::engine
