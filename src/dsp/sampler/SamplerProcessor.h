#pragma once

#include "SampleStorage.h"
#include "SamplerEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

/** The Spinus Sampler: plays samples, one-shot or across the keyboard. Spinus hosts it
    through its internal plugin format; the same code builds the standalone "Spinus Sampler" plugin.

    Knobs are parameters (automatable). Zones (samples and where they play) are edited on the message
    thread; each edit hands the audio thread a new SampleSet, and old sets are freed once the audio
    thread can no longer be using them.

    The saved state holds the samples' audio itself (FLAC-compressed), so a project sounds the same on
    any computer, from the project folder alone.
*/
class SamplerProcessor final : public juce::AudioPluginInstance, public juce::ChangeBroadcaster
{
public:
    static constexpr const char* pluginName = "Spinus Sampler";
    static constexpr const char* identifier = "spinus:sampler";
    static constexpr double maxSampleSeconds = 10.0 * 60.0;

    SamplerProcessor();
    ~SamplerProcessor() override;

    //==============================================================================
    // Zones (message thread). Every change sends a change message.

    /** Loads audio files as zones. One file becomes one zone over the whole keyboard. Several files:
        if their names contain notes ("Piano_C4.wav") each plays around its note; otherwise they are
        round-robin variations, played in turn. `replace`: the files replace all zones. */
    juce::Result loadFiles (const juce::Array<juce::File>&, bool replace = true);

    /** Adds a zone playing this audio (tests, and samples made in Spinus). */
    void addZone (juce::AudioBuffer<float> audio, double sampleRate, const juce::String& name, int rootNote = 60);

    std::vector<SampleZone> getZones() const;
    void setZones (std::vector<SampleZone>);
    void updateZone (int index, const std::function<void (SampleZone&)>&);
    void removeZone (int index);

    juce::AudioProcessorValueTreeState& getParameterState() noexcept { return parameters; }
    SamplerSettings getSettings() const noexcept;

    /** Voices playing now (approximate: read from the message thread). */
    int getNumActiveVoices() const noexcept { return activeVoices.load(); }

    static juce::AudioFormatManager& getFormats();

    //==============================================================================
    void fillInPluginDescription (juce::PluginDescription&) const override;

    const juce::String getName() const override { return pluginName; }
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioPluginInstance::processBlock;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    double getTailLengthSeconds() const override;
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
    void publish (std::vector<SampleZone>);

    juce::AudioProcessorValueTreeState parameters;
    SamplerEngine engine;
    SampleSetHolder sets;
    std::atomic<int> activeVoices { 0 };

    struct ParameterPointers
    {
        std::atomic<float>*gain, *attack, *decay, *sustain, *release, *filter, *cutoff, *resonance, *velocity, *coarse,
            *fine, *oneShot;
    } p {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerProcessor)
};

} // namespace spinus::dsp
