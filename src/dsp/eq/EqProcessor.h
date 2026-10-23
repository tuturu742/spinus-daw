#pragma once

#include "EqEngine.h"
#include "dsp/OwnEffect.h"

namespace spinus::dsp
{

/** Spinus EQ: eight bands (bell, low and high shelf, low and high cut, notch) with
    frequency, gain, Q and on/off, and an output gain; its window draws the curve over a live spectrum
    and the bands can be dragged on it. Every control is an automatable parameter. Also the standalone
    "Spinus EQ" plugin. */
class EqProcessor final : public OwnEffect
{
public:
    static constexpr const char* pluginName = "Spinus EQ";
    static constexpr const char* identifier = "spinus:eq";

    EqProcessor();

    /** A band's parameter ID (band from 0): bandParameter (0, "freq") is "band1_freq". The parts are
        on, type, freq, gain, q and slope. */
    static juce::String bandParameter (int band, const char* part);
    static juce::StringArray getTypeNames();

    EqSettings getSettings() const noexcept;
    EqEngine& getEngine() noexcept { return engine; }

    void fillInPluginDescription (juce::PluginDescription&) const override;
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioPluginInstance::processBlock;
    double getTailLengthSeconds() const override { return 0.1; }
    juce::AudioProcessorEditor* createEditor() override;

private:
    struct BandParameters
    {
        std::atomic<float>*on = nullptr, *type = nullptr, *frequency = nullptr, *gain = nullptr, *q = nullptr,
        *slope = nullptr;
    };

    EqEngine engine;
    std::array<BandParameters, EqSettings::numBands> bandParameters;
    std::atomic<float>* output = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqProcessor)
};

} // namespace spinus::dsp
