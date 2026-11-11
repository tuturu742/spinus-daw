#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "project/ProjectFiles.h"
#include "session/ProjectDocument.h"
#include "session/Session.h"
#include "ui/MainView.h"

using namespace spinus;

namespace
{
constexpr double rate = 48000.0;

juce::File writeTone (const juce::File& file, double seconds)
{
    juce::AudioBuffer<float> audio (1, (int)(seconds * rate));
    for (int i = 0; i < audio.getNumSamples(); ++i)
        audio.setSample (0, i, 0.5f * (float)std::sin (2.0 * M_PI * 440.0 * i / rate));
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (16));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

/** A song with a long tone through an insert that has the Crash Test effect (gain 50 %), which runs in a
    separate process once the list says so. */
struct Fixture
{
    Fixture()
    {
        root.createDirectory();
        // The engine knows the plugin, as a scan would have found it.
        plugins::ClapPluginFormat clap;
        juce::OwnedArray<juce::PluginDescription> found;
        const auto file = juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("crash-test.clap").getFullPathName();
        clap.findAllTypesForFile (found, file);
        REQUIRE (found.size() == 1);
        host.getEngine().getPluginManager().knownPluginList.addType (*found[0]);

        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        devices->setRunSeparately (
            [this] (const juce::PluginDescription& d)
            { return list.contains (plugins::SandboxList::keyOf (d.pluginFormatName, d.fileOrIdentifier)); });
        session = std::make_unique<Session> (host.getEngine(), std::move (devices));
        session->setSandboxList (&list);

        const auto lane = run ("lane.add", "{}")["lane"].toString();
        const auto made =
            run ("clip.addAudio",
                 R"({"lane": )" + lane + R"(, "file": )" +
                     juce::JSON::toString (writeTone (root.getChildFile ("tone.wav"), 20.0).getFullPathName()) +
                     R"(, "start": 0, "length": 38400, "name": "Tone"})");
        device = model::DeviceId { (uint32_t)(int)run (
            "insert.addEffect",
            R"({"insert": )" + made["insert"].toString() +
                R"(, "device": {"kind": "plugin", "plugin": {"format": "CLAP", "name": "Crash Test", "manufacturer": "Spinus tests", "fileOrIdentifier": )" +
                juce::JSON::toString (file) + R"(}}})")["device"] };
        run ("device.setParameters", R"({"device": )" + juce::String (device.value) + R"(, "values": {"0": 0.5}})");
    }

