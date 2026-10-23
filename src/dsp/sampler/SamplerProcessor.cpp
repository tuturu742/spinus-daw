#include "SamplerProcessor.h"

#include "SampleStorage.h"
#include "SamplerEditor.h"
#include "dsp/ParameterText.h"

namespace spinus::dsp
{

namespace
{
using Range = juce::NormalisableRange<float>;

Range seconds (float max)
{
    Range r (0.0f, max);
    r.setSkewForCentre (max / 20.0f);
    return r;
}

// How each kind of knob shows its value (typed values read as dsp/ParameterText.h says).
enum class Unit
{
    decibels,
    seconds,
    hertz,
    percent,
    semitones,
    cents
};

juce::String format (Unit unit, float v)
{
    switch (unit)
    {
    case Unit::decibels:
        return juce::String (v, 1) + " dB";
    case Unit::seconds:
        return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s";
    case Unit::hertz:
        return v < 1000.0f ? juce::String (juce::roundToInt (v)) + " Hz" : juce::String (v / 1000.0f, 1) + " kHz";
    case Unit::percent:
        return juce::String (juce::roundToInt (v * 100.0f)) + "%";
    case Unit::semitones:
        return juce::String (juce::roundToInt (v)) + " st";
    case Unit::cents:
        return juce::String (juce::roundToInt (v)) + " ct";
    }
    return juce::String (v);
}

juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    auto add = [&] (const char* id, const char* name, const Range& range, float value, Unit unit)
    {
        layout.add (std::make_unique<spinus::dsp::param::FloatSetting> (
            ParameterID { id, 1 }, name, range, value,
            AudioParameterFloatAttributes().withStringFromValueFunction ([unit] (float v, int)
                                                                         { return format (unit, v); })));
    };

    add ("gain", "Volume", Range (-48.0f, 12.0f), 0.0f, Unit::decibels);
    add ("attack", "Attack", seconds (10.0f), 0.001f, Unit::seconds);
    add ("decay", "Decay", seconds (10.0f), 0.3f, Unit::seconds);
    add ("sustain", "Sustain", Range (0.0f, 1.0f), 1.0f, Unit::percent);
    add ("release", "Release", seconds (10.0f), 0.05f, Unit::seconds);
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "filter", 1 }, "Filter", StringArray { "Off", "Low pass", "High pass", "Band pass" }, 0));
    Range cutoff (20.0f, 20000.0f);
    cutoff.setSkewForCentre (1000.0f);
    add ("cutoff", "Cutoff", cutoff, 20000.0f, Unit::hertz);
    add ("resonance", "Resonance", Range (0.0f, 1.0f), 0.0f, Unit::percent);
    add ("velocity", "Velocity", Range (0.0f, 1.0f), 1.0f, Unit::percent);
    add ("coarse", "Pitch", Range (-24.0f, 24.0f, 1.0f), 0.0f, Unit::semitones);
    add ("fine", "Fine", Range (-100.0f, 100.0f), 0.0f, Unit::cents);
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "oneShot", 1 }, "One shot", false));
    return layout;
}

} // namespace

//==============================================================================
SamplerProcessor::SamplerProcessor()
    : juce::AudioPluginInstance (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "SamplerParameters", makeLayout())
{
    auto get = [this] (const char* id) { return parameters.getRawParameterValue (id); };
    p = { get ("gain"),   get ("attack"),    get ("decay"),    get ("sustain"), get ("release"), get ("filter"),
          get ("cutoff"), get ("resonance"), get ("velocity"), get ("coarse"),  get ("fine"),    get ("oneShot") };
    publish ({});
}

SamplerProcessor::~SamplerProcessor() = default;

juce::AudioFormatManager& SamplerProcessor::getFormats() { return getAudioFormats(); }

SamplerSettings SamplerProcessor::getSettings() const noexcept
{
    SamplerSettings s;
    s.gainDb = p.gain->load();
    s.attack = p.attack->load();
    s.decay = p.decay->load();
    s.sustain = p.sustain->load();
    s.release = p.release->load();
    s.filter = (SamplerSettings::Filter)juce::jlimit (0, 3, juce::roundToInt (p.filter->load()));
    s.cutoffHz = p.cutoff->load();
    s.resonance = p.resonance->load();
    s.velocitySensitivity = p.velocity->load();
    s.coarse = p.coarse->load();
    s.fine = p.fine->load();
    s.oneShot = p.oneShot->load() > 0.5f;
    return s;
}

