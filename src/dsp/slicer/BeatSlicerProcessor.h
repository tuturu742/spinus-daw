#pragma once

#include "Slicing.h"
#include "dsp/sampler/SampleStorage.h"
#include "dsp/sampler/SamplerEngine.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus::dsp
{

/** The Beat Slicer: chops a loop into slices, by its hits or a grid, and plays slice 1
    from C5, slice 2 from C#5 and so on, or from any key chosen for each slice (one slice a key). Spinus turns the
   slices into a pattern that plays the loop back ("Slices to pattern"), which can then be rearranged note by note.

    Like the Sampler, the loop's audio is kept in the saved state. Slicing is edited on the
    message thread; each edit hands the audio thread a new set of zones. */
class BeatSlicerProcessor final : public juce::AudioPluginInstance, public juce::ChangeBroadcaster
{
public:
    static constexpr const char* pluginName = "Spinus Beat Slicer";
    static constexpr const char* identifier = "spinus:beat-slicer";
    static constexpr int firstNote = 60; // C5
    static constexpr double maxLoopSeconds = 120.0;

    enum class Method
    {
        transients,
        grid
    };

    /** Lengths offered for a loop, in quarter notes, and for a grid slice. */
    static constexpr std::array<double, 8> loopLengths { 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0 };
    static constexpr std::array<double, 4> gridLengths { 1.0, 0.5, 0.25, 0.125 };
    static juce::String describeBeats (double beats);

    BeatSlicerProcessor();
    ~BeatSlicerProcessor() override;

    //==============================================================================
    // The loop and its slices (message thread). Every change sends a change message.

    juce::Result loadFile (const juce::File&);
    /** Uses this audio as the loop (tests, and audio made in Spinus). */
    void setLoop (juce::AudioBuffer<float>, double sampleRate, const juce::String& name);
    bool hasLoop() const noexcept { return loop.audio != nullptr; }
    const SampleZone& getLoop() const noexcept { return loop; }

    double getLoopBeats() const noexcept { return loopBeats; }
    void setLoopBeats (double);

    Method getMethod() const noexcept { return method; }
    float getSensitivity() const noexcept { return sensitivity; }
    double getGridBeats() const noexcept { return gridBeats; }
    /** Slices again with this method (replacing slices moved by hand). */
    void slice (Method, float sensitivity, double gridBeats);

    const std::vector<juce::int64>& getSlicePoints() const noexcept { return points; }
    void setSlicePoints (std::vector<juce::int64>);
    void addSlicePoint (juce::int64);
    void moveSlicePoint (int index, juce::int64 position);
    void removeSlicePoint (int index);

    /** The key (MIDI note) each slice plays from; one slice a key. Slicing again resets them to C5 up. */
    const std::vector<int>& getSliceKeys() const noexcept { return keys; }
    int getSliceKey (int slice) const;
    /** Moves a slice to a key; a slice already there takes this slice's old key (they swap). */
    void setSliceKey (int slice, int key);
    /** Slice 1 on C5, slice 2 on C#5 and so on. */
    void resetSliceKeys();
    /** "C5", "F#6": a key's name as Spinus shows it (C5 is MIDI note 60). */
    static juce::String keyName (int key);

    /** Each slice as a note: where it starts in the loop and how long it is, in quarter notes. */
    struct SliceNote
    {
        int note = firstNote;
        double startBeats = 0.0, lengthBeats = 0.0;
    };
    std::vector<SliceNote> getSliceNotes() const;

    /** Plays a slice now (clicked in the window). */
    void audition (int slice, bool on);

    /** Set by Spinus to offer "Slices to pattern" in the window; empty in other hosts. */
    std::function<void()> onSlicesToPattern;

    juce::AudioProcessorValueTreeState& getParameterState() noexcept { return parameters; }
    SamplerSettings getSettings() const noexcept;

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
    void publish();

    juce::AudioProcessorValueTreeState parameters;
    SamplerEngine engine;
    SampleSetHolder sets;

    SampleZone loop; // the whole loop; slices are zones over parts of its audio
    std::vector<juce::int64> points { 0 };
    std::vector<int> keys { firstNote }; // one per slice
    void defaultKeys();
    double loopBeats = 4.0, gridBeats = 0.25;
    Method method = Method::transients;
    float sensitivity = 0.5f;
    std::atomic<double> hostTempo { 120.0 };

    juce::AbstractFifo auditionFifo { 64 };
    std::array<std::pair<int, int>, 64> auditionEvents {};

    std::atomic<float>*gain = nullptr, *attack = nullptr, *release = nullptr, *coarse = nullptr, *fine = nullptr,
    *playToEnd = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BeatSlicerProcessor)
};

} // namespace spinus::dsp
