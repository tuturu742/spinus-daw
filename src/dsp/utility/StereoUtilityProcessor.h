#pragma once

#include "StereoUtilityEngine.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

/** Stereo Utility: gain, pan, stereo width, mono and polarity per side. Every control
    is an automatable parameter. Also the standalone "Spinus Stereo Utility" plugin. */
class StereoUtilityProcessor final : public juce::AudioPluginInstance
{
public:
    static constexpr const char* pluginName = "Spinus Stereo Utility";
    static constexpr const char* identifier = "spinus:stereo-utility";

    StereoUtilityProcessor();

    juce::AudioProcessorValueTreeState& getParameterState() noexcept { return parameters; }
    StereoUtilitySettings getSettings() const noexcept;
    const StereoUtilityEngine& getEngine() const noexcept { return engine; }

    void fillInPluginDescription (juce::PluginDescription&) const override;
    const juce::String getName() const override { return pluginName; }
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioPluginInstance::processBlock;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    juce::AudioProcessorValueTreeState parameters;
    StereoUtilityEngine engine;
    std::atomic<float>*gain = nullptr, *pan = nullptr, *width = nullptr, *mono = nullptr, *invertLeft = nullptr,
    *invertRight = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StereoUtilityProcessor)
};

} // namespace spinus::dsp
