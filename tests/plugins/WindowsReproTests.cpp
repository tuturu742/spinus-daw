// Hidden repro tests for Windows plugins (Wine + yabridge) in a live-like engine. Not run by CI: they need
// a set-up Windows folder with real plugins. Run with
//   SPINUS_REPRO_WINDOWS=<windows folder> [SPINUS_REPRO_PLUGIN=ABP.vst3] [SPINUS_REPRO_PROJECT=<folder>]
//   spinus_tests "[.windows-repro]"
#include <catch2/catch_test_macros.hpp>

#include "RealtimeRunner.h"
#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "plugins/SandboxList.h"
#include "plugins/windows/WindowsPlugins.h"
#include "project/ProjectFiles.h"
#include "session/Session.h"

#include <csignal>
#include <iostream>

using namespace spinus;

namespace
{
juce::String env (const char* name) { return juce::SystemStats::getEnvironmentVariable (name, {}); }

/** Aborts (so a debugger stops with every thread's stack) when the audio thread makes no progress. */
struct Watchdog
{
    explicit Watchdog (test::RealtimeRunner& r) : runner (r)
    {
        thread = std::thread (
            [this]
            {
                auto last = runner.getSamplesProcessed();
                auto since = std::chrono::steady_clock::now();
                while (! done)
                {
                    std::this_thread::sleep_for (std::chrono::milliseconds (100));
                    const auto now = runner.getSamplesProcessed();
                    if (now != last)
                    {
                        last = now;
                        since = std::chrono::steady_clock::now();
                    }
                    else if (std::chrono::steady_clock::now() - since > std::chrono::seconds (limitSeconds))
                    {
                        std::cerr << "WATCHDOG: no audio block finished for " << limitSeconds << " s\n";
                        std::raise (SIGTRAP);
                        since = std::chrono::steady_clock::now();
                    }
                }
            });
    }
    ~Watchdog()
    {
        done = true;
        thread.join();
    }
    test::RealtimeRunner& runner;
    std::atomic<bool> done { false };
    int limitSeconds =
        env ("SPINUS_REPRO_WATCHDOG").getIntValue() > 0 ? env ("SPINUS_REPRO_WATCHDOG").getIntValue() : 30;
    std::thread thread;
};

struct Fixture
{
    Fixture()
    {
        folder = juce::File (env ("SPINUS_REPRO_WINDOWS"));
        REQUIRE (folder.isDirectory());
        windows = std::make_unique<plugins::WindowsPlugins> (folder, true);
        const auto runtime = windows->getRuntime();
        REQUIRE (runtime.has_value());
        if (runtime)
            plugins::WindowsPlugins::applyEnvironment (*runtime);

        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        session = std::make_unique<Session> (host.getEngine(), std::move (devices));
    }

    /** Adds every bridged VST3 to the engine's list, as a scan would (loads each once). */
    void scan (const juce::StringArray& only = {})
    {
        auto& manager = host.getEngine().getPluginManager();
        juce::AudioPluginFormat* vst3 = nullptr;
        for (auto* f : manager.pluginFormatManager.getFormats())
            if (f->getName() == "VST3")
                vst3 = f;
        REQUIRE (vst3 != nullptr);
        for (const auto& entry : juce::RangedDirectoryIterator (folder.getChildFile ("bridged/vst3"), false, "*.vst3",
                                                                juce::File::findDirectories))
        {
            if (! only.isEmpty() && ! only.contains (entry.getFile().getFileName()))
                continue;
            juce::OwnedArray<juce::PluginDescription> found;
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            vst3->findAllTypesForFile (found, entry.getFile().getFullPathName());
            std::cerr << "scanned " << entry.getFile().getFileName() << ": " << found.size() << " in "
                      << (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0 << " s\n";
            for (auto* d : found)
            {
                manager.knownPluginList.addType (*d);
                descriptions.add (*d);
            }
        }
    }

    juce::var run (const juce::String& name, const juce::String& params)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
        INFO (name << ": " << error);
        REQUIRE (command != nullptr);
        auto* raw = command.get();
        const auto result = session->getHistory().perform (std::move (command));
        INFO (result.getErrorMessage());
        REQUIRE (result.wasOk());
        return raw->getResult();
    }

    juce::File folder;
    std::unique_ptr<plugins::WindowsPlugins> windows;
    EngineHost host { { .openAudioDevice = false, .pluginHost = juce::File (SPINUS_PLUGIN_HOST) } };
    std::unique_ptr<Session> session;
    juce::Array<juce::PluginDescription> descriptions;
};

void report (const test::RealtimeRunner& runner, const char* what)
{
    const auto& times = runner.getBlockTimesMs();
    float worst = 0.0f;
    for (auto t : times)
        worst = std::max (worst, t);
    std::cerr << what << ": " << times.size() << " blocks, " << runner.getNumOverrunBlocks() << " over budget, worst "
              << worst << " ms, message thread stall " << runner.getMaxMessageThreadStallMs() << " ms\n";
    // Loudness of each second played so far (channel 1): silence means the plugins made no sound.
    const auto& left = runner.getChannel (0);
    const auto rate = (int64_t)runner.getParams().sampleRate;
    std::cerr << "  rms per second:";
    for (int64_t s = 0; (s + 1) * rate <= runner.getNumRecordedSamples(); ++s)
    {
        double sum = 0.0;
        for (int64_t i = s * rate; i < (s + 1) * rate; ++i)
            sum += (double)left[(size_t)i] * left[(size_t)i];
        std::cerr << " " << juce::String (std::sqrt (sum / (double)rate), 4);
    }
    std::cerr << "\n";
}
} // namespace

