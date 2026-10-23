/*  Windows plugins through yabridge.

    yabridge's "chainloader" libraries look like native Linux VST2/VST3/CLAP plugins; behind them the
    Windows plugin runs in a separate Wine process. These tests load the Windows builds of Dragonfly
    Reverb that way, in all three formats, and compare them with the native Linux CLAP builds.

    Needs Wine and yabridge, so it only runs when SPINUS_CHECK_WINDOWS_PLUGINS points at a folder with
    vst2/, vst3/ and clap/ subfolders set up as tools/owner-checks/windows-plugins-setup.sh does.
*/

#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "engine/EngineHost.h"
#include "plugins/clap/ClapPluginFormat.h"

#include <chrono>
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

juce::File windowsPluginsDir()
{
    return juce::File (juce::SystemStats::getEnvironmentVariable ("SPINUS_CHECK_WINDOWS_PLUGINS", {}));
}

struct Bridged
{
    const char* format; // JUCE / Spinus format name
    const char* label;
    juce::String (*path) (const juce::String& plugin);
};

const std::vector<Bridged> bridgedFormats {
    { "VST", "VST2", [] (const juce::String& p)
      { return windowsPluginsDir().getChildFile ("vst2/" + p + "-vst.so").getFullPathName(); } },
    { "VST3", "VST3", [] (const juce::String& p)
      { return windowsPluginsDir().getChildFile ("vst3/" + p + ".vst3").getFullPathName(); } },
    { plugins::ClapPluginFormat::formatName, "CLAP", [] (const juce::String& p)
      { return windowsPluginsDir().getChildFile ("clap/" + p + ".clap").getFullPathName(); } },
};

const juce::StringArray windowsPlugins { "DragonflyHallReverb", "DragonflyRoomReverb" };

juce::AudioPluginFormat* findFormat (te::Engine& engine, const juce::String& name)
{
    for (auto* format : engine.getPluginManager().pluginFormatManager.getFormats())
        if (format->getName() == name)
            return format;

    return nullptr;
}

double secondsSince (std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
}

std::unique_ptr<juce::PluginDescription> describe (te::Engine& engine, const juce::String& formatName,
                                                   const juce::String& path, double* seconds = nullptr)
{
    auto* format = findFormat (engine, formatName);
    REQUIRE (format != nullptr);

    const auto start = std::chrono::steady_clock::now();
    juce::OwnedArray<juce::PluginDescription> found;
    format->findAllTypesForFile (found, path);

    if (seconds != nullptr)
        *seconds = secondsSince (start);

    INFO (formatName << ": " << path);
    REQUIRE (found.size() == 1);
    engine.getPluginManager().knownPluginList.addType (*found[0]);
    return std::make_unique<juce::PluginDescription> (*found[0]);
}

struct RenderResult
{
    float rms = 0.0f;
    double loadSeconds = 0.0;
    double meanLoadPercent = 0.0;
    int overruns = 0;
};

/** Plays a tone through `instances` copies of the plugin (one per track) in real time and measures
    output level and DSP load. Real-time pacing matters: the bridge talks to a Wine process. */
RenderResult render (te::Engine& engine, const juce::PluginDescription& desc, int instances)
{
    engine.getPluginManager().createBuiltInType<te::ToneGeneratorPlugin>();
    RealtimeRunner runner (engine, { .outputChannels = 2, .maxSeconds = 6.0, .paceInRealTime = true });
    auto edit = te::Edit::createSingleTrackEdit (engine);
    edit->getMasterVolumePlugin()->setVolumeDb (0.0f - 20.0f * std::log10 ((float)instances));

    RenderResult result;
    const auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < instances; ++i)
    {
        auto tracks = te::getAudioTracks (*edit);
        auto track =
            i == 0 ? tracks[0] : edit->insertNewAudioTrack (te::TrackInsertPoint (nullptr, tracks.getLast()), nullptr);

        track->pluginList.insertPlugin (
            edit->getPluginCache().createNewPlugin (te::ToneGeneratorPlugin::xmlTypeName, {}), 0, nullptr);

        auto plugin = edit->getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, desc);
        auto* external = dynamic_cast<te::ExternalPlugin*> (plugin.get());
        REQUIRE (external != nullptr);
        INFO ("Load error: " << external->getLoadError());
        REQUIRE (external->getAudioPluginInstance() != nullptr);
        track->pluginList.insertPlugin (plugin, 1, nullptr);
    }

    result.loadSeconds = secondsSince (start);

    edit->dispatchPendingUpdatesSynchronously();
    edit->getTransport().ensureContextAllocated();
    edit->getTransport().play (false);
    runner.start();
    runner.runUntil (5.0);
    runner.stop();
    edit->getTransport().stop (false, true);

    const auto sr = runner.getParams().sampleRate;
    result.rms = checks::rms (runner.getChannel (0), (int64_t)(1.0 * sr), (int64_t)(4.0 * sr));

    const auto& times = runner.getBlockTimesMs();
    std::vector<float> steady (times.begin() + (long)(times.size() / 5), times.end());
    double sum = 0.0;
    for (auto t : steady)
    {
        sum += t;
        result.overruns += t > runner.getBlockPeriodMs() ? 1 : 0;
    }

    result.meanLoadPercent = 100.0 * sum / (double)steady.size() / runner.getBlockPeriodMs();
    return result;
}

} // namespace

