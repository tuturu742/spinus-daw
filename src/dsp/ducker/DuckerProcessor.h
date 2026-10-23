#pragma once

#include "DuckerEngine.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

/** Sidechain Ducker: one knob for how far the sound dips each time the sidechain input
    is hit (or on every beat), plus how long and in what shape it comes back. Every control is an
    automatable parameter. Also the standalone "Spinus Ducker" plugin, with a sidechain input bus. */
class DuckerProcessor final : public juce::AudioPluginInstance
{
public:
    static constexpr const char* pluginName = "Spinus Ducker";
    static constexpr const char* identifier = "spinus:ducker";

    DuckerProcessor();

    juce::AudioProcessorValueTreeState& getParameterState() noexcept { return parameters; }
    DuckerSettings getSettings() const noexcept;
    const DuckerEngine& getEngine() const noexcept { return engine; }

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
    DuckerEngine engine;
    std::atomic<float>*trigger = nullptr, *amount = nullptr, *release = nullptr, *curve = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DuckerProcessor)
};

} // namespace spinus::dsp
