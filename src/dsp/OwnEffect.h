#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

/** What Spinus's own effects (EQ, Compressor, Limiter, Gate) share: their parameters in an
    AudioProcessorValueTreeState, their state as a ValueTree of their own type, a mono or stereo main bus
    (and, for those that have one, a sidechain input), and no MIDI or programs. */
class OwnEffect : public juce::AudioPluginInstance
{
public:
    juce::AudioProcessorValueTreeState& getParameterState() noexcept { return parameters; }

    /** Sets a parameter by its real value (tests, the editors' drags). */
    void setParameterValue (const juce::String& id, float value)
    {
        if (auto* p = parameters.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (value));
    }

    const juce::String getName() const override { return name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    bool hasEditor() const override { return true; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
        if (in != out || ! (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo()))
            return false;
        const auto side = layouts.inputBuses.size() > 1 ? layouts.inputBuses[1] : juce::AudioChannelSet::disabled();
        return side.isDisabled() || side == juce::AudioChannelSet::mono() || side == juce::AudioChannelSet::stereo();
    }

    void getStateInformation (juce::MemoryBlock& destination) override
    {
        juce::ValueTree state (stateType);
        state.setProperty ("version", 1, nullptr);
        state.appendChild (parameters.copyState(), nullptr);
        juce::MemoryOutputStream out (destination, false);
        state.writeToStream (out);
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        const auto state = juce::ValueTree::readFromData (data, (size_t)sizeInBytes);
        if (state.hasType (stateType))
            if (auto params = state.getChildWithName (parameters.state.getType()); params.isValid())
                parameters.replaceState (params.createCopy());
    }

protected:
    /** `withSidechain`: a second (stereo) input bus that the Mixer's "Sidechain from" feeds. */
    OwnEffect (const char* pluginName, const char* stateTypeName, bool withSidechain,
               juce::AudioProcessorValueTreeState::ParameterLayout layout)
        : juce::AudioPluginInstance (busesFor (withSidechain)),
          parameters (*this, nullptr, juce::Identifier (juce::String (stateTypeName) + "Parameters"),
                      std::move (layout)),
          name (pluginName), stateType (stateTypeName)
    {
    }

    void describe (juce::PluginDescription& d, const char* identifier, const juce::String& what) const
    {
        d.name = name;
        d.descriptiveName = what;
        d.pluginFormatName = "Spinus";
        d.category = "Effect";
        d.manufacturerName = "Spinus";
        d.version = "1";
        d.fileOrIdentifier = identifier;
        d.uniqueId = d.deprecatedUid = (int)juce::String (identifier).hashCode();
        d.isInstrument = false;
        d.numInputChannels = getBus (true, 1) != nullptr ? 4 : 2;
        d.numOutputChannels = 2;
    }

    /** The sidechain's channels in the host's buffer (no copy, no allocation); none when not connected. */
    juce::AudioBuffer<float> getSidechain (juce::AudioBuffer<float>& buffer)
    {
        const auto* bus = getBus (true, 1);
        return bus != nullptr && bus->isEnabled() && bus->getNumberOfChannels() > 0 ? getBusBuffer (buffer, true, 1)
                                                                                    : juce::AudioBuffer<float>();
    }

    juce::AudioProcessorValueTreeState parameters;

private:
    static BusesProperties busesFor (bool withSidechain)
    {
        auto buses = BusesProperties()
                         .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
        return withSidechain ? buses.withInput ("Sidechain", juce::AudioChannelSet::stereo(), true) : buses;
    }

    const juce::String name;
    const juce::Identifier stateType;
};

} // namespace spinus::dsp
