#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include <atomic>
#include <memory>

namespace spinus::engine
{

/** Where an OutputCapturePlugin puts the sound: a lock-free stereo FIFO the writer thread drains, and how many
    samples are wanted. Made on the message thread, before the plugin is placed. */
struct OutputCapture
{
    explicit OutputCapture (int capacitySamples) : fifo (capacitySamples), buffer (2, capacitySamples) {}

    juce::AbstractFifo fifo;
    juce::AudioBuffer<float> buffer;
    std::atomic<juce::int64> wanted { 0 }, captured { 0 };
    std::atomic<bool> overflowed { false }; // the writer fell behind: samples were lost
    std::atomic<double> sampleRate { 0.0 };
};

/** Copies the sound passing through into an OutputCapture: from the first block that plays from the song's
    start, until `wanted`
    samples are in, including after the transport stops (reverb tails). Sits last on the Master's plugin list
    while a song is exported in real time. The sound passes unchanged.

    Real-time safe: no allocation, no locks; the FIFO is lock-free. */
class OutputCapturePlugin final : public tracktion::Plugin
{
public:
    explicit OutputCapturePlugin (tracktion::PluginCreationInfo);
    ~OutputCapturePlugin() override;

    static const char* xmlTypeName;
    static const char* getPluginName() { return "Output Capture"; }
    static juce::ValueTree create();

    /** Set before the plugin is placed in a list. Tracktion applies the Master's fader after the Master's plugin
        list, so the capture applies it too (its gain and pan, as the fader does). */
    void setCapture (std::shared_ptr<OutputCapture> c, tracktion::VolumeAndPanPlugin::Ptr masterFader) noexcept
    {
        capture = std::move (c);
        fader = std::move (masterFader);
    }

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
    std::shared_ptr<OutputCapture> capture;
    tracktion::VolumeAndPanPlugin::Ptr fader;
    juce::SmoothedValue<float> gainLeft { 1.0f }, gainRight { 1.0f };
    bool started = false;
};

} // namespace spinus::engine
