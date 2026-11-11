#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "commands/CommandRegistry.h"
#include "dsp/sampler/SamplerProcessor.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "session/ProjectDocument.h"
#include "ui/ChannelRack.h"

using namespace spinus;

namespace
{

constexpr double rate = 48000.0;

void requireOk (const juce::Result& r)
{
    INFO (r.getErrorMessage());
    REQUIRE (r.wasOk());
}

/** A Sampler channel holding one long note of a long sample (a big device state to write), in a saved project. */
struct Fixture
{
    Fixture()
        : root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("spinus-autosave-test-" + juce::Uuid().toString())),
          session (host.getEngine(), makeDevices (host.getEngine())), rack (session)
    {
        root.createDirectory();
        document = std::make_unique<ProjectDocument> (session, root.getChildFile ("settings"));

        REQUIRE (rack.addChannel ("spinus.sampler", "Sampler"));
        const auto& channel = session.getProject().channels.back();
        const auto device = channel.instrument.value_or (model::Device {}).id;
        auto* plugin = dynamic_cast<tracktion::ExternalPlugin*> (session.getAdapter().getDevicePlugin (device));
        REQUIRE (plugin != nullptr);
        auto* sampler = dynamic_cast<dsp::SamplerProcessor*> (plugin->getAudioPluginInstance());
        REQUIRE (sampler != nullptr);

        // 20 seconds of a 220 Hz tone: about 2 MB of FLAC in the state each autosave writes.
        juce::AudioBuffer<float> tone (2, (int)(20.0 * rate));
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < tone.getNumSamples(); ++i)
                tone.setSample (c, i,
                                0.4f * (float)std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / rate));
        sampler->addZone (tone, rate, "Tone");

        // One note as long as the 4-bar pattern, so the tone never stops while the pattern loops.
        const auto pattern = session.getCurrentPattern().isValid() ? session.getCurrentPattern() : addPattern();
        auto* params = new juce::DynamicObject();
        params->setProperty ("pattern", (int)pattern.value);
        params->setProperty ("channel", (int)channel.id.value);
        params->setProperty ("notes",
                             juce::JSON::parse (R"([{"start": 0, "length": 15360, "pitch": 60, "velocity": 1}])"));
        juce::String error;
        auto command = commands::CommandRegistry::get().create ("pattern.setNotes", juce::var (params), error);
        INFO (error);
        REQUIRE (command != nullptr);
        requireOk (session.getHistory().perform (std::move (command)));
        requireOk (document->saveAs (root.getChildFile ("Song")));
    }

    ~Fixture()
    {
        document = nullptr;
        root.deleteRecursively();
    }

    static std::unique_ptr<engine::DeviceFactory> makeDevices (tracktion::Engine& engine)
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerSpinusDevices (*devices, engine);
        return devices;
    }

    model::PatternId addPattern()
    {
        juce::String error;
        auto command =
            commands::CommandRegistry::get().create ("pattern.add", juce::JSON::parse (R"({"length": 15360})"), error);
        requireOk (session.getHistory().perform (std::move (command)));
        session.setCurrentPattern (session.getProject().patterns.back().id);
        return session.getCurrentPattern();
    }

    /** Something to autosave: a change that is not heard (a marker on the Playlist). */
    void edit (int n)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (
            "marker.add", juce::JSON::parse ("{\"at\": " + juce::String (960 * (n + 1)) + "}"), error);
        requireOk (session.getHistory().perform (std::move (command)));
    }

    juce::File root;
    EngineHost host { { .openAudioDevice = false } };
    Session session;
    ui::ChannelRack rack;
    std::unique_ptr<ProjectDocument> document;
};

} // namespace

TEST_CASE ("Autosave while the song plays: the sound goes on without a gap", "[session][document][audio]")
{
    Fixture f;
    f.session.setMode (Session::Mode::pattern);
    test::RealtimeRunner runner (f.host.getEngine(),
                                 { .outputChannels = 2, .maxSeconds = 8.0, .paceInRealTime = true });
    f.session.play();
    runner.start();
    runner.runUntil (0.5);

    const project::Autosaves autosaves (f.document->getFolder());
    double slowest = 0.0;
    int saved = 0;
    for (int quarter = 3; quarter < 20; ++quarter)
    {
        const double t = quarter * 0.25;
        f.edit (saved);
        const auto started = juce::Time::getMillisecondCounterHiRes();
        requireOk (f.document->autosave());
        slowest = juce::jmax (slowest, juce::Time::getMillisecondCounterHiRes() - started);
        ++saved;
        runner.runUntil (t);
    }
    runner.runUntil (5.5);
    runner.stop();
    f.session.stop();

    INFO ("autosaves: " << saved << ", the slowest took " << slowest
                        << " ms; blocks over time: " << runner.getNumOverrunBlocks());
    CHECK (autosaves.list().size() == project::Autosaves::keep);

    // No 5 ms stretch of silence anywhere: the tone played through every autosave.
    const auto& left = runner.getChannel (0);
    const auto window = (int64_t)(0.005 * rate);
    int silentWindows = 0;
    for (int64_t start = (int64_t)(0.6 * rate); start + window < (int64_t)(5.4 * rate); start += window)
        if (test::checks::rms (left, start, start + window) < 0.01f)
            ++silentWindows;
    CHECK (silentWindows == 0);
}

TEST_CASE ("Autosave while a song exports: the export is as without it", "[session][document][export]")
{
    Fixture f;
    f.session.setMode (Session::Mode::pattern);

    auto exportOnce = [&] (const juce::File& file, bool autosaving)
    {
        engine::ExportSettings settings;
        settings.format = engine::ExportSettings::Format::wav32Float;
        settings.pattern = f.session.getCurrentPattern();
        settings.tailSeconds = 0.5;
        std::optional<engine::ExportJob::Result> result;
        auto job = f.session.createExportJob (file, settings, [&] (const engine::ExportJob::Result& r) { result = r; });
        job->start();
        int saved = 0;
        for (int i = 0; i < 1200 && ! result; ++i)
        {
            if (autosaving && i % 2 == 0)
            {
                f.edit (saved++);
                requireOk (f.document->autosave());
            }
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        }
        REQUIRE (result.has_value());
        INFO (result->error);
        REQUIRE (result->ok);
        return saved;
    };

    exportOnce (f.root.getChildFile ("Plain.wav"), false);
    const auto saved = exportOnce (f.root.getChildFile ("Autosaving.wav"), true);
    INFO ("autosaves during the export: " << saved);
    CHECK (saved > 0);

    auto read = [] (const juce::File& file)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (
            juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (file), true));
        REQUIRE (reader != nullptr);
        juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->lengthInSamples);
        reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
        return audio;
    };
    const auto plain = read (f.root.getChildFile ("Plain.wav"));
    const auto autosaving = read (f.root.getChildFile ("Autosaving.wav"));
    REQUIRE (plain.getNumSamples() == autosaving.getNumSamples());
    float largest = 0.0f;
    for (int c = 0; c < plain.getNumChannels(); ++c)
        for (int i = 0; i < plain.getNumSamples(); ++i)
            largest = juce::jmax (largest, std::abs (plain.getSample (c, i) - autosaving.getSample (c, i)));
    CHECK (largest < 1.0e-4f);
    CHECK (plain.getRMSLevel (0, 0, plain.getNumSamples()) > 0.05f);
}
