#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace spinus::session
{

/** Notes the audio device in the session log (core::Logs): what opened (type, device, rate, buffer, latency,
    channels), when it closes or changes, when none could be opened, and dropouts (xruns) as the driver counts
    them.

    Message thread only: it listens to the device manager's change messages and reads the dropout counter
    from a timer every few seconds, so the audio thread does nothing for it. Dropouts are noted at most every
    30 seconds, summed up, so a run with many stays readable. */
class AudioDeviceLog final : private juce::ChangeListener, private juce::Timer
{
public:
    explicit AudioDeviceLog (juce::AudioDeviceManager&);
    ~AudioDeviceLog() override;

    /** The device's line as the log writes it ("JACK: system, 48.0 kHz, 256 samples ..."). */
    static juce::String describe (juce::AudioDeviceManager&);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void noteDevice();
    void noteDropouts (bool now);

    juce::AudioDeviceManager& manager;
    juce::String lastDescription;
    int lastXruns = 0, unreported = 0;
    juce::uint32 lastReport = 0;
};

} // namespace spinus::session