TEST_CASE ("Windows repro: add and remove a bridged instrument while playing", "[.windows-repro]")
{
    Fixture f;
    const auto pluginFile = env ("SPINUS_REPRO_PLUGIN").isNotEmpty() ? env ("SPINUS_REPRO_PLUGIN") : "ABP.vst3";
    f.scan ({ pluginFile });
    REQUIRE (! f.descriptions.isEmpty());
    const auto& d = f.descriptions.getReference (0);

    test::RealtimeRunner runner (f.host.getEngine(), { .outputChannels = 2, .maxSeconds = 300.0 });
    Watchdog watchdog (runner);
    runner.start();
    f.session->play();
    runner.runUntil (0.5);

    const int rounds = env ("SPINUS_REPRO_ROUNDS").getIntValue() > 0 ? env ("SPINUS_REPRO_ROUNDS").getIntValue() : 4;
    for (int i = 0; i < rounds; ++i)
    {
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const auto added = f.run (
            "channel.add",
            R"({"name": "Bass", "instrument": {"kind": "plugin", "plugin": {"format": "VST3", "name": )" +
                juce::JSON::toString (d.name) + R"(, "manufacturer": )" + juce::JSON::toString (d.manufacturerName) +
                R"(, "fileOrIdentifier": )" + juce::JSON::toString (d.fileOrIdentifier) + R"(}}})");
        std::cerr << "round " << i << ": added in " << (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0
                  << " s\n";
        runner.runUntil (runner.getSecondsProcessed() + 3.0);
        if (env ("SPINUS_REPRO_KEEP").isEmpty())
        {
            f.run ("channel.remove", R"({"channel": )" + added["channel"].toString() + "}");
            runner.runUntil (runner.getSecondsProcessed() + 1.0);
        }
    }
    report (runner, "add/remove");
    runner.stop();
}

TEST_CASE ("Windows repro: open a project with bridged plugins while the engine plays", "[.windows-repro]")
{
    Fixture f;
    f.scan();
    const juce::File projectFolder (env ("SPINUS_REPRO_PROJECT"));
    project::ProjectData data;
    const auto loaded = project::loadProject (projectFolder, data);
    INFO (loaded.getErrorMessage());
    REQUIRE (loaded.wasOk());

    test::RealtimeRunner runner (f.host.getEngine(), { .outputChannels = 2, .maxSeconds = 300.0 });
    Watchdog watchdog (runner);
    runner.start(); // the sound card runs while the project opens, as in Spinus
    runner.runUntil (0.5);

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    f.session->setProjectFolder (projectFolder);
    f.session->loadProject (std::move (data));
    std::cerr << "project opened in " << (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0 << " s\n";
    for (auto& channel : f.session->getProject().channels)
        if (channel.instrument)
        {
            auto* external = dynamic_cast<tracktion::ExternalPlugin*> (
                f.session->getAdapter().getDevicePlugin (channel.instrument->id));
            std::cerr << "  " << channel.name << ": "
                      << (external == nullptr                             ? "no plugin"
                          : external->getAudioPluginInstance() == nullptr ? "not loaded"
                                                                          : "loaded")
                      << "\n";
        }
    report (runner, "while opening");
    runner.resetMaxMessageThreadStall();
    f.session->setMode (Session::Mode::song);
    f.session->play();
    runner.runUntil (runner.getSecondsProcessed() + (env ("SPINUS_REPRO_SECONDS").getIntValue() > 0
                                                         ? env ("SPINUS_REPRO_SECONDS").getIntValue()
                                                         : 20));
    report (runner, "playing");
    runner.stop();
}

TEST_CASE ("Windows repro: add a bridged instrument to an empty project while stopped", "[.windows-repro]")
{
    Fixture f;
    const auto pluginFile = env ("SPINUS_REPRO_PLUGIN").isNotEmpty() ? env ("SPINUS_REPRO_PLUGIN") : "ABP.vst3";
    f.scan ({ pluginFile });
    REQUIRE (! f.descriptions.isEmpty());
    const auto& d = f.descriptions.getReference (0);
    REQUIRE (f.session->getProject().patterns.empty()); // "No pattern", as a new project
    REQUIRE (f.session->getMode() == Session::Mode::pattern);

    test::RealtimeRunner runner (f.host.getEngine(), { .outputChannels = 2, .maxSeconds = 300.0 });
    Watchdog watchdog (runner);
    runner.start();
    runner.runUntil (0.5);
    const int rounds = env ("SPINUS_REPRO_ROUNDS").getIntValue() > 0 ? env ("SPINUS_REPRO_ROUNDS").getIntValue() : 4;
    for (int i = 0; i < rounds; ++i)
    {
        f.run ("channel.add",
               R"({"name": "Bass", "instrument": {"kind": "plugin", "plugin": {"format": "VST3", "name": )" +
                   juce::JSON::toString (d.name) + R"(, "manufacturer": )" + juce::JSON::toString (d.manufacturerName) +
                   R"(, "fileOrIdentifier": )" + juce::JSON::toString (d.fileOrIdentifier) + R"(}}})");
        runner.runUntil (runner.getSecondsProcessed() + 2.0);
    }
    report (runner, "stopped adds");
    runner.stop();
}
