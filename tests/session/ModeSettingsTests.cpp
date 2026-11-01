#include <catch2/catch_test_macros.hpp>

#include "ai/Proposal.h"
#include "commands/CommandRegistry.h"
#include "dsp/ParameterText.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "engine/ParameterIds.h"
#include "model/ModelJson.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "session/Session.h"

#include <atomic>
#include <thread>

using namespace spinus;
namespace te = tracktion;

// An effect whose type picks its other settings, and takes effect only when the plugin processes audio
// (tests/plugins/ModesClap.c): Spinus lets such a change take effect while nothing plays (the command line, a
// copy read for a proposal, no audio device), so the settings can be set from words and the type is saved.

namespace
{
std::unique_ptr<juce::PluginDescription> modesDescription()
{
    plugins::ClapPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found,
                                juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("modes.clap").getFullPathName());
    REQUIRE (found.size() == 1);
    return std::make_unique<juce::PluginDescription> (*found[0]);
}

model::Device installModes (te::Engine& engine)
{
    const auto description = modesDescription();
    engine.getPluginManager().knownPluginList.addType (*description);
    model::Device device;
    device.kind = model::Device::Kind::plugin;
    device.plugin = { description->pluginFormatName, description->name, description->manufacturerName,
                      description->fileOrIdentifier, description->uniqueId };
    return device;
}

std::unique_ptr<engine::DefaultDeviceFactory> makeDevices (te::Engine& engine,
                                                           const std::vector<juce::File>& folders = {})
{
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerBundledDevices (*devices, engine, folders);
    return devices;
}

juce::Result perform (Session& session, const juce::String& name, const juce::var& params)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, params, error);
    if (command == nullptr)
        return juce::Result::fail (error);
    return session.getHistory().perform (std::move (command));
}

model::DeviceId addEffect (Session& session, const model::Device& device)
{
    juce::String error;
    auto parameters = juce::JSON::parse (R"({"insert": 1})");
    parameters.getDynamicObject()->setProperty ("device", model::deviceToJson (device));
    auto command = commands::CommandRegistry::get().create ("insert.addEffect", parameters, error);
    INFO (error);
    REQUIRE (command != nullptr);
    auto* raw = command.get();
    REQUIRE (session.getHistory().perform (std::move (command)).wasOk());
    session.getEdit().dispatchPendingUpdatesSynchronously();
    return model::DeviceId { (uint32_t)(int)raw->getResult()["device"] };
}

juce::Result setText (Session& session, model::DeviceId device, const juce::String& text)
{
    return perform (session, "device.setParameters",
                    juce::JSON::parse (R"({"device": )" + juce::String (device.value) + R"(, "text": )" + text + "}"));
}

juce::AudioPluginInstance* instanceOf (Session& session, model::DeviceId device)
{
    auto* plugin = dynamic_cast<te::ExternalPlugin*> (session.getAdapter().getDevicePlugin (device));
    return plugin != nullptr ? plugin->getAudioPluginInstance() : nullptr;
}

juce::String shown (juce::AudioProcessorParameter& p) { return p.getText (p.getValue(), 100); }

/** The type the plugin plays, as its state has it (the state's first number). */
int savedType (juce::AudioProcessor& plugin)
{
    juce::MemoryBlock state;
    plugin.getStateInformation (state);
    REQUIRE (state.getSize() == 4 * sizeof (double));
    return (int)static_cast<const double*> (state.getData())[0];
}
} // namespace

TEST_CASE ("A type set while the plugin is not played takes effect: its settings are named and saved",
           "[session][engine][plugins]")
{
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    const auto effect = addEffect (session, installModes (host.getEngine()));
    auto* instance = instanceOf (session, effect);
    REQUIRE (instance != nullptr);
    auto& ps = instance->getParameters();
    REQUIRE (ps.size() == 4);

    // Its type reads as a choice, though the plugin does not mark it as stepped.
    CHECK (dsp::param::choicesOf (*ps[3]) == juce::StringArray { "Echo", "Wobble", "Crunch" });

    // The type alone, then the type's own settings by name, in a later command.
    auto done = setText (session, effect, R"({"Type": "Wobble"})");
    INFO (done.getErrorMessage());
    REQUIRE (done.wasOk());
    done = setText (session, effect, R"({"Rate": "2 Hz", "Depth": "70%"})");
    INFO (done.getErrorMessage());
    REQUIRE (done.wasOk());
    CHECK (ps[0]->getName (100) == "Rate");
    CHECK (shown (*ps[0]) == "2.00 Hz");
    CHECK (shown (*ps[1]) == "70 %");
    CHECK (savedType (*instance) == 1);

    // Type and settings in one: the type first, so the rest read as that type shows them and stay as set
    // (the plugin puts its settings back to the new type's defaults when the type changes).
    done = setText (session, effect, R"({"Drive": "12 dB", "Type": "Crunch", "Mix": "25%"})");
    INFO (done.getErrorMessage());
    REQUIRE (done.wasOk());
    CHECK (ps[0]->getName (100) == "Drive");
    CHECK (shown (*ps[3]) == "Crunch");
    CHECK (shown (*ps[0]) == "12.0 dB");
    CHECK (shown (*ps[2]) == "25 %");
    CHECK (shown (*ps[1]) == "5050 Hz"); // Crunch's default Tone
    CHECK (savedType (*instance) == 2);

    // A setting found by a word of its name; one that is not there lists those it has now.
    REQUIRE (setText (session, effect, R"({"tone": "1000 Hz"})").wasOk());
    CHECK (shown (*ps[1]) == "1000 Hz");
    const auto missing = setText (session, effect, R"({"Rate": "1 Hz"})");
    CHECK (missing.getErrorMessage().contains ("has no setting"));
    CHECK (missing.getErrorMessage().contains ("Drive, Tone, Mix, Type"));

    // Undo goes back to Wobble with its settings.
    REQUIRE (session.getHistory().undo()); // tone
    REQUIRE (session.getHistory().undo()); // crunch
    instance = instanceOf (session, effect);
    REQUIRE (instance != nullptr);
    engine::letChangesTakeEffect (*instance);
    CHECK (shown (*instance->getParameters()[3]) == "Wobble");
    CHECK (instance->getParameters()[0]->getName (100) == "Rate");
    CHECK (shown (*instance->getParameters()[0]) == "2.00 Hz");
    CHECK (savedType (*instance) == 1);
}

