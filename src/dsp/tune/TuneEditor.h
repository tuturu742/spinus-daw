#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

class TuneProcessor;
class ParameterKnob;

/** Spinus Tune's window: the note being sung and the note it is pulled to, how far off it was, one
    octave of keys showing the scale, and the controls. */
class TuneEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit TuneEditor (TuneProcessor&);
    ~TuneEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** What the display says, e.g. "A4 +39 cents -> A4", or "No pitch" (tests). */
    juce::String getStatusText() const;

private:
    void timerCallback() override;

    TuneProcessor& processor;
    juce::ComboBox key, scale, formants, range;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> comboAttachments;
    std::unique_ptr<ParameterKnob> retune, humanize, mix;
    juce::Label keyLabel, scaleLabel, formantsLabel, rangeLabel;
    juce::Rectangle<int> display, keyboard;
    float shownPitch = 0.0f, shownCorrection = 0.0f;
    int shownTarget = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TuneEditor)
};

} // namespace spinus::dsp
