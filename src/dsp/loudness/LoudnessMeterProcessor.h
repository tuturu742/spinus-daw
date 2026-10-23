#pragma once

#include "LoudnessEngine.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

/** Loudness Meter: measures the loudness and true peak of the sound passing through,
    against a target (streaming, broadcast, club). The sound passes unchanged. Also the standalone
    "Spinus Loudness Meter" plugin. */
class LoudnessMeterProcessor final : public juce::AudioPluginInstance
{
public:
    static constexpr const char* pluginName = "Spinus Loudness Meter";
    static constexpr const char* identifier = "spinus:loudness-meter";

    struct Target
    {
        const char* name;
        float lufs;     // integrated loudness aimed for
        float peakDbTp; // highest true peak allowed
    };
    static const std::array<Target, 4>& getTargets();

    LoudnessMeterProcessor();

    juce::AudioProcessorValueTreeState& getParameterState() noexcept { return parameters; }
    const Target& getTarget() const noexcept;
    LoudnessEngine& getEngine() noexcept { return engine; }
    const LoudnessEngine& getEngine() const noexcept { return engine; }

    void fillInPluginDescription (juce::PluginDescription&) const override;
    const juce::String getName() const override { return pluginName; }
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
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
    LoudnessEngine engine;
    std::atomic<float>*target = nullptr, *resetOnPlay = nullptr;
    bool wasPlaying = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoudnessMeterProcessor)
};

} // namespace spinus::dsp
