#include "RecordingTime.h"

namespace spinus::recording
{

namespace te = tracktion;

te::InputDeviceInstance* findAudioInput (te::Edit& edit)
{
    edit.getTransport().ensureContextAllocated();
    if (auto* context = edit.getCurrentPlaybackContext())
        for (auto* instance : context->getAllInputs())
            if (instance->getInputDevice().getDeviceType() == te::InputDevice::waveDevice &&
                instance->getInputDevice().isEnabled())
                return instance;
    return nullptr;
}

double editSecondsAt (te::Edit& edit, double streamTime, double sampleRate)
{
    auto* context = edit.getCurrentPlaybackContext();
    if (context == nullptr)
        return 0.0;

    auto& devices = edit.engine.getDeviceManager();
    double latency = devices.getOutputLatencySeconds();
    if (auto* device = devices.deviceManager.getCurrentAudioDevice(); device != nullptr && sampleRate > 0.0)
        latency += device->getInputLatencyInSamples() / sampleRate;
    if (sampleRate > 0.0)
        latency += context->getLatencySamples() / sampleRate;
    return context->globalStreamTimeToEditTimeUnlooped (streamTime).inSeconds() - latency;
}

namespace
{
constexpr const char* defaultInputSetting = "spinusRecordInput";
}

std::vector<int> activeInputChannels (te::Engine& engine)
{
    std::vector<int> inputs;
    if (auto* device = engine.getDeviceManager().deviceManager.getCurrentAudioDevice())
    {
        const auto active = device->getActiveInputChannels();
        for (int i = active.findNextSetBit (0); i >= 0; i = active.findNextSetBit (i + 1))
            inputs.push_back (i);
    }
    return inputs;
}

juce::StringArray inputChannelNames (te::Engine& engine)
{
    auto* device = engine.getDeviceManager().deviceManager.getCurrentAudioDevice();
    return device != nullptr ? device->getInputChannelNames() : juce::StringArray();
}

int getDefaultInput (te::Engine& engine)
{
    const auto active = activeInputChannels (engine);
    const auto saved = engine.getPropertyStorage().getPropertiesFile().getIntValue (defaultInputSetting, -1);
    if (saved >= 0 && std::find (active.begin(), active.end(), saved) != active.end())
        return saved;
    return active.empty() ? 0 : active.front();
}

void setDefaultInput (te::Engine& engine, int input)
{
    auto& settings = engine.getPropertyStorage().getPropertiesFile();
    settings.setValue (defaultInputSetting, input);
    settings.saveIfNeeded();
}

juce::Result useInputs (te::Engine& engine, const std::set<int>& inputs)
{
    auto& manager = engine.getDeviceManager().deviceManager;
    auto* device = manager.getCurrentAudioDevice();
    if (device == nullptr)
        return juce::Result::fail ("Spinus has no sound card open: choose one in Options › Audio.");
    const auto count = device->getInputChannelNames().size();
    const auto active = device->getActiveInputChannels();
    bool missing = false;
    for (auto input : inputs)
    {
        if (! juce::isPositiveAndBelow (input, count))
            return juce::Result::fail ("The sound card has no input " + juce::String (input + 1) + " (it has " +
                                       juce::String (count) + ").");
        missing = missing || ! active[input];
    }
    if (missing)
    {
        auto setup = manager.getAudioDeviceSetup();
        setup.useDefaultInputChannels = false;
        for (auto input : inputs)
            setup.inputChannels.setBit (input);
        if (const auto error = manager.setAudioDeviceSetup (setup, true); error.isNotEmpty())
            return juce::Result::fail ("The sound card could not switch the inputs on: " + error);
    }
    refreshInputLayout (engine);
    return juce::Result::ok();
}

void refreshInputLayout (te::Engine& engine)
{
    auto& devices = engine.getDeviceManager();
    auto* device = devices.deviceManager.getCurrentAudioDevice();
    if (device == nullptr || device->getInputChannelNames().isEmpty())
        return;
    const auto inUse = juce::jmax (1, device->getActiveInputChannels().countNumberOfSetBits());
    auto* input = devices.getNumWaveInDevices() > 0 ? devices.getWaveInDevice (0) : nullptr;
    if (input != nullptr && input->getChannelSet().size() == inUse)
        return;
    // The same sound card with other inputs in use: Tracktion keeps its layout unless asked again.
    devices.applyWaveDeviceLayout (devices.getCurrentWaveDeviceLayout());
    devices.dispatchPendingUpdates();
}

} // namespace spinus::recording
