#include <catch2/catch_test_macros.hpp>

#include "ai/Proposal.h"
#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "engine/ParameterIds.h"
#include "model/ModelJson.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "session/Session.h"

using namespace spinus;
namespace te = tracktion;

namespace
{
std::unique_ptr<commands::Command> make (const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
    INFO (error);
    REQUIRE (command != nullptr);
    return command;
}

juce::AudioProcessorParameter* parameterOf (Session& session, model::DeviceId device, const juce::String& id)
{
    auto* plugin = dynamic_cast<te::ExternalPlugin*> (session.getAdapter().getDevicePlugin (device));
    if (plugin == nullptr || plugin->getAudioPluginInstance() == nullptr)
        return nullptr;
    for (auto* p : plugin->getAudioPluginInstance()->getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p);
            withId != nullptr && withId->paramID == id)
            return p;
    return nullptr;
}
} // namespace

TEST_CASE ("Settings set by command reach the plugin; undo gives back what it had before", "[session][engine]")
{
    EngineHost host { { .openAudioDevice = false } };
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerBundledDevices (*devices, host.getEngine(), {});
    Session session (host.getEngine(), std::move (devices));

    auto add = make ("insert.addEffect", R"({"insert": 1, "device": {"kind": "stock", "stockType": "spinus.filter"}})");
    auto* raw = add.get();
    REQUIRE (session.getHistory().perform (std::move (add)).wasOk());
    const model::DeviceId filter { (uint32_t)(int)raw->getResult()["device"] };
    auto* cutoff = parameterOf (session, filter, "cutoff");
    REQUIRE (cutoff != nullptr);

    // The user turned it by hand first.
    cutoff->setValueNotifyingHost (0.8f);

    REQUIRE (session.getHistory()
                 .perform (make ("device.setParameters",
                                 R"({"device": )" + juce::String (filter.value) + R"(, "values": {"cutoff": 0.25}})"))
                 .wasOk());
    CHECK (juce::approximatelyEqual (cutoff->getValue(), 0.25f));

    REQUIRE (session.getHistory().undo());
    CHECK (juce::approximatelyEqual (cutoff->getValue(), 0.8f)); // what it was before
    REQUIRE (session.getHistory().redo());
    CHECK (juce::approximatelyEqual (cutoff->getValue(), 0.25f));

    // A knob turned afterwards stays until a command sets it again.
    cutoff->setValueNotifyingHost (0.5f);
    REQUIRE (session.getHistory()
                 .perform (make ("device.setParameters",
                                 R"({"device": )" + juce::String (filter.value) + R"(, "values": {"resonance": 0.4}})"))
                 .wasOk());
    CHECK (juce::approximatelyEqual (cutoff->getValue(), 0.5f));
    CHECK (juce::approximatelyEqual (parameterOf (session, filter, "resonance")->getValue(), 0.4f));

    // Removing the effect and undoing brings the settings back with it.
    REQUIRE (session.getHistory()
                 .perform (make ("insert.removeEffect", R"({"device": )" + juce::String (filter.value) + "}"))
                 .wasOk());
    REQUIRE (session.getHistory().undo());
    auto* again = parameterOf (session, filter, "resonance");
    REQUIRE (again != nullptr);
    CHECK (juce::approximatelyEqual (again->getValue(), 0.4f));
}

