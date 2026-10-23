#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "session/ProjectDocument.h"
#include "session/Session.h"
#include "FakeSoundCard.h"
#include "RealtimeRunner.h"

#include <iostream>

using namespace spinus;

namespace
{
juce::File bundledPlugins()
{
#if defined(SPINUS_PLUGINS_DIR)
    return juce::File (SPINUS_PLUGINS_DIR);
#else
    return {};
#endif
}

std::unique_ptr<engine::DeviceFactory> makeDevices (tracktion::Engine& engine)
{
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerBundledDevices (*devices, engine, { bundledPlugins() });
    return devices;
}

juce::var perform (Session& session, const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
    INFO (name << ": " << error);
    REQUIRE (command != nullptr);
    auto* raw = command.get();
    const auto result = session.getHistory().perform (std::move (command));
    INFO (result.getErrorMessage());
    REQUIRE (result.wasOk());
    return raw->getResult();
}

float peak (const test::RealtimeRunner& runner, double fromSeconds, double toSeconds)
{
    const auto sr = runner.getParams().sampleRate;
    const auto end = std::min<int64_t> (runner.getNumRecordedSamples(), (int64_t)(toSeconds * sr));
    float found = 0.0f;
    for (int c = 0; c < runner.getParams().outputChannels; ++c)
        for (auto i = (int64_t)(fromSeconds * sr); i < end; ++i)
            found = std::max (found, std::abs (runner.getChannel (c)[(size_t)i]));
    return found;
}
} // namespace

// No clicks or clipping while nothing plays: when a project loads, and when the Automixer switches Before
// and After (undoing and redoing its effects).
TEST_CASE ("Nothing is heard while stopped: loading a project, adding effects, undo and redo",
           "[session][silence][surge]")
{
    if (! bundledPlugins().getChildFile ("Surge XT.clap").existsAsFile() ||
        ! bundledPlugins().getChildFile ("lsp-plugins.clap").existsAsFile())
        SKIP ("Surge XT or LSP not downloaded");

    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));

    test::RealtimeRunner runner (host.getEngine(), { .outputChannels = 2, .maxSeconds = 40.0 });
    runner.start();
    runner.runUntil (0.3);

    // A typical song: Surge XT, a reverb bus, compression on an insert.
    const auto surge =
        perform (session, "channel.add",
                 R"({"name": "Lead", "instrument": {"kind": "stock", "stockType": "bundled.surge-xt"}})");
    const auto insert = (int)surge["insert"];
    perform (session, "insert.addEffect",
             R"({"insert": )" + juce::String (insert) +
                 R"(, "device": {"kind": "stock", "stockType": "bundled.lsp.compressor"}})");
    const auto bus = (int)perform (session, "insert.add", R"({"name": "Reverb"})")["insert"];
    perform (session, "insert.addEffect",
             R"({"insert": )" + juce::String (bus) +
                 R"(, "device": {"kind": "stock", "stockType": "bundled.dragonfly.hall"}})");
    runner.runUntil (runner.getSecondsProcessed() + 2.0);
    const auto building = peak (runner, 0.0, runner.getSecondsProcessed());

    // Loading it again, as opening a project does.
    const auto loadStart = runner.getSecondsProcessed();
    session.loadProject ({ session.getProject(), {} });
    runner.runUntil (loadStart + 3.0);
    const auto loading = peak (runner, loadStart, runner.getSecondsProcessed());

    // The Automixer's kind of change on the master (a limiter with input gain), undone and redone as
    // Before and After are switched.
    const auto mixStart = runner.getSecondsProcessed();
    const auto limiter = perform (session, "insert.addEffect",
                                  R"({"insert": 1, "device": {"kind": "stock", "stockType": "bundled.lsp.limiter"}})");
    const auto eq = perform (session, "insert.addEffect",
                             R"({"insert": 1, "device": {"kind": "stock", "stockType": "bundled.lsp.eq"}})");
    runner.runUntil (runner.getSecondsProcessed() + 1.0);
    for (int i = 0; i < 3; ++i)
    {
        REQUIRE (session.getHistory().undo());
        REQUIRE (session.getHistory().undo());
        runner.runUntil (runner.getSecondsProcessed() + 1.0);
        REQUIRE (session.getHistory().redo());
        REQUIRE (session.getHistory().redo());
        runner.runUntil (runner.getSecondsProcessed() + 1.0);
    }
    const auto switching = peak (runner, mixStart, runner.getSecondsProcessed());
    runner.stop();

    INFO ("peaks: building " << building << ", loading " << loading << ", switching " << switching << "; overruns "
                             << runner.getNumOverrunBlocks());
    CHECK (building < 1.0e-4f);
    CHECK (loading < 1.0e-4f);
    CHECK (switching < 1.0e-4f);
    (void)limiter;
    (void)eq;
}

