#pragma once

#include "core/FileDialog.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

class DrumMachineProcessor;
class ParameterKnob;

/** The Drum Machine's window: 16 pads (click to hear and select, drop a file on one to load it) and the
    selected pad's settings; the Kit menu loads the Synth Kit or saves and loads .spinuskit files. */
class DrumMachineEditor final : public juce::AudioProcessorEditor, private juce::ChangeListener
{
public:
    explicit DrumMachineEditor (DrumMachineProcessor&);
    ~DrumMachineEditor() override;

    int getSelectedPad() const noexcept { return selectedPad; }
    void selectPad (int);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class PadButton;

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void refresh();
    void attachToPad();
    void showKitMenu();
    void choosePadFile();

    DrumMachineProcessor& drums;
    int selectedPad = 0;

    std::vector<std::unique_ptr<PadButton>> padButtons;
    juce::TextButton kitButton { "Kit" }, loadButton { "Load sample..." }, clearButton { "Clear pad" };
    juce::Label padTitle, message;
    std::vector<std::unique_ptr<ParameterKnob>> knobs;
    juce::StringArray knobIds;
    juce::ComboBox filter, choke;
    juce::Slider note;
    juce::Label noteLabel;
    juce::ToggleButton oneShot { "One shot" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> filterAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> oneShotAttachment;
    std::unique_ptr<core::FileDialog> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumMachineEditor)
};

} // namespace spinus::dsp