TEST_CASE ("A saved project keeps the type set while nothing played, and opens with it", "[session][engine][plugins]")
{
    EngineHost host { { .openAudioDevice = false } };
    juce::var saved;
    model::Device device;
    {
        Session session (host.getEngine(), makeDevices (host.getEngine()));
        const auto effect = addEffect (session, installModes (host.getEngine()));
        REQUIRE (setText (session, effect, R"({"Type": "Echo"})").wasOk()); // already Echo: nothing to take in
        REQUIRE (setText (session, effect, R"({"Type": "Crunch", "Drive": "6 dB"})").wasOk());
        auto* instance = instanceOf (session, effect);
        REQUIRE (instance != nullptr);
        CHECK (savedType (*instance) == 2);
        device = session.getProject().findInsert (model::InsertId { 1 })->effects.back().device;
    }
    // A new copy with the saved settings (as a project opens) plays Crunch with them.
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    const auto effect = addEffect (session, device);
    auto* instance = instanceOf (session, effect);
    REQUIRE (instance != nullptr);
    engine::letChangesTakeEffect (*instance);
    CHECK (shown (*instance->getParameters()[3]) == "Crunch");
    CHECK (shown (*instance->getParameters()[0]) == "6.0 dB");
    CHECK (savedType (*instance) == 2);
}

TEST_CASE ("A proposal adds an effect and sets its type and the type's settings", "[session][engine][plugins]")
{
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    const auto plugin = installModes (host.getEngine());

    // Read on a copy of the plugin made for the proposal (the effect does not play yet), in two steps too.
    auto steps = juce::JSON::parse (R"([{"command": "insert.addEffect", "parameters": {"insert": 1}},
        {"command": "device.setParameters", "parameters": {"device": "$1.device", "text": {"Type": "Wobble"}}},
        {"command": "device.setParameters", "parameters": {"device": "$1.device", "text": {"Rate": "5 Hz"}}}])");
    steps[0]["parameters"].getDynamicObject()->setProperty ("device", model::deviceToJson (plugin));
    auto* o = new juce::DynamicObject();
    o->setProperty ("summary", "A wobble");
    o->setProperty ("steps", steps);
    auto proposal = ai::Proposal::prepare (juce::var (o), session.getHistory());
    INFO (proposal.error);
    REQUIRE (proposal.canApply());
    REQUIRE (proposal.apply (session.getHistory()).wasOk());
    session.getEdit().dispatchPendingUpdatesSynchronously();
    const auto& added = session.getProject().findInsert (model::InsertId { 1 })->effects.back().device;
    auto* instance = instanceOf (session, added.id);
    REQUIRE (instance != nullptr);
    engine::letChangesTakeEffect (*instance);
    CHECK (shown (*instance->getParameters()[3]) == "Wobble");
    CHECK (shown (*instance->getParameters()[0]) == "5.00 Hz");

    // The AI is told the effect's types (Session::getDeviceChoices), read from a copy.
    const auto choices = session.getDeviceChoices (plugin);
    REQUIRE (choices.size() == 1);
    CHECK (choices[0].first == "Type");
    CHECK (choices[0].second == juce::StringArray { "Echo", "Wobble", "Crunch" });
}

