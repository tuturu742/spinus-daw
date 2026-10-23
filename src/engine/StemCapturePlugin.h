#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include <memory>

namespace spinus::engine
{

/** Where a StemCapturePlugin writes one insert's stem: an open audio file writer and the stretch of the song it
    covers. Made on the message thread before the render; the render thread writes; read again after the render. */
struct StemCapture
{
    std::unique_ptr<juce::AudioFormatWriter> writer;
    tracktion::TimePosition start; // the song time of the stem's first sample
    juce::int64 lengthSamples = 0; // how long the stem is
    double sampleRate = 48000.0;
    bool dither = false; // for 16-bit files, as the mix is
    bool silent = false; // a muted insert: its stem is silence

    // The render thread's.
    juce::int64 written = 0;
    bool failed = false;
    tracktion::Ditherer ditherers[2];

    /** Writes `count` samples of silence (message thread, after the render: the rest of a stem). */
    bool writeSilence (juce::int64 count);
    /** Fills the stem to its length and closes the file; false if anything failed to write. */
    bool finish();
};

/** Captures one insert's stem during the export's single render of the song (ExportJob): it sits last
    on the insert's plugin list, so it sees what the insert puts out (after its effects and fader, which is what
    the insert sends on to the master or a bus; its sends are copies and leave the sound as it is). Each block is
    written at the song time its sound belongs to: Tracktion gives a plugin the block's time minus the latency
    before it, so the stem lines up with the mix even when plugins before it have latency.

    Only in offline renders (`isRendering`): it writes to its file on the render thread, as Tracktion's Renderer
    writes the mix there. It never runs on the audio device's thread: it is only placed in the export's own Edit.
    The sound passes unchanged. */
class StemCapturePlugin final : public tracktion::Plugin
{
public:
    explicit StemCapturePlugin (tracktion::PluginCreationInfo);
    ~StemCapturePlugin() override;

    static const char* xmlTypeName;
    static const char* getPluginName() { return "Stem Capture"; }
    static juce::ValueTree create();

    /** Set before the render starts. */
    void setCapture (std::shared_ptr<StemCapture> c) noexcept { capture = std::move (c); }

    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    bool canBeDisabled() override { return false; }
    int getNumOutputChannelsGivenInputs (int numInputChannels) override { return numInputChannels; }
    BusLayout getBusses() const override { return BusLayout::singlePassThrough(); }
    bool producesAudioWhenNoAudioInput() override { return false; }

    void initialise (const tracktion::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void applyToBuffer (const tracktion::PluginRenderContext&) override;
    void restorePluginStateFromValueTree (const juce::ValueTree&) override {}

private:
    std::shared_ptr<StemCapture> capture;
    juce::AudioBuffer<float> scratch;
};

} // namespace spinus::engine