// The Automixer measures the mix by exporting it, and Freeze records in the background: each is an offline
// render of a copy of the song. It must not be heard, and must not stop the song playing.
TEST_CASE ("An export in the background is not heard and does not stop the song", "[session][silence][surge]")
{
    if (! bundledPlugins().getChildFile ("Surge XT.clap").existsAsFile() ||
        ! bundledPlugins().getChildFile ("lsp-plugins.clap").existsAsFile())
        SKIP ("Surge XT or LSP not downloaded");

    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    const auto channel = (int)perform (
        session, "channel.add",
        R"({"name": "Lead", "instrument": {"kind": "stock", "stockType": "bundled.surge-xt"}})")["channel"];
    const auto pattern = (int)perform (session, "pattern.add", R"({"length": 15360})")["pattern"];
    perform (session, "pattern.setNotes",
             "{\"pattern\": " + juce::String (pattern) + ", \"channel\": " + juce::String (channel) +
                 R"(, "notes": [{"start": 0, "length": 15360, "pitch": 60, "velocity": 0.8}]})");
    const auto lane = (int)perform (session, "lane.add", "{}")["lane"];
    perform (session, "clip.addPattern",
             "{\"lane\": " + juce::String (lane) + ", \"pattern\": " + juce::String (pattern) + ", \"start\": 0}");

    test::RealtimeRunner runner (host.getEngine(), { .outputChannels = 2, .maxSeconds = 40.0 });
    runner.start();
    runner.runUntil (0.5);

    const auto folder = juce::File::createTempFile ("silence");
    folder.createDirectory();
    auto exportOnce = [&]
    {
        std::optional<engine::ExportJob::Result> result;
        auto job = session.createExportJob (folder.getChildFile ("mix.wav"), {},
                                            [&] (const engine::ExportJob::Result& r) { result = r; });
        REQUIRE (job != nullptr);
        job->start();
        while (! result)
            runner.runUntil (runner.getSecondsProcessed() + 0.05);
        CHECK (result->ok);
    };

    // Stopped: nothing heard while it renders.
    const auto quietStart = runner.getSecondsProcessed();
    exportOnce();
    runner.runUntil (runner.getSecondsProcessed() + 0.5);
    CHECK (peak (runner, quietStart, runner.getSecondsProcessed()) < 1.0e-4f);

    // Playing: the song carries on through the render.
    session.play();
    runner.runUntil (runner.getSecondsProcessed() + 1.0);
    REQUIRE (session.isPlaying());
    const auto renderStart = runner.getSecondsProcessed();
    exportOnce();
    runner.runUntil (runner.getSecondsProcessed() + 0.3);
    CHECK (session.isPlaying());
    // The held note sounds all the way through: no tenth of a second goes quiet.
    int quietWindows = 0;
    for (int window = 0; renderStart + 0.1 * (window + 1) < runner.getSecondsProcessed(); ++window)
    {
        const auto at = renderStart + 0.1 * window;
        if (peak (runner, at, at + 0.1) < 1.0e-3f)
            ++quietWindows;
    }
    INFO ("render took " << runner.getSecondsProcessed() - renderStart << " s");
    CHECK (quietWindows == 0);
    runner.stop();
    folder.deleteRecursively();
}

// No loud burst when the system's output changes (the sound card is opened again), also in a project with
// something in it.
TEST_CASE ("Nothing is heard when the sound card is opened again", "[session][silence][surge]")
{
    if (! bundledPlugins().getChildFile ("Surge XT.clap").existsAsFile() ||
        ! bundledPlugins().getChildFile ("lsp-plugins.clap").existsAsFile())
        SKIP ("Surge XT or LSP not downloaded");

    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    const auto surge =
        perform (session, "channel.add",
                 R"({"name": "Lead", "instrument": {"kind": "stock", "stockType": "bundled.surge-xt"}})");
    const auto insert = juce::String ((int)surge["insert"]);
    for (auto type : { "bundled.lsp.eq", "bundled.lsp.compressor", "bundled.dragonfly.hall" })
        perform (session, "insert.addEffect",
                 R"({"insert": )" + insert + R"(, "device": {"kind": "stock", "stockType": ")" + type + R"("}})");
    perform (session, "insert.addEffect",
             R"({"insert": 1, "device": {"kind": "stock", "stockType": "bundled.lsp.limiter"}})");

    test::RealtimeRunner runner (host.getEngine(), { .outputChannels = 2, .maxSeconds = 20.0 });
    runner.start();
    runner.runUntil (1.0);
    for (int i = 0; i < 4; ++i)
    {
        runner.restartDevice();
        runner.runUntil (runner.getSecondsProcessed() + 1.0);
    }
    runner.stop();
    const auto loudest = peak (runner, 0.0, runner.getSecondsProcessed());
    INFO ("peak " << loudest);
    CHECK (loudest < 1.0e-4f);
}

