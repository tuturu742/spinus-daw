#include "BeatSlicerProcessor.h"

#include "BeatSlicerEditor.h"
#include "dsp/ParameterText.h"

#include <set>

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

String time (float v, int) { return v < 1.0f ? String (juce::roundToInt (v * 1000.0f)) + " ms" : String (v, 2) + " s"; }

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
    layout.add (floatParameter ("gain", "Volume", Range (-48.0f, 12.0f), 0.0f,
                                [] (float v, int) { return String (v, 1) + " dB"; }),
                floatParameter ("attack", "Attack", seconds (2.0f), 0.001f, time),
                floatParameter ("release", "Release", seconds (2.0f), 0.01f, time),
                floatParameter ("coarse", "Pitch", Range (-24.0f, 24.0f, 1.0f), 0.0f,
                                [] (float v, int) { return String (roundToInt (v)) + " st"; }),
                floatParameter ("fine", "Fine", Range (-100.0f, 100.0f), 0.0f,
                                [] (float v, int) { return String (roundToInt (v)) + " ct"; }),
                std::make_unique<AudioParameterBool> (ParameterID { "playToEnd", 1 }, "Play to slice end", true));
    return layout;
}

juce::String pointsToText (const std::vector<juce::int64>& points)
{
    juce::StringArray parts;
    for (auto p : points)
        parts.add (juce::String (p));
    return parts.joinIntoString (" ");
}

std::vector<juce::int64> pointsFromText (const juce::String& text)
{
    std::vector<juce::int64> points;
    for (auto& part : juce::StringArray::fromTokens (text, " ", ""))
        if (part.isNotEmpty())
            points.push_back (part.getLargeIntValue());
    return points;
}
} // namespace

juce::String BeatSlicerProcessor::describeBeats (double beats)
{
    if (beats >= 4.0)
    {
        const auto bars = beats / 4.0;
        return juce::String (bars, juce::exactlyEqual (bars, std::floor (bars)) ? 0 : 2) +
               (juce::exactlyEqual (bars, 1.0) ? " bar" : " bars");
    }
    if (beats >= 1.0)
        return juce::String (beats, 0) + (juce::exactlyEqual (beats, 1.0) ? " beat" : " beats");
    return "1/" + juce::String (juce::roundToInt (4.0 / beats)); // a note length: 1/8, 1/16...
}

BeatSlicerProcessor::BeatSlicerProcessor()
    : juce::AudioPluginInstance (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "BeatSlicerParameters", makeLayout())
{
    gain = parameters.getRawParameterValue ("gain");
    attack = parameters.getRawParameterValue ("attack");
    release = parameters.getRawParameterValue ("release");
    coarse = parameters.getRawParameterValue ("coarse");
    fine = parameters.getRawParameterValue ("fine");
    playToEnd = parameters.getRawParameterValue ("playToEnd");
    publish();
}

BeatSlicerProcessor::~BeatSlicerProcessor() = default;

SamplerSettings BeatSlicerProcessor::getSettings() const noexcept
{
    SamplerSettings s;
    s.gainDb = gain->load();
    s.attack = attack->load();
    s.decay = 0.3f;
    s.sustain = 1.0f;
    s.release = release->load();
    s.coarse = coarse->load();
    s.fine = fine->load();
    s.oneShot = playToEnd->load() > 0.5f;
    return s;
}

//==============================================================================
juce::String BeatSlicerProcessor::keyName (int key) { return juce::MidiMessage::getMidiNoteName (key, true, true, 5); }

void BeatSlicerProcessor::defaultKeys()
{
    keys.resize (points.size());
    for (size_t i = 0; i < keys.size(); ++i)
        keys[i] = firstNote + (int)i;
}

int BeatSlicerProcessor::getSliceKey (int slice) const
{
    return juce::isPositiveAndBelow (slice, (int)keys.size()) ? keys[(size_t)slice] : -1;
}

void BeatSlicerProcessor::setSliceKey (int slice, int key)
{
    if (! juce::isPositiveAndBelow (slice, (int)keys.size()) || ! juce::isPositiveAndBelow (key, 128))
        return;
    for (auto& k : keys)
        if (k == key)
            k = keys[(size_t)slice]; // the slice that was there swaps
    keys[(size_t)slice] = key;
    publish();
}

void BeatSlicerProcessor::resetSliceKeys()
{
    defaultKeys();
    publish();
}

