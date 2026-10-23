#pragma once

#include "ThreeOscEngine.h"
#include "ThreeOscPresets.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

/** 3x Osc: a quick, simple synth like FL's 3xOsc. Three oscillators (sine, triangle, saw,
    square, noise) with tuning, level, pan and start phase; an amp envelope; a filter the envelope can
    move. Every control is an automatable parameter. Also the standalone "Spinus 3x Osc" plugin. */
class ThreeOscProcessor final : public juce::AudioPluginInstance
{
public:
    static constexpr const char* pluginName = "Spinus 3x Osc";
    static constexpr const char* identifier = "spinus:3x-osc";

    ThreeOscProcessor();

    juce::AudioProcessorValueTreeState& getParameterState() noexcept { return parameters; }
    ThreeOscSettings getSettings() const noexcept;
    int getNumActiveVoices() const noexcept { return activeVoices.load(); }

    /** "osc1_shape" and so on: the parameter ID of an oscillator's setting (osc 0..2). */
    static juce::String parameterId (const juce::String& setting, int oscillator);

    void fillInPluginDescription (juce::PluginDescription&) const override;
    const juce::String getName() const override { return pluginName; }
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioPluginInstance::processBlock;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    /** Every parameter, made once without a processor (any thread): to turn a setting into 0..1. */
    static const std::vector<std::unique_ptr<juce::RangedAudioParameter>>& getParameterList();
    static const juce::RangedAudioParameter* findInParameterList (const juce::String& parameterId);

    //==============================================================================
    // Presets: program 0 is "Default", then the factory presets (threeOscPresets::all()).

    /** Sets every control to the preset's settings (nullptr: the defaults), as a plugin's program does. In Spinus
        the window asks onChoosePreset instead, so that it is one undo step (device.loadPreset). */
    void applyPreset (const ThreeOscPreset*);
    /** The program whose settings the controls have now, or -1 when they match none (changed since). */
    int findCurrentProgram() const;
    /** Set by Spinus: the window's preset menu calls it with the preset's name ("Default" for the defaults)
        instead of applying the preset itself. Empty in other hosts. */
    std::function<void (const juce::String& presetName)> onChoosePreset;
    /** What the window's preset menu does: onChoosePreset if set, else applyPreset. */
    void choosePreset (int program);

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    juce::AudioProcessorValueTreeState parameters;
    ThreeOscEngine engine;
    std::atomic<int> activeVoices { 0 };
    int lastProgram = 0; // the program chosen last (getCurrentProgram while the controls match none)

    struct OscillatorParameters
    {
        std::atomic<float>*shape, *coarse, *fine, *level, *pan, *phase;
    };
    std::array<OscillatorParameters, 3> oscillatorParameters {};
    std::atomic<float>*attack = nullptr, *decay = nullptr, *sustain = nullptr, *release = nullptr, *filter = nullptr,
    *cutoff = nullptr, *resonance = nullptr, *envelopeAmount = nullptr, *gain = nullptr, *velocity = nullptr,
    *voices = nullptr, *glide = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThreeOscProcessor)
};

} // namespace spinus::dsp
