#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "dsp/threeosc/ThreeOscEditor.h"
#include "dsp/threeosc/ThreeOscPresets.h"
#include "dsp/threeosc/ThreeOscProcessor.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "session/BuiltInPresets.h"
#include "session/InstrumentPresets.h"
#include "ui/ChannelRack.h"

#include <set>

using namespace spinus;
using namespace spinus::dsp;

namespace
{
constexpr double rate = 48000.0;
constexpr int block = 256;

struct Levels
{
    float peak = 0.0f, rms = 0.0f;
};

/** Plays the notes (velocity 100) for `held` seconds, then lets them go and renders `tail` more. */
Levels play (ThreeOscProcessor& synth, std::initializer_list<int> notes, double held, double tail)
{
    synth.prepareToPlay (rate, block);
    juce::AudioBuffer<float> buffer (2, block);
    juce::MidiBuffer midi;
    for (auto n : notes)
        midi.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8)100), 0);
    Levels levels;
    double sum = 0.0;
    int64_t count = 0;
    const auto heldBlocks = (int)(held * rate / block), allBlocks = (int)((held + tail) * rate / block);
    for (int b = 0; b < allBlocks; ++b)
    {
        if (b == heldBlocks)
            for (auto n : notes)
                midi.addEvent (juce::MidiMessage::noteOff (1, n), 0);
        synth.processBlock (buffer, midi);
        midi.clear();
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < block; ++i)
            {
                const auto v = buffer.getSample (c, i);
                levels.peak = juce::jmax (levels.peak, std::abs (v));
                sum += (double)v * v;
                ++count;
            }
    }
    levels.rms = (float)std::sqrt (sum / (double)juce::jmax ((int64_t)1, count));
    synth.releaseResources();
    return levels;
}

/** Where a preset is played: basses low, the rest around middle C. */
int noteFor (const ThreeOscPreset& preset) { return preset.category == "Bass" ? 36 : 60; }

std::unique_ptr<commands::Command> make (const juce::String& name, const juce::String& json)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (json), error);
    INFO (error);
    REQUIRE (command != nullptr);
    return command;
}

float valueOf (ThreeOscProcessor& synth, const juce::String& id)
{
    auto* parameter = synth.getParameterState().getParameter (id);
    REQUIRE (parameter != nullptr);
    return parameter->getValue();
}
} // namespace

TEST_CASE ("3x Osc has a bank of factory presets, every one setting real controls", "[dsp][3xosc][presets]")
{
    const auto& presets = threeOscPresets::all();
    CHECK (presets.size() >= 16);
    CHECK (presets.size() <= 24);
    std::set<juce::String> names, categories;
    for (auto& preset : presets)
    {
        INFO (preset.name);
        CHECK (names.insert (preset.name.toLowerCase()).second);
        CHECK (! InstrumentPresets::isPlaceholderName (preset.name)); // closest preset lists every one
        CHECK (preset.description.isNotEmpty());
        categories.insert (preset.category);
        for (auto& [id, value] : preset.settings)
        {
            INFO (id);
            const auto* parameter = ThreeOscProcessor::findInParameterList (id);
            REQUIRE (parameter != nullptr);
            // In range: not clamped when it is turned into 0..1.
            const auto range = parameter->getNormalisableRange();
            CHECK (value >= range.start);
            CHECK (value <= range.end);
        }
        CHECK (threeOscPresets::find (preset.name.toUpperCase()) == &preset);
        CHECK (threeOscPresets::values (&preset).size() == ThreeOscProcessor::getParameterList().size());
    }
    CHECK (categories == std::set<juce::String> { "Bass", "Lead", "Pad", "Keys", "FX" });
    CHECK (threeOscPresets::find ("Default") == nullptr);
    CHECK (threeOscPresets::find ("No such preset") == nullptr);

    // They are the plugin's programs, after "Default".
    ThreeOscProcessor synth;
    CHECK (synth.getNumPrograms() == (int)presets.size() + 1);
    CHECK (synth.getProgramName (0) == "Default");
    CHECK (synth.getProgramName (1) == presets.front().name);
    CHECK (synth.getCurrentProgram() == 0);
    CHECK (synth.findCurrentProgram() == 0); // a new 3x Osc has the defaults
}

