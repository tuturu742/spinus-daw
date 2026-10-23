/*  Native Linux VST2 hosting.

    JUCE's VST2 host is compiled against FST, a reverse-engineered GPL-3.0-or-later VST2 header
    (external/fst), instead of Steinberg's withdrawn proprietary VST2 SDK. These tests load the VST2
    builds of Dragonfly Reverb (built with DPF, which has its own VST2 header) through Tracktion, and
    compare them with the same plugins' CLAP builds.

    Measurements are printed as "Hosting | name | value" lines.
    Run: ./build/release/tests/checks/spinus_plugin_hosting_checks --order decl
*/

#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "engine/EngineHost.h"
#include "plugins/clap/ClapPluginFormat.h"

#include <iostream>

using namespace spinus;
using namespace spinus::test;
namespace te = tracktion;

namespace
{

void metric (const std::string& name, const juce::String& value)
{
    std::cout << "Hosting | " << name << " | " << value << '\n';
}

juce::AudioPluginFormat* findFormat (te::Engine& engine, const juce::String& name)
{
    for (auto* format : engine.getPluginManager().pluginFormatManager.getFormats())
        if (format->getName() == name)
            return format;

    return nullptr;
}

std::unique_ptr<juce::PluginDescription> describe (te::Engine& engine, const juce::String& formatName,
                                                   const juce::String& path)
{
    auto* format = findFormat (engine, formatName);
    REQUIRE (format != nullptr);

    juce::OwnedArray<juce::PluginDescription> found;
    format->findAllTypesForFile (found, path);
    REQUIRE (found.size() == 1);
    engine.getPluginManager().knownPluginList.addType (*found[0]);
    return std::make_unique<juce::PluginDescription> (*found[0]);
}

const juce::File testPlugins { SPINUS_TEST_PLUGINS_DIR };
const juce::File pluginsDir { SPINUS_PLUGINS_DIR };
const juce::StringArray reverbs { "DragonflyHallReverb", "DragonflyRoomReverb", "DragonflyPlateReverb",
                                  "DragonflyEarlyReflections" };

juce::String vst2Path (const juce::String& name)
{
    return testPlugins.getChildFile (name + "-vst.so").getFullPathName();
}

/** Plays a 440 Hz tone through the plugin in a Tracktion Edit and returns the output RMS. */
float renderThrough (te::Engine& engine, const juce::PluginDescription& desc)
{
    engine.getPluginManager().createBuiltInType<te::ToneGeneratorPlugin>();
    RealtimeRunner runner (engine, { .outputChannels = 2, .maxSeconds = 3.0, .paceInRealTime = false });
    auto edit = te::Edit::createSingleTrackEdit (engine);
    auto track = te::getAudioTracks (*edit)[0];

    auto tone = edit->getPluginCache().createNewPlugin (te::ToneGeneratorPlugin::xmlTypeName, {});
    track->pluginList.insertPlugin (tone, 0, nullptr);

    auto plugin = edit->getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, desc);
    auto* external = dynamic_cast<te::ExternalPlugin*> (plugin.get());
    REQUIRE (external != nullptr);
    INFO ("Load error: " << external->getLoadError());
    REQUIRE (external->getAudioPluginInstance() != nullptr);
    track->pluginList.insertPlugin (plugin, 1, nullptr);

    edit->dispatchPendingUpdatesSynchronously();
    edit->getTransport().ensureContextAllocated();
    edit->getTransport().play (false);
    runner.start();
    runner.runUntil (2.0);
    runner.stop();
    edit->getTransport().stop (false, true);

    const auto sr = runner.getParams().sampleRate;
    return checks::rms (runner.getChannel (0), (int64_t)(0.5 * sr), (int64_t)(1.5 * sr));
}

} // namespace

//==============================================================================
TEST_CASE ("Plugin hosting: The VST2 format is available and scans the Dragonfly VST2 builds", "[hosting]")
{
    EngineHost host ({ .openAudioDevice = false });
    auto* vst2 = findFormat (host.getEngine(), "VST");
    REQUIRE (vst2 != nullptr);
    metric ("VST2 format", vst2->getName());

    for (auto& name : reverbs)
    {
        auto desc = describe (host.getEngine(), "VST", vst2Path (name));
        metric (name.toStdString() + " (VST2)", desc->name + " " + desc->version + " by " + desc->manufacturerName +
                                                    ", " + juce::String (desc->numInputChannels) + " in / " +
                                                    juce::String (desc->numOutputChannels) + " out");
        CHECK (desc->numOutputChannels == 2);
    }
}

TEST_CASE ("Plugin hosting: VST2 plugins process audio through Tracktion like their CLAP builds", "[hosting]")
{
    for (auto& name : reverbs)
    {
        float vst2Level = 0.0f, clapLevel = 0.0f;

        {
            EngineHost host ({ .openAudioDevice = false });
            vst2Level = renderThrough (host.getEngine(), *describe (host.getEngine(), "VST", vst2Path (name)));
        }

        {
            EngineHost host ({ .openAudioDevice = false });
            const auto clap = pluginsDir.getChildFile (name + ".clap").getFullPathName();
            clapLevel = renderThrough (host.getEngine(),
                                       *describe (host.getEngine(), plugins::ClapPluginFormat::formatName, clap));
        }

        metric (name.toStdString() + ": rms VST2 / CLAP",
                juce::String (vst2Level, 4) + " / " + juce::String (clapLevel, 4));
        CHECK (vst2Level > 0.01f);
        CHECK (std::abs (vst2Level - clapLevel) < 0.05f * clapLevel + 1.0e-3f);
    }
}

TEST_CASE ("Plugin hosting: VST2 parameters and state round trip", "[hosting]")
{
    EngineHost host ({ .openAudioDevice = false });
    auto desc = describe (host.getEngine(), "VST", vst2Path ("DragonflyHallReverb"));
    auto& formats = host.getEngine().getPluginManager().pluginFormatManager;

    juce::String error;
    auto a = formats.createPluginInstance (*desc, 48000.0, 512, error);
    auto b = formats.createPluginInstance (*desc, 48000.0, 512, error);
    REQUIRE ((a != nullptr && b != nullptr));

    const auto& params = a->getParameters();
    metric ("Dragonfly Hall (VST2) parameters", juce::String (params.size()));
    REQUIRE (params.size() > 4);

    for (int i = 0; i < params.size(); i += 3)
        params[i]->setValueNotifyingHost (0.3f);

    metric ("parameter text (" + params[3]->getName (32).toStdString() + ")", params[3]->getCurrentValueAsText());

    juce::MemoryBlock state;
    a->getStateInformation (state);
    metric ("VST2 state bytes", juce::String ((int)state.getSize()));
    REQUIRE (state.getSize() > 0);
    b->setStateInformation (state.getData(), (int)state.getSize());

    juce::StringArray differing;
    for (int i = 0; i < params.size(); ++i)
        if (std::abs (params[i]->getValue() - b->getParameters()[i]->getValue()) > 1.0e-3f)
            differing.add (params[i]->getName (32));

    metric ("parameters differing after restore",
            juce::String (differing.size()) +
                (differing.isEmpty() ? "" : " (" + differing.joinIntoString (", ") + ")"));
    CHECK (differing.isEmpty());
}
