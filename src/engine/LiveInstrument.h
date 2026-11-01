#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <vector>

namespace spinus::engine
{

/** One instrument played live through the sound card, outside the song: the Track creator's Save this sound
    window, so what the user does in the plugin's own window (a drum player's pads, a synth's keyboard) is
    heard, and Play plays a phrase on it. SamplePreview mixes it into the output (setLive).

    prepare(), play(), stopNotes() and takePeak() are called from the message thread; mix() from the audio
    thread. Notes reach the audio thread through a lock-free FIFO; mix() allocates nothing
    (docs/dev/realtime-checklist.md). The instrument is not processed anywhere else while it is live. */
class LiveInstrument
{
public:
    struct Note
    {
        int note = 60;
        double start = 0.0, length = 0.25; // seconds from when it is played
        float velocity = 0.8f;
    };

    static constexpr int maxBlockSize = 512;
    static constexpr int maxEvents = 2048; // note ons and offs waiting to be played

    explicit LiveInstrument (juce::AudioPluginInstance&);

    /** Before it is mixed: the instrument at the sound card's rate. */
    void prepare (double sampleRate);
    double getSampleRate() const noexcept { return sampleRate; }

    /** Plays the notes from the next block on (cutting what still plays). False if there are too many. */
    bool play (const std::vector<Note>&);
    /** Ends every note now. */
    void stopNotes();
    /** The loudest sample since the last call. */
    float takePeak() noexcept { return peak.exchange (0.0f); }

    /** Audio thread: adds its next block to the outputs (a mono instrument on both sides). */
    void mix (float* const* outputs, int numChannels, int numSamples) noexcept;

private:
    struct Event
    {
        juce::int64 at = 0;       // samples: from play() in the FIFO, from the start of mixing once taken
        int note = 0;             // -1: all notes off
        juce::uint8 velocity = 0; // 0: note off
    };

    bool push (const std::vector<Event>&);

    juce::AudioPluginInstance& instance;
    double sampleRate = 48000.0;
    int right = 1; // the output channel heard on the right
    std::atomic<float> peak { 0.0f };

    juce::AbstractFifo fifo { maxEvents };
    std::array<Event, maxEvents> queued {};

    // Audio thread only (and prepare(), before it is mixed).
    juce::AudioBuffer<float> block;
    juce::MidiBuffer midi;
    std::array<Event, maxEvents> waiting {};
    int numWaiting = 0;
    juce::int64 now = 0;
};

} // namespace spinus::engine