TEST_CASE ("Each 3x Osc preset sets the controls to its settings; Default puts them back", "[dsp][3xosc][presets]")
{
    ThreeOscProcessor synth, fresh;
    const auto& presets = threeOscPresets::all();
    for (size_t i = 0; i < presets.size(); ++i)
    {
        const auto& preset = presets[i];
        INFO (preset.name);
        synth.setCurrentProgram ((int)i + 1);
        CHECK (synth.findCurrentProgram() == (int)i + 1);
        CHECK (synth.getCurrentProgram() == (int)i + 1);
        for (auto& [id, value] : preset.settings)
        {
            INFO (id);
            auto* parameter = synth.getParameterState().getParameter (id);
            REQUIRE (parameter != nullptr);
            CHECK (parameter->convertFrom0to1 (parameter->getValue()) == Catch::Approx (value).margin (0.01));
        }
        // Settings it leaves out are back at their defaults (from the preset before).
        for (auto* parameter : synth.getParameters())
            if (auto* withId = dynamic_cast<juce::RangedAudioParameter*> (parameter))
                if (std::none_of (preset.settings.begin(), preset.settings.end(),
                                  [withId] (auto& s) { return s.first == withId->getParameterID(); }))
                {
                    INFO (withId->getParameterID());
                    CHECK (withId->getValue() == Catch::Approx (withId->getDefaultValue()).margin (0.001));
                }
    }

    // A control turned: no preset matches; the menu shows the one chosen last as changed.
    synth.getParameterState().getParameter ("cutoff")->setValueNotifyingHost (0.123f);
    CHECK (synth.findCurrentProgram() == -1);
    CHECK (synth.getCurrentProgram() == (int)presets.size());

    synth.setCurrentProgram (0);
    CHECK (synth.findCurrentProgram() == 0);
    for (auto* parameter : fresh.getParameters())
        if (auto* withId = dynamic_cast<juce::RangedAudioParameter*> (parameter))
            CHECK (valueOf (synth, withId->getParameterID()) == Catch::Approx (withId->getValue()).margin (0.001));

    // Saved and restored like any setting.
    synth.setCurrentProgram (3);
    juce::MemoryBlock state;
    synth.getStateInformation (state);
    ThreeOscProcessor restored;
    restored.setStateInformation (state.getData(), (int)state.getSize());
    CHECK (restored.findCurrentProgram() == 3);
}

TEST_CASE ("Every 3x Osc preset plays a note: not silent, not clipping", "[dsp][3xosc][presets][audio]")
{
    for (auto& preset : threeOscPresets::all())
    {
        INFO (preset.name);
        ThreeOscProcessor synth;
        synth.applyPreset (&preset);
        // Held long enough for the slowest attack (Noise Riser's 4 s) to be well under way.
        const auto held = preset.name == "Noise Riser" ? 3.0 : 1.0;
        const auto one = play (synth, { noteFor (preset) }, held, 0.5);
        CHECK (std::isfinite (one.peak));
        CHECK (one.rms > 0.01f);
        CHECK (one.peak > 0.08f);
        CHECK (one.peak < 0.9f);
        // A chord too (headroom for a few notes), except for the mono presets, which play one.
        const auto root = noteFor (preset);
        const auto chord = play (synth, { root, root + 4, root + 7 }, held, 0.5);
        CHECK (chord.peak < 1.0f);
    }
}

TEST_CASE ("3x Osc presets print their levels", "[.levels][3xosc]")
{
    for (auto& preset : threeOscPresets::all())
    {
        ThreeOscProcessor synth;
        synth.applyPreset (&preset);
        const auto root = noteFor (preset);
        const auto one = play (synth, { root }, preset.name == "Noise Riser" ? 3.0 : 1.0, 0.5);
        const auto chord = play (synth, { root, root + 4, root + 7 }, 1.0, 0.5);
        WARN (preset.name << ": peak " << juce::Decibels::gainToDecibels (one.peak) << " dB, rms "
                          << juce::Decibels::gainToDecibels (one.rms) << " dB; chord peak "
                          << juce::Decibels::gainToDecibels (chord.peak) << " dB");
    }
}

TEST_CASE ("device.loadPreset loads a 3x Osc preset as one undo step", "[commands][3xosc][presets]")
{
    model::Project project;
    model::Channel channel;
    channel.id = project.allocateId<model::ChannelId>();
    channel.name = "Bass";
    channel.instrument =
        model::Device { project.allocateId<model::DeviceId>(), model::Device::Kind::stock, "spinus.3x-osc", {}, false };
    model::Channel other = channel;
    other.id = project.allocateId<model::ChannelId>();
    other.instrument->id = project.allocateId<model::DeviceId>();
    other.instrument->stockType = "spinus.sampler";
    project.channels = { channel, other };
    commands::CommandHistory history (project);
    const auto device = juce::String (channel.instrument->id.value);
    auto parametersNow = [&history] { return history.getProject().channels[0].instrument->parameters; };

    REQUIRE (history.perform (make ("device.loadPreset", R"({"device": )" + device + R"(, "preset": "acid bass"})"))
                 .wasOk());
    CHECK (history.getUndoDescriptions().size() == 1);
    CHECK (history.getUndoDescriptions()[0] == "Load preset 'Acid Bass' on '3x Osc'");
    CHECK (parametersNow() == threeOscPresets::values (threeOscPresets::find ("Acid Bass")));

    REQUIRE (
        history.perform (make ("device.loadPreset", R"({"device": )" + device + R"(, "preset": "Organ"})")).wasOk());
    CHECK (parametersNow() == threeOscPresets::values (threeOscPresets::find ("Organ")));
    REQUIRE (history.undo());
    CHECK (parametersNow() == threeOscPresets::values (threeOscPresets::find ("Acid Bass")));
    REQUIRE (history.undo());
    CHECK (parametersNow().empty());
    REQUIRE (history.redo());
    CHECK (parametersNow() == threeOscPresets::values (threeOscPresets::find ("Acid Bass")));

    REQUIRE (
        history.perform (make ("device.loadPreset", R"({"device": )" + device + R"(, "preset": "Default"})")).wasOk());
    CHECK (parametersNow() == threeOscPresets::values (nullptr));

    // Not a preset, not a 3x Osc, not a device: nothing changes.
    const auto before = history.getProject();
    const auto failed =
        history.perform (make ("device.loadPreset", R"({"device": )" + device + R"(, "preset": "Dubstep Wobble"})"));
    CHECK (failed.getErrorMessage().contains ("Reese Bass"));
    CHECK (history
               .perform (make ("device.loadPreset", R"({"device": )" + juce::String (other.instrument->id.value) +
                                                        R"(, "preset": "Organ"})"))
               .failed());
    CHECK (history.perform (make ("device.loadPreset", R"({"device": 999, "preset": "Organ"})")).failed());
    CHECK (history.getProject() == before);

    juce::String error;
    CHECK (commands::CommandRegistry::get().create ("device.loadPreset", juce::JSON::parse (R"({"device": 1})"),
                                                    error) == nullptr);
    CHECK (commands::CommandRegistry::get().create (
               "device.loadPreset", juce::JSON::parse (R"({"device": 1, "preset": " "})"), error) == nullptr);
}

