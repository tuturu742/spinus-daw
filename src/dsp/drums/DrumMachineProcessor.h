#pragma once

#include "../sampler/SampleStorage.h"
#include "../sampler/SamplerEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

/** The Spinus Drum Machine: 16 pads, each a sample with its own volume, pan, tuning,
    envelope, filter and choke group. Pad 1 plays at C5 (the note the Channel Rack's steps play), pad 2
    at C#5, and so on; each pad's note can be changed. New Drum Machines start with the Spinus Synth Kit.

    Kits (all pads and their settings, with the samples) save to and load from .spinuskit files. Like
    the Sampler, the saved state holds the samples themselves.
*/
class DrumMachineProcessor final : public juce::AudioPluginInstance, public juce::ChangeBroadcaster
{
public:
    static constexpr const char* pluginName = "Spinus Drum Machine";
    static constexpr const char* identifier = "spinus:drum-machine";
    static constexpr const char* kitExtension = ".spinuskit";
    static constexpr int numPads = 16;
    static constexpr int firstNote = 60; // C5

    struct Pad
    {
        juce::String name;
        std::shared_ptr<const juce::AudioBuffer<float>> audio;
        double sampleRate = 48000.0;
        int note = firstNote;
        int chokeGroup = 0; // 0: none, 1..8

        bool operator== (const Pad&) const = default;
    };

    DrumMachineProcessor();
    ~DrumMachineProcessor() override;

    //==============================================================================
    // Pads (message thread). Every change sends a change message.
    const std::array<Pad, numPads>& getPads() const noexcept { return pads; }
    juce::Result loadPadFile (int pad, const juce::File&);
    void setPadAudio (int pad, juce::AudioBuffer<float>, double sampleRate, const juce::String& name);
    void clearPad (int pad);
    void setPadNote (int pad, int note);
    void setPadChoke (int pad, int group);

    /** Replaces every pad with the Spinus Synth Kit and resets the pad settings. */
    void loadSynthKit();
    juce::Result saveKit (const juce::File&);
    juce::Result loadKit (const juce::File&);

    /** Plays or stops a pad from the window (clicking it). Real-time safe hand-over to the audio thread. */
    void audition (int pad, bool on);

    /** Parameter ID of a pad's setting, e.g. ("volume", 0) -> "pad1_volume". */
    static juce::String parameterId (const juce::String& setting, int pad);
    juce::AudioProcessorValueTreeState& getParameterState() noexcept { return parameters; }
    SamplerSettings getPadSettings (int pad) const noexcept;

    int getNumActiveVoices() const noexcept { return activeVoices.load(); }

    //==============================================================================
    void fillInPluginDescription (juce::PluginDescription&) const override;
    const juce::String getName() const override { return pluginName; }
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioPluginInstance::processBlock;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    double getTailLengthSeconds() const override { return 2.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    void publish();
    void resetPadParameters();

    struct PadParameters
    {
        std::atomic<float>*volume, *pan, *tune, *fine, *attack, *decay, *sustain, *release, *filter, *cutoff,
            *resonance, *velocity, *oneShot;
    };

    juce::AudioProcessorValueTreeState parameters;
    std::array<PadParameters, numPads> padParameters {};
    std::atomic<float>* masterVolume = nullptr;
    std::array<Pad, numPads> pads;

    SamplerEngine engine;
    SampleSetHolder sets;
    std::atomic<int> activeVoices { 0 };

    // Pad clicks from the window: (note, velocity; 0 = off), read by the audio thread.
    juce::AbstractFifo auditionFifo { 64 };
    std::array<std::pair<int, int>, 64> auditionEvents {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumMachineProcessor)
};

} // namespace spinus::dsp