TEST_CASE ("While the audio thread plays a plugin, a change takes effect there, not on the message thread", "[plugins]")
{
    plugins::ClapPluginFormat format;
    juce::String error;
    auto plugin = format.createInstanceFromDescription (*modesDescription(), 48000.0, 256, error);
    REQUIRE (plugin != nullptr);
    plugin->prepareToPlay (48000.0, 256);
    std::atomic<bool> stop { false };
    std::atomic<int> blocks { 0 };
    std::thread audio (
        [&]
        {
            juce::AudioBuffer<float> buffer (2, 256);
            juce::MidiBuffer midi;
            while (! stop)
            {
                buffer.clear();
                plugin->processBlock (buffer, midi);
                ++blocks;
                std::this_thread::sleep_for (std::chrono::milliseconds (2));
            }
        });
    while (blocks < 3)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));

    auto& ps = plugin->getParameters();
    const auto wobble = dsp::param::normalisedForText (*ps[3], "Wobble").value_or (-1.0f);
    REQUIRE (wobble >= 0.0f);
    ps[3]->setValueNotifyingHost (wobble);
    plugins::ClapPluginFormat::applyPendingChanges (*plugin);
    CHECK (ps[0]->getName (100) == "Rate");
    CHECK (shown (*ps[0]) == "1.00 Hz"); // Wobble's default
    stop = true;
    audio.join();
    plugin->releaseResources();
}

/** The real Surge XT Effects: built with Spinus (SPINUS_BUILD_SURGE), or downloaded: point
    SPINUS_CHECK_DOWNLOADED at a folder holding the unpacked "Surge XT Effects.clap" (searched recursively), as the
    OB-Xf check in DeviceParametersTests.cpp. */
TEST_CASE ("Surge XT Effects is set up as a chorus from words, and saved as one", "[session][engine][surge]")
{
    std::vector<juce::File> folders;
#if defined(SPINUS_PLUGINS_DIR)
    if (juce::File (SPINUS_PLUGINS_DIR).getChildFile ("Surge XT Effects.clap").exists())
        folders.push_back (juce::File (SPINUS_PLUGINS_DIR));
#endif
    const juce::File folder { juce::SystemStats::getEnvironmentVariable ("SPINUS_CHECK_DOWNLOADED", {}) };
    if (folders.empty() && folder.isDirectory())
        for (const auto& found : folder.findChildFiles (juce::File::findFiles, true, "Surge XT Effects.clap"))
            folders.push_back (found.getParentDirectory());
    if (folders.empty())
        SKIP ("Surge XT Effects is not built, and SPINUS_CHECK_DOWNLOADED names no folder with it");

    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine(), folders));
    model::Device fx;
    fx.kind = model::Device::Kind::stock;
    fx.stockType = "bundled.surge-xt-effects";
    const auto choices = session.getDeviceChoices (fx);
    REQUIRE (! choices.empty());
    CHECK (choices[0].second.contains ("Chorus"));
    CHECK (choices[0].second.contains ("Phaser"));

    const auto effect = addEffect (session, fx);
    for (auto* type : { "Phaser", "Flanger", "Distortion" })
    {
        INFO (type);
        CHECK (setText (session, effect, R"({"FX Type": ")" + juce::String (type) + R"("})").wasOk());
    }
    const auto done = setText (session, effect, R"({"FX Type": "Chorus", "Rate": "1.5 Hz", "Mix": "40%"})");
    INFO (done.getErrorMessage());
    REQUIRE (done.wasOk());
    auto* instance = instanceOf (session, effect);
    REQUIRE (instance != nullptr);
    juce::AudioProcessorParameter *rate = nullptr, *type = nullptr;
    for (auto* p : instance->getParameters())
    {
        if (p->getName (100) == "Modulation Rate")
            rate = p;
        if (p->getName (100) == "FX Type")
            type = p;
    }
    REQUIRE ((rate != nullptr && type != nullptr));
    CHECK (shown (*type) == "Chorus");
    CHECK (shown (*rate).startsWith ("1.5"));

    // Its state has the chorus: a new copy given it plays one.
    juce::MemoryBlock state;
    instance->getStateInformation (state);
    plugins::ClapPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, folders[0].getChildFile ("Surge XT Effects.clap").getFullPathName());
    REQUIRE (! found.isEmpty());
    juce::String error;
    auto copy = format.createInstanceFromDescription (*found[0], 48000.0, 512, error);
    REQUIRE (copy != nullptr);
    copy->setStateInformation (state.getData(), (int)state.getSize());
    plugins::ClapPluginFormat::applyPendingChanges (*copy);
    for (auto* p : copy->getParameters())
        if (p->getName (100) == "FX Type")
            CHECK (shown (*p) == "Chorus");

    // Its type and settings as values at once, in any order: each stays as set (the type is no longer reset
    // by the others, nor they by the type).
    auto& ps = instance->getParameters();
    const auto typeId = engine::getParameterId (*type);
    const auto flanger = dsp::param::normalisedForText (*type, "Flanger").value_or (-1.0f);
    REQUIRE (flanger >= 0.0f);
    const auto values =
        R"({"0": 0.8125, "1": 0.1875, "2": 0.8125, ")" + typeId + R"(": )" + juce::String (flanger) + "}";
    REQUIRE (
        perform (session, "device.setParameters",
                 juce::JSON::parse (R"({"device": )" + juce::String (effect.value) + R"(, "values": )" + values + "}"))
            .wasOk());
    engine::letChangesTakeEffect (*instance);
    CHECK (shown (*type) == "Flanger");
    for (auto* p : { ps[0], ps[1], ps[2] })
    {
        INFO (p->getName (100));
        CHECK (std::abs (p->getValue() - (p == ps[1] ? 0.1875f : 0.8125f)) < 0.01f);
    }
}
