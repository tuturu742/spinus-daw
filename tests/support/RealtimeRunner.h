#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include <atomic>
#include <functional>
#include <thread>

namespace spinus::test
{

/*  Drives a Tracktion Engine through its HostedAudioDeviceInterface from a dedicated thread,
    paced like a real audio callback, while the calling (message) thread keeps editing.
    Every output sample and every block's processing time is recorded for later analysis.
*/
class RealtimeRunner
{
public:
    struct Params
    {
        double sampleRate = 48000.0;
        int blockSize = 128;
        int outputChannels = 4; // two stereo output devices
        int inputChannels = 0;
        bool loopback = false; // each block's output (channels 1/2) is the next block's input, as a cable
        double maxSeconds = 30.0;
        bool paceInRealTime = true;
        /** Fills the inputs of each block (after loopback), from the block's first sample: a sound card
            with something different on each input. */
        std::function<void (juce::AudioBuffer<float>& block, int inputChannels, int64_t firstSample)> inputSource;
    };

    RealtimeRunner (tracktion::Engine&, Params);
    ~RealtimeRunner();

    void start();
    void stop();
    /** As when the sound card is opened again (another output chosen, a project opened): the device stops
        and starts again, with the same sample rate and block size. The recording carries on. */
    void restartDevice();

    int64_t getSamplesProcessed() const noexcept { return samplesProcessed.load(); }
    double getSecondsProcessed() const noexcept { return (double)getSamplesProcessed() / params.sampleRate; }

    /** Keeps the message thread running (so Tracktion can rebuild its graph) until the audio
        thread has rendered the given number of seconds in total. */
    void runUntil (double secondsProcessed);

    /** Device ID of an output pair: 0 = channels 1/2, 1 = channels 3/4. */
    juce::String getOutputDeviceID (int pairIndex) const;

    const std::vector<float>& getChannel (int channel) const { return output[(size_t)channel]; }
    int64_t getNumRecordedSamples() const noexcept { return std::min<int64_t> (getSamplesProcessed(), capacity); }

    const Params& getParams() const noexcept { return params; }
    double getBlockPeriodMs() const noexcept { return 1000.0 * params.blockSize / params.sampleRate; }

    /** Processing times of each block, in milliseconds. */
    const std::vector<float>& getBlockTimesMs() const { return blockTimesMs; }
    int getNumOverrunBlocks() const;

    /** Longest single pass of the message loop inside runUntil(), in ms (includes the 2 ms wait).
        Long passes mean the UI thread was blocked, e.g. by a graph rebuild. */
    double getMaxMessageThreadStallMs() const noexcept { return maxDispatchMs; }
    void resetMaxMessageThreadStall() noexcept { maxDispatchMs = 0.0; }

    /** With no sound processed for this long while running, the watchdog prints which thread is stuck and
        aborts, so a hang shows where it is instead of running into the test timeout. */
    static constexpr double stallSeconds = 120.0;

private:
    void audioThreadLoop();
    void watchdogLoop();

    tracktion::Engine& engine;
    Params params;
    tracktion::HostedAudioDeviceInterface& device;

    int64_t capacity = 0;
    std::vector<std::vector<float>> output;
    std::vector<float> blockTimesMs;
    juce::AudioBuffer<float> block, previous; // previous: the last block's output, for loopback
    juce::MidiBuffer midi;

    double maxDispatchMs = 0.0;
    std::atomic<int64_t> samplesProcessed { 0 };
    std::atomic<bool> running { false };
    std::thread thread;

    // For the watchdog: when the audio thread entered processBlock and the message thread its last
    // dispatch pass (milliseconds on the steady clock), 0 while outside.
    std::atomic<int64_t> processingSinceMs { 0 }, dispatchingSinceMs { 0 };
    std::atomic<bool> watching { false };
    std::thread watchdog;
};

} // namespace spinus::test
