#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include <cmath>
#include <vector>

/** A tiny instrument with programs, as a VST2, VST3 or LV2 plugin shows them to a host (closest preset for
    any instrument): one voice, a sine, saw or square through a simple low pass. Of its six
    programs, "Init" and "Default" are placeholders, so four count as presets. Its state is the program
    number. Made through Tracktion's plugin manager (install()), so a channel or a hidden instrument gets it
    as it would an installed plugin. */
namespace spinus::test
{
class ProgramsTestPlugin : public juce::AudioPluginInstance
{
public:
    static constexpr const char* name = "Spinus Test Programs";

    struct Program
    {
        const char* name;
        int wave;         // 0 sine, 1 saw, 2 square
        float brightness; // the low pass, 0 to 1
    };
    static const std::vector<Program>& getPrograms()
    {
        static const std::vector<Program> programs { { "Init", 0, 0.5f },       { "Warm Sine", 0, 0.3f },
                                                     { "Bright Saw", 1, 1.0f }, { "Hollow Square", 2, 0.6f },
                                                     { "Default", 0, 0.5f },    { "Dull Saw", 1, 0.08f } };
        return programs;
    }

    static juce::PluginDescription describe()
    {
        juce::PluginDescription d;
        d.name = name;
        d.descriptiveName = name;
        d.pluginFormatName = "VST3";
        d.manufacturerName = "Spinus tests";
        d.version = "1.0";
        d.fileOrIdentifier = "/nowhere/Spinus Test Programs.vst3";
        d.uniqueId = d.deprecatedUid = 0x5350746f;
        d.isInstrument = true;
        d.category = "Instrument";
        d.numInputChannels = 0;
        d.numOutputChannels = 2;
        return d;
    }

    /** Makes the engine create this plugin for its description, and lists it as installed there. */
    static void install (tracktion::Engine& engine)
    {
        auto& plugins = engine.getPluginManager();
        plugins.knownPluginList.addType (describe());
        plugins.createPluginInstance =
            [others = plugins.createPluginInstance] (const juce::PluginDescription& d, double rate, int block,
                                                     juce::String& error) -> std::unique_ptr<juce::AudioPluginInstance>
        {
            if (d.name == name)
                return std::make_unique<ProgramsTestPlugin>();
            return others ? others (d, rate, block, error) : nullptr;
        };
    }

    ProgramsTestPlugin()
        : juce::AudioPluginInstance (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    void fillInPluginDescription (juce::PluginDescription& d) const override { d = describe(); }
    const juce::String getName() const override { return name; }
    void prepareToPlay (double sampleRate, int) override { rate = sampleRate; }
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();
        const auto& program = getPrograms()[(size_t)current];
        int position = 0;
        const auto render = [&] (int until)
        {
            for (; position < until; ++position)
            {
                if (note < 0)
                    continue;
                phase += 440.0 * std::pow (2.0, (note - 69) / 12.0) / rate;
                phase -= std::floor (phase);
                const auto x = program.wave == 0   ? std::sin (juce::MathConstants<double>::twoPi * phase)
                               : program.wave == 1 ? 2.0 * phase - 1.0
                                                   : (phase < 0.5 ? 1.0 : -1.0);
                low += (0.02 + 0.98 * program.brightness * program.brightness) * (x - low);
                for (int c = 0; c < buffer.getNumChannels(); ++c)
                    buffer.setSample (c, position, (float)(0.3 * low));
            }
        };
        for (const auto event : midi)
        {
            render (juce::jmin (event.samplePosition, buffer.getNumSamples()));
            const auto message = event.getMessage();
            if (message.isNoteOn())
                note = message.getNoteNumber();
            else if ((message.isNoteOff() && message.getNoteNumber() == note) || message.isAllNotesOff() ||
                     message.isAllSoundOff())
                note = -1;
        }
        render (buffer.getNumSamples());
    }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return (int)getPrograms().size(); }
    int getCurrentProgram() override { return current; }
    void setCurrentProgram (int index) override
    {
        if (juce::isPositiveAndBelow (index, getNumPrograms()))
            current = index;
    }
    const juce::String getProgramName (int index) override
    {
        return juce::isPositiveAndBelow (index, getNumPrograms()) ? getPrograms()[(size_t)index].name : "";
    }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& data) override
    {
        data.reset();
        data.append (&current, sizeof (current));
    }
    void setStateInformation (const void* data, int size) override
    {
        if (size == (int)sizeof (current))
            setCurrentProgram (*static_cast<const int*> (data));
    }

private:
    double rate = 48000.0, phase = 0.0, low = 0.0;
    int note = -1, current = 0;
};
} // namespace spinus::test
