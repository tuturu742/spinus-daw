#include <catch2/catch_test_macros.hpp>

#include "session/OfflineInstrument.h"

#include <cmath>

using namespace spinus;

namespace
{
/** An instrument laid out like a drum player with an input and many outputs (a stereo input, a stereo main
    output and two more stereo outputs), or with one mono output. It plays a sine on its main output while a
    note is held, and notes whether a host changed its buses after it opened (a bridged plugin may crash on
    its first block then). */
class BusesTestInstrument final : public juce::AudioPluginInstance
{
public:
    explicit BusesTestInstrument (bool mono)
        : juce::AudioPluginInstance (mono ? BusesProperties().withOutput ("Out", juce::AudioChannelSet::mono(), true)
                                          : BusesProperties()
                                                .withInput ("In", juce::AudioChannelSet::stereo(), true)
                                                .withOutput ("Main", juce::AudioChannelSet::stereo(), true)
                                                .withOutput ("Kick", juce::AudioChannelSet::stereo(), true)
                                                .withOutput ("Snare", juce::AudioChannelSet::stereo(), true))
    {
        opened = getBusesLayout();
    }

    void fillInPluginDescription (juce::PluginDescription& d) const override { d.name = getName(); }
    const juce::String getName() const override { return "Buses test"; }
    void prepareToPlay (double, int) override { preparedAs = getBusesLayout(); }
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        ++blocks;
        changed = changed || getBusesLayout() != opened;
        for (const auto m : midi)
            if (m.getMessage().isNoteOn())
                held = true;
            else if (m.getMessage().isNoteOff() || m.getMessage().isAllNotesOff() || m.getMessage().isAllSoundOff())
                held = false;
        buffer.clear();
        if (! held)
            return;
        const auto main = getChannelCountOfBus (false, 0);
        for (int i = 0; i < buffer.getNumSamples(); ++i, phase += 0.05)
            for (int c = 0; c < main; ++c)
                buffer.setSample (c, i, 0.5f * (float)std::sin (phase));
    }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

protected:
    bool isBusesLayoutSupported (const BusesLayout&) const override { return true; }

public:
    BusesLayout opened, preparedAs;
    bool changed = false, held = false;
    int blocks = 0;
    double phase = 0.0;
};
} // namespace

TEST_CASE ("Offline instrument: plays an instrument with its own buses, an input and many outputs as it has them",
           "[session][sounds]")
{
    BusesTestInstrument plugin (false);
    {
        OfflineInstrument player (plugin);
        CHECK (plugin.preparedAs == plugin.opened); // the input is not switched off, the outputs stay
        const auto sound = player.renderStereo ({ { 60, 0.0, 0.3, 0.8f } }, 0.1);
        REQUIRE (sound.getNumChannels() == 2);
        CHECK (sound.getMagnitude (0, 0, sound.getNumSamples()) > 0.1f);
        CHECK (sound.getMagnitude (1, 0, sound.getNumSamples()) > 0.1f);
    }
    CHECK (plugin.blocks > 0);
    CHECK_FALSE (plugin.changed);
    CHECK (plugin.getChannelCountOfBus (true, 0) == 2);
    CHECK (plugin.getTotalNumOutputChannels() == 6);
}

TEST_CASE ("Offline instrument: a mono instrument sounds on both sides", "[session][sounds]")
{
    BusesTestInstrument plugin (true);
    OfflineInstrument player (plugin);
    const auto sound = player.renderStereo ({ { 60, 0.0, 0.3, 0.8f } }, 0.1);
    REQUIRE (sound.getNumChannels() == 2);
    CHECK (sound.getMagnitude (0, 0, sound.getNumSamples()) > 0.1f);
    CHECK (sound.getMagnitude (1, 0, sound.getNumSamples()) > 0.1f);
}