    ~Fixture()
    {
        session = nullptr;
        root.deleteRecursively();
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

    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-sandbox-engine") + "-" + juce::Uuid().toString());
    EngineHost host { { .openAudioDevice = false, .pluginHost = juce::File (SPINUS_PLUGIN_HOST) } };
    plugins::SandboxList list { root.getChildFile ("run-separately.json") };
    std::unique_ptr<Session> session;
    model::DeviceId device;
};
} // namespace

TEST_CASE ("A plugin moved into its own process keeps playing, and a crash there leaves the song playing",
           "[engine][sandbox][audio]")
{
    Fixture f;
    auto& session = *f.session;
    CHECK (! session.runsSeparately (f.device));

    // Into its own process: remembered for the plugin, settings kept.
    session.setRunSeparately (f.device, true);
    CHECK (session.runsSeparately (f.device));
    CHECK (f.list.contains (plugins::SandboxList::keyOf (
        "CLAP", juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("crash-test.clap").getFullPathName())));
    CHECK (plugins::SandboxList (f.root.getChildFile ("run-separately.json")).getPlugins().size() == 1);

    test::RealtimeRunner runner (f.host.getEngine(),
                                 { .outputChannels = 2, .maxSeconds = 10.0, .paceInRealTime = true });
    const auto sr = runner.getParams().sampleRate;
    auto level = [&] (double from, double to)
    {
        REQUIRE (to <= runner.getSecondsProcessed()); // within what was played (slow steps can use it up)
        return test::checks::rms (runner.getChannel (0), (int64_t)(from * sr), (int64_t)(to * sr));
    };
    runner.start();
    session.setMode (Session::Mode::song);
    session.play();
    runner.runUntil (1.5);
    const auto through = level (0.8, 1.4);
    CHECK (through > 0.05f);

    // It crashes: reported once; the song plays on, the effect passed by (louder: no 50 % gain).
    f.run ("device.setParameters", R"({"device": )" + juce::String (f.device.value) + R"(, "values": {"1": 1}})");
    std::vector<model::DeviceId> failed;
    for (int i = 0; i < 100 && failed.empty(); ++i)
    {
        runner.runUntil (runner.getSecondsProcessed() + 0.05);
        failed = session.getAdapter().takeFailedDevices();
    }
    REQUIRE (failed == std::vector<model::DeviceId> { f.device });
    CHECK (session.getAdapter().takeFailedDevices().empty());
    CHECK (session.getAdapter().getStoppedDevices() == std::vector<model::DeviceId> { f.device }); // until reloaded
    const auto after = runner.getSecondsProcessed();
    runner.runUntil (after + 1.0);
    CHECK (level (after + 0.3, after + 0.9) > through * 1.5f);

    // Without the crash switch, Reload brings it back at 50 %.
    session.getHistory().undo();
    session.reloadDevice (f.device);
    CHECK (session.runsSeparately (f.device));
    CHECK (session.getAdapter().getStoppedDevices().empty());
    const auto reloaded = runner.getSecondsProcessed();
    runner.runUntil (reloaded + 1.0);
    CHECK (std::abs (level (reloaded + 0.5, reloaded + 0.95) - through) < through * 0.2f);
    session.stop();
    runner.stop();

    // And back inside Spinus.
    session.setRunSeparately (f.device, false);
    CHECK (! session.runsSeparately (f.device));
    CHECK (f.list.getPlugins().empty());
}

TEST_CASE ("Exports wait for plugins in their own process", "[engine][sandbox]")
{
    Fixture f;
    f.session->setRunSeparately (f.device, true);
    f.session->setMode (Session::Mode::song);
    engine::ExportSettings settings;
    settings.format = engine::ExportSettings::Format::wav32Float;
    settings.tailSeconds = 0.0;
    std::optional<engine::ExportJob::Result> result;
    const auto file = f.root.getChildFile ("Song.wav");
    auto job = f.session->createExportJob (file, settings, [&] (const engine::ExportJob::Result& r) { result = r; });
    job->start();
    for (int i = 0; i < 1200 && ! result; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    REQUIRE (result.has_value());
    INFO (result.value_or (engine::ExportJob::Result {}).error);
    REQUIRE (result.value_or (engine::ExportJob::Result {}).ok);

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (new juce::FileInputStream (file), true));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio (1, (int)reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, false);
    // The tone at 50 % all through (no gaps from a plugin that could not keep up).
    const auto expected = 0.5f * 0.5f / std::sqrt (2.0f);
    for (int second = 0; second < 9; ++second)
    {
        const auto t = 0.5 + second;
        CHECK (std::abs (audio.getRMSLevel (0, (int)(t * rate), (int)(0.4 * rate)) - expected) < expected * 0.25f);
    }
}

TEST_CASE ("An export waits for an instrument in its own process that loads its sounds in the background",
           "[engine][sandbox][export]")
{
    // The busy synth stays silent for its first 3 seconds, as big sample libraries do while they load, and is slow
    // to answer meanwhile (1.5 s a block), as through a bridge. In a separate process too, an export waits until it
    // sounds: the song's first note must not be lost, and the plugin is not taken for hung (it answers each
    // block well within the 5 s an offline render gives it).
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-sandbox-export-" + juce::Uuid().toString());
    root.createDirectory();
    EngineHost host ({ .openAudioDevice = false, .pluginHost = juce::File (SPINUS_PLUGIN_HOST) });
    plugins::ClapPluginFormat clap;
    juce::OwnedArray<juce::PluginDescription> found;
    const auto file = juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("busy-synth.clap");
    clap.findAllTypesForFile (found, file.getFullPathName());
    const juce::PluginDescription* synth = nullptr;
    for (auto* d : found)
        if (d->name == "Spinus Busy Synth")
            synth = d;
    REQUIRE (synth != nullptr);
    host.getEngine().getPluginManager().knownPluginList.addType (*synth);

    plugins::SandboxList list { root.getChildFile ("run-separately.json") };
    list.set (plugins::SandboxList::keyOf ("CLAP", file.getFullPathName()), true, synth->name);
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    devices->setRunSeparately (
        [&list] (const juce::PluginDescription& d)
        { return list.contains (plugins::SandboxList::keyOf (d.pluginFormatName, d.fileOrIdentifier)); });
    auto session = std::make_unique<Session> (host.getEngine(), std::move (devices));
    session->setSandboxList (&list);
    auto run = [&] (const juce::String& name, const juce::String& params)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
        INFO (name << ": " << error);
        REQUIRE (command != nullptr);
        auto* raw = command.get();
        REQUIRE (session->getHistory().perform (std::move (command)).wasOk());
        return raw->getResult();
    };
    const auto channel = run (
        "channel.add",
        R"({"name": "Busy", "instrument": {"kind": "plugin", "plugin": {"format": "CLAP", "name": "Spinus Busy Synth", "manufacturer": )" +
            juce::JSON::toString (synth->manufacturerName) + R"(, "fileOrIdentifier": )" +
            juce::JSON::toString (file.getFullPathName()) + "}}}");
    const auto pattern = run ("pattern.add", R"({"name": "P", "length": 3840})");
    run ("pattern.setNotes", R"({"pattern": )" + pattern["pattern"].toString() + R"(, "channel": )" +
                                 channel["channel"].toString() +
                                 R"(, "notes": [{"start": 0, "length": 3600, "pitch": 60, "velocity": 0.9}]})");
    const auto lane = run ("lane.add", "{}");
    run ("clip.addPattern", R"({"lane": )" + lane["lane"].toString() + R"(, "pattern": )" +
                                pattern["pattern"].toString() + R"(, "start": 0})");
    session->setMode (Session::Mode::song);
    session->getEdit().dispatchPendingUpdatesSynchronously();

