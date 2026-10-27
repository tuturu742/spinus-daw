#include "DrumMachineProcessor.h"

#include "../sampler/SampleSet.h"
#include "DrumMachineEditor.h"
#include "dsp/ParameterText.h"
#include "DrumSynth.h"

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

juce::String percent (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; }
juce::String time (float v, int)
{
    return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s";
}

juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<spinus::dsp::param::FloatSetting> (
        ParameterID { "volume", 1 }, "Volume", Range (-48.0f, 12.0f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                                                                     { return String (v, 1) + " dB"; })));

    for (int pad = 0; pad < DrumMachineProcessor::numPads; ++pad)
    {
        auto group =
            std::make_unique<AudioProcessorParameterGroup> ("pad" + String (pad + 1), "Pad " + String (pad + 1), " | ");
        auto add = [&] (const char* setting, const char* name, const Range& range, float value,
                        std::function<String (float, int)> text)
        {
            group->addChild (std::make_unique<spinus::dsp::param::FloatSetting> (
                ParameterID { DrumMachineProcessor::parameterId (setting, pad), 1 },
                "Pad " + String (pad + 1) + " " + name, range, value,
                AudioParameterFloatAttributes().withStringFromValueFunction (std::move (text))));
        };

        add ("volume", "Volume", Range (-48.0f, 12.0f), 0.0f, [] (float v, int) { return String (v, 1) + " dB"; });
        add ("pan", "Pan", Range (-1.0f, 1.0f), 0.0f,
             [] (float v, int)
             {
                 const auto p = roundToInt (v * 100.0f);
                 return p == 0 ? String ("Centre") : String (std::abs (p)) + (p < 0 ? "% L" : "% R");
             });
        add ("tune", "Tune", Range (-24.0f, 24.0f, 1.0f), 0.0f,
             [] (float v, int) { return String (roundToInt (v)) + " st"; });
        add ("fine", "Fine", Range (-100.0f, 100.0f), 0.0f,
             [] (float v, int) { return String (roundToInt (v)) + " ct"; });
        add ("attack", "Attack", seconds (2.0f), 0.0f, time);
        add ("decay", "Decay", seconds (10.0f), 0.3f, time);
        add ("sustain", "Sustain", Range (0.0f, 1.0f), 1.0f, percent);
        add ("release", "Release", seconds (5.0f), 0.1f, time);
        group->addChild (std::make_unique<AudioParameterChoice> (
            ParameterID { DrumMachineProcessor::parameterId ("filter", pad), 1 }, "Pad " + String (pad + 1) + " Filter",
            StringArray { "Off", "Low pass", "High pass", "Band pass" }, 0));
        Range cutoff (20.0f, 20000.0f);
        cutoff.setSkewForCentre (1000.0f);
        add ("cutoff", "Cutoff", cutoff, 20000.0f, [] (float v, int)
             { return v < 1000.0f ? String (roundToInt (v)) + " Hz" : String (v / 1000.0f, 1) + " kHz"; });
        add ("resonance", "Resonance", Range (0.0f, 1.0f), 0.0f, percent);
        add ("velocity", "Velocity", Range (0.0f, 1.0f), 1.0f, percent);
        group->addChild (
            std::make_unique<AudioParameterBool> (ParameterID { DrumMachineProcessor::parameterId ("oneShot", pad), 1 },
                                                  "Pad " + String (pad + 1) + " One shot", true));
        layout.add (std::move (group));
    }
    return layout;
}
} // namespace

juce::String DrumMachineProcessor::parameterId (const juce::String& setting, int pad)
{
    return "pad" + juce::String (pad + 1) + "_" + setting;
}

//==============================================================================
DrumMachineProcessor::DrumMachineProcessor()
    : juce::AudioPluginInstance (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "DrumMachineParameters", makeLayout())
{
    masterVolume = parameters.getRawParameterValue ("volume");
    for (int pad = 0; pad < numPads; ++pad)
    {
        auto get = [&] (const char* setting) { return parameters.getRawParameterValue (parameterId (setting, pad)); };
        padParameters[(size_t)pad] = { get ("volume"), get ("pan"),    get ("tune"),      get ("fine"),
                                       get ("attack"), get ("decay"),  get ("sustain"),   get ("release"),
                                       get ("filter"), get ("cutoff"), get ("resonance"), get ("velocity"),
                                       get ("oneShot") };
    }

    for (int pad = 0; pad < numPads; ++pad)
        pads[(size_t)pad].note = firstNote + pad;
    loadSynthKit();
    for (auto* parameter : juce::AudioProcessor::getParameters())
        parameter->addListener (this);
}

