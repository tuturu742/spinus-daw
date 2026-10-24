#include "AudioDeviceLog.h"

#include "core/Log.h"
#include "engine/AudioSetup.h"

namespace spinus::session
{

namespace
{
constexpr int checkEveryMs = 5000;
constexpr juce::uint32 reportAtMostEveryMs = 30000;
} // namespace

AudioDeviceLog::AudioDeviceLog (juce::AudioDeviceManager& m) : manager (m)
{
    manager.addChangeListener (this);
    noteDevice();
    startTimer (checkEveryMs);
}

AudioDeviceLog::~AudioDeviceLog()
{
    stopTimer();
    manager.removeChangeListener (this);
    noteDropouts (true);
}

juce::String AudioDeviceLog::describe (juce::AudioDeviceManager& m)
{
    auto* device = m.getCurrentAudioDevice();
    if (device == nullptr)
        return "no audio device is open";
    const auto status = engine::AudioSetup::getStatus (m);
    return status.type + ": " + status.device + ", " + juce::String (status.sampleRate / 1000.0, 1) + " kHz, " +
           juce::String (status.bufferSize) + " samples (" + juce::String (status.getBufferMs(), 1) + " ms; about " +
           juce::String (status.getOutputLatencyMs(), 1) + " ms output latency), " +
           juce::String (device->getActiveOutputChannels().countNumberOfSetBits()) + " outputs and " +
           juce::String (device->getActiveInputChannels().countNumberOfSetBits()) + " inputs in use";
}

void AudioDeviceLog::changeListenerCallback (juce::ChangeBroadcaster*) { noteDevice(); }

void AudioDeviceLog::noteDevice()
{
    const auto description = describe (manager);
    if (description == lastDescription)
        return;
    noteDropouts (true); // the old device's
    const bool open = manager.getCurrentAudioDevice() != nullptr;
    if (open)
        core::Logs::info ("audio",
                          (lastDescription.isEmpty() || lastDescription.startsWith ("no ") ? "Opened " : "Now ") +
                              description);
    else if (lastDescription.isEmpty())
        core::Logs::warning ("audio", "No audio device could be opened: Spinus is silent until one is chosen in "
                                      "Options > Audio settings");
    else
        core::Logs::warning ("audio", "The audio device closed (" +
                                          lastDescription.upToFirstOccurrenceOf (",", false, false) +
                                          "); no audio device is open");
    lastDescription = description;
    lastXruns = open ? juce::jmax (0, manager.getCurrentAudioDevice()->getXRunCount()) : 0;
}

void AudioDeviceLog::timerCallback()
{
    // A device that went away without a change message (a USB card unplugged) is noticed here too.
    if (describe (manager).upToFirstOccurrenceOf (",", false, false) !=
        lastDescription.upToFirstOccurrenceOf (",", false, false))
        noteDevice();
    noteDropouts (false);
}

void AudioDeviceLog::noteDropouts (bool now)
{
    if (auto* device = manager.getCurrentAudioDevice())
    {
        const auto xruns = juce::jmax (0, device->getXRunCount());
        if (xruns > lastXruns)
            unreported += xruns - lastXruns;
        lastXruns = xruns; // a restarted device counts from 0 again
    }
    const auto time = juce::Time::getMillisecondCounter();
    if (unreported == 0 || (! now && time - lastReport < reportAtMostEveryMs))
        return;
    core::Logs::warning ("audio", juce::String (unreported) + (unreported == 1 ? " dropout" : " dropouts") +
                                      " (the sound card was not given audio in time; CPU load " +
                                      juce::String (juce::roundToInt (manager.getCpuUsage() * 100.0)) + "%)");
    unreported = 0;
    lastReport = time;
}

} // namespace spinus::session
