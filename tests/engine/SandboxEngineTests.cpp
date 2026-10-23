#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "session/Session.h"

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
