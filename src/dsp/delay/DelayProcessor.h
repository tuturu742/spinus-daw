#pragma once

#include "DelayEngine.h"
#include "dsp/OwnEffect.h"

namespace spinus::dsp
{

/** Spinus Delay: echoes at a note length locked to the song's tempo (1/16, 1/8 dotted, 1/4 triplet...) or a
    time in milliseconds, with feedback, a low cut and a high cut on the echoes, stereo, ping-pong or mono,
    width and mix. Every control is an automatable parameter. Also the standalone "Spinus Delay" plugin. */
class DelayProcessor final : public OwnEffect
{
public:
    static constexpr const char* pluginName = "Spinus Delay";
    static constexpr const char* identifier = "spinus:delay";

    /** The synced times, in quarter notes, and their names ("1/8 dotted"). */
    static constexpr std::array<double, 14> noteBeats { 0.125,     1.0 / 6.0, 0.25, 0.375,     1.0 / 3.0, 0.5, 0.75,
                                                        2.0 / 3.0, 1.0,       1.5,  4.0 / 3.0, 2.0,       3.0, 4.0 };
    static juce::StringArray getNoteNames();
    static constexpr int defaultNote = 6; // 1/8 dotted

    /** A time as the Time knob shows it: "8.50 ms", "350 ms", "1.50 s". */
    static juce::String timeText (double ms);

    DelayProcessor();

    DelaySettings getSettings() const noexcept;
    const DelayEngine& getEngine() const noexcept { return engine; }

    void fillInPluginDescription (juce::PluginDescription&) const override;
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioPluginInstance::processBlock;
    /** Until the echoes have fallen 60 dB, at the settings now (the longest time when synced). */
    double getTailLengthSeconds() const override;
    juce::AudioProcessorEditor* createEditor() override;

private:
    DelayEngine engine;
    std::atomic<float>*sync = nullptr, *note = nullptr, *time = nullptr, *feedback = nullptr, *mode = nullptr,
    *lowCut = nullptr, *highCut = nullptr, *width = nullptr, *mix = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayProcessor)
};

} // namespace spinus::dsp