    engine::ExportSettings settings;
    settings.format = engine::ExportSettings::Format::wav32Float;
    settings.sampleRate = 48000.0;
    settings.tailSeconds = 0.5;
    std::optional<engine::ExportJob::Result> result;
    const auto out = root.getChildFile ("Song.wav");
    auto job = session->createExportJob (out, settings, [&] (const engine::ExportJob::Result& r) { result = r; });
    job->start();
    for (int i = 0; i < 1200 && ! result; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    job = nullptr;
    REQUIRE (result.has_value());
    INFO (result.value_or (engine::ExportJob::Result {}).error);
    REQUIRE (result.value_or (engine::ExportJob::Result {}).ok);

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (new juce::FileInputStream (out), true));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio (1, (int)reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, false);
    // The note from its first beat, and all through its 1.9 seconds.
    for (const double from : { 0.05, 0.5, 1.0, 1.5 })
        CHECK (audio.getRMSLevel (0, (int)(from * rate), (int)(0.3 * rate)) > 0.01f);
    session = nullptr;
    root.deleteRecursively();
}

TEST_CASE ("A plugin crashing in its own process: the window names it, the project still saves and opens with it",
           "[engine][sandbox][ui]")
{
    Fixture f;
    f.session->setRunSeparately (f.device, true);
    ProjectDocument document (*f.session, f.root.getChildFile ("settings"));
    ui::MainView view (*f.session, &document);
    juce::StringArray titles, messages;
    view.setMessageBoxHandler (
        [&] (const juce::MessageBoxOptions& o, const std::function<void (int)>& done)
        {
            titles.add (o.getTitle());
            messages.add (o.getMessage());
            if (done)
                done (1); // "Leave it off"
        });

    // It crashes while the song plays: the main window says so, by name, once.
    test::RealtimeRunner runner (f.host.getEngine(),
                                 { .outputChannels = 2, .maxSeconds = 10.0, .paceInRealTime = true });
    runner.start();
    f.session->setMode (Session::Mode::song);
    f.session->play();
    runner.runUntil (0.5);
    f.run ("device.setParameters", R"({"device": )" + juce::String (f.device.value) + R"(, "values": {"1": 1}})");
    for (int i = 0; i < 200 && titles.isEmpty(); ++i)
    {
        runner.runUntil (runner.getSecondsProcessed() + 0.05);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    }
    f.session->stop();
    runner.stop();
    REQUIRE (titles == juce::StringArray { "A plugin stopped working" });
    CHECK (messages[0].contains ("Crash Test"));

    // The project saves, and opens again with the plugin and the settings it had (gain 50 %).
    f.session->getHistory().undo(); // the crash switch
    REQUIRE (view.saveProjectTo (f.root.getChildFile ("After crash")));
    CHECK (titles.size() == 1); // no problem shown
    REQUIRE (document.open (f.root.getChildFile ("After crash")).wasOk());
    const auto* device = f.session->getProject().findDevice (f.device);
    REQUIRE (device != nullptr);
    CHECK (device->plugin.name == "Crash Test");
    CHECK (f.session->getAdapter().getStoppedDevices().empty()); // loaded again
    auto* plugin = dynamic_cast<tracktion::ExternalPlugin*> (f.session->getAdapter().getDevicePlugin (f.device));
    REQUIRE (plugin != nullptr);
    REQUIRE (plugin->getAudioPluginInstance() != nullptr);
    CHECK (std::abs (plugin->getAudioPluginInstance()->getParameters()[0]->getValue() - 0.5f) < 0.01f);
}

