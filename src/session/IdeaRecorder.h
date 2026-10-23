#pragma once

#include <tracktion_engine/tracktion_engine.h>

namespace spinus
{

class Session;

/** "Record idea": records the audio input while the current pattern plays,
    for hum, sing or beatbox to notes.

    In time with the song: the pattern plays from its start with the metronome; the first time through
    is a count-in and the second is the take. The pattern's start is found in the file by mapping the
    input's stream time to the song's time, as Tracktion does for its own recordings, less the input,
    output and playback latency. Free takes (for teaching beatbox sounds) record a few seconds without
    playing anything.

    The input is the first audio input Spinus has (Options › Audio). The take is a 24-bit mono WAV in
    the project's cache/ideas/ (or a temporary folder while the project is untitled).

    Message thread, except acceptInputBuffer() (audio thread: it only writes to a lock-free FIFO).
*/
class IdeaRecorder : private juce::Timer, private tracktion::InputDeviceInstance::Consumer
{
public:
    explicit IdeaRecorder (Session&);
    ~IdeaRecorder() override;

    struct Take
    {
        juce::File file;
        double patternStart = 0.0;   // where in the file the pattern's start is, in seconds
        double patternSeconds = 0.0; // the pattern's length (0 for a free take)
        double bpm = 120.0;
    };

    /** Records the current pattern's second time through. Fails without an audio input. */
    juce::Result startWithPattern();

    /** Records `seconds` of input without playing anything (e.g. teaching a beatbox sound). */
    juce::Result startFree (double seconds);

    /** Stops without a take. */
    void cancel();

    enum class State
    {
        idle,
        countIn,   // the pattern's first time through: listen
        recording, // the take
    };
    State getState() const noexcept { return state; }

    /** While counting in: whole beats until the take starts. While recording: 0..1 of the take. */
    int getBeatsToGo() const noexcept { return beatsToGo; }
    float getProgress() const noexcept { return progress; }

    /** The input's level (peak of the last block, 0..1), for a meter. */
    float getInputLevel() const noexcept { return level.load(); }
    juce::String getInputName() const { return inputName; }

    /** Called on the message thread with the finished take. */
    std::function<void (const Take&)> onFinished;
    /** Called on the message thread when recording stops without a take (cancel, or a problem). */
    std::function<void (const juce::String& why)> onStopped;

private:
    juce::Result begin();
    void finish();
    void stopRecording();
    void timerCallback() override;
    void acceptInputBuffer (choc::buffer::ChannelArrayView<float>) override;
    double editSecondsNow (double streamTime) const;

    Session& session;
    State state = State::idle;
    bool free = false;
    double freeSeconds = 0.0;
    bool metronomeWas = false;
    int beatsToGo = 0;
    float progress = 0.0f;
    juce::String inputName;
    int inputChannel = 0; // the chosen input's channel in the recording input

    tracktion::InputDeviceInstance* input = nullptr;
    juce::TimeSliceThread writerThread { "Spinus idea writer" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    juce::AudioBuffer<float> mono;
    juce::File file;
    double sampleRate = 48000.0;
    std::atomic<double> firstStreamTime { -1.0 };
    std::atomic<int64_t> samplesWritten { 0 };
    std::atomic<float> level { 0.0f };
    std::optional<double> fileStartSeconds; // the song time (unlooped) of the file's first sample

    JUCE_DECLARE_NON_COPYABLE (IdeaRecorder)
};

} // namespace spinus