void BeatSlicerProcessor::publish()
{
    if (keys.size() != points.size())
        defaultKeys();
    std::vector<SampleZone> zones;
    if (hasLoop())
    {
        const auto length = loop.getLength();
        for (size_t i = 0; i < points.size(); ++i)
        {
            auto zone = loop;
            zone.name = "Slice " + juce::String ((int)i + 1);
            zone.start = points[i];
            zone.end = i + 1 < points.size() ? points[i + 1] : length;
            zone.rootNote = zone.lowKey = zone.highKey = keys[i];
            zone.loop = false;
            // A short fade at the end: a slice usually ends just before the next hit, mid-waveform.
            zone.fadeOutSamples =
                (int)std::min<juce::int64> ((juce::int64)(0.002 * loop.sampleRate), (zone.end - zone.start) / 4);
            zones.push_back (std::move (zone));
        }
    }
    sets.publish (std::move (zones));
    sendChangeMessage();
}

juce::Result BeatSlicerProcessor::loadFile (const juce::File& file)
{
    SampleZone zone;
    if (auto r = readAudioFile (file, zone, maxLoopSeconds); r.failed())
        return r;
    auto audio = *zone.audio;
    setLoop (std::move (audio), zone.sampleRate, zone.name);
    return juce::Result::ok();
}

void BeatSlicerProcessor::setLoop (juce::AudioBuffer<float> audio, double sampleRate, const juce::String& name)
{
    loop = {};
    loop.name = name;
    loop.sampleRate = sampleRate;
    loop.end = audio.getNumSamples();
    loop.audio = std::make_shared<const juce::AudioBuffer<float>> (std::move (audio));
    loopBeats = slicing::estimateBeats ((double)loop.end / sampleRate, hostTempo.load(), name);
    slice (method, sensitivity, gridBeats); // publishes
}

void BeatSlicerProcessor::setLoopBeats (double beats)
{
    loopBeats = juce::jlimit (0.25, 512.0, beats);
    if (method == Method::grid)
        slice (method, sensitivity, gridBeats);
    else
        sendChangeMessage();
}

void BeatSlicerProcessor::slice (Method newMethod, float newSensitivity, double newGridBeats)
{
    method = newMethod;
    sensitivity = juce::jlimit (0.0f, 1.0f, newSensitivity);
    gridBeats = juce::jlimit (1.0 / 64.0, 16.0, newGridBeats);
    if (! hasLoop())
        points = { 0 };
    else if (method == Method::grid)
        points = slicing::byGrid (loop.getLength(), loopBeats, gridBeats);
    else
        points = slicing::byTransients (*loop.audio, loop.sampleRate, sensitivity);
    defaultKeys();
    publish();
}

void BeatSlicerProcessor::setSlicePoints (std::vector<juce::int64> newPoints)
{
    points = slicing::tidy (std::move (newPoints), std::max<juce::int64> (1, loop.getLength()));
    publish(); // keys stay if the number of slices did not change
}

void BeatSlicerProcessor::addSlicePoint (juce::int64 position)
{
    auto changed = slicing::tidy (
        [&]
        {
            auto p = points;
            p.push_back (position);
            return p;
        }(),
        std::max<juce::int64> (1, loop.getLength()));
    if (changed.size() == points.size())
        return; // already a point there, or too many slices

    // The new slice gets the lowest free key from C5 up; the others keep theirs.
    const auto index = (size_t)std::distance (changed.begin(), std::find (changed.begin(), changed.end(), position));
    int key = firstNote;
    while (std::find (keys.begin(), keys.end(), key) != keys.end() && key < 127)
        ++key;
    keys.insert (keys.begin() + (std::ptrdiff_t)juce::jmin (index, keys.size()), key);
    points = std::move (changed);
    publish();
}

void BeatSlicerProcessor::moveSlicePoint (int index, juce::int64 position)
{
    // The first slice always starts at the beginning; others stay between their neighbours.
    if (index <= 0 || index >= (int)points.size())
        return;
    auto changed = points;
    const auto low = changed[(size_t)index - 1] + 1;
    const auto high = (size_t)index + 1 < changed.size() ? changed[(size_t)index + 1] - 1 : loop.getLength() - 1;
    changed[(size_t)index] = std::clamp (position, low, std::max (low, high));
    setSlicePoints (std::move (changed));
}

void BeatSlicerProcessor::removeSlicePoint (int index)
{
    if (index <= 0 || index >= (int)points.size())
        return;
    // The slice before takes this one's audio and keeps its key.
    points.erase (points.begin() + index);
    keys.erase (keys.begin() + index);
    publish();
}

