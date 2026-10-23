#include "DynamicsProcessors.h"

#include "DynamicsEditors.h"
#include "dsp/OwnEffectParameters.h"

namespace spinus::dsp
{

using namespace param;
using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;

namespace
{
juce::StringArray sidechainChoices() { return { "Own input", "Sidechain" }; }

juce::NormalisableRange<float> skewed (float min, float max, float centre)
{
    juce::NormalisableRange<float> r (min, max);
    r.setSkewForCentre (centre);
    return r;
}

std::unique_ptr<juce::AudioParameterBool> boolParameter (const juce::String& id, const juce::String& name, bool value)
{
    return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, value);
}
} // namespace

//==============================================================================
CompressorProcessor::CompressorProcessor()
    : OwnEffect (pluginName, "SpinusCompressor", true,
                 []
                 {
                     Layout layout;
                     layout.add (floatParameter ("threshold", "Threshold", { -60.0f, 0.0f }, -18.0f, decibels),
                                 floatParameter ("ratio", "Ratio", skewed (1.0f, 20.0f, 4.0f), 4.0f, param::ratio),
                                 floatParameter ("knee", "Knee", { 0.0f, 24.0f }, 6.0f, decibels),
                                 floatParameter ("attack", "Attack", logRange (0.1f, 200.0f), 10.0f, milliseconds),
                                 floatParameter ("release", "Release", logRange (5.0f, 2000.0f), 120.0f, milliseconds),
                                 floatParameter ("makeup", "Makeup", { 0.0f, 24.0f }, 0.0f, decibels),
                                 boolParameter ("auto_makeup", "Auto makeup", false),
                                 floatParameter ("mix", "Mix", { 0.0f, 1.0f }, 1.0f, percent),
                                 choiceParameter ("detector", "Detector", { "Peak", "RMS" }, 0),
                                 choiceParameter ("sidechain", "Sidechain", sidechainChoices(), 0));
                     return layout;
                 }())
{
    auto get = [this] (const char* id) { return parameters.getRawParameterValue (id); };
    threshold = get ("threshold");
    ratio = get ("ratio");
    knee = get ("knee");
    attack = get ("attack");
    release = get ("release");
    makeup = get ("makeup");
    autoMakeup = get ("auto_makeup");
    mix = get ("mix");
    detector = get ("detector");
    sidechain = get ("sidechain");
}

CompressorSettings CompressorProcessor::getSettings() const noexcept
{
    CompressorSettings s;
    s.thresholdDb = threshold->load();
    s.ratio = ratio->load();
    s.kneeDb = knee->load();
    s.attackMs = attack->load();
    s.releaseMs = release->load();
    s.makeupDb = makeup->load();
    s.autoMakeup = autoMakeup->load() > 0.5f;
    s.mix = mix->load();
    s.rms = detector->load() > 0.5f;
    s.sidechain = sidechain->load() > 0.5f;
    return s;
}

void CompressorProcessor::fillInPluginDescription (juce::PluginDescription& d) const
{
    describe (d, identifier, "A compressor with soft knee, parallel mix and a sidechain input");
}

void CompressorProcessor::prepareToPlay (double sampleRate, int) { engine.prepare (sampleRate); }
void CompressorProcessor::releaseResources() { engine.reset(); }

void CompressorProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    auto main = getBusBuffer (buffer, false, 0);
    const auto side = getSidechain (buffer);
    engine.process (main, side, getSettings());
}

juce::AudioProcessorEditor* CompressorProcessor::createEditor() { return new CompressorEditor (*this); }

//==============================================================================
LimiterProcessor::LimiterProcessor()
    : OwnEffect (pluginName, "SpinusLimiter", false,
                 []
                 {
                     Layout layout;
                     layout.add (
                         floatParameter ("input", "Input gain", { -12.0f, 24.0f }, 0.0f, decibels),
                         floatParameter ("ceiling", "Ceiling", { -12.0f, 0.0f }, -1.0f, decibels),
                         floatParameter ("release", "Release", logRange (1.0f, 1000.0f), 60.0f, milliseconds),
                         // Not automatable: a new look-ahead changes the latency.
                         std::make_unique<spinus::dsp::param::FloatSetting> (
                             juce::ParameterID { "lookahead", 1 }, "Lookahead",
                             juce::NormalisableRange<float> (0.5f, LimiterEngine::maxLookaheadMs), 3.0f,
                             juce::AudioParameterFloatAttributes().withAutomatable (false).withStringFromValueFunction (
                                 milliseconds)),
                         boolParameter ("true_peak", "True peak", true));
                     return layout;
                 }())
{
    auto get = [this] (const char* id) { return parameters.getRawParameterValue (id); };
    input = get ("input");
    ceiling = get ("ceiling");
    release = get ("release");
    lookahead = get ("lookahead");
    truePeak = get ("true_peak");
    startTimerHz (10);
}