TEST_CASE ("Choosing a 3x Osc preset in its window is one undo step and sets the sound", "[dsp][3xosc][presets][audio]")
{
    EngineHost host ({ .openAudioDevice = false });
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerSpinusDevices (*devices, host.getEngine());
    Session session (host.getEngine(), std::move (devices));
    ui::ChannelRack rack (session);
    REQUIRE (rack.addChannel ("spinus.3x-osc", "3x Osc"));
    const auto device = session.getProject().channels.back().instrument->id;
    auto* external = dynamic_cast<tracktion::ExternalPlugin*> (session.getAdapter().getDevicePlugin (device));
    REQUIRE (external != nullptr);
    auto* synth = dynamic_cast<ThreeOscProcessor*> (external->getAudioPluginInstance());
    REQUIRE (synth != nullptr);

    connectBuiltInPresets (session, *external);
    REQUIRE (synth->onChoosePreset != nullptr);
    std::unique_ptr<juce::AudioProcessorEditor> editor (synth->createEditorAndMakeActive());
    auto* window = dynamic_cast<ThreeOscEditor*> (editor.get());
    REQUIRE (window != nullptr);
    auto& menu = window->getPresetMenu();
    CHECK (menu.getNumItems() == synth->getNumPrograms());
    CHECK (menu.getText() == "Default");

    const auto steps = session.getHistory().getUndoDescriptions().size();
    const auto reese = 1 + (int)(threeOscPresets::find ("Reese Bass") - threeOscPresets::all().data());
    menu.setSelectedId (reese + 1, juce::sendNotificationSync);
    CHECK (session.getHistory().getUndoDescriptions().size() == steps + 1);
    CHECK (synth->findCurrentProgram() == reese);
    CHECK (menu.getText() == "Reese Bass");
    CHECK (synth->getSettings().cutoffHz == Catch::Approx (700.0f).epsilon (0.01));

    // A knob turned, then the same preset again: the sound is the preset's again, with no empty undo step.
    synth->getParameterState().getParameter ("cutoff")->setValueNotifyingHost (0.9f);
    CHECK (synth->findCurrentProgram() == -1);
    REQUIRE (loadBuiltInPreset (session, device, "Reese Bass").wasOk());
    CHECK (synth->findCurrentProgram() == reese);
    CHECK (session.getHistory().getUndoDescriptions().size() == steps + 1);

    // The arrows step through the list; undo goes back one preset at a time.
    window->stepPreset (1);
    CHECK (synth->findCurrentProgram() == reese + 1);
    CHECK (session.getHistory().getUndoDescriptions().size() == steps + 2);
    REQUIRE (session.getHistory().undo());
    CHECK (synth->findCurrentProgram() == reese);
    REQUIRE (session.getHistory().undo());
    CHECK (synth->findCurrentProgram() == 0); // back to the new 3x Osc's sound

    CHECK (loadBuiltInPreset (session, device, "No such preset").failed());
    synth->editorBeingDeleted (editor.get());
}

TEST_CASE ("Closest preset can list 3x Osc's presets", "[3xosc][presets]")
{
    ThreeOscProcessor synth;
    model::Device device { model::DeviceId { 1 }, model::Device::Kind::stock, "spinus.3x-osc", {}, false };
    const auto found = InstrumentPresets::find (synth, device);
    REQUIRE (found.size() == threeOscPresets::all().size()); // "Default" is no preset
    CHECK (found.front().name == threeOscPresets::all().front().name);
    REQUIRE (InstrumentPresets::load (synth, found[2]).wasOk());
    CHECK (synth.findCurrentProgram() == 3);
}
