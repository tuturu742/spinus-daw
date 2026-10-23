#include "StemCapturePlugin.h"

namespace spinus::engine
{

namespace te = tracktion;

namespace
{
/** Writes silence through a small buffer. */
bool writeZeros (juce::AudioFormatWriter& writer, juce::AudioBuffer<float>& scratch, juce::int64 count)
{
    scratch.clear();
    while (count > 0)
    {
        const auto n = (int)juce::jmin (count, (juce::int64)scratch.getNumSamples());
        if (! writer.writeFromAudioSampleBuffer (scratch, 0, n))
            return false;
        count -= n;
    }
    return true;
}
} // namespace

bool StemCapture::writeSilence (juce::int64 count)
{
    if (writer == nullptr || count <= 0)
        return true;
    juce::AudioBuffer<float> zeros (2, 4096);
    if (! writeZeros (*writer, zeros, count))
        return false;
    written += count;
    return true;
}

bool StemCapture::finish()
{
    if (writer == nullptr)
        return ! failed;
    const bool ok = writeSilence (lengthSamples - written) && ! failed;
    writer.reset(); // closes the file
    return ok;
}

//==============================================================================
const char* StemCapturePlugin::xmlTypeName = "spinusStemCapture";

StemCapturePlugin::StemCapturePlugin (te::PluginCreationInfo info) : te::Plugin (std::move (info)) {}

StemCapturePlugin::~StemCapturePlugin() { notifyListenersOfDeletion(); }

juce::ValueTree StemCapturePlugin::create()
{
    juce::ValueTree v (te::IDs::PLUGIN);
    v.setProperty (te::IDs::type, xmlTypeName, nullptr);
    return v;
}

void StemCapturePlugin::initialise (const te::PluginInitialisationInfo& info)
{
    scratch.setSize (2, juce::jmax (512, info.blockSizeSamples));
    if (capture != nullptr)
        for (auto& d : capture->ditherers)
            d.reset (capture->writer != nullptr ? (int)capture->writer->getBitsPerSample() : 24);
}

void StemCapturePlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (! fc.isRendering || ! fc.isPlaying || capture == nullptr || fc.destBuffer == nullptr ||
        fc.bufferNumSamples <= 0)
        return;

    auto& c = *capture;
    if (c.silent || c.failed || c.writer == nullptr || c.written >= c.lengthSamples)
        return;

    // The sample of the stem this block starts at: Tracktion gives the time minus the latency before this plugin.
    const auto first = (juce::int64)std::llround ((fc.editTime.getStart() - c.start).inSeconds() * c.sampleRate);
    const auto end = juce::jmin (first + fc.bufferNumSamples, c.lengthSamples);

    if (first > c.written) // a block that did not play (the track was not processed): silence there
    {
        const auto gap = juce::jmin (first, c.lengthSamples) - c.written;
        if (! writeZeros (*c.writer, scratch, gap))
            c.failed = true;
        c.written += gap;
    }

    const auto from = juce::jmax (c.written, first);
    if (end <= from || c.failed)
        return;

    const auto& source = *fc.destBuffer;
    const auto channels = source.getNumChannels();
    auto offset = (int)(from - first);
    auto remaining = (int)(end - from);

    while (remaining > 0)
    {
        const auto n = juce::jmin (remaining, scratch.getNumSamples());
        for (int ch = 0; ch < 2; ++ch)
        {
            if (channels > 0)
                scratch.copyFrom (ch, 0, source, juce::jmin (ch, channels - 1), fc.bufferStartSample + offset, n);
            else
                scratch.clear (ch, 0, n);
            if (c.dither)
                c.ditherers[ch].process (scratch.getWritePointer (ch), n);
        }
        if (! c.writer->writeFromAudioSampleBuffer (scratch, 0, n))
        {
            c.failed = true;
            return;
        }
        c.written += n;
        offset += n;
        remaining -= n;
    }
}

} // namespace spinus::engine
