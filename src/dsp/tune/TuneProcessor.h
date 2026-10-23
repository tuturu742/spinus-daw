#pragma once

#include "TuneEngine.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

/** Spinus Tune, real-time mode: pulls a voice to the notes of a key and scale, or to the
    MIDI notes it receives, with a retune speed from natural to "robot". The delay (10 ms, or 20 ms in the
    low range) is reported to the host, so playback stays in time. Every control except the range is an
    automatable parameter. Also the standalone "Spinus Tune" plugin. */
class TuneProcessor final : public juce::AudioPluginInstance, private juce::AudioProcessorValueTreeState::Listener
{
public:
    static constexpr const char* pluginName = "Spinus Tune";
    static constexpr const char* identifier = "spinus:tune";

    TuneProcessor();
    ~TuneProcessor() override;

    juce::AudioProcessorValueTreeState& getParameterState() noexcept { return parameters; }
    TuneSettings getSettings() const noexcept;
    const TuneEngine& getEngine() const noexcept { return engine; }

    void fillInPluginDescription (juce::PluginDescription&) const override;
    const juce::String getName() const override { return pluginName; }
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioPluginInstance::processBlock;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; } // target notes
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

    static const juce::StringArray& getKeyNames();
    static const juce::StringArray& getScaleNames();

private:
    void parameterChanged (const juce::String& id, float) override;
    void updateLatency();

    juce::AudioProcessorValueTreeState parameters;
    TuneEngine engine;
    std::atomic<float>*key = nullptr, *scale = nullptr, *retune = nullptr, *humanize = nullptr, *formants = nullptr,
    *range = nullptr, *mix = nullptr;
    double preparedRate = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TuneProcessor)
};

} // namespace spinus::dsp
