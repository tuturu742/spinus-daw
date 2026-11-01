#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <tracktion_engine/tracktion_engine.h>

#include <atomic>

#include "LiveInstrument.h"

namespace spinus::engine
{

/** Plays one audio file at a time through the sound card, outside the song: the Browser's sample
    preview. The file is read and resampled on the calling (message) thread; the audio thread only adds
    from that buffer into the engine's output, as Tracktion's global output processor (after the song,
    never touching the input, which may share the output's memory). Sounds are handed to the audio
    thread through an atomic pointer and handed back through a lock-free FIFO, to be freed on the
    message thread (docs/dev/realtime-checklist.md). One engine has one: a second SamplePreview on the
    same engine stays silent. A live instrument (LiveInstrument: Save this sound's window) is added to the
    output too, while one is set.
*/
class SamplePreview final : private juce::Timer
{
public:
    static constexpr double maxSeconds = 30.0; // longer files preview their start
    static constexpr float gain = 0.7f;

    /** Without a device manager (tests), nothing is heard, but mix() can be called. */
    explicit SamplePreview (tracktion::DeviceManager* = nullptr);
    ~SamplePreview() override;

    /** Starts playing the file from its start, stopping the one before. False if it cannot be read. */
    bool play (const juce::File&, juce::AudioFormatManager&);
    void stop();
    /** True from play() until the sound ends or stop(). */
    bool isPlaying() const noexcept { return playing.load(); }
    juce::File getFile() const { return file; }

    /** What the audio thread does: adds the next block, at `sampleRate`, to the outputs. */
    void mix (float* const* outputs, int numChannels, int numSamples) noexcept;
    void setSampleRate (double rate) noexcept { sampleRate.store (rate); }
    double getSampleRate() const noexcept { return sampleRate.load(); }

    /** Message thread: plays `instrument` (prepared at getSampleRate()) live until another one or none is set.
        Once it returns, the audio thread no longer uses the one before: it can be deleted. */
    void setLive (LiveInstrument* instrument);
    LiveInstrument* getLive() const noexcept { return live.load(); }

private:
    struct Sound
    {
        juce::AudioBuffer<float> audio; // stereo, at the device's rate; empty: silence (stop)
    };

    class Output;

    void timerCallback() override { freeRetired(); }

    void mixFile (float* const* outputs, int numChannels, int numSamples) noexcept;
    void publish (Sound*);
    void freeRetired();

    tracktion::DeviceManager* devices;
    Output* output = nullptr; // installed in `devices`, owned by it
    juce::File file;
    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<Sound*> pending { nullptr };
    std::atomic<bool> playing { false };
    std::atomic<LiveInstrument*> live { nullptr };
    std::atomic<bool> mixingLive { false };

    // Audio thread only.
    Sound* current = nullptr;
    int position = 0;

    // Sounds the audio thread is done with, freed by the message thread.
    static constexpr int retiredSize = 64;
    juce::AbstractFifo retiredFifo { retiredSize };
    std::array<Sound*, retiredSize> retired {};
};

} // namespace spinus::engine
