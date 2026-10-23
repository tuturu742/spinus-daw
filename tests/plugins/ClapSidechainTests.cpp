#include <catch2/catch_test_macros.hpp>

#include "../engine/TestDevices.h"
#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "engine/EngineAdapter.h"
#include "engine/EngineHost.h"
#include "plugins/clap/ClapPluginFormat.h"

using namespace spinus;
using namespace spinus::model;
namespace te = tracktion;

namespace
{
juce::String echoPath()
{
    return juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("sidechain-echo.clap").getFullPathName();
}

std::unique_ptr<juce::PluginDescription> echoDescription()
{
    plugins::ClapPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, echoPath());
    REQUIRE (found.size() == 1);
    return std::make_unique<juce::PluginDescription> (*found[0]);
}
} // namespace

TEST_CASE ("A CLAP plugin's second input port is a sidechain bus that receives audio", "[plugins][sidechain]")
{
    plugins::ClapPluginFormat format;
    juce::String error;
    auto plugin = format.createInstanceFromDescription (*echoDescription(), 48000.0, 256, error);
    REQUIRE (plugin != nullptr);
    REQUIRE (plugin->getBusCount (true) == 2);
    CHECK (plugin->getBus (true, 1)->getNumberOfChannels() == 2);
    CHECK (plugin->getTotalNumInputChannels() == 4);

    plugin->prepareToPlay (48000.0, 256);
    juce::AudioBuffer<float> buffer (4, 256);
    buffer.clear();
    for (int i = 0; i < 256; ++i)
        for (int ch : { 2, 3 })
            buffer.setSample (ch, i, 0.5f * std::sin ((float)i * 0.1f));

    juce::MidiBuffer midi;
    plugin->processBlock (buffer, midi);
    CHECK (std::abs (buffer.getSample (0, 100) - 0.5f * std::sin (10.0f)) < 1.0e-6f);
    CHECK (std::abs (buffer.getSample (1, 200) - 0.5f * std::sin (20.0f)) < 1.0e-6f);
    plugin->releaseResources();
}

TEST_CASE ("Insert sidechain routing feeds a CLAP plugin's sidechain input", "[plugins][sidechain][engine]")
{
    EngineHost host ({ .openAudioDevice = false });
    test::TestDevices devices (host.getEngine());
    const auto description = echoDescription();
    host.getEngine().getPluginManager().knownPluginList.addType (*description);

    // Tone -> insert "source" -> master. Insert "echo" has the echo plugin and no input of its own:
    // whatever it outputs came through its sidechain.
    auto render = [&] (bool withSidechain)
    {
        Project p;
        Insert source;
        source.id = p.allocateId<InsertId>();
        Insert echo;
        echo.id = p.allocateId<InsertId>();
        echo.effects.push_back ({ Device { p.allocateId<DeviceId>(),
                                           Device::Kind::plugin,
                                           {},
                                           { "CLAP", description->name, description->manufacturerName,
                                             description->fileOrIdentifier, description->uniqueId },
                                           false },
                                  1.0f });
        if (withSidechain)
            echo.sidechainSources.push_back (source.id);
        p.mixer.inserts = { source, echo };

        Channel tone;
        tone.id = p.allocateId<ChannelId>();
        tone.instrument = Device { p.allocateId<DeviceId>(), Device::Kind::stock, "test.tone", {}, false };
        tone.targetInsert = source.id;
        p.channels = { tone };

        // Paced in real time: the sidechain graph is built asynchronously, so an unpaced render could
        // finish before it exists.
        test::RealtimeRunner runner (host.getEngine(),
                                     { .outputChannels = 2, .maxSeconds = 3.0, .paceInRealTime = true });
        auto edit = te::Edit::createSingleTrackEdit (host.getEngine());
        engine::EngineAdapter adapter (*edit, devices);
        adapter.sync (p);
        REQUIRE (adapter.getMissingDevices().empty());

        edit->dispatchPendingUpdatesSynchronously();
        edit->getTransport().ensureContextAllocated();
        edit->getTransport().play (false);
        runner.start();
        runner.runUntil (2.0);
        runner.stop();
        edit->getTransport().stop (false, true);
        const auto sr = runner.getParams().sampleRate;
        return test::checks::rms (runner.getChannel (0), (int64_t)(0.5 * sr), (int64_t)(1.5 * sr));
    };

    const auto without = render (false);
    const auto with = render (true);
    INFO ("rms without sidechain " << without << ", with " << with);
    CHECK (without > 0.01f);
    CHECK (with > without * 1.5f); // the echo insert adds a copy of the tone
}
