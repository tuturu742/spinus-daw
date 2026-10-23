#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include <atomic>
#include <memory>

namespace spinus::engine
{

/** What one device does to the sound, measured by the probe before it and the guard after it (written
    on the audio thread, read by the UI). Approximate by nature: a plugin's own meters are not readable. */
struct DeviceMeter
{
    // Probe -> guard, within one block.
    std::atomic<float> inputMeanSquare { 0.0f };
    std::atomic<juce::int64> startTicks { 0 };

    // Guard -> UI.
    std::atomic<float> gainReductionDb { 0.0f }; // <= 0: how far below its usual gain the device is now
    std::atomic<float> load { 0.0f };            // the device's share of the audio block's time, smoothed

    // Guard only (audio thread).
    float restingGainDb = 0.0f; // the device's gain when it is not reducing (follows the highest seen)
    bool hasResting = false;
};

/** Sits right after every instrument and effect. If the device produced NaN or
    infinity in a block, the block is replaced by silence and the guard records it, so one broken
    plugin cannot silence or damage the whole mix. The Engine Adapter reports tripped guards to the UI.

    Real-time safe: one pass over the block, no allocation, no locks; flags are atomics.
*/
class AudioGuardPlugin final : public tracktion::Plugin
{
public:
    explicit AudioGuardPlugin (tracktion::PluginCreationInfo);
    ~AudioGuardPlugin() override;

    static const char* xmlTypeName;
    static const char* getPluginName() { return "Audio Guard"; }
    static juce::ValueTree create();

    /** A probe sits right before the device and only measures what goes in; a guard (the default) sits
        right after it. Both of one device share its meter. Set before the plugin is placed in a list. */
    void setMeter (std::shared_ptr<DeviceMeter> m, bool isProbe) noexcept
    {
        meter = std::move (m);
        probe = isProbe;
    }
    const DeviceMeter* getMeter() const noexcept { return meter.get(); }

    /** Blocks silenced since the last call (message thread). */
    int takeSilencedBlockCount() noexcept { return silencedBlocks.exchange (0, std::memory_order_acq_rel); }

    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getPluginName(); }
    bool canBeDisabled() override { return false; }
    bool canBeAddedToFolderTrack() override { return true; }
    int getNumOutputChannelsGivenInputs (int numInputChannels) override { return numInputChannels; }
    BusLayout getBusses() const override { return BusLayout::singlePassThrough(); }
    bool takesMidiInput() override { return true; }
    bool producesAudioWhenNoAudioInput() override { return false; }

    void initialise (const tracktion::PluginInitialisationInfo& info) override { sampleRate = info.sampleRate; }
    void deinitialise() override {}
    void applyToBuffer (const tracktion::PluginRenderContext&) override;
    void restorePluginStateFromValueTree (const juce::ValueTree&) override {}

    /** True if any sample in the range is NaN or infinite. */
    static bool containsInvalidSamples (const juce::AudioBuffer<float>&, int start, int numSamples) noexcept;

    /** Mean of the squared samples over all channels (0 for an empty range). */
    static float meanSquare (const juce::AudioBuffer<float>&, int start, int numSamples) noexcept;

private:
    void measureAfter (const juce::AudioBuffer<float>&, int start, int numSamples) noexcept;

    std::atomic<int> silencedBlocks { 0 };
    std::shared_ptr<DeviceMeter> meter;
    bool probe = false;
    double sampleRate = 44100.0;
};

} // namespace spinus::engine
