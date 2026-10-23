#pragma once

#include "core/FileDialog.h"
#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

class BeatSlicerProcessor;
class ParameterKnob;

/** The Beat Slicer's window: the loop's waveform with its slices and the key each plays from (click
    one to hear it; drag a marker to move it; double-click to add one; right-click a marker to remove
    it; right-click inside a slice to choose its key), how to slice, the loop's length,
    "Slices to pattern" (in Spinus), and the knobs. Drop an audio file on it to load a loop. */
class BeatSlicerEditor final : public juce::AudioProcessorEditor,
                               public juce::FileDragAndDropTarget,
                               private juce::ChangeListener
{
public:
    explicit BeatSlicerEditor (BeatSlicerProcessor&);
    ~BeatSlicerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

    /** The line under the waveform (tests). */
    juce::String getMessage() const { return message.getText(); }

private:
    class Waveform;

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void refresh();
    void reslice();
    void chooseFile();

    BeatSlicerProcessor& slicer;
    std::unique_ptr<Waveform> waveform;
    juce::TextButton load { "Load loop..." }, toPattern { "Slices to pattern" };
    juce::ComboBox method, grid, length;
    juce::Slider sensitivity;
    juce::Label methodLabel, lengthLabel, message;
    juce::ToggleButton playToEnd { "Play to slice end" };
    std::vector<std::unique_ptr<ParameterKnob>> knobs;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> playToEndAttachment;
    std::unique_ptr<core::FileDialog> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatSlicerEditor)
};

} // namespace spinus::dsp
