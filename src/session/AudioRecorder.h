#pragma once

#include "RecordingTime.h"
#include "model/Model.h"

#include <tracktion_engine/tracktion_engine.h>

#include <optional>

namespace spinus
{

class Session;

/** Recording audio: from one microphone, or from several inputs at once (drums, vocals, guitar).
    The song plays from the current position and the sound card's inputs are written to WAV files
    until playback stops:

    - with audio tracks armed, each armed track records its own input (Channel::recordInput, mono or a
      stereo pair) into a take of its own, all at once and sample-aligned, as from a multi-input sound card;
    - with none armed, the input chosen in Options › Audio is recorded into one take, which the caller
      places (a new track, or the "Record into" track).

    Each take goes on the Playlist where it was played: the files' start is found by mapping the input's
    stream time to the song's time, less the input, output and playback latency, as for Record idea. The
    takes are 24-bit WAVs in the project's audio/ folder ("Recording 1.wav", "Snare mic 1.wav"), or a
    temporary folder while the project is untitled (saving copies them into the project).

    While a track is armed, the inputs are metered (getInputLevel) so levels can be set before recording.

    Message thread, except acceptInputBuffer() (audio thread: it only hands the input to the writers'
    lock-free FIFOs and updates the meters; no allocation, no locks).
*/
class AudioRecorder : private juce::Timer,
                      private juce::ChangeListener,
                      private tracktion::InputDeviceInstance::Consumer
{
public:
    explicit AudioRecorder (Session&);
    ~AudioRecorder() override;

    struct Take
    {
        juce::File file;
        double startSeconds = 0.0; // where in the song the file's first sample was played (may be < 0)
        int channels = 1;
        std::optional<model::ChannelId> track; // the armed track it was recorded for; none: the caller decides
    };

    /** Starts playing the song from the current position and recording the armed tracks (or the default
        input). Fails without an audio input, or when an armed track's input is not on the sound card. */
    juce::Result start();
    /** Stops playback and recording; the takes arrive in onFinished. */
    void stop();
    /** Stops without takes (the files are deleted). */
    void cancel();

    bool isRecording() const noexcept { return recording; }
    /** Seconds recorded so far. */
    double getSeconds() const noexcept { return sampleRate > 0.0 ? (double)samplesWritten.load() / sampleRate : 0.0; }
    /** The loudest take's level (peak of the last block, 0..1), for a meter. */
    float getInputLevel() const noexcept;
    /** An input's level (0..1, counted from 0 as on the sound card), while recording or while a track is
        armed (see setMetering); 0 when that input is not in use. */
    float getInputLevel (int input) const noexcept;
    /** What is being recorded: "Input 1" or "3 tracks". */
    juce::String getInputName() const { return inputName; }
    /** How many takes the recording makes. */
    int getNumTakes() const noexcept { return (int)sources.size(); }

    /** Meters the inputs of the armed tracks while not recording (switching those inputs on). */
    juce::Result setMetering (bool on);
    bool isMetering() const noexcept { return metering; }
    /** Called now and then while metering (a timer): follows the recording input when the sound card
        rebuilt it. */
    void keepMetering();

    /** Called on the message thread with the finished takes (one per armed track, or one). */
    std::function<void (const std::vector<Take>&)> onFinished;
    /** Called on the message thread when recording stops without a take, with why. */
    std::function<void (const juce::String& why)> onStopped;

    static constexpr int maxInputs = 64;

private:
    struct Source
    {
        std::optional<model::ChannelId> track;
        juce::String name;
        int first = 0, channels = 1; // inputs on the sound card
        int index[2] = { 0, 0 };     // their channels in the recording input
        juce::File file;
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        std::atomic<float> level { 0.0f };
    };

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void acceptInputBuffer (choc::buffer::ChannelArrayView<float>) override;
    void finish (bool keep, const juce::String& why = {});
    juce::Result attach();
    void detach();
    bool isAttachedInputAlive() const;
    std::set<int> armedInputs() const;

    Session& session;
    tracktion::InputDeviceInstance* input = nullptr;
    std::vector<std::unique_ptr<Source>> sources; // set before the consumer is added, read by the audio thread
    std::vector<int> inputOrder;                  // the sound card input of each channel of the recording input
    std::array<std::atomic<float>, maxInputs> levels {};
    juce::String inputName;
    double sampleRate = 0.0;
    bool recording = false, metering = false;
    std::optional<recording::InputUse> inputUse; // while metering or recording
    std::atomic<bool> writing { false };
    juce::TimeSliceThread writerThread { "Spinus audio recording" };
    std::atomic<double> firstStreamTime { -1.0 };
    std::atomic<juce::int64> samplesWritten { 0 };
    std::optional<double> fileStartSeconds;
    bool sawPlaying = false;
    std::set<int> lastWanted;    // the armed inputs keepMetering() last switched on...
    std::vector<int> lastActive; // ...and the inputs that were on then
};

} // namespace spinus
