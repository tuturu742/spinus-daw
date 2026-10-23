#pragma once

#include "FilterEngine.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

/** Filter: low pass, high pass, band pass or notch, 12 or 24 dB per octave, with an LFO
    (free or locked to the song's tempo) and an envelope follower moving the cutoff. Every control is an
    automatable parameter. Also the standalone "Spinus Filter" plugin. */
class FilterProcessor final : public juce::AudioPluginInstance
{
public:
    static constexpr const char* pluginName = "Spinus Filter";
    static constexpr const char* identifier = "spinus:filter";

    /** The synced LFO lengths, in quarter notes, and their names. */
    static constexpr std::array<double, 7> lfoLengths { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0 };
    static juce::StringArray getLfoLengthNames();

    FilterProcessor();

    juce::AudioProcessorValueTreeState& getParameterState() noexcept { return parameters; }
    FilterSettings getSettings() const noexcept;
    float getModulatedCutoff() const noexcept { return engine.getModulatedCutoff(); }

    void fillInPluginDescription (juce::PluginDescription&) const override;
    const juce::String getName() const override { return pluginName; }
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioPluginInstance::processBlock;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    double getTailLengthSeconds() const override { return 0.1; }
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
    FilterEngine engine;

    std::atomic<float>*type = nullptr, *slope = nullptr, *cutoff = nullptr, *resonance = nullptr, *lfoAmount = nullptr,
    *lfoShape = nullptr, *lfoSync = nullptr, *lfoRate = nullptr, *lfoLength = nullptr, *envelopeAmount = nullptr,
    *envelopeAttack = nullptr, *envelopeRelease = nullptr, *mix = nullptr, *gain = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilterProcessor)
};

} // namespace spinus::dsp