namespace
{
std::unique_ptr<engine::DefaultDeviceFactory> makeDevices (te::Engine& engine, std::vector<juce::File> folders = {})
{
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
#if defined(SPINUS_PLUGINS_DIR)
    folders.push_back (juce::File (SPINUS_PLUGINS_DIR));
#endif
    engine::registerBundledDevices (*devices, engine, folders);
    return devices;
}

/** The test effect in param-ids.clap, installed: its CLAP parameter IDs are 2, 1000, 0, 77 (Drive, Tone, Mix,
    Level), not their positions. */
model::Device installParamIdsPlugin (te::Engine& engine)
{
    plugins::ClapPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (
        found, juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("param-ids.clap").getFullPathName());
    REQUIRE (found.size() == 1);
    engine.getPluginManager().knownPluginList.addType (*found[0]);
    model::Device device;
    device.kind = model::Device::Kind::plugin;
    device.plugin = { found[0]->pluginFormatName, found[0]->name, found[0]->manufacturerName,
                      found[0]->fileOrIdentifier, found[0]->uniqueId };
    return device;
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

juce::AudioPluginInstance* instanceOf (Session& session, model::DeviceId device)
{
    auto* plugin = dynamic_cast<te::ExternalPlugin*> (session.getAdapter().getDevicePlugin (device));
    return plugin != nullptr ? plugin->getAudioPluginInstance() : nullptr;
}

juce::AudioProcessorParameter* byName (juce::AudioPluginInstance& instance, const juce::String& name)
{
    for (auto* p : instance.getParameters())
        if (p->getName (100) == name)
            return p;
    return nullptr;
}

bool setParameters (Session& session, model::DeviceId device, const juce::String& values)
{
    return session.getHistory()
        .perform (make ("device.setParameters",
                        R"({"device": )" + juce::String (device.value) + R"(, "values": )" + values + "}"))
        .wasOk();
}

/** Every parameter's Spinus ID is the one Tracktion's automation knows it by, for the same parameter. */
void checkIdsAgreeWithAutomation (Session& session, model::DeviceId device)
{
    auto* plugin = session.getAdapter().getDevicePlugin (device);
    auto* instance = instanceOf (session, device);
    REQUIRE (instance != nullptr);
    int automatable = 0;
    for (auto* p : instance->getParameters())
    {
        if (! p->isAutomatable())
            continue;
        ++automatable;
        const auto id = engine::getParameterId (*p);
        INFO (p->getName (1024) << " (" << id << ")");
        auto automated = plugin->getAutomatableParameterByID (id);
        REQUIRE (automated != nullptr);
        CHECK (automated->getParameterName().startsWith (p->getName (1024).trim()));
        model::ParameterTarget target;
        target.kind = model::ParameterTarget::Kind::deviceParameter;
        target.device = device;
        target.parameterId = id;
        CHECK (session.getAdapter().findParameter (target) == automated.get());
    }
    CHECK (automatable > 0);
}
} // namespace

TEST_CASE ("Settings set by command reach a plugin whose own parameter IDs are not their positions",
           "[session][engine]")
{
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    const auto effect = addEffect (session, installParamIdsPlugin (host.getEngine()));
    auto* instance = instanceOf (session, effect);
    REQUIRE (instance != nullptr);
    auto* drive = byName (*instance, "Drive");
    auto* tone = byName (*instance, "Tone");
    auto* mix = byName (*instance, "Mix");
    auto* level = byName (*instance, "Level");
    REQUIRE ((drive != nullptr && tone != nullptr && mix != nullptr && level != nullptr));

    // Spinus's IDs are the positions, as automation, the device menu and the parameter list have them.
    CHECK (engine::getParameterId (*drive) == "0");
    CHECK (engine::getParameterId (*mix) == "2");
    CHECK (engine::getPluginOwnParameterId (*drive) == "2"); // the plugin's own: Mix's position
    checkIdsAgreeWithAutomation (session, effect);
    const auto listed = session.getDeviceParameters (effect);
    CHECK (std::find (listed.begin(), listed.end(), std::pair<juce::String, juce::String> ("0", "Drive")) !=
           listed.end());

    // "0" is Drive (position 0), not Mix (whose CLAP ID is 0).
    REQUIRE (setParameters (session, effect, R"({"0": 0.25, "2": 0.75})"));
    CHECK (juce::approximatelyEqual (drive->getValue(), 0.25f));
    CHECK (juce::approximatelyEqual (mix->getValue(), 0.75f));
    CHECK (juce::approximatelyEqual (tone->getValue(), 0.2f));
    CHECK (juce::approximatelyEqual (level->getValue(), 0.4f));

    // The plugin itself has them: its state is its values in list order.
    juce::MemoryBlock state;
    instance->getStateInformation (state);
    REQUIRE (state.getSize() == 4 * sizeof (double));
    const auto* values = static_cast<const double*> (state.getData());
    CHECK (std::abs (values[0] - 0.25) < 1.0e-6);
    CHECK (std::abs (values[1] - 0.2) < 1.0e-6);
    CHECK (std::abs (values[2] - 0.75) < 1.0e-6);

    // Copy effect reads them by the same IDs.
    const auto read = session.getAdapter().readParameters (effect);
    CHECK ((read.contains ("0") && juce::approximatelyEqual (read.at ("0"), 0.25f)));
    CHECK ((read.contains ("2") && juce::approximatelyEqual (read.at ("2"), 0.75f)));
    CHECK (! read.contains ("1000"));

    REQUIRE (session.getHistory().undo());
    CHECK (juce::approximatelyEqual (drive->getValue(), 0.1f));
    CHECK (juce::approximatelyEqual (mix->getValue(), 0.3f));
}

TEST_CASE ("Settings saved by the plugin's own parameter IDs (older projects) still load", "[session][engine]")
{
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    auto device = installParamIdsPlugin (host.getEngine());
    // As Copy effect saved them before: "1000" and "77" are no setting's position, so they are Tone's and
    // Level's own IDs. "3" is Level's position: by Spinus's ID it counts over Level's own "77".
    device.parameters = { { "1000", 0.6f }, { "77", 0.1f }, { "3", 0.9f } };
    const auto effect = addEffect (session, device);
    auto* instance = instanceOf (session, effect);
    REQUIRE (instance != nullptr);
    CHECK (juce::approximatelyEqual (byName (*instance, "Tone")->getValue(), 0.6f));
    CHECK (juce::approximatelyEqual (byName (*instance, "Level")->getValue(), 0.9f));
    CHECK (juce::approximatelyEqual (byName (*instance, "Drive")->getValue(), 0.1f));

    // A setting changed afterwards (by Spinus's ID) leaves the others as they are.
    REQUIRE (setParameters (session, effect, R"({"0": 0.5})"));
    CHECK (juce::approximatelyEqual (byName (*instance, "Drive")->getValue(), 0.5f));
    CHECK (juce::approximatelyEqual (byName (*instance, "Level")->getValue(), 0.9f));
}

TEST_CASE ("device.setParameters reads text for any plugin, as the plugin shows its values", "[session][engine]")
{
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    const auto plugin = installParamIdsPlugin (host.getEngine());
    const auto effect = addEffect (session, plugin);
    auto* instance = instanceOf (session, effect);
    REQUIRE (instance != nullptr);
    auto* drive = byName (*instance, "Drive");
    auto* tone = byName (*instance, "Tone");
    auto* mix = byName (*instance, "Mix");
    auto* level = byName (*instance, "Level");
    REQUIRE ((drive != nullptr && tone != nullptr && mix != nullptr && level != nullptr));
    auto setText = [&] (model::DeviceId device, const juce::String& text)
    {
        return session.getHistory().perform (make (
            "device.setParameters", R"({"device": )" + juce::String (device.value) + R"(, "text": )" + text + "}"));
    };

    // The plugin reads no text itself: what it shows over its range says where each value is. By name or ID.
    const auto done = setText (effect, R"({"Drive": "-12 dB", "1": "1.2 kHz", "Mix": "35", "level": "2.5 s"})");
    INFO (done.getErrorMessage());
    REQUIRE (done.wasOk());
    CHECK (drive->getText (drive->getValue(), 100) == "-12.0 dB");
    CHECK (tone->getText (tone->getValue(), 100) == "1200 Hz"); // logarithmic
    CHECK (mix->getText (mix->getValue(), 100) == "35%");
    CHECK (level->getText (level->getValue(), 100) == "2.500"); // seconds, shown without the unit
    CHECK (std::abs (drive->getValue() - 0.8f) < 0.002f);

    // A word it shows, and a number past its range (the closest end).
    REQUIRE (setText (effect, R"({"Level": "Off", "Drive": "+6 dB"})").wasOk());
    CHECK (juce::approximatelyEqual (level->getValue(), 0.0f));
    CHECK (juce::approximatelyEqual (drive->getValue(), 1.0f));

    // What it cannot read says what it shows.
    const auto wrongUnit = setText (effect, R"({"Drive": "50%"})");
    CHECK (wrongUnit.failed());
    CHECK (wrongUnit.getErrorMessage().contains ("\"-60.0 dB\" at 0"));
    CHECK (wrongUnit.getErrorMessage().contains ("\"0.0 dB\" at 1"));
    CHECK (setText (effect, R"({"Level": "Loud"})").failed());
    CHECK (setText (effect, R"({"Width": "50%"})").getErrorMessage().contains ("has no setting"));

    // In one proposal with the effect it sets (not playing yet): read on a copy of the plugin.
    auto steps = juce::JSON::parse (R"([{"command": "insert.addEffect", "parameters": {"insert": 1}},
        {"command": "device.setParameters", "parameters": {"device": "$1.device", "text": {"Mix": "70%"}}}])");
    steps[0]["parameters"].getDynamicObject()->setProperty ("device", model::deviceToJson (plugin));
    auto* o = new juce::DynamicObject();
    o->setProperty ("summary", "Add Param IDs at 70%");
    o->setProperty ("steps", steps);
    auto proposal = ai::Proposal::prepare (juce::var (o), session.getHistory());
    INFO (proposal.error);
    REQUIRE (proposal.canApply());
    REQUIRE (proposal.apply (session.getHistory()).wasOk());
    session.getEdit().dispatchPendingUpdatesSynchronously();
    const auto* insert = session.getProject().findInsert (model::InsertId { 1 });
    REQUIRE (insert != nullptr);
    const auto& added = insert->effects.back().device;
    CHECK (added.id != effect);
    REQUIRE (added.parameters.contains ("2"));
    CHECK (std::abs (added.parameters.at ("2") - 0.7f) < 0.003f);
}

