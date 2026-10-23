#include "StereoUtilityProcessor.h"

#include "StereoUtilityEditor.h"
#include "dsp/ParameterText.h"

namespace spinus::dsp
{

namespace
{
using Range = juce::NormalisableRange<float>;
using juce::String;

juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    auto floatParameter = [] (const String& id, const String& name, const Range& range, float value,
                              std::function<String (float, int)> text)
    {
        return std::make_unique<spinus::dsp::param::FloatSetting> (
            ParameterID { id, 1 }, name, range, value,
            AudioParameterFloatAttributes().withStringFromValueFunction (std::move (text)));
    };

    layout.add (floatParameter ("gain", "Gain", Range (-48.0f, 24.0f), 0.0f,
                                [] (float v, int) { return String (v, 1) + " dB"; }),
                floatParameter ("pan", "Pan", Range (-1.0f, 1.0f), 0.0f,
                                [] (float v, int)
                                {
                                    const auto p = roundToInt (v * 100.0f);
                                    return p == 0 ? String ("Centre") : String (std::abs (p)) + (p < 0 ? "% L" : "% R");
                                }),
                floatParameter ("width", "Width", Range (0.0f, 2.0f), 1.0f,
                                [] (float v, int) { return String (roundToInt (v * 100.0f)) + "%"; }),
                std::make_unique<AudioParameterBool> (ParameterID { "mono", 1 }, "Mono", false),
                std::make_unique<AudioParameterBool> (ParameterID { "invert_left", 1 }, "Invert left", false),
                std::make_unique<AudioParameterBool> (ParameterID { "invert_right", 1 }, "Invert right", false));
    return layout;
}
} // namespace

StereoUtilityProcessor::StereoUtilityProcessor()
    : juce::AudioPluginInstance (BusesProperties()
                                     .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                     .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "StereoUtilityParameters", makeLayout())
{
    gain = parameters.getRawParameterValue ("gain");
    pan = parameters.getRawParameterValue ("pan");
    width = parameters.getRawParameterValue ("width");
    mono = parameters.getRawParameterValue ("mono");
    invertLeft = parameters.getRawParameterValue ("invert_left");
    invertRight = parameters.getRawParameterValue ("invert_right");
}

StereoUtilitySettings StereoUtilityProcessor::getSettings() const noexcept
{
    StereoUtilitySettings s;
    s.gainDb = gain->load();
    s.pan = pan->load();
    s.width = width->load();
    s.mono = mono->load() > 0.5f;
    s.invertLeft = invertLeft->load() > 0.5f;
    s.invertRight = invertRight->load() > 0.5f;
    return s;
}

void StereoUtilityProcessor::fillInPluginDescription (juce::PluginDescription& d) const
{
    d.name = pluginName;
    d.descriptiveName = "Gain, pan, stereo width, mono and polarity";
    d.pluginFormatName = "Spinus";
    d.category = "Effect";
    d.manufacturerName = "Spinus";
    d.version = "1";
    d.fileOrIdentifier = identifier;
    d.uniqueId = d.deprecatedUid = (int)juce::String (identifier).hashCode();
    d.isInstrument = false;
    d.numInputChannels = 2;
    d.numOutputChannels = 2;
}

void StereoUtilityProcessor::prepareToPlay (double sampleRate, int) { engine.prepare (sampleRate); }

void StereoUtilityProcessor::releaseResources() { engine.reset(); }

bool StereoUtilityProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void StereoUtilityProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    auto main = getBusBuffer (buffer, false, 0);
    engine.process (main, getSettings());
}

juce::AudioProcessorEditor* StereoUtilityProcessor::createEditor() { return new StereoUtilityEditor (*this); }

void StereoUtilityProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    juce::ValueTree state ("SpinusStereoUtility");
    state.setProperty ("version", 1, nullptr);
    state.appendChild (parameters.copyState(), nullptr);
    juce::MemoryOutputStream out (destination, false);
    state.writeToStream (out);
}

void StereoUtilityProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto state = juce::ValueTree::readFromData (data, (size_t)sizeInBytes);
    if (state.hasType ("SpinusStereoUtility"))
        if (auto params = state.getChildWithName (parameters.state.getType()); params.isValid())
            parameters.replaceState (params.createCopy());
}

} // namespace spinus::dsp
