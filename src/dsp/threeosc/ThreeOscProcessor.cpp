#include "ThreeOscProcessor.h"

#include "ThreeOscEditor.h"
#include "dsp/ParameterText.h"

namespace spinus::dsp
{

namespace
{
using Range = juce::NormalisableRange<float>;
using juce::String;

Range seconds (float max)
{
    Range r (0.0f, max);
    r.setSkewForCentre (max / 20.0f);
    return r;
}

String percent (float v, int) { return String (juce::roundToInt (v * 100.0f)) + "%"; }
String time (float v, int) { return v < 1.0f ? String (juce::roundToInt (v * 1000.0f)) + " ms" : String (v, 2) + " s"; }

/** Every parameter, in order: each oscillator's six (shape, coarse, fine, level, pan, phase), then the rest. */
std::vector<std::unique_ptr<juce::RangedAudioParameter>> makeParameters()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> list;
    auto floatParameter = [] (const String& id, const String& name, const Range& range, float value,
                              std::function<String (float, int)> text)
    {
        return std::make_unique<spinus::dsp::param::FloatSetting> (
            ParameterID { id, 1 }, name, range, value,
            AudioParameterFloatAttributes().withStringFromValueFunction (std::move (text)));
    };

    for (int osc = 0; osc < 3; ++osc)
    {
        const auto prefix = "Osc " + String (osc + 1) + " ";
        list.push_back (std::make_unique<AudioParameterChoice> (
            ParameterID { ThreeOscProcessor::parameterId ("shape", osc), 1 }, prefix + "Shape",
            StringArray { "Sine", "Triangle", "Saw", "Square", "Noise" }, osc == 0 ? 2 : (osc == 1 ? 3 : 0)));
        list.push_back (floatParameter (ThreeOscProcessor::parameterId ("coarse", osc), prefix + "Coarse",
                                        Range (-36.0f, 36.0f, 1.0f), osc == 1 ? -12.0f : 0.0f,
                                        [] (float v, int) { return String (roundToInt (v)) + " st"; }));
        list.push_back (floatParameter (ThreeOscProcessor::parameterId ("fine", osc), prefix + "Fine",
                                        Range (-100.0f, 100.0f), osc == 2 ? 7.0f : 0.0f,
                                        [] (float v, int) { return String (roundToInt (v)) + " ct"; }));
        // FL's default: oscillator 1 loud, 2 and 3 quieter.
        list.push_back (floatParameter (ThreeOscProcessor::parameterId ("level", osc), prefix + "Level",
                                        Range (0.0f, 1.0f), osc == 0 ? 1.0f : 0.0f, percent));
        list.push_back (
            floatParameter (ThreeOscProcessor::parameterId ("pan", osc), prefix + "Pan", Range (-1.0f, 1.0f), 0.0f,
                            [] (float v, int)
                            {
                                const auto p = roundToInt (v * 100.0f);
                                return p == 0 ? String ("Centre") : String (std::abs (p)) + (p < 0 ? "% L" : "% R");
                            }));
        list.push_back (floatParameter (
            ThreeOscProcessor::parameterId ("phase", osc), prefix + "Phase", Range (0.0f, 1.0f), 0.0f, [] (float v, int)
            { return String (roundToInt (v * 360.0f)) + String (juce::CharPointer_UTF8 ("\xc2\xb0")); }));
    }