namespace
{
/** Sets each of up to 8 of the device's parameters whose own ID is not its position, by Spinus's ID, and checks
    it reached the plugin (and the automation parameter of the same ID). Returns how many it checked. */
int checkSettingsReach (Session& session, model::DeviceId device)
{
    auto* instance = instanceOf (session, device);
    REQUIRE (instance != nullptr);
    checkIdsAgreeWithAutomation (session, device);
    auto* o = new juce::DynamicObject();
    juce::var values (o);
    std::vector<std::pair<juce::AudioProcessorParameter*, float>> wanted;
    for (auto* p : instance->getParameters())
        if (p->isAutomatable() && ! p->isDiscrete() && wanted.size() < 8 &&
            engine::getPluginOwnParameterId (*p) != engine::getParameterId (*p))
        {
            const auto value = p->getValue() < 0.5f ? 0.8125f : 0.1875f;
            o->setProperty (engine::getParameterId (*p), value);
            wanted.emplace_back (p, value);
        }
    REQUIRE (! wanted.empty());
    REQUIRE (setParameters (session, device, juce::JSON::toString (values, true)));
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200); // Tracktion follows the plugin's values
    for (auto& [p, value] : wanted)
    {
        INFO (p->getName (100) << " (" << engine::getParameterId (*p) << ", own ID "
                               << engine::getPluginOwnParameterId (*p) << ")");
        CHECK (std::abs (p->getValue() - value) < 0.01f);
        auto automated =
            session.getAdapter().getDevicePlugin (device)->getAutomatableParameterByID (engine::getParameterId (*p));
        REQUIRE (automated != nullptr);
        CHECK (std::abs (automated->valueRange.convertTo0to1 (automated->getCurrentValue()) - value) < 0.01f);
    }
    return (int)wanted.size();
}
} // namespace