//==============================================================================
void SamplerProcessor::publish (std::vector<SampleZone> zones)
{
    sets.publish (std::move (zones));
    sendChangeMessage();
}

std::vector<SampleZone> SamplerProcessor::getZones() const { return sets.getCurrent().zones; }

void SamplerProcessor::setZones (std::vector<SampleZone> zones) { publish (std::move (zones)); }

void SamplerProcessor::updateZone (int index, const std::function<void (SampleZone&)>& change)
{
    auto zones = getZones();
    if (! juce::isPositiveAndBelow (index, (int)zones.size()))
        return;
    change (zones[(size_t)index]);
    publish (std::move (zones));
}

void SamplerProcessor::removeZone (int index)
{
    auto zones = getZones();
    if (! juce::isPositiveAndBelow (index, (int)zones.size()))
        return;
    zones.erase (zones.begin() + index);
    publish (std::move (zones));
}

void SamplerProcessor::addZone (juce::AudioBuffer<float> audio, double rate, const juce::String& name, int root)
{
    SampleZone zone;
    zone.name = name;
    zone.sampleRate = rate;
    zone.rootNote = root;
    zone.end = audio.getNumSamples();
    zone.loopEnd = zone.end;
    zone.audio = std::make_shared<const juce::AudioBuffer<float>> (std::move (audio));

    auto zones = getZones();
    zones.push_back (std::move (zone));
    publish (std::move (zones));
}

juce::Result SamplerProcessor::loadFiles (const juce::Array<juce::File>& files, bool replace)
{
    std::vector<SampleZone> loaded;
    bool anyNoteNames = false;

    for (auto& file : files)
    {
        SampleZone zone;
        if (auto r = readAudioFile (file, zone, maxSampleSeconds); r.failed())
            return r;
        if (auto note = noteFromFileName (zone.name))
        {
            zone.rootNote = *note;
            anyNoteNames = true;
        }
        loaded.push_back (std::move (zone));
    }

    if (loaded.size() > 1 && anyNoteNames)
        mapZonesByRoot (loaded);

    auto zones = replace ? std::vector<SampleZone>() : getZones();
    for (auto& z : loaded)
        zones.push_back (std::move (z));
    publish (std::move (zones));
    return juce::Result::ok();
}

//==============================================================================
void SamplerProcessor::fillInPluginDescription (juce::PluginDescription& d) const
{
    d.name = pluginName;
    d.descriptiveName = "Plays samples: one-shots, loops and multi-sampled instruments";
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

void SamplerProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    engine.prepare (sampleRate, maximumExpectedSamplesPerBlock);
}

void SamplerProcessor::releaseResources() { engine.reset(); }

bool SamplerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return layouts.getMainInputChannelSet().isDisabled() &&
           (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

double SamplerProcessor::getTailLengthSeconds() const { return (double)p.release->load(); }

void SamplerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    const auto settings = getSettings();
    engine.render (buffer, midi, sets.getPlaying(), { &settings, 1 }, 0, buffer.getNumSamples());
    activeVoices.store (engine.getNumActiveVoices());
    sets.blockDone();
}

juce::AudioProcessorEditor* SamplerProcessor::createEditor() { return new SamplerEditor (*this); }

//==============================================================================
void SamplerProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    juce::ValueTree state ("SpinusSampler");
    state.setProperty ("version", 1, nullptr);
    state.appendChild (parameters.copyState(), nullptr);

    for (auto& z : sets.getCurrent().zones)
        state.appendChild (zoneToState (z), nullptr);

    juce::MemoryOutputStream out (destination, false);
    state.writeToStream (out);
}

void SamplerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto state = juce::ValueTree::readFromData (data, (size_t)sizeInBytes);
    if (! state.hasType ("SpinusSampler"))
        return;

    if (auto params = state.getChildWithName (parameters.state.getType()); params.isValid())
        parameters.replaceState (params.createCopy());

    std::vector<SampleZone> zones;
    for (const auto& zone : state)
        if (zone.hasType ("Zone"))
            zones.push_back (zoneFromState (zone));

    publish (std::move (zones));
}

} // namespace spinus::dsp
