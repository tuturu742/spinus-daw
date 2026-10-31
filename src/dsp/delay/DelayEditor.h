#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

class DelayProcessor;
class ParameterKnob;

/** The Delay's window: the echoes as they will come (when, on which side, how loud), the time they are at
    ("1/8 dotted at 120 BPM: 375 ms"), and the controls. */
class DelayEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit DelayEditor (DelayProcessor&);
    ~DelayEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** "1/8 dotted at 120 BPM: 375 ms", or "350 ms" when free (tests). */
    juce::String getTimeText() const;

private:
    void timerCallback() override;
    juce::Rectangle<int> getDisplayArea() const;

    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    DelayProcessor& processor;
    juce::ComboBox sync, mode;
    std::vector<std::unique_ptr<ComboAttachment>> comboAttachments;
    std::unique_ptr<ParameterKnob> note, time, feedback, lowCut, highCut, width, mix;
    juce::String shownTime;
    juce::Rectangle<int> timeTitle, colourTitle, outputTitle;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayEditor)
};

} // namespace spinus::dsp
