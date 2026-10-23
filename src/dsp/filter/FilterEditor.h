#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

class FilterProcessor;
class ParameterKnob;

/** The Filter's window: the response curve (moving with the LFO and the envelope), then the filter,
    output, LFO and envelope controls. */
class FilterEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit FilterEditor (FilterProcessor&);
    ~FilterEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    juce::Rectangle<int> getDisplayArea() const;

    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    FilterProcessor& processor;
    juce::ComboBox type, slope, lfoShape, lfoSync;
    std::vector<std::unique_ptr<ComboAttachment>> comboAttachments;
    std::unique_ptr<ParameterKnob> cutoff, resonance, mix, gain, lfoAmount, lfoRate, lfoLength, envelopeAmount,
        envelopeAttack, envelopeRelease;
    float shownCutoff = -1.0f;
    juce::Rectangle<int> filterTitle, outputTitle, lfoTitle, envelopeTitle;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilterEditor)
};

} // namespace spinus::dsp
