#include "TuneProcessor.h"

#include "TuneEditor.h"
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
    Range retune (0.0f, 400.0f);
    retune.setSkewForCentre (60.0f);
    layout.add (
        std::make_unique<AudioParameterChoice> (ParameterID { "key", 1 }, "Key", TuneProcessor::getKeyNames(), 0),
        std::make_unique<AudioParameterChoice> (ParameterID { "scale", 1 }, "Scale", TuneProcessor::getScaleNames(), 0),
        std::make_unique<spinus::dsp::param::FloatSetting> (
            ParameterID { "retune", 1 }, "Retune speed", retune, 20.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction (
                [] (float v, int) { return v < 0.5f ? String ("Robot") : String (roundToInt (v)) + " ms"; })),
        std::make_unique<spinus::dsp::param::FloatSetting> (
            ParameterID { "humanize", 1 }, "Humanize", Range (0.0f, 1.0f), 0.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction (percent)),
        std::make_unique<AudioParameterChoice> (ParameterID { "formants", 1 }, "Formants",
                                                StringArray { "Keep", "Follow pitch" }, 0),
        std::make_unique<AudioParameterChoice> (ParameterID { "range", 1 }, "Range",
                                                StringArray { "Normal (10 ms)", "Low voice (20 ms)" }, 0,
                                                AudioParameterChoiceAttributes().withAutomatable (false)),
        std::make_unique<spinus::dsp::param::FloatSetting> (
            ParameterID { "mix", 1 }, "Mix", Range (0.0f, 1.0f), 1.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction (percent)));
    return layout;
}
} // namespace

const juce::StringArray& TuneProcessor::getKeyNames()
{
    static const juce::StringArray names { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names;
}

const juce::StringArray& TuneProcessor::getScaleNames()
{
    // In the order of TuneSettings::Scale.
    static const juce::StringArray names { "Chromatic",        "Major", "Minor",  "Harmonic minor", "Major pentatonic",
                                           "Minor pentatonic", "Blues", "Dorian", "Mixolydian" };
    return names;
}

TuneProcessor::TuneProcessor()
    : juce::AudioPluginInstance (BusesProperties()
                                     .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                     .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "TuneParameters", makeLayout())
{
    key = parameters.getRawParameterValue ("key");
    scale = parameters.getRawParameterValue ("scale");
    retune = parameters.getRawParameterValue ("retune");
    humanize = parameters.getRawParameterValue ("humanize");
    formants = parameters.getRawParameterValue ("formants");
    range = parameters.getRawParameterValue ("range");
    mix = parameters.getRawParameterValue ("mix");
    parameters.addParameterListener ("range", this);
    updateLatency();
}

TuneProcessor::~TuneProcessor() { parameters.removeParameterListener ("range", this); }

TuneSettings TuneProcessor::getSettings() const noexcept
{
    TuneSettings s;
    s.key = juce::jlimit (0, 11, juce::roundToInt (key->load()));
    s.scale = (TuneSettings::Scale)juce::jlimit (0, getScaleNames().size() - 1, juce::roundToInt (scale->load()));
    s.retuneMs = retune->load();
    s.humanize = humanize->load();
    s.keepFormants = juce::roundToInt (formants->load()) == 0;
    s.range = juce::roundToInt (range->load()) == 1 ? TuneSettings::Range::low : TuneSettings::Range::normal;
    s.mix = mix->load();
    return s;
}

void TuneProcessor::fillInPluginDescription (juce::PluginDescription& d) const
{
    d.name = pluginName;
    d.descriptiveName = "Real-time pitch correction for vocals";
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

void TuneProcessor::updateLatency()
{
    const auto rate = preparedRate > 0.0 ? preparedRate : 48000.0;
    setLatencySamples (TuneEngine::getLatency (rate, getSettings().range));
}

void TuneProcessor::parameterChanged (const juce::String&, float)
{
    // The range changes the delay: the host is told, so playback stays in time.
    updateLatency();
}

void TuneProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    preparedRate = sampleRate;
    engine.prepare (sampleRate, maximumExpectedSamplesPerBlock, juce::jmax (1, getTotalNumOutputChannels()));
    updateLatency();
}

void TuneProcessor::releaseResources() { engine.reset(); }

bool TuneProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void TuneProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    auto main = getBusBuffer (buffer, false, 0);
    engine.process (main, midi, getSettings());
    midi.clear(); // the notes chose the pitch; they go no further
}

juce::AudioProcessorEditor* TuneProcessor::createEditor() { return new TuneEditor (*this); }

void TuneProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    juce::ValueTree state ("SpinusTune");
    state.setProperty ("version", 1, nullptr);
    state.appendChild (parameters.copyState(), nullptr);
    juce::MemoryOutputStream out (destination, false);
    state.writeToStream (out);
}

void TuneProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto state = juce::ValueTree::readFromData (data, (size_t)sizeInBytes);
    if (state.hasType ("SpinusTune"))
        if (auto params = state.getChildWithName (parameters.state.getType()); params.isValid())
            parameters.replaceState (params.createCopy());
    updateLatency();
}

} // namespace spinus::dsp
