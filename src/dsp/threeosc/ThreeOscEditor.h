#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

class ThreeOscProcessor;
class ParameterKnob;

/** 3x Osc's window: its preset menu, a row per oscillator (shape, coarse, fine, level, pan, phase), then the envelope,
    the filter and the volume. */
class ThreeOscEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ThreeOscEditor (ThreeOscProcessor&);
    ~ThreeOscEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** The preset menu (tests): its items' IDs are the program + 1. */
    juce::ComboBox& getPresetMenu() noexcept { return presets; }
    /** Chooses the previous (-1) or next (+1) preset, as the menu's arrows do. */
    void stepPreset (int delta);

private:
    void timerCallback() override;
    void showCurrentPreset();

    ThreeOscProcessor& synth;
    juce::ComboBox presets;
    juce::TextButton previous { "<" }, next { ">" };

    struct Row
    {
        juce::ComboBox shape;
        std::vector<std::unique_ptr<ParameterKnob>> knobs;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> shapeAttachment;
    };

    std::array<Row, 3> rows;
    std::vector<std::unique_ptr<ParameterKnob>> envelope, filterKnobs, master;
    juce::ComboBox filter, voices;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> filterAttachment, voicesAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThreeOscEditor)
};

} // namespace spinus::dsp
