#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include <atomic>

namespace spinus::engine
{

/** The Master's mute button: sits after the Master's effects (and before Tracktion's Master fader, which plays
    after the Master's plugin list) and silences everything when muted, fading over a few milliseconds so a
    mute does not click. Unmuted, it leaves the sound untouched. Inserts are muted as Tracktion tracks; the Master
    is no track, so it has this instead. An automated Master fader does not undo the mute.

    Real-time safe: the mute is an atomic; no allocation, no locks. */
class MutePlugin final : public tracktion::Plugin
{
public:
    explicit MutePlugin (tracktion::PluginCreationInfo);
    ~MutePlugin() override;

    static const char* xmlTypeName;
    static const char* getPluginName() { return "Mute"; }
    static juce::ValueTree create();

    /** Any thread. */
    void setMuted (bool shouldBeMuted) noexcept { muted.store (shouldBeMuted, std::memory_order_relaxed); }
    bool isMuted() const noexcept { return muted.load (std::memory_order_relaxed); }

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
    std::atomic<bool> muted { false };
    juce::SmoothedValue<float> gain { 1.0f };
};

} // namespace spinus::engine
