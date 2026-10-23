#include "EqProcessor.h"

#include "EqEditor.h"
#include "dsp/OwnEffectParameters.h"

namespace spinus::dsp
{

namespace
{
using juce::String;

struct BandDefault
{
    EqBand::Type type;
    float frequency, q;
    bool on;
};

// Flat until changed: the cuts start off, the other bands at 0 dB.
constexpr std::array<BandDefault, EqSettings::numBands> defaults { {
    { EqBand::Type::lowCut, 30.0f, 0.71f, false },
    { EqBand::Type::lowShelf, 100.0f, 0.71f, true },
    { EqBand::Type::bell, 250.0f, 1.0f, true },
    { EqBand::Type::bell, 600.0f, 1.0f, true },
    { EqBand::Type::bell, 1500.0f, 1.0f, true },
    { EqBand::Type::bell, 4000.0f, 1.0f, true },
    { EqBand::Type::highShelf, 8000.0f, 0.71f, true },
    { EqBand::Type::highCut, 18000.0f, 0.71f, false },
} };

juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
{
    using namespace param;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (int b = 0; b < EqSettings::numBands; ++b)
    {
        const auto& d = defaults[(size_t)b];
        const auto name = "Band " + String (b + 1);
        layout.add (
            std::make_unique<juce::AudioParameterBool> (juce::ParameterID { EqProcessor::bandParameter (b, "on"), 1 },
                                                        name + " on", d.on),
            choiceParameter (EqProcessor::bandParameter (b, "type"), name + " type", EqProcessor::getTypeNames(),
                             (int)d.type),
            floatParameter (EqProcessor::bandParameter (b, "freq"), name + " frequency", logRange (20.0f, 20000.0f),
                            d.frequency, hertz),
            floatParameter (EqProcessor::bandParameter (b, "gain"), name + " gain", { -24.0f, 24.0f }, 0.0f, decibels),
            floatParameter (EqProcessor::bandParameter (b, "q"), name + " Q", logRange (0.1f, 18.0f), d.q, plain),
            choiceParameter (EqProcessor::bandParameter (b, "slope"), name + " slope",
                             { "12 dB/oct", "24 dB/oct", "48 dB/oct" }, 0));
    }
    layout.add (floatParameter ("output", "Output", { -24.0f, 24.0f }, 0.0f, decibels));
    return layout;
}
} // namespace

String EqProcessor::bandParameter (int band, const char* part) { return "band" + String (band + 1) + "_" + part; }

juce::StringArray EqProcessor::getTypeNames()
{
    return { "Bell", "Low shelf", "High shelf", "Low cut", "High cut", "Notch" };
}

EqProcessor::EqProcessor() : OwnEffect (pluginName, "SpinusEq", false, makeLayout())
{
    for (int b = 0; b < EqSettings::numBands; ++b)
    {
        auto get = [&] (const char* part) { return parameters.getRawParameterValue (bandParameter (b, part)); };
        bandParameters[(size_t)b] = { get ("on"), get ("type"), get ("freq"), get ("gain"), get ("q"), get ("slope") };
    }
    output = parameters.getRawParameterValue ("output");
}

EqSettings EqProcessor::getSettings() const noexcept
{
    EqSettings s;
    for (size_t b = 0; b < bandParameters.size(); ++b)
    {
        const auto& p = bandParameters[b];
        auto& band = s.bands[b];
        band.on = p.on->load() > 0.5f;
        band.type = (EqBand::Type)juce::jlimit (0, 5, juce::roundToInt (p.type->load()));
        band.frequencyHz = p.frequency->load();
        band.gainDb = p.gain->load();
        band.q = p.q->load();
        band.slope = juce::jlimit (0, 2, juce::roundToInt (p.slope->load()));
    }
    s.outputDb = output->load();
    return s;
}

void EqProcessor::fillInPluginDescription (juce::PluginDescription& d) const
{
    describe (d, identifier, "An eight-band parametric EQ with a live spectrum");
}

void EqProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    engine.prepare (sampleRate, maximumExpectedSamplesPerBlock);
}

void EqProcessor::releaseResources() { engine.reset(); }

void EqProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    for (auto c = getTotalNumInputChannels(); c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());
    auto main = getBusBuffer (buffer, false, 0);
    engine.process (main, getSettings());
}

juce::AudioProcessorEditor* EqProcessor::createEditor() { return new EqEditor (*this); }

} // namespace spinus::dsp