DrumMachineProcessor::~DrumMachineProcessor()
{
    for (auto* parameter : juce::AudioProcessor::getParameters())
        parameter->removeListener (this);
}

void DrumMachineProcessor::setKit (const juce::String& name)
{
    kitName = name;
    kitModified = false;
    sendChangeMessage();
}

void DrumMachineProcessor::markModified()
{
    if (! kitModified.exchange (true))
        sendChangeMessage();
}

void DrumMachineProcessor::parameterGestureChanged (int, bool starting)
{
    // A control turned in a window (Spinus's or a host's); automation changes values without gestures.
    if (starting)
        markModified();
}

juce::String DrumMachineProcessor::getKitDisplayName() const
{
    if (kitName.isEmpty())
        return "New kit";
    return kitModified.load() ? kitName + " *" : kitName;
}

SamplerSettings DrumMachineProcessor::getPadSettings (int pad) const noexcept
{
    const auto& p = padParameters[(size_t)pad];
    SamplerSettings s;
    s.gainDb = p.volume->load() + masterVolume->load();
    s.pan = p.pan->load();
    s.coarse = p.tune->load();
    s.fine = p.fine->load();
    s.attack = p.attack->load();
    s.decay = p.decay->load();
    s.sustain = p.sustain->load();
    s.release = p.release->load();
    s.filter = (SamplerSettings::Filter)juce::jlimit (0, 3, juce::roundToInt (p.filter->load()));
    s.cutoffHz = p.cutoff->load();
    s.resonance = p.resonance->load();
    s.velocitySensitivity = p.velocity->load();
    s.oneShot = p.oneShot->load() > 0.5f;
    return s;
}

//==============================================================================
void DrumMachineProcessor::publish()
{
    std::vector<SampleZone> zones;
    for (int i = 0; i < numPads; ++i)
    {
        const auto& pad = pads[(size_t)i];
        if (pad.audio == nullptr)
            continue;

        SampleZone z;
        z.name = pad.name;
        z.audio = pad.audio;
        z.sampleRate = pad.sampleRate;
        z.rootNote = z.lowKey = z.highKey = pad.note;
        z.end = pad.audio->getNumSamples();
        z.settingsIndex = i;
        z.chokeGroup = pad.chokeGroup;
        zones.push_back (std::move (z));
    }
    sets.publish (std::move (zones));
    sendChangeMessage();
}

juce::Result DrumMachineProcessor::loadPadFile (int pad, const juce::File& file)
{
    if (! juce::isPositiveAndBelow (pad, numPads))
        return juce::Result::fail ("No such pad");

    SampleZone zone;
    if (auto r = readAudioFile (file, zone, 60.0); r.failed())
        return r;

    auto& p = pads[(size_t)pad];
    p.name = zone.name;
    p.audio = zone.audio;
    p.sampleRate = zone.sampleRate;
    markModified();
    publish();
    return juce::Result::ok();
}

void DrumMachineProcessor::setPadAudio (int pad, juce::AudioBuffer<float> audio, double rate, const juce::String& name)
{
    if (! juce::isPositiveAndBelow (pad, numPads))
        return;
    auto& p = pads[(size_t)pad];
    p.name = name;
    p.sampleRate = rate;
    p.audio = std::make_shared<const juce::AudioBuffer<float>> (std::move (audio));
    markModified();
    publish();
}

void DrumMachineProcessor::clearPad (int pad)
{
    if (! juce::isPositiveAndBelow (pad, numPads))
        return;
    auto& p = pads[(size_t)pad];
    if (p.audio != nullptr)
        markModified();
    p.name = {};
    p.audio = nullptr;
    publish();
}

void DrumMachineProcessor::clearAllPads()
{
    for (auto& p : pads)
    {
        p.name = {};
        p.audio = nullptr;
    }
    publish();
    setKit ({});
}

void DrumMachineProcessor::setPadNote (int pad, int note)
{
    if (juce::isPositiveAndBelow (pad, numPads) && pads[(size_t)pad].note != juce::jlimit (0, 127, note))
    {
        pads[(size_t)pad].note = juce::jlimit (0, 127, note);
        markModified();
        publish();
    }
}

