#include "AudioSetup.h"

namespace spinus::engine
{

namespace
{
bool tryType (juce::AudioDeviceManager& manager, const juce::String& type, const juce::String& preferredDevice,
              double sampleRate = AudioSetup::defaultSampleRate)
{
    bool typeAvailable = false;
    for (auto* t : manager.getAvailableDeviceTypes())
        typeAvailable |= t->getTypeName() == type;

    if (! typeAvailable)
        return false;

    manager.setCurrentAudioDeviceType (type, true);
    auto setup = manager.getAudioDeviceSetup();

    if (preferredDevice.isNotEmpty())
        if (auto* t = manager.getCurrentDeviceTypeObject();
            t != nullptr && t->getDeviceNames (false).contains (preferredDevice))
            setup.outputDeviceName = preferredDevice;

    setup.sampleRate = sampleRate; // 0: the device's own
    setup.bufferSize = AudioSetup::defaultBufferSize;
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = true;

    return manager.setAudioDeviceSetup (setup, true).isEmpty() && manager.getCurrentAudioDevice() != nullptr;
}
} // namespace

juce::String AudioSetup::applyDefaults (juce::AudioDeviceManager& manager)
{
#if JUCE_WINDOWS
    // An ASIO driver when one is installed (the sound card's own, lowest latency); otherwise WASAPI, whose
    // shared mode must run at the rate Windows set for the device, so the last try leaves the rate alone.
    if (tryType (manager, "ASIO", {}) || tryType (manager, "Windows Audio (Low Latency Mode)", {}) ||
        tryType (manager, "Windows Audio", {}) || tryType (manager, "Windows Audio", {}, 0.0) ||
        tryType (manager, "DirectSound", {}))
        return getStatus (manager).describe();
#else
    // JACK only works when a server is already running (PipeWire provides one); JUCE does not start one.
    if (tryType (manager, "JACK", {}) || tryType (manager, "ALSA", "PipeWire Sound Server") ||
        tryType (manager, "ALSA", {}))
        return getStatus (manager).describe();
#endif

    return "No audio device could be opened";
}

juce::String AudioSetup::pickJackInput (const juce::StringArray& jackInputs, const juce::StringArray& microphones)
{
    for (const auto& input : jackInputs)
        if (microphones.contains (input))
            return input;
    return {};
}

AudioSetup::Status AudioSetup::getStatus (juce::AudioDeviceManager& manager)
{
    Status s;
    if (auto* device = manager.getCurrentAudioDevice())
    {
        s.type = device->getTypeName();
        s.device = device->getName();
        s.sampleRate = device->getCurrentSampleRate();
        s.bufferSize = device->getCurrentBufferSizeSamples();
        s.outputLatencySamples = s.bufferSize + device->getOutputLatencyInSamples();
        s.xruns = juce::jmax (0, device->getXRunCount());
    }
    return s;
}

juce::String AudioSetup::Status::describe() const
{
    if (type.isEmpty())
        return "No audio device open";

    return type + ": " + device + ", " + juce::String (sampleRate / 1000.0, 1) + " kHz, " + juce::String (bufferSize) +
           " samples (" + juce::String (getBufferMs(), 1) + " ms buffer, about " +
           juce::String (getOutputLatencyMs(), 1) + " ms output latency), " + juce::String (xruns) + " dropouts";
}

} // namespace spinus::engine
