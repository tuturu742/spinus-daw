#include "DuckerProcessor.h"

#include "DuckerEditor.h"
#include "dsp/ParameterText.h"

namespace spinus::dsp
{

namespace
{
using Range = juce::NormalisableRange<float>;
using juce::String;

String percent (float v, int) { return String (juce::roundToInt (v * 100.0f)) + "%"; }

juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    Range release (0.02f, 1.0f);
    release.setSkewForCentre (0.2f);
    layout.add (std::make_unique<AudioParameterChoice> (
                    ParameterID { "trigger", 1 }, "Trigger",
                    StringArray { "Sidechain", "Every beat", "Every half beat", "Every 2 beats" }, 0),
                std::make_unique<spinus::dsp::param::FloatSetting> (
                    ParameterID { "amount", 1 }, "Amount", Range (0.0f, 1.0f), 0.7f,
                    AudioParameterFloatAttributes().withStringFromValueFunction (percent)),
                std::make_unique<spinus::dsp::param::FloatSetting> (
                    ParameterID { "release", 1 }, "Release", release, 0.25f,
                    AudioParameterFloatAttributes().withStringFromValueFunction (
                        [] (float v, int) { return String (roundToInt (v * 1000.0f)) + " ms"; })),
                std::make_unique<spinus::dsp::param::FloatSetting> (
                    ParameterID { "curve", 1 }, "Curve", Range (0.0f, 1.0f), 0.5f,
                    AudioParameterFloatAttributes().withStringFromValueFunction (percent)));
    return layout;
}
} // namespace

DuckerProcessor::DuckerProcessor()
    : juce::AudioPluginInstance (BusesProperties()
                                     .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                     .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                                     .withInput ("Sidechain", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "DuckerParameters", makeLayout())
{
    trigger = parameters.getRawParameterValue ("trigger");
    amount = parameters.getRawParameterValue ("amount");
    release = parameters.getRawParameterValue ("release");
    curve = parameters.getRawParameterValue ("curve");
}

DuckerSettings DuckerProcessor::getSettings() const noexcept
{
    DuckerSettings s;
    s.trigger = (DuckerSettings::Trigger)juce::jlimit (0, 3, juce::roundToInt (trigger->load()));
    s.amount = amount->load();
    s.release = release->load();
    s.curve = curve->load();
    return s;
}

void DuckerProcessor::fillInPluginDescription (juce::PluginDescription& d) const
{
    d.name = pluginName;
    d.descriptiveName = "Dips the volume on each sidechain hit or beat";
    d.pluginFormatName = "Spinus";
    d.category = "Effect";
    d.manufacturerName = "Spinus";
    d.version = "1";
    d.fileOrIdentifier = identifier;
    d.uniqueId = d.deprecatedUid = (int)juce::String (identifier).hashCode();
    d.isInstrument = false;
    d.numInputChannels = 4;
    d.numOutputChannels = 2;
}

void DuckerProcessor::prepareToPlay (double sampleRate, int) { engine.prepare (sampleRate); }

void DuckerProcessor::releaseResources() { engine.reset(); }

bool DuckerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (in != out || ! (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo()))
        return false;
    const auto sidechain = layouts.inputBuses.size() > 1 ? layouts.inputBuses[1] : juce::AudioChannelSet::disabled();
    return sidechain.isDisabled() || sidechain == juce::AudioChannelSet::mono() ||
           sidechain == juce::AudioChannelSet::stereo();
}

void DuckerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    auto main = getBusBuffer (buffer, false, 0);
    // Both refer to the host's channels (no copy, no allocation).
    const auto* sidechainBus = getBus (true, 1);
    const auto sidechain = sidechainBus != nullptr && sidechainBus->isEnabled() ? getBusBuffer (buffer, true, 1)
                                                                                : juce::AudioBuffer<float>();

    double bpm = 120.0, songBeats = -1.0;
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
        {
            bpm = position->getBpm().orFallback (120.0);
            if (position->getIsPlaying())
                songBeats = position->getPpqPosition().orFallback (-1.0);
        }

    engine.process (main, sidechain, getSettings(), bpm, songBeats);
}

juce::AudioProcessorEditor* DuckerProcessor::createEditor() { return new DuckerEditor (*this); }

void DuckerProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    juce::ValueTree state ("SpinusDucker");
    state.setProperty ("version", 1, nullptr);
    state.appendChild (parameters.copyState(), nullptr);
    juce::MemoryOutputStream out (destination, false);
    state.writeToStream (out);
}

void DuckerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto state = juce::ValueTree::readFromData (data, (size_t)sizeInBytes);
    if (state.hasType ("SpinusDucker"))
        if (auto params = state.getChildWithName (parameters.state.getType()); params.isValid())
            parameters.replaceState (params.createCopy());
}

} // namespace spinus::dsp
