#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace spinus::test
{

/*  A sound card made of software, for tests that need JUCE's real device handling (opening, starting,
    stopping, switching outputs) without a real card: two outputs, "Fake output A" and "B", and a "Fake mic"
    with two inputs that play a 400 Hz tone. It calls the audio callback from its own thread at real-time pace
    and keeps everything played, across restarts and switches, in one recording. Add it to an
    AudioDeviceManager with addAudioDeviceType.
*/
class FakeSoundCard
{
public:
    static constexpr const char* typeName = "Fake";

    /** The device type to add to the manager (it records into this). */
    std::unique_ptr<juce::AudioIODeviceType> createType();

    /** Everything played so far, left channel. */
    std::vector<float> getRecording() const;
    int64_t getSamplesPlayed() const noexcept { return played.load(); }
    double getSampleRate() const noexcept { return 48000.0; }
    /** How many times a device was started. */
    int getStarts() const noexcept { return starts.load(); }
    /** How many times a device was opened with an input in use. */
    int getInputOpens() const noexcept { return inputOpens.load(); }

private:
    friend class FakeDevice;
    void record (const float* samples, int count);

    mutable std::mutex lock;
    std::vector<float> recording;
    std::atomic<int64_t> played { 0 };
    std::atomic<int> starts { 0 }, inputOpens { 0 };
};

} // namespace spinus::test