TEST_CASE ("Settings set by command reach Surge XT Effects (its own parameter IDs are not positions)",
           "[session][engine][surge]")
{
#if defined(SPINUS_PLUGINS_DIR)
    if (! juce::File (SPINUS_PLUGINS_DIR).getChildFile ("Surge XT Effects.clap").exists())
#endif
        SKIP ("Surge XT Effects is not built");
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    auto add = make ("insert.addEffect",
                     R"({"insert": 1, "device": {"kind": "stock", "stockType": "bundled.surge-xt-effects"}})");
    auto* raw = add.get();
    REQUIRE (session.getHistory().perform (std::move (add)).wasOk());
    session.getEdit().dispatchPendingUpdatesSynchronously();
    CHECK (checkSettingsReach (session, model::DeviceId { (uint32_t)(int)raw->getResult()["device"] }) > 0);
}

/** OB-Xf, downloaded (as the presets check in InstrumentPresetsTests): point SPINUS_CHECK_DOWNLOADED at a folder
    holding the unpacked OB-Xf.clap (searched recursively). */
TEST_CASE ("Settings set by command reach OB-Xf, downloaded", "[session][engine][downloaded]")
{
    const juce::File folder { juce::SystemStats::getEnvironmentVariable ("SPINUS_CHECK_DOWNLOADED", {}) };
    if (! folder.isDirectory())
        SKIP ("SPINUS_CHECK_DOWNLOADED is not set to a folder");
    std::vector<juce::File> folders { folder };
    for (const auto& found : folder.findChildFiles (juce::File::findFiles, true, "OB-Xf.clap"))
        folders.push_back (found.getParentDirectory());
    if (folders.size() == 1)
        SKIP ("OB-Xf.clap is not in " << folder.getFullPathName());

    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine(), folders));
    auto add =
        make ("channel.add", R"({"name": "OB-Xf", "instrument": {"kind": "stock", "stockType": "bundled.ob-xf"}})");
    auto* raw = add.get();
    REQUIRE (session.getHistory().perform (std::move (add)).wasOk());
    session.getEdit().dispatchPendingUpdatesSynchronously();
    CHECK (checkSettingsReach (session, model::DeviceId { (uint32_t)(int)raw->getResult()["device"] }) > 0);
}