    list.push_back (floatParameter ("attack", "Attack", seconds (10.0f), 0.005f, time));
    list.push_back (floatParameter ("decay", "Decay", seconds (10.0f), 0.3f, time));
    list.push_back (floatParameter ("sustain", "Sustain", Range (0.0f, 1.0f), 0.8f, percent));
    list.push_back (floatParameter ("release", "Release", seconds (10.0f), 0.2f, time));
    list.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID { "filter", 1 }, "Filter", StringArray { "Off", "Low pass", "High pass", "Band pass" }, 0));
    Range cutoff (20.0f, 20000.0f);
    cutoff.setSkewForCentre (1000.0f);
    list.push_back (
        floatParameter ("cutoff", "Cutoff", cutoff, 20000.0f, [] (float v, int)
                        { return v < 1000.0f ? String (roundToInt (v)) + " Hz" : String (v / 1000.0f, 1) + " kHz"; }));
    list.push_back (floatParameter ("resonance", "Resonance", Range (0.0f, 1.0f), 0.0f, percent));
    list.push_back (floatParameter ("envelope", "Envelope", Range (-1.0f, 1.0f), 0.0f, percent));
    list.push_back (floatParameter ("gain", "Volume", Range (-48.0f, 12.0f), 0.0f,
                                    [] (float v, int) { return String (v, 1) + " dB"; }));
    list.push_back (floatParameter ("velocity", "Velocity", Range (0.0f, 1.0f), 1.0f, percent));
    // Mono with slides (303-style): appended, so older projects keep their settings.
    list.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "voices", 1 }, "Voices",
                                                            StringArray { "Poly", "Mono (slides)" }, 0));
    list.push_back (floatParameter ("glide", "Glide", seconds (2.0f), 0.08f, time));
    return list;
}

constexpr size_t perOscillator = 6;

juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    auto list = makeParameters();
    for (size_t osc = 0; osc < 3; ++osc)
    {
        const auto name = "Osc " + juce::String ((int)osc + 1);
        auto group =
            std::make_unique<juce::AudioProcessorParameterGroup> ("osc" + juce::String ((int)osc + 1), name, " | ");
        for (size_t i = 0; i < perOscillator; ++i)
            group->addChild (std::move (list[osc * perOscillator + i]));
        layout.add (std::move (group));
    }
    for (size_t i = 3 * perOscillator; i < list.size(); ++i)
        layout.add (std::move (list[i]));
    return layout;
}
} // namespace

juce::String ThreeOscProcessor::parameterId (const juce::String& setting, int oscillator)
{
    return "osc" + juce::String (oscillator + 1) + "_" + setting;
}

const std::vector<std::unique_ptr<juce::RangedAudioParameter>>& ThreeOscProcessor::getParameterList()
{
    static const auto list = makeParameters();
    return list;
}

const juce::RangedAudioParameter* ThreeOscProcessor::findInParameterList (const juce::String& id)
{
    for (auto& parameter : getParameterList())
        if (parameter->getParameterID() == id)
            return parameter.get();
    return nullptr;
}

//==============================================================================
void ThreeOscProcessor::applyPreset (const ThreeOscPreset* preset)
{
    for (auto& [id, value] : threeOscPresets::values (preset))
        if (auto* parameter = parameters.getParameter (id))
            parameter->setValueNotifyingHost (value);
}

int ThreeOscProcessor::findCurrentProgram() const
{
    std::map<juce::String, float> now;
    for (auto* parameter : getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (parameter))
            now[withId->paramID] = parameter->getValue();
    auto matches = [&now] (const ThreeOscPreset* preset)
    {
        for (auto& [id, value] : threeOscPresets::values (preset))
            if (auto it = now.find (id); it == now.end() || std::abs (it->second - value) > 0.002f)
                return false;
        return true;
    };
    if (matches (nullptr))
        return 0;
    const auto& presets = threeOscPresets::all();
    for (size_t i = 0; i < presets.size(); ++i)
        if (matches (&presets[i]))
            return (int)i + 1;
    return -1;
}

void ThreeOscProcessor::choosePreset (int program)
{
    if (! juce::isPositiveAndBelow (program, getNumPrograms()))
        return;
    if (onChoosePreset)
    {
        lastProgram = program;
        onChoosePreset (getProgramName (program));
    }
    else
        setCurrentProgram (program);
}

int ThreeOscProcessor::getNumPrograms() { return 1 + (int)threeOscPresets::all().size(); }

int ThreeOscProcessor::getCurrentProgram()
{
    const auto current = findCurrentProgram();
    return current >= 0 ? current : lastProgram;
}

void ThreeOscProcessor::setCurrentProgram (int program)
{
    if (! juce::isPositiveAndBelow (program, getNumPrograms()))
        return;
    lastProgram = program;
    applyPreset (program == 0 ? nullptr : &threeOscPresets::all()[(size_t)program - 1]);
}

