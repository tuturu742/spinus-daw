#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

class DuckerProcessor;
class ParameterKnob;

/** The Ducker's window: the dip's shape and the live volume, the big Amount knob, Release, Curve and
    what triggers it, and whether sound arrives on the sidechain. */
class DuckerEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit DuckerEditor (DuckerProcessor&);
    ~DuckerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** The status line under the curve (tests). */
    juce::String getStatusText() const;

private:
    void timerCallback() override;

    DuckerProcessor& processor;
    juce::ComboBox trigger;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> triggerAttachment;
    std::unique_ptr<ParameterKnob> amount, release, curve;
    juce::Rectangle<int> display, meter, statusArea;
    float shownGain = 1.0f;
    juce::uint32 lastSidechainSound = 0; // when sound last arrived on the sidechain (ms counter)
    juce::String status;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DuckerEditor)
};

} // namespace spinus::dsp
