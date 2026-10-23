#pragma once

#include "LoudnessEngine.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

class LoudnessMeterProcessor;

/** The Loudness Meter's window: short-term, integrated and momentary loudness, the true peak, the last
    minute as a graph against the target, and a plain-words verdict. */
class LoudnessMeterEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit LoudnessMeterEditor (LoudnessMeterProcessor&);
    ~LoudnessMeterEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** The verdict under the readings, e.g. "2.0 dB louder than the target: ..." (tests). */
    juce::String getVerdict() const;

private:
    void timerCallback() override;
    void drawReading (juce::Graphics&, juce::Rectangle<int>, const juce::String& title, float value,
                      const juce::String& unit, bool big, juce::Colour) const;

    LoudnessMeterProcessor& processor;
    juce::ComboBox target;
    juce::ToggleButton resetOnPlay { "Start again on Play" };
    juce::TextButton reset { "Reset" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> targetAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> resetOnPlayAttachment;

    juce::Rectangle<int> readings, graph, verdict;
    float momentary = LoudnessEngine::silence, shortTerm = LoudnessEngine::silence,
          integrated = LoudnessEngine::silence, truePeak = LoudnessEngine::silence;
    double seconds = 0.0;
    std::array<float, LoudnessEngine::historySteps> history {};
    int historyCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoudnessMeterEditor)
};

} // namespace spinus::dsp