const juce::String ThreeOscProcessor::getProgramName (int program)
{
    if (program == 0)
        return threeOscPresets::defaultName;
    return juce::isPositiveAndBelow (program, getNumPrograms()) ? threeOscPresets::all()[(size_t)program - 1].name
                                                                : juce::String();
}

ThreeOscProcessor::ThreeOscProcessor()
    : juce::AudioPluginInstance (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "ThreeOscParameters", makeLayout())
{
    auto get = [this] (const juce::String& id) { return parameters.getRawParameterValue (id); };
    for (int osc = 0; osc < 3; ++osc)
        oscillatorParameters[(size_t)osc] = { get (parameterId ("shape", osc)), get (parameterId ("coarse", osc)),
                                              get (parameterId ("fine", osc)),  get (parameterId ("level", osc)),
                                              get (parameterId ("pan", osc)),   get (parameterId ("phase", osc)) };
    attack = get ("attack");
    decay = get ("decay");
    sustain = get ("sustain");
    release = get ("release");
    filter = get ("filter");
    cutoff = get ("cutoff");
    resonance = get ("resonance");
    envelopeAmount = get ("envelope");
    gain = get ("gain");
    velocity = get ("velocity");
    voices = get ("voices");
    glide = get ("glide");
}

ThreeOscSettings ThreeOscProcessor::getSettings() const noexcept
{
    ThreeOscSettings s;
    for (size_t i = 0; i < 3; ++i)
    {
        const auto& p = oscillatorParameters[i];
        s.oscillators[i] = { (ThreeOscSettings::Shape)juce::jlimit (0, 4, juce::roundToInt (p.shape->load())),
                             p.coarse->load(),
                             p.fine->load(),
                             p.level->load(),
                             p.pan->load(),
                             p.phase->load() };
    }
    s.attack = attack->load();
    s.decay = decay->load();
    s.sustain = sustain->load();
    s.release = release->load();
    s.filter = (ThreeOscSettings::Filter)juce::jlimit (0, 3, juce::roundToInt (filter->load()));
    s.cutoffHz = cutoff->load();
    s.resonance = resonance->load();
    s.envelopeAmount = envelopeAmount->load();
    s.gainDb = gain->load();
    s.velocitySensitivity = velocity->load();
    s.mono = voices->load() > 0.5f;
    s.glide = glide->load();
    return s;
}

void ThreeOscProcessor::fillInPluginDescription (juce::PluginDescription& d) const
{
    d.name = pluginName;
    d.descriptiveName = "A quick, simple synth: three oscillators, an envelope and a filter";
    d.pluginFormatName = "Spinus";
    d.category = "Instrument";
    d.manufacturerName = "Spinus";
    d.version = "1";
    d.fileOrIdentifier = identifier;
    d.uniqueId = d.deprecatedUid = (int)juce::String (identifier).hashCode();
    d.isInstrument = true;
    d.numInputChannels = 0;
    d.numOutputChannels = 2;
}

void ThreeOscProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    engine.prepare (sampleRate, maximumExpectedSamplesPerBlock);
}

void ThreeOscProcessor::releaseResources() { engine.reset(); }

bool ThreeOscProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return layouts.getMainInputChannelSet().isDisabled() &&
           (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

double ThreeOscProcessor::getTailLengthSeconds() const { return (double)release->load(); }

void ThreeOscProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    engine.render (buffer, midi, getSettings(), 0, buffer.getNumSamples());
    activeVoices.store (engine.getNumActiveVoices());
}

juce::AudioProcessorEditor* ThreeOscProcessor::createEditor() { return new ThreeOscEditor (*this); }

void ThreeOscProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    juce::ValueTree state ("Spinus3xOsc");
    state.setProperty ("version", 1, nullptr);
    state.appendChild (parameters.copyState(), nullptr);
    juce::MemoryOutputStream out (destination, false);
    state.writeToStream (out);
}

void ThreeOscProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto state = juce::ValueTree::readFromData (data, (size_t)sizeInBytes);
    if (state.hasType ("Spinus3xOsc"))
        if (auto params = state.getChildWithName (parameters.state.getType()); params.isValid())
            parameters.replaceState (params.createCopy());
}

} // namespace spinus::dsp