TEST_CASE ("A plugin crashing in its own process during an export: the export says which, and keeps no file",
           "[engine][sandbox][export]")
{
    Fixture f;
    f.session->setRunSeparately (f.device, true);
    f.session->setMode (Session::Mode::song);
    f.run ("device.setParameters", R"({"device": )" + juce::String (f.device.value) + R"(, "values": {"1": 1}})");
    engine::ExportSettings settings;
    settings.format = engine::ExportSettings::Format::wav32Float;
    settings.tailSeconds = 0.0;
    std::optional<engine::ExportJob::Result> result;
    const auto file = f.root.getChildFile ("Song.wav");
    auto job = f.session->createExportJob (file, settings, [&] (const engine::ExportJob::Result& r) { result = r; });
    job->start();
    for (int i = 0; i < 1200 && ! result; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    job = nullptr;
    REQUIRE (result.has_value());
    const auto r = result.value_or (engine::ExportJob::Result {});
    INFO (r.error);
    // Not a file that silently lacks the plugin's sound.
    CHECK (! r.ok);
    CHECK (r.error.startsWith ("Crash Test stopped working"));
    CHECK (! file.existsAsFile());

    // Spinus goes on: the song plays (the live plugin was not touched), and the project still saves.
    CHECK (f.session->getAdapter().getStoppedDevices().empty());
    CHECK (project::saveProject (f.root.getChildFile ("Saved"), { f.session->getProject(), {} }).wasOk());

    // Without the crash, the export is made.
    f.session->getHistory().undo();
    result.reset();
    job = f.session->createExportJob (file, settings, [&] (const engine::ExportJob::Result& r2) { result = r2; });
    job->start();
    for (int i = 0; i < 1200 && ! result; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    job = nullptr;
    REQUIRE (result.has_value());
    CHECK (result.value_or (engine::ExportJob::Result {}).ok);
    CHECK (file.existsAsFile());
}