LimiterProcessor::~LimiterProcessor() { stopTimer(); }

LimiterSettings LimiterProcessor::getSettings() const noexcept
{
    LimiterSettings s;
    s.inputDb = input->load();
    s.ceilingDb = ceiling->load();
    s.releaseMs = release->load();
    s.truePeak = truePeak->load() > 0.5f;
    return s;
}

void LimiterProcessor::updateLatency()
{
    if (preparedRate <= 0.0)
        return;
    const auto latency = LimiterEngine::latencyFor (lookahead->load(), preparedRate);
    if (latency != getLatencySamples())
    {
        engine.setLookahead (lookahead->load());
        setLatencySamples (latency);
    }
}

void LimiterProcessor::fillInPluginDescription (juce::PluginDescription& d) const
{
    describe (d, identifier, "A look-ahead brickwall limiter with true-peak detection");
}

void LimiterProcessor::prepareToPlay (double sampleRate, int)
{
    preparedRate = sampleRate;
    engine.prepare (sampleRate, lookahead->load());
    setLatencySamples (engine.getLatencySamples());
}

void LimiterProcessor::releaseResources() { engine.reset(); }

void LimiterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    for (auto c = getTotalNumInputChannels(); c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());
    auto main = getBusBuffer (buffer, false, 0);
    engine.process (main, getSettings());
}

juce::AudioProcessorEditor* LimiterProcessor::createEditor() { return new LimiterEditor (*this); }

//==============================================================================
GateProcessor::GateProcessor()
    : OwnEffect (pluginName, "SpinusGate", true,
                 []
                 {
                     Layout layout;
                     layout.add (
                         floatParameter ("threshold", "Threshold", { -80.0f, 0.0f }, -40.0f, decibels),
                         floatParameter ("hysteresis", "Hysteresis", { 0.0f, 12.0f }, 3.0f, decibels),
                         floatParameter (
                             "range", "Range", { -80.0f, 0.0f }, -80.0f, [] (float v, int)
                             { return v <= GateEngine::fullyClosedDb ? juce::String ("Silent") : decibels (v, 0); }),
                         floatParameter ("attack", "Attack", logRange (0.05f, 50.0f), 0.5f, milliseconds),
                         floatParameter ("hold", "Hold", skewed (0.0f, 500.0f, 50.0f), 20.0f, milliseconds),
                         floatParameter ("release", "Release", logRange (5.0f, 2000.0f), 100.0f, milliseconds),
                         choiceParameter ("sidechain", "Sidechain", sidechainChoices(), 0));
                     return layout;
                 }())
{
    auto get = [this] (const char* id) { return parameters.getRawParameterValue (id); };
    threshold = get ("threshold");
    hysteresis = get ("hysteresis");
    range = get ("range");
    attack = get ("attack");
    hold = get ("hold");
    release = get ("release");
    sidechain = get ("sidechain");
}

GateSettings GateProcessor::getSettings() const noexcept
{
    GateSettings s;
    s.thresholdDb = threshold->load();
    s.hysteresisDb = hysteresis->load();
    s.rangeDb = range->load();
    s.attackMs = attack->load();
    s.holdMs = hold->load();
    s.releaseMs = release->load();
    s.sidechain = sidechain->load() > 0.5f;
    return s;
}

void GateProcessor::fillInPluginDescription (juce::PluginDescription& d) const
{
    describe (d, identifier, "A noise gate with hysteresis, hold and a sidechain input");
}

void GateProcessor::prepareToPlay (double sampleRate, int) { engine.prepare (sampleRate); }
void GateProcessor::releaseResources() { engine.reset(); }

void GateProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    auto main = getBusBuffer (buffer, false, 0);
    const auto side = getSidechain (buffer);
    engine.process (main, side, getSettings());
}

juce::AudioProcessorEditor* GateProcessor::createEditor() { return new GateEditor (*this); }

} // namespace spinus::dsp