TEST_CASE ("Typed values and device.setParameters' text are in the units the device shows", "[session][engine]")
{
    EngineHost host { { .openAudioDevice = false } };
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerBundledDevices (*devices, host.getEngine(), {});
    Session session (host.getEngine(), std::move (devices));

    auto addEffect = [&session] (const char* stockType)
    {
        auto add = make ("insert.addEffect", R"({"insert": 1, "device": {"kind": "stock", "stockType": ")" +
                                                 juce::String (stockType) + R"("}})");
        auto* raw = add.get();
        REQUIRE (session.getHistory().perform (std::move (add)).wasOk());
        return model::DeviceId { (uint32_t)(int)raw->getResult()["device"] };
    };
    const auto ducker = addEffect ("spinus.ducker");
    const auto utility = addEffect ("spinus.stereo-utility");
    const auto filter = addEffect ("spinus.filter");
    auto target = [] (model::DeviceId device, const char* id)
    {
        model::ParameterTarget t;
        t.kind = model::ParameterTarget::Kind::deviceParameter;
        t.device = device;
        t.parameterId = id;
        return t;
    };
    auto normalised = [&] (model::DeviceId device, const char* id, float value)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (parameterOf (session, device, id));
        REQUIRE (p != nullptr);
        return p->convertTo0to1 (value);
    };
    auto near = [] (std::optional<float> v, float expected)
    {
        INFO ((v ? juce::String (*v) : juce::String ("nothing")) << " for " << expected);
        return v.has_value() && std::abs (*v - expected) < 1.0e-4f;
    };

    // The Automation editor's Value box (and the demo generators) read typed text like this.
    CHECK (near (session.parseTargetValue (target (ducker, "amount"), "35"), 0.35f));
    CHECK (near (session.parseTargetValue (target (ducker, "amount"), "35%"), 0.35f));
    CHECK (near (session.parseTargetValue (target (ducker, "release"), "220"), normalised (ducker, "release", 0.22f)));
    CHECK (
        near (session.parseTargetValue (target (ducker, "release"), "220 ms"), normalised (ducker, "release", 0.22f)));
    CHECK (near (session.parseTargetValue (target (utility, "width"), "150%"), 0.75f)); // 0 to 200%
    CHECK (near (session.parseTargetValue (target (utility, "gain"), "-6 dB"), normalised (utility, "gain", -6.0f)));
    CHECK (
        near (session.parseTargetValue (target (filter, "cutoff"), "1.2 kHz"), normalised (filter, "cutoff", 1200.0f)));
    CHECK (near (session.parseTargetValue (target (filter, "type"), "High pass"), 1.0f / 3.0f));
    CHECK (! session.parseTargetValue (target (ducker, "amount"), "-6 dB").has_value());
    CHECK (! session.parseTargetValue (target (filter, "type"), "Sideways").has_value());

    // device.setParameters' text: by ID or name, read on a copy of the device, stored as 0 to 1.
    REQUIRE (session.getHistory()
                 .perform (make ("device.setParameters",
                                 R"({"device": )" + juce::String (ducker.value) +
                                     R"(, "text": {"amount": "60%", "Release": "180 ms", "curve": "25"}})"))
                 .wasOk());
    const auto* insert = session.getProject().findInsert (model::InsertId { 1 });
    REQUIRE (insert != nullptr);
    const auto& stored = insert->effects[0].device.parameters;
    CHECK (near (stored.at ("amount"), 0.6f));
    CHECK (near (stored.at ("release"), normalised (ducker, "release", 0.18f)));
    CHECK (near (stored.at ("curve"), 0.25f));
    CHECK (near (parameterOf (session, ducker, "amount")->getValue(), 0.6f)); // the plugin follows
    CHECK (session.getHistory().getUndoDescriptions().strings.getLast().startsWith ("Change 3 settings of"));
    REQUIRE (session.getHistory().undo());
    CHECK (stored.empty());
    REQUIRE (session.getHistory().redo());
    CHECK (near (stored.at ("amount"), 0.6f));

    auto fails = [&session] (const juce::String& params)
    {
        juce::String error;
        auto command =
            commands::CommandRegistry::get().create ("device.setParameters", juce::JSON::parse (params), error);
        if (command == nullptr)
            return true;
        const auto result = session.getHistory().perform (std::move (command));
        INFO (result.getErrorMessage());
        return result.failed();
    };
    const auto d = juce::String (ducker.value);
    CHECK (fails (R"({"device": )" + d + R"(, "text": {"amount": "-6 dB"}})")); // a unit of another kind
    CHECK (fails (R"({"device": )" + d + R"(, "text": {"wobble": "10%"}})"));   // no such setting
    CHECK (fails (R"({"device": )" + d + R"(, "text": {"amount": 0.5}})"));     // text, not numbers
    CHECK (fails (R"({"device": )" + d + R"(, "text": {}})"));
    CHECK (near (stored.at ("amount"), 0.6f)); // nothing changed
}
