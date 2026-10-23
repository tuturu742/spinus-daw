#pragma once

#include "engine/PipeWireInput.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace spinus::ui
{

/** Settings → Audio, in plain words: the sound system (JACK or ALSA), what plays the sound,
    which outputs are the speakers, what records (a device and one input channel, with a level meter),
    sample rate and buffer size, plus what they mean in practice: total output latency and the number of
    dropouts since the device opened.

    Spinus plays through one stereo pair of outputs and records from one input channel: only those are
    switched on, so a PipeWire device with 64 channels shows as two choices, not 64 tick boxes. */
class AudioSettings final : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    /** With a PipeWireInput, "Record from" lists PipeWire's microphones by name. */
    explicit AudioSettings (juce::AudioDeviceManager&, PipeWireInput* = nullptr);
    ~AudioSettings() override;

    /** Opens the settings in a dialog window. */
    static void show (juce::AudioDeviceManager&, juce::Component* centreAround, PipeWireInput* = nullptr,
                      std::function<int()> getDefaultInput = {}, std::function<void (int)> onDefaultInputChosen = {});

    /** The input recorded when no audio track is armed (from 0), and telling when the user chooses one:
        armed tracks switch other inputs on too, so the inputs in use don't say which it is. */
    std::function<int()> getDefaultInput;
    std::function<void (int)> onDefaultInputChosen;

    /** ALSA's own plugins (rate converters, up/down mixers, OSS and JACK bridges) that are no use to choose
        directly; hidden from the lists. */
    static bool isClutter (const juce::String& deviceName);

    juce::String getStatusText() const { return status.getText(); }

    // What the choices offer and choose (tests).
    juce::StringArray getSpeakerChoices() const;
    juce::StringArray getInputChoices() const;
    bool chooseSpeakers (int pairIndex);
    /** -1: no input. */
    bool chooseInputChannel (int channel);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void refresh();
    juce::Result apply (const std::function<void (juce::AudioDeviceManager::AudioDeviceSetup&)>& change);

    juce::AudioDeviceManager& manager;
    juce::ComboBox typeBox, outputBox, speakersBox, inputBox, channelBox, rateBox, bufferBox;
    juce::Label typeLabel, outputLabel, speakersLabel, inputLabel, channelLabel, rateLabel, bufferLabel;
    juce::TextButton testButton { "Test" };
    juce::Label status, advice, problem;
    juce::AudioDeviceManager::LevelMeter::Ptr inputLevel;
    float shownLevel = 0.0f;
    juce::Rectangle<int> meterArea;
    bool refreshing = false;
    juce::StringArray outputNames, inputNames; // the devices' real names, in the boxes' order
    PipeWireInput* pipeWire;
    int pipeWireDevice = -1;                    // index into inputNames of the device PipeWire records through
    std::vector<PipeWireInput::Source> sources; // PipeWire's microphones, from item ID firstSourceId
};

} // namespace spinus::ui
