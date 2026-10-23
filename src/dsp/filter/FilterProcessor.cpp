#include "FilterProcessor.h"

#include "FilterEditor.h"
#include "dsp/ParameterText.h"

namespace spinus::dsp
{

namespace
{
using Range = juce::NormalisableRange<float>;
using juce::String;

String percent (float v, int) { return String (juce::roundToInt (v * 100.0f)) + "%"; }
String time (float v, int) { return v < 1.0f ? String (juce::roundToInt (v * 1000.0f)) + " ms" : String (v, 2) + " s"; }
String hertz (float v, int)
{
    return v < 1000.0f ? String (v, v < 10.0f ? 2 : 0) + " Hz" : String (v / 1000.0f, 1) + " kHz";
}

Range skewed (float min, float max, float centre)
{
    Range r (min, max);
    r.setSkewForCentre (centre);
    return r;
}

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
    auto choice = [] (const String& id, const String& name, const StringArray& items, int value)
    { return std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, items, value); };

    layout.add (choice ("type", "Type", { "Low pass", "High pass", "Band pass", "Notch" }, 0),
                choice ("slope", "Slope", { "12 dB", "24 dB" }, 0),
                floatParameter ("cutoff", "Cutoff", skewed (20.0f, 20000.0f, 1000.0f), 1000.0f, hertz),
                floatParameter ("resonance", "Resonance", Range (0.0f, 1.0f), 0.2f, percent));
    layout.add (
        floatParameter ("lfo_amount", "LFO amount", Range (-1.0f, 1.0f), 0.0f, percent),
        choice ("lfo_shape", "LFO shape", { "Sine", "Triangle", "Ramp up", "Ramp down", "Square", "Random" }, 0),
        choice ("lfo_sync", "LFO sync", { "Free", "Tempo" }, 1),
        floatParameter ("lfo_rate", "LFO rate", skewed (0.05f, 20.0f, 2.0f), 1.0f, hertz),
        choice ("lfo_length", "LFO length", FilterProcessor::getLfoLengthNames(), 2));
    layout.add (floatParameter ("env_amount", "Envelope amount", Range (-1.0f, 1.0f), 0.0f, percent),
                floatParameter ("env_attack", "Envelope attack", skewed (0.001f, 1.0f, 0.05f), 0.01f, time),
                floatParameter ("env_release", "Envelope release", skewed (0.01f, 2.0f, 0.3f), 0.2f, time));
    layout.add (floatParameter ("mix", "Mix", Range (0.0f, 1.0f), 1.0f, percent),
                floatParameter ("gain", "Volume", Range (-24.0f, 12.0f), 0.0f,
                                [] (float v, int) { return String (v, 1) + " dB"; }));
    return layout;
}
} // namespace

juce::StringArray FilterProcessor::getLfoLengthNames()
{
    return { "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars" };
}

FilterProcessor::FilterProcessor()
    : juce::AudioPluginInstance (BusesProperties()
                                     .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                     .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "FilterParameters", makeLayout())
{
    auto get = [this] (const juce::String& id) { return parameters.getRawParameterValue (id); };
    type = get ("type");
    slope = get ("slope");
    cutoff = get ("cutoff");
    resonance = get ("resonance");
    lfoAmount = get ("lfo_amount");
    lfoShape = get ("lfo_shape");
    lfoSync = get ("lfo_sync");
    lfoRate = get ("lfo_rate");
    lfoLength = get ("lfo_length");
    envelopeAmount = get ("env_amount");
    envelopeAttack = get ("env_attack");
    envelopeRelease = get ("env_release");
    mix = get ("mix");
    gain = get ("gain");
}

FilterSettings FilterProcessor::getSettings() const noexcept
{
    FilterSettings s;
    s.type = (FilterSettings::Type)juce::jlimit (0, 3, juce::roundToInt (type->load()));
    s.steep = slope->load() > 0.5f;
    s.cutoffHz = cutoff->load();
    s.resonance = resonance->load();
    s.lfoAmount = lfoAmount->load();
    s.lfoShape = (FilterSettings::LfoShape)juce::jlimit (0, 5, juce::roundToInt (lfoShape->load()));
    s.lfoSync = lfoSync->load() > 0.5f;
    s.lfoRateHz = lfoRate->load();
    s.lfoBeats = lfoLengths[(size_t)juce::jlimit (0, (int)lfoLengths.size() - 1, juce::roundToInt (lfoLength->load()))];
    s.envelopeAmount = envelopeAmount->load();
    s.envelopeAttack = envelopeAttack->load();
    s.envelopeRelease = envelopeRelease->load();
    s.mix = mix->load();
    s.gainDb = gain->load();
    return s;
}

void FilterProcessor::fillInPluginDescription (juce::PluginDescription& d) const
{
    d.name = pluginName;
    d.descriptiveName = "A filter with an LFO and an envelope follower";
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

void FilterProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    engine.prepare (sampleRate, maximumExpectedSamplesPerBlock);
}

void FilterProcessor::releaseResources() { engine.reset(); }

bool FilterProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void FilterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    for (auto c = getTotalNumInputChannels(); c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    double bpm = 120.0, songBeats = -1.0;
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
        {
            bpm = position->getBpm().orFallback (120.0);
            if (position->getIsPlaying())
                songBeats = position->getPpqPosition().orFallback (-1.0);
        }

    engine.process (buffer, getSettings(), bpm, songBeats);
}

juce::AudioProcessorEditor* FilterProcessor::createEditor() { return new FilterEditor (*this); }

void FilterProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    juce::ValueTree state ("SpinusFilter");
    state.setProperty ("version", 1, nullptr);
    state.appendChild (parameters.copyState(), nullptr);
    juce::MemoryOutputStream out (destination, false);
    state.writeToStream (out);
}

void FilterProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto state = juce::ValueTree::readFromData (data, (size_t)sizeInBytes);
    if (state.hasType ("SpinusFilter"))
        if (auto params = state.getChildWithName (parameters.state.getType()); params.isValid())
            parameters.replaceState (params.createCopy());
}

} // namespace spinus::dsp
