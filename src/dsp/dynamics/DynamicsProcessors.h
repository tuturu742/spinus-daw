#pragma once

#include "CompressorEngine.h"
#include "GateEngine.h"
#include "LimiterEngine.h"
#include "dsp/OwnEffect.h"

namespace spinus::dsp
{

/** Spinus Compressor: threshold, ratio, knee, attack, release, makeup (or automatic
    makeup), a dry/wet mix for parallel compression, peak or RMS detection, and a sidechain input that the
    Mixer's "Sidechain from" feeds. Also the standalone "Spinus Compressor" plugin. */
class CompressorProcessor final : public OwnEffect
{
public:
    static constexpr const char* pluginName = "Spinus Compressor";
    static constexpr const char* identifier = "spinus:compressor";

    CompressorProcessor();

    CompressorSettings getSettings() const noexcept;
    const CompressorEngine& getEngine() const noexcept { return engine; }

    void fillInPluginDescription (juce::PluginDescription&) const override;
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioPluginInstance::processBlock;
    double getTailLengthSeconds() const override { return 0.0; }
    juce::AudioProcessorEditor* createEditor() override;

private:
    CompressorEngine engine;
    std::atomic<float>*threshold = nullptr, *ratio = nullptr, *attack = nullptr, *release = nullptr, *knee = nullptr,
    *makeup = nullptr, *autoMakeup = nullptr, *mix = nullptr, *detector = nullptr, *sidechain = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompressorProcessor)
};

/** Spinus Limiter: input gain, ceiling, release, look-ahead (reported as the plugin's
    latency, so the Mixer's delay compensation keeps it in time) and true-peak detection. The output never
    goes above the ceiling. Also the standalone "Spinus Limiter" plugin. */
class LimiterProcessor final : public OwnEffect, private juce::Timer
{
public:
    static constexpr const char* pluginName = "Spinus Limiter";
    static constexpr const char* identifier = "spinus:limiter";

    LimiterProcessor();
    ~LimiterProcessor() override;

    LimiterSettings getSettings() const noexcept;
    float getLookaheadMs() const noexcept { return lookahead->load(); }
    const LimiterEngine& getEngine() const noexcept { return engine; }

    /** Takes a new look-ahead: the engine's delay and the reported latency (on the message thread; a timer
        does it when the parameter changes). */
    void updateLatency();

    void fillInPluginDescription (juce::PluginDescription&) const override;
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioPluginInstance::processBlock;
    double getTailLengthSeconds() const override { return 0.0; }
    juce::AudioProcessorEditor* createEditor() override;

private:
    void timerCallback() override { updateLatency(); }

    LimiterEngine engine;
    double preparedRate = 0.0;
    std::atomic<float>*input = nullptr, *ceiling = nullptr, *release = nullptr, *lookahead = nullptr,
    *truePeak = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterProcessor)
};

/** Spinus Gate: threshold with hysteresis, range, attack, hold and release, and a
    sidechain input that the Mixer's "Sidechain from" feeds. Also the standalone "Spinus Gate" plugin. */
class GateProcessor final : public OwnEffect
{
public:
    static constexpr const char* pluginName = "Spinus Gate";
    static constexpr const char* identifier = "spinus:gate";

    GateProcessor();

    GateSettings getSettings() const noexcept;
    const GateEngine& getEngine() const noexcept { return engine; }

    void fillInPluginDescription (juce::PluginDescription&) const override;
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioPluginInstance::processBlock;
    double getTailLengthSeconds() const override { return 0.0; }
    juce::AudioProcessorEditor* createEditor() override;

private:
    GateEngine engine;
    std::atomic<float>*threshold = nullptr, *hysteresis = nullptr, *range = nullptr, *attack = nullptr, *hold = nullptr,
    *release = nullptr, *sidechain = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GateProcessor)
};

} // namespace spinus::dsp
