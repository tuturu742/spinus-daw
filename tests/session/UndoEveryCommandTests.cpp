#include <catch2/catch_test_macros.hpp>

#include "../commands/ExampleCalls.h"
#include "../engine/TestDevices.h"
#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "session/Session.h"

using namespace spinus;

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

/** Two device states hold the same settings: the same bytes, or the same tree but for what the engine sets up from
    the project and not from the device's own settings: the item ID Tracktion gives each new plugin object (an undo
    that brings a device back makes a new one), and sidechain wiring left behind with no sidechain source. */
bool sameSettings (const juce::MemoryBlock& a, const juce::MemoryBlock& b)
{
    if (a == b)
        return true;
    auto settingsOf = [] (const juce::MemoryBlock& block)
    {
        auto tree = juce::ValueTree::readFromData (block.getData(), block.getSize());
        if (! tree.isValid())
            return tree;
        tree.removeProperty (tracktion::IDs::id, nullptr);
        if (tree.getProperty ("sidechainSourceID", "0").toString() == "0")
        {
            tree.removeProperty ("sidechainSourceID", nullptr);
            tree.removeChild (tree.getChildWithName ("SIDECHAINCONNECTIONS"), nullptr);
        }
        return tree;
    };
    const auto x = settingsOf (a), y = settingsOf (b);
    return x.isValid() && y.isValid() && x.isEquivalentTo (y);
}

/** Devices whose settings differ between two captures, by ID (missing on one side counts). */
juce::StringArray differingDevices (const std::map<model::DeviceId, juce::MemoryBlock>& a,
                                    const std::map<model::DeviceId, juce::MemoryBlock>& b)
{
    juce::StringArray result;
    for (auto& [id, state] : a)
        if (auto it = b.find (id); it == b.end() || ! sameSettings (it->second, state))
            result.add (juce::String (id.value));
    for (auto& [id, state] : b)
        if (! a.contains (id))
            result.add (juce::String (id.value));
    return result;
}

} // namespace

TEST_CASE ("Every command: undo and redo also bring back each device's settings", "[session][commands]")
{
    // The model test checks the project; this one what is saved beside it, plugin-states/: a removed effect
    // that an undo brings back must come back with its settings, not the plugin's defaults.
    EngineHost host ({ .openAudioDevice = false });

    for (auto& [name, params] : test::exampleCalls())
    {
        INFO (name << " " << params);
        Session session (host.getEngine(), std::make_unique<test::TestDevices> (host.getEngine()));

        project::ProjectData data;
        data.project = test::exampleProjectFor (name);
        // The example's reverb (device 5) is a plugin not installed here: its saved settings must be kept.
        const juce::String missingState ("the missing reverb's own settings");
        data.deviceStates[model::DeviceId { 5 }] =
            juce::MemoryBlock (missingState.toRawUTF8(), missingState.getNumBytesAsUTF8());
        session.loadProject (std::move (data));

        // Every device that loaded gets a setting of its own, away from its default.
        for (auto id : { 3u, 8u, 10u })
            if (auto* plugin = session.getAdapter().getDevicePlugin (model::DeviceId { id }))
                if (auto parameter = plugin->getAutomatableParameter (0))
                    parameter->setParameter (0.3f + 0.1f * (float)(id % 3), juce::sendNotificationSync);

        const auto before = session.captureProjectData();
        REQUIRE (before.deviceStates.contains (model::DeviceId { 5 }));

        const auto result = session.getHistory().perform (make (name, params));
        INFO (result.getErrorMessage());
        REQUIRE (result.wasOk());
        const auto after = session.captureProjectData();

        for (int round = 0; round < 2; ++round)
        {
            INFO ("round " << round);
            REQUIRE (session.getHistory().undo());
            const auto undone = session.captureProjectData();
            CHECK (differingDevices (before.deviceStates, undone.deviceStates).joinIntoString (", ") == "");

            REQUIRE (session.getHistory().redo());
            const auto redone = session.captureProjectData();
            CHECK (differingDevices (after.deviceStates, redone.deviceStates).joinIntoString (", ") == "");
        }
    }
}
