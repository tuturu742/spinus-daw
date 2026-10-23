#include "OutputCapturePlugin.h"

namespace spinus::engine
{

namespace te = tracktion;

const char* OutputCapturePlugin::xmlTypeName = "spinusOutputCapture";

OutputCapturePlugin::OutputCapturePlugin (te::PluginCreationInfo info) : te::Plugin (std::move (info)) {}

OutputCapturePlugin::~OutputCapturePlugin() { notifyListenersOfDeletion(); }

juce::ValueTree OutputCapturePlugin::create()
{
    juce::ValueTree v (te::IDs::PLUGIN);
    v.setProperty (te::IDs::type, xmlTypeName, nullptr);
    return v;
}

void OutputCapturePlugin::initialise (const te::PluginInitialisationInfo& info)
{
    if (capture != nullptr)
        capture->sampleRate.store (info.sampleRate);
    gainLeft.reset (info.sampleRate, 0.05);
    gainRight.reset (info.sampleRate, 0.05);
}

void OutputCapturePlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (capture == nullptr || fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
        return;

    // From the first block that plays from the start of the song (not a later one: the plugin may have joined
    // the playback graph late); then every block, as the tail plays on after the transport stops.
    started = started || (fc.isPlaying && fc.editTime.getStart() <= te::TimePosition::fromSeconds (0.001));
    if (! started)
        return;

    auto& c = *capture;
    const auto remaining = c.wanted.load() - c.captured.load();
    const auto count = (int)juce::jmin ((juce::int64)fc.bufferNumSamples, remaining);
    if (count <= 0)
        return;

    const auto& source = *fc.destBuffer;
    const auto channels = source.getNumChannels();
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    c.fifo.prepareToWrite (count, start1, size1, start2, size2);
    if (size1 + size2 < count)
        c.overflowed.store (true);
    auto copy = [&] (int to, int size, int from)
    {
        for (int ch = 0; ch < 2; ++ch)
            if (size > 0)
                c.buffer.copyFrom (ch, to, source, juce::jmin (ch, channels - 1), fc.bufferStartSample + from, size);
    };
    if (channels > 0)
    {
        copy (start1, size1, 0);
        copy (start2, size2, size1);
    }

    // The Master's fader, which plays after this plugin.
    if (fader != nullptr && fader->isEnabled())
    {
        float left = 1.0f, right = 1.0f;
        te::getGainsFromVolumeFaderPositionAndPan (fader->getSliderPos(), fader->getPan(), fader->getPanLaw(), left,
                                                   right);
        gainLeft.setTargetValue (left);
        gainRight.setTargetValue (right);
        for (auto [at, size] : { std::pair { start1, size1 }, std::pair { start2, size2 } })
            if (size > 0)
            {
                gainLeft.applyGain (c.buffer.getWritePointer (0, at), size);
                gainRight.applyGain (c.buffer.getWritePointer (1, at), size);
            }
    }
    c.fifo.finishedWrite (size1 + size2);
    c.captured.fetch_add (count);
}

} // namespace spinus::engine
