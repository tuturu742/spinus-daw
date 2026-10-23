#include "MutePlugin.h"

namespace spinus::engine
{

namespace te = tracktion;

const char* MutePlugin::xmlTypeName = "spinusMute";

MutePlugin::MutePlugin (te::PluginCreationInfo info) : te::Plugin (std::move (info)) {}

MutePlugin::~MutePlugin() { notifyListenersOfDeletion(); }

juce::ValueTree MutePlugin::create()
{
    juce::ValueTree v (te::IDs::PLUGIN);
    v.setProperty (te::IDs::type, xmlTypeName, nullptr);
    return v;
}

void MutePlugin::initialise (const te::PluginInitialisationInfo& info)
{
    gain.reset (info.sampleRate, 0.01);
    gain.setCurrentAndTargetValue (isMuted() ? 0.0f : 1.0f); // a render of a muted Master is silent from the start
}

void MutePlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
        return;

    gain.setTargetValue (isMuted() ? 0.0f : 1.0f);
    auto& buffer = *fc.destBuffer;
    const auto channels = buffer.getNumChannels();

    if (! gain.isSmoothing())
    {
        if (gain.getCurrentValue() < 0.5f) // muted
            for (int ch = 0; ch < channels; ++ch)
                buffer.clear (ch, fc.bufferStartSample, fc.bufferNumSamples);
        return; // unmuted: untouched
    }

    juce::ScopedNoDenormals noDenormals;
    for (int i = 0; i < fc.bufferNumSamples; ++i)
    {
        const auto g = gain.getNextValue();
        for (int ch = 0; ch < channels; ++ch)
            buffer.getWritePointer (ch, fc.bufferStartSample)[i] *= g;
    }
}

} // namespace spinus::engine