// The same through JUCE's real device handling, on a software sound card: switching the output (as when
// the system's default output changes) stops the card and starts the new one.
TEST_CASE ("Nothing is heard when the output is switched", "[session][silence][surge]")
{
    if (! bundledPlugins().getChildFile ("Surge XT.clap").existsAsFile() ||
        ! bundledPlugins().getChildFile ("lsp-plugins.clap").existsAsFile())
        SKIP ("Surge XT or LSP not downloaded");

    EngineHost host { { .openAudioDevice = false } };
    test::FakeSoundCard card;
    auto& devices = host.getEngine().getDeviceManager();
    devices.deviceManager.addAudioDeviceType (card.createType());
    // What Tracktion opens: the software card (never a real one).
    juce::XmlElement saved ("DEVICESETUP");
    saved.setAttribute ("deviceType", test::FakeSoundCard::typeName);
    saved.setAttribute ("audioOutputDeviceName", "Fake output A");
    saved.setAttribute ("audioDeviceRate", 48000.0);
    saved.setAttribute ("audioDeviceBufferSize", 256);
    host.getEngine().getPropertyStorage().setXmlProperty (tracktion::SettingID::audio_device_setup, saved);
    devices.initialise (0, 2);
    REQUIRE (devices.deviceManager.getCurrentAudioDevice() != nullptr);
    REQUIRE (devices.deviceManager.getCurrentAudioDevice()->getTypeName() == test::FakeSoundCard::typeName);

    Session session (host.getEngine(), makeDevices (host.getEngine()));
    const auto surge =
        perform (session, "channel.add",
                 R"({"name": "Lead", "instrument": {"kind": "stock", "stockType": "bundled.surge-xt"}})");
    const auto insert = juce::String ((int)surge["insert"]);
    for (auto type : { "bundled.lsp.eq", "bundled.lsp.compressor", "bundled.dragonfly.hall" })
        perform (session, "insert.addEffect",
                 R"({"insert": )" + insert + R"(, "device": {"kind": "stock", "stockType": ")" + type + R"("}})");
    perform (session, "insert.addEffect",
             R"({"insert": 1, "device": {"kind": "stock", "stockType": "bundled.lsp.limiter"}})");
    // Spinus's own, as the Analyzer's projects have them.
    for (auto type : { "spinus.3x-osc", "spinus.drum-machine", "spinus.sampler", "spinus.beat-slicer" })
    {
        const auto added = perform (
            session, "channel.add",
            juce::String (R"({"name": "Spinus", "instrument": {"kind": "stock", "stockType": ")") + type + R"("}})");
        const auto into = juce::String ((int)added["insert"]);
        for (auto effect :
             { "spinus.filter", "spinus.ducker", "spinus.stereo-utility", "spinus.tune", "spinus.loudness-meter" })
            perform (session, "insert.addEffect",
                     R"({"insert": )" + into + R"(, "device": {"kind": "stock", "stockType": ")" + effect + R"("}})");
    }

    auto wait = [&] (double seconds)
    {
        const auto until = card.getSamplesPlayed() + (int64_t)(seconds * card.getSampleRate());
        while (card.getSamplesPlayed() < until)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
    };
    wait (1.0);
    for (auto output : { "Fake output B", "Fake output A", "Fake output B" })
    {
        auto setup = devices.deviceManager.getAudioDeviceSetup();
        setup.outputDeviceName = output;
        REQUIRE (devices.deviceManager.setAudioDeviceSetup (setup, true).isEmpty());
        wait (1.0);
    }
    devices.deviceManager.closeAudioDevice();
    devices.deviceManager.restartLastAudioDevice();
    wait (1.0);
    CHECK (card.getStarts() >= 5);

    float loudest = 0.0f;
    int64_t at = 0;
    const auto played = card.getRecording();
    for (size_t i = 0; i < played.size(); ++i)
        if (std::abs (played[i]) > loudest)
        {
            loudest = std::abs (played[i]);
            at = (int64_t)i;
        }
    INFO ("loudest " << loudest << " at " << (double)at / card.getSampleRate() << " s of "
                     << (double)played.size() / card.getSampleRate());
    CHECK (loudest < 1.0e-4f);
    devices.deviceManager.closeAudioDevice();
    host.getEngine().getPropertyStorage().removeProperty (tracktion::SettingID::audio_device_setup);
}

// Your own projects, opened and with the output switched, on the software sound card: where is anything
// heard while nothing plays? Run by hand:
//   SPINUS_PROJECTS=~/Music/my-projects build/release/tests/spinus_tests "[.owner-silence]"
TEST_CASE ("Your own projects: anything heard when opening them or switching the output", "[.owner-silence]")
{
    const auto* folderVar = std::getenv ("SPINUS_PROJECTS");
    if (folderVar == nullptr)
        SKIP ("set SPINUS_PROJECTS");

    EngineHost host { { .openAudioDevice = false } };
    test::FakeSoundCard card;
    auto& devices = host.getEngine().getDeviceManager();
    devices.deviceManager.addAudioDeviceType (card.createType());
    juce::XmlElement saved ("DEVICESETUP");
    saved.setAttribute ("deviceType", test::FakeSoundCard::typeName);
    saved.setAttribute ("audioOutputDeviceName", "Fake output A");
    saved.setAttribute ("audioDeviceRate", 48000.0);
    saved.setAttribute ("audioDeviceBufferSize", 256);
    host.getEngine().getPropertyStorage().setXmlProperty (tracktion::SettingID::audio_device_setup, saved);
    devices.initialise (0, 2);
    REQUIRE (devices.deviceManager.getCurrentAudioDevice() != nullptr);
    REQUIRE (devices.deviceManager.getCurrentAudioDevice()->getTypeName() == test::FakeSoundCard::typeName);

    // The installed plugins, from a plugin list (as the app knows them after a scan).
    if (const auto* list = std::getenv ("SPINUS_PLUGIN_LIST"))
        if (auto xml = juce::XmlDocument::parse (juce::File (list)))
            host.getEngine().getPluginManager().knownPluginList.recreateFromXml (*xml);
    Session session (host.getEngine(), makeDevices (host.getEngine()));
    const auto settings = juce::File::createTempFile ("settings");
    ProjectDocument document { session, settings };
    auto wait = [&] (double seconds)
    {
        const auto until = card.getSamplesPlayed() + (int64_t)(seconds * card.getSampleRate());
        while (card.getSamplesPlayed() < until)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
    };
    auto loudestSince = [&] (int64_t from)
    {
        const auto played = card.getRecording();
        float loudest = 0.0f;
        for (auto i = (size_t)from; i < played.size(); ++i)
            loudest = std::max (loudest, std::abs (played[i]));
        return loudest;
    };

    wait (0.5);
    auto folders = juce::File (folderVar).findChildFiles (juce::File::findDirectories, false);
    folders.sort();
    for (const auto& folder : folders)
    {
        const auto beforeOpen = card.getSamplesPlayed();
        const auto opened = document.open (folder);
        wait (2.0);
        const auto openPeak = loudestSince (beforeOpen);
        const auto beforeSwitch = card.getSamplesPlayed();
        for (auto output : { "Fake output B", "Fake output A" })
        {
            auto setup = devices.deviceManager.getAudioDeviceSetup();
            setup.outputDeviceName = output;
            devices.deviceManager.setAudioDeviceSetup (setup, true);
            wait (1.0);
        }
        const auto switchPeak = loudestSince (beforeSwitch);
        std::cout << folder.getFileName() << ": "
                  << (opened.wasOk() ? "" : "(not opened: " + opened.getErrorMessage() + ") ") << "opening "
                  << juce::Decibels::gainToDecibels (openPeak, -150.0f) << " dB, switching "
                  << juce::Decibels::gainToDecibels (switchPeak, -150.0f) << " dB\n";
        document.newProject();
        wait (0.5);
    }
    devices.deviceManager.closeAudioDevice();
    host.getEngine().getPropertyStorage().removeProperty (tracktion::SettingID::audio_device_setup);
    settings.deleteRecursively();
}
