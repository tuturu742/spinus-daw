#include "LoudnessMeterProcessor.h"

#include "LoudnessMeterEditor.h"

namespace spinus::dsp
{

const std::array<LoudnessMeterProcessor::Target, 4>& LoudnessMeterProcessor::getTargets()
{
    // What the services and standards ask for (2026): Spotify, YouTube, Tidal and Amazon turn louder songs
    // down to about -14 LUFS; Apple Music to -16; EBU R128 broadcast is -23; club and CD masters are louder.
    static const std::array<Target, 4> targets { { { "Streaming (-14 LUFS)", -14.0f, -1.0f },
                                                   { "Apple Music (-16 LUFS)", -16.0f, -1.0f },
                                                   { "Broadcast, EBU R128 (-23 LUFS)", -23.0f, -1.0f },
                                                   { "Club or CD (-9 LUFS)", -9.0f, -0.3f } } };
    return targets;
}

LoudnessMeterProcessor::LoudnessMeterProcessor()
    : juce::AudioPluginInstance (BusesProperties()
                                     .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                     .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "LoudnessMeterParameters",
                  []
                  {
                      juce::StringArray names;
                      for (auto& t : getTargets())
                          names.add (t.name);
                      juce::AudioProcessorValueTreeState::ParameterLayout layout;
                      layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "target", 1 },
                                                                                "Target", names, 0),
                                  std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "reset_on_play", 1 },
                                                                              "Start again on Play", true));
                      return layout;
                  }())
{
    target = parameters.getRawParameterValue ("target");
    resetOnPlay = parameters.getRawParameterValue ("reset_on_play");
}

const LoudnessMeterProcessor::Target& LoudnessMeterProcessor::getTarget() const noexcept
{
    const auto& targets = getTargets();
    return targets[(size_t)juce::jlimit (0, (int)targets.size() - 1, juce::roundToInt (target->load()))];
}

void LoudnessMeterProcessor::fillInPluginDescription (juce::PluginDescription& d) const
{
    d.name = pluginName;
    d.descriptiveName = "Loudness (LUFS) and true peak, against a streaming or broadcast target";
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

void LoudnessMeterProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate);
    wasPlaying = false;
}

bool LoudnessMeterProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void LoudnessMeterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Each time playback starts, the song is measured from there.
    bool playing = false;
    if (auto* head = getPlayHead())
        if (auto position = head->getPosition())
            playing = position->getIsPlaying();
    if (playing && ! wasPlaying && resetOnPlay->load() > 0.5f)
        engine.reset();
    wasPlaying = playing;

    engine.process (getBusBuffer (buffer, true, 0));
}

juce::AudioProcessorEditor* LoudnessMeterProcessor::createEditor() { return new LoudnessMeterEditor (*this); }

void LoudnessMeterProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    juce::ValueTree state ("SpinusLoudnessMeter");
    state.setProperty ("version", 1, nullptr);
    state.appendChild (parameters.copyState(), nullptr);
    juce::MemoryOutputStream out (destination, false);
    state.writeToStream (out);
}

void LoudnessMeterProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto state = juce::ValueTree::readFromData (data, (size_t)sizeInBytes);
    if (state.hasType ("SpinusLoudnessMeter"))
        if (auto params = state.getChildWithName (parameters.state.getType()); params.isValid())
            parameters.replaceState (params.createCopy());
}

} // namespace spinus::dsp