void DrumMachineProcessor::setPadChoke (int pad, int group)
{
    if (juce::isPositiveAndBelow (pad, numPads) && pads[(size_t)pad].chokeGroup != juce::jlimit (0, 8, group))
    {
        pads[(size_t)pad].chokeGroup = juce::jlimit (0, 8, group);
        markModified();
        publish();
    }
}

void DrumMachineProcessor::resetPadParameters()
{
    for (auto* parameter : juce::AudioProcessor::getParameters())
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (parameter))
            p->setValueNotifyingHost (p->getDefaultValue());
}

void DrumMachineProcessor::loadSynthKit()
{
    constexpr double kitRate = 48000.0;
    auto kit = makeSynthKit (kitRate);
    for (int i = 0; i < numPads; ++i)
    {
        auto& p = pads[(size_t)i];
        p.note = firstNote + i;
        if (i < (int)kit.size())
        {
            p.name = kit[(size_t)i].name;
            p.sampleRate = kitRate;
            p.chokeGroup = kit[(size_t)i].chokeGroup;
            p.audio = std::make_shared<const juce::AudioBuffer<float>> (std::move (kit[(size_t)i].audio));
        }
        else
        {
            p = Pad { {}, nullptr, kitRate, firstNote + i, 0 };
        }
    }
    resetPadParameters();
    publish();
    setKit ("Spinus Synth Kit");
}

juce::Result DrumMachineProcessor::loadFactoryKit (const starter::Kit& kit, const juce::File& pack)
{
    juce::StringArray missing;
    for (int i = 0; i < numPads; ++i)
    {
        auto& p = pads[(size_t)i];
        p = Pad { {}, nullptr, 48000.0, firstNote + i, 0 };
        if (i >= (int)kit.pads.size())
            continue;
        const auto& pad = kit.pads[(size_t)i];
        SampleZone zone;
        if (const auto file = starter::padFile (pack, pad);
            ! file.existsAsFile() || readAudioFile (file, zone, 60.0).failed())
        {
            missing.add (pad.name.isNotEmpty() ? pad.name : pad.file);
            continue;
        }
        p.name = pad.name.isNotEmpty() ? pad.name : zone.name;
        p.audio = zone.audio;
        p.sampleRate = zone.sampleRate;
        p.chokeGroup = pad.chokeGroup;
    }
    resetPadParameters();
    for (int i = 0; i < juce::jmin (numPads, (int)kit.pads.size()); ++i)
        for (const auto& [setting, value] : { std::pair { "volume", kit.pads[(size_t)i].volumeDb },
                                              std::pair { "tune", kit.pads[(size_t)i].tuneSemitones } })
            if (auto* parameter = parameters.getParameter (parameterId (setting, i));
                parameter != nullptr && std::abs (value) > 0.001f)
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    publish();
    setKit (kit.name);
    if (missing.isEmpty())
        return juce::Result::ok();
    return juce::Result::fail ("The kit " + kit.name + " is missing some sounds (" + missing.joinIntoString (", ") +
                               "): the starter pack is not fully installed");
}

juce::Result DrumMachineProcessor::saveKit (const juce::File& file)
{
    const auto previousName = kitName;
    const bool previouslyModified = kitModified.load();
    setKit (file.getFileNameWithoutExtension()); // the saved kit is this file, unchanged
    juce::MemoryBlock state;
    getStateInformation (state);
    if (file.replaceWithData (state.getData(), state.getSize()))
        return juce::Result::ok();
    kitName = previousName;
    kitModified = previouslyModified;
    sendChangeMessage();
    return juce::Result::fail ("Could not write " + file.getFullPathName());
}

juce::Result DrumMachineProcessor::loadKit (const juce::File& file)
{
    juce::MemoryBlock state;
    if (! file.loadFileAsData (state))
        return juce::Result::fail ("Could not read " + file.getFullPathName());
    if (! juce::ValueTree::readFromData (state.getData(), state.getSize()).hasType ("SpinusDrumMachine"))
        return juce::Result::fail (file.getFileName() + " is not a Spinus drum kit");
    setStateInformation (state.getData(), (int)state.getSize());
    setKit (file.getFileNameWithoutExtension());
    return juce::Result::ok();
}

