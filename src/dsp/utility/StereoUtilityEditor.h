#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

class StereoUtilityProcessor;
class ParameterKnob;

/** The Stereo Utility's window: Gain, Pan and Width knobs, Mono and polarity switches, and meters for
    the left and right levels and how alike the two sides are. */
class StereoUtilityEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit StereoUtilityEditor (StereoUtilityProcessor&);
    ~StereoUtilityEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    StereoUtilityProcessor& processor;
    std::unique_ptr<ParameterKnob> gain, pan, width;
    juce::ToggleButton mono { "Mono" }, invertLeft { "Invert left" }, invertRight { "Invert right" };
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> buttonAttachments;
    juce::Rectangle<int> meters;
    std::array<float, 2> shownPeaks {};
    float shownCorrelation = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StereoUtilityEditor)
};

} // namespace spinus::dsp
