#pragma once

#include "core/FileDialog.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

class SamplerProcessor;
class ParameterKnob;

/** The Sampler's window: the zones (samples) with their waveform, start/end and loop markers, keys,
    velocities and root note; and the knobs. Drop audio files on it to load them. */
class SamplerEditor final : public juce::AudioProcessorEditor,
                            public juce::FileDragAndDropTarget,
                            private juce::ChangeListener
{
public:
    explicit SamplerEditor (SamplerProcessor&);
    ~SamplerEditor() override;

    int getSelectedZone() const noexcept { return selectedZone; }
    void selectZone (int);

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    class Waveform;

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void refresh();
    void chooseFiles (bool replace);

    SamplerProcessor& sampler;
    int selectedZone = 0;

    juce::ComboBox zones;
    juce::TextButton load { "Load..." }, add { "Add..." }, remove { "Remove" };
    juce::ToggleButton loop { "Loop" };
    std::unique_ptr<Waveform> waveform;
    juce::Slider root, lowKey, highKey, lowVelocity, highVelocity;
    juce::Label rootLabel, keysLabel, velocityLabel, message;
    std::vector<std::unique_ptr<ParameterKnob>> knobs;
    juce::ComboBox filter;
    juce::ToggleButton oneShot { "One shot" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> filterAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> oneShotAttachment;
    std::unique_ptr<core::FileDialog> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerEditor)
};

} // namespace spinus::dsp