void DrumMachineProcessor::audition (int pad, bool on)
{
    if (! juce::isPositiveAndBelow (pad, numPads))
        return;
    const auto scope = auditionFifo.write (1);
    if (scope.blockSize1 > 0)
        auditionEvents[(size_t)scope.startIndex1] = { pads[(size_t)pad].note, on ? 100 : 0 };
}

//==============================================================================
void DrumMachineProcessor::fillInPluginDescription (juce::PluginDescription& d) const
{
    d.name = pluginName;
    d.descriptiveName = "16 drum pads with choke groups and kits";
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

void DrumMachineProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    engine.prepare (sampleRate, maximumExpectedSamplesPerBlock);
}

void DrumMachineProcessor::releaseResources() { engine.reset(); }

bool DrumMachineProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return layouts.getMainInputChannelSet().isDisabled() &&
           (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void DrumMachineProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    std::array<SamplerSettings, numPads> settings;
    for (int pad = 0; pad < numPads; ++pad)
        settings[(size_t)pad] = getPadSettings (pad);

    const auto* set = sets.getPlaying();
    const auto read = auditionFifo.read (auditionFifo.getNumReady());
    auto play = [&] (int start, int count)
    {
        for (int i = start; i < start + count; ++i)
        {
            const auto [note, velocity] = auditionEvents[(size_t)i];
            engine.handleNow (velocity > 0 ? juce::MidiMessage::noteOn (1, note, (juce::uint8)velocity)
                                           : juce::MidiMessage::noteOff (1, note),
                              set, settings);
        }
    };
    play (read.startIndex1, read.blockSize1);
    play (read.startIndex2, read.blockSize2);

    engine.render (buffer, midi, set, settings, 0, buffer.getNumSamples());
    activeVoices.store (engine.getNumActiveVoices());
    sets.blockDone();
}

juce::AudioProcessorEditor* DrumMachineProcessor::createEditor() { return new DrumMachineEditor (*this); }

//==============================================================================
void DrumMachineProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    juce::ValueTree state ("SpinusDrumMachine");
    state.setProperty ("version", 1, nullptr);
    state.setProperty ("kitName", kitName, nullptr);
    state.setProperty ("kitModified", kitModified.load(), nullptr);
    state.appendChild (parameters.copyState(), nullptr);

    for (int i = 0; i < numPads; ++i)
    {
        const auto& pad = pads[(size_t)i];
        juce::ValueTree node ("Pad");
        node.setProperty ("index", i, nullptr)
            .setProperty ("note", pad.note, nullptr)
            .setProperty ("choke", pad.chokeGroup, nullptr);
        if (pad.audio != nullptr)
        {
            SampleZone z;
            z.name = pad.name;
            z.audio = pad.audio;
            z.sampleRate = pad.sampleRate;
            z.end = pad.audio->getNumSamples();
            node.appendChild (zoneToState (z), nullptr);
        }
        state.appendChild (node, nullptr);
    }

    juce::MemoryOutputStream out (destination, false);
    state.writeToStream (out);
}

void DrumMachineProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto state = juce::ValueTree::readFromData (data, (size_t)sizeInBytes);
    if (! state.hasType ("SpinusDrumMachine"))
        return;

    if (auto params = state.getChildWithName (parameters.state.getType()); params.isValid())
        parameters.replaceState (params.createCopy());

    for (int i = 0; i < numPads; ++i)
        pads[(size_t)i] = Pad { {}, nullptr, 48000.0, firstNote + i, 0 };

    for (const auto& node : state)
    {
        const int index = node.getProperty ("index", -1);
        if (! node.hasType ("Pad") || ! juce::isPositiveAndBelow (index, numPads))
            continue;

        auto& pad = pads[(size_t)index];
        pad.note = juce::jlimit (0, 127, (int)node.getProperty ("note", firstNote + index));
        pad.chokeGroup = juce::jlimit (0, 8, (int)node.getProperty ("choke", 0));
        if (auto zoneState = node.getChildWithName ("Zone"); zoneState.isValid())
        {
            const auto zone = zoneFromState (zoneState);
            pad.name = zone.name;
            pad.audio = zone.audio;
            pad.sampleRate = zone.sampleRate;
        }
    }
    // A state saved before kits had names: "Kit", as the button said then.
    kitName = state.getProperty ("kitName", "Kit").toString();
    kitModified = (bool)state.getProperty ("kitModified", false);
    publish();
}

} // namespace spinus::dsp