std::vector<BeatSlicerProcessor::SliceNote> BeatSlicerProcessor::getSliceNotes() const
{
    std::vector<SliceNote> notes;
    const auto length = (double)loop.getLength();
    if (! hasLoop() || length <= 0.0)
        return notes;
    for (size_t i = 0; i < points.size(); ++i)
    {
        const auto end = i + 1 < points.size() ? (double)points[i + 1] : length;
        notes.push_back (
            { keys[i], (double)points[i] / length * loopBeats, (end - (double)points[i]) / length * loopBeats });
    }
    return notes;
}

void BeatSlicerProcessor::audition (int slice, bool on)
{
    if (! juce::isPositiveAndBelow (slice, (int)points.size()))
        return;
    const auto scope = auditionFifo.write (1);
    if (scope.blockSize1 > 0)
        auditionEvents[(size_t)scope.startIndex1] = { keys[(size_t)slice], on ? 100 : 0 };
}

//==============================================================================
void BeatSlicerProcessor::fillInPluginDescription (juce::PluginDescription& d) const
{
    d.name = pluginName;
    d.descriptiveName = "Chops a loop into slices played from the keyboard";
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

void BeatSlicerProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    engine.prepare (sampleRate, maximumExpectedSamplesPerBlock);
}

void BeatSlicerProcessor::releaseResources() { engine.reset(); }

bool BeatSlicerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return layouts.getMainInputChannelSet().isDisabled() &&
           (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

double BeatSlicerProcessor::getTailLengthSeconds() const { return (double)release->load(); }

void BeatSlicerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
            if (const auto bpm = position->getBpm())
                hostTempo.store (*bpm, std::memory_order_relaxed);

    const auto settings = getSettings();
    const auto* set = sets.getPlaying();
    const auto read = auditionFifo.read (auditionFifo.getNumReady());
    auto play = [&] (int start, int count)
    {
        for (int i = start; i < start + count; ++i)
        {
            const auto [note, velocity] = auditionEvents[(size_t)i];
            engine.handleNow (velocity > 0 ? juce::MidiMessage::noteOn (1, note, (juce::uint8)velocity)
                                           : juce::MidiMessage::noteOff (1, note),
                              set, { &settings, 1 });
        }
    };
    play (read.startIndex1, read.blockSize1);
    play (read.startIndex2, read.blockSize2);

    engine.render (buffer, midi, set, { &settings, 1 }, 0, buffer.getNumSamples());
    sets.blockDone();
}

juce::AudioProcessorEditor* BeatSlicerProcessor::createEditor() { return new BeatSlicerEditor (*this); }

//==============================================================================
void BeatSlicerProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    juce::ValueTree state ("SpinusBeatSlicer");
    state.setProperty ("version", 1, nullptr);
    state.setProperty ("points", pointsToText (points), nullptr);
    juce::StringArray keyText;
    for (auto k : keys)
        keyText.add (juce::String (k));
    state.setProperty ("keys", keyText.joinIntoString (" "), nullptr);
    state.setProperty ("loopBeats", loopBeats, nullptr);
    state.setProperty ("method", method == Method::grid ? "grid" : "transients", nullptr);
    state.setProperty ("sensitivity", sensitivity, nullptr);
    state.setProperty ("gridBeats", gridBeats, nullptr);
    state.appendChild (parameters.copyState(), nullptr);
    if (hasLoop())
        state.appendChild (zoneToState (loop), nullptr);

    juce::MemoryOutputStream out (destination, false);
    state.writeToStream (out);
}

void BeatSlicerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto state = juce::ValueTree::readFromData (data, (size_t)sizeInBytes);
    if (! state.hasType ("SpinusBeatSlicer"))
        return;

    if (auto params = state.getChildWithName (parameters.state.getType()); params.isValid())
        parameters.replaceState (params.createCopy());

    loop = {};
    if (auto zone = state.getChildWithName ("Zone"); zone.isValid())
        loop = zoneFromState (zone);
    loopBeats = state.getProperty ("loopBeats", 4.0);
    method = state["method"].toString() == "grid" ? Method::grid : Method::transients;
    sensitivity = state.getProperty ("sensitivity", 0.5f);
    gridBeats = state.getProperty ("gridBeats", 0.25);
    points = slicing::tidy (pointsFromText (state["points"].toString()), std::max<juce::int64> (1, loop.getLength()));
    keys.clear();
    for (auto& k : juce::StringArray::fromTokens (state["keys"].toString(), " ", ""))
        if (k.isNotEmpty())
            keys.push_back (juce::jlimit (0, 127, k.getIntValue()));
    if (keys.size() != points.size() || std::set<int> (keys.begin(), keys.end()).size() != keys.size())
        defaultKeys(); // saved before keys could be chosen
    publish();
}

} // namespace spinus::dsp