//==============================================================================
TEST_CASE ("Plugin hosting: Windows plugins load through yabridge in all three formats", "[hosting][windows]")
{
    if (! windowsPluginsDir().isDirectory())
        SKIP ("Set SPINUS_CHECK_WINDOWS_PLUGINS (see tools/owner-checks/windows-plugins-setup.sh)");

    for (auto& name : windowsPlugins)
    {
        float nativeLevel = 0.0f;
        {
            EngineHost host ({ .openAudioDevice = false });
            const auto clap = juce::File (SPINUS_PLUGINS_DIR).getChildFile (name + ".clap").getFullPathName();
            nativeLevel =
                render (host.getEngine(), *describe (host.getEngine(), plugins::ClapPluginFormat::formatName, clap), 1)
                    .rms;
        }

        for (auto& bridged : bridgedFormats)
        {
            EngineHost host ({ .openAudioDevice = false });
            double scanSeconds = 0.0;
            auto desc = describe (host.getEngine(), bridged.format, bridged.path (name), &scanSeconds);
            const auto result = render (host.getEngine(), *desc, 1);

            const auto label = name.toStdString() + " (Windows " + bridged.label + ")";
            metric (label + ": scan s / load s",
                    juce::String (scanSeconds, 2) + " / " + juce::String (result.loadSeconds, 2));
            metric (label + ": rms bridged / native Linux CLAP",
                    juce::String (result.rms, 4) + " / " + juce::String (nativeLevel, 4));
            metric (label + ": DSP load % (1 instance) / overruns",
                    juce::String (result.meanLoadPercent, 1) + " / " + juce::String (result.overruns));

            CHECK (result.rms > 0.01f);
            CHECK (std::abs (result.rms - nativeLevel) < 0.1f * nativeLevel);
        }
    }
}

TEST_CASE ("Plugin hosting: Windows plugin state round trip through yabridge", "[hosting][windows]")
{
    if (! windowsPluginsDir().isDirectory())
        SKIP ("Set SPINUS_CHECK_WINDOWS_PLUGINS (see tools/owner-checks/windows-plugins-setup.sh)");

    for (auto& bridged : bridgedFormats)
    {
        EngineHost host ({ .openAudioDevice = false });
        auto desc = describe (host.getEngine(), bridged.format, bridged.path ("DragonflyHallReverb"));
        auto& formats = host.getEngine().getPluginManager().pluginFormatManager;

        juce::String error;
        auto a = formats.createPluginInstance (*desc, 48000.0, 512, error);
        auto b = formats.createPluginInstance (*desc, 48000.0, 512, error);
        REQUIRE ((a != nullptr && b != nullptr));

        const auto& params = a->getParameters();
        for (int i = 0; i < params.size(); i += 3)
            params[i]->setValueNotifyingHost (0.3f);

        // Bridged and CLAP plugins apply parameter changes on the next processing call.
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        for (auto* p : { a.get(), b.get() })
            p->prepareToPlay (48000.0, 512);
        a->processBlock (buffer, midi);

        juce::MemoryBlock state;
        a->getStateInformation (state);
        REQUIRE (state.getSize() > 0);
        b->setStateInformation (state.getData(), (int)state.getSize());
        b->processBlock (buffer, midi);

        int differing = 0;
        for (int i = 0; i < params.size(); ++i)
            if (std::abs (params[i]->getValue() - b->getParameters()[i]->getValue()) > 1.0e-3f)
                ++differing;

        metric (std::string ("Dragonfly Hall (Windows ") + bridged.label +
                    "): parameters / state bytes / differing after restore",
                juce::String (params.size()) + " / " + juce::String ((int)state.getSize()) + " / " +
                    juce::String (differing));
        CHECK (differing == 0);

        for (auto* p : { a.get(), b.get() })
            p->releaseResources();
    }
}

TEST_CASE ("Plugin hosting: Eight bridged Windows plugin instances in real time", "[hosting][windows][realtime]")
{
    if (! windowsPluginsDir().isDirectory())
        SKIP ("Set SPINUS_CHECK_WINDOWS_PLUGINS (see tools/owner-checks/windows-plugins-setup.sh)");

    for (auto& bridged : bridgedFormats)
    {
        EngineHost host ({ .openAudioDevice = false });
        auto desc = describe (host.getEngine(), bridged.format, bridged.path ("DragonflyRoomReverb"));
        const auto result = render (host.getEngine(), *desc, 8);

        metric (std::string ("8 x Dragonfly Room (Windows ") + bridged.label + "): load s / DSP load % / overruns",
                juce::String (result.loadSeconds, 1) + " / " + juce::String (result.meanLoadPercent, 1) + " / " +
                    juce::String (result.overruns));
        CHECK (result.rms > 0.01f);
    }
}
