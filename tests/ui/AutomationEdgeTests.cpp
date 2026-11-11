/*  Automation edge cases (rc.1 checklist, section 1): mixer faders, a plugin's setting, clips that are moved,
    made longer, cut, copied, muted, deleted and undone, the device they control deleted, replaced or moved to
    another slot, save and reopen, export, loops and jumps of the playhead. */

#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "commands/CommandRegistry.h"
#include "engine/Beats.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "engine/Exporter.h"
#include "model/Automation.h"
#include "model/ModelJson.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "project/ProjectFiles.h"
#include "ui/ChannelRack.h"
#include "ui/Mixer.h"
#include "ui/Playlist.h"

using namespace spinus;
using namespace spinus::ui;
namespace te = tracktion;

namespace
{
using Kind = model::ParameterTarget::Kind;
constexpr model::Ticks beat = model::ticksPerQuarterNote, bar = 4 * beat;

void settle (int ms = 100) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

struct Fixture
{
    Fixture()
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerSpinusDevices (*devices, host.getEngine());
        session = std::make_unique<Session> (host.getEngine(), std::move (devices));
        rack = std::make_unique<ChannelRack> (*session);
        playlist = std::make_unique<Playlist> (*session);
        mixer = std::make_unique<Mixer> (*session);
        playlist->setSize (900, 400);
        REQUIRE (rack->addChannel ("spinus.3x-osc", "Lead"));
        REQUIRE (mixer->addEffect (lead().targetInsert, "spinus.filter"));
        session->setMode (Session::Mode::song);
    }

    ~Fixture()
    {
        mixer = nullptr;
        playlist = nullptr;
        rack = nullptr;
        session = nullptr;
    }

    const model::Channel& lead() const { return session->getProject().channels.front(); }
    model::DeviceId synth() const { return lead().instrument.value_or (model::Device {}).id; }
    const model::Insert& leadInsert() const { return *session->getProject().findInsert (lead().targetInsert); }
    model::DeviceId filter() const { return leadInsert().effects.front().device.id; }

    static model::ParameterTarget deviceParameter (model::DeviceId device, const juce::String& id)
    {
        model::ParameterTarget t;
        t.kind = Kind::deviceParameter;
        t.device = device;
        t.parameterId = id;
        return t;
    }

    model::ParameterTarget cutoff() const { return deviceParameter (filter(), "cutoff"); }

    te::AutomatableParameter* parameter (const model::ParameterTarget& t) const
    {
        return session->getAdapter().findParameter (t);
    }

    /** The automation's value (0..1) at a time, as the engine plays it. */
    float at (const model::ParameterTarget& t, double beats) const
    {
        auto* p = parameter (t);
        REQUIRE (p != nullptr);
        const auto time = session->getEdit().tempoSequence.toTime (te::BeatPosition::fromBeats (beats));
        return p->valueRange.convertTo0to1 (p->getCurve().getValueAt (time, p->getCurrentExplicitValue()));
    }

    /** The knob's own value (0..1): what was set by hand or command. */
    float knob (const model::ParameterTarget& t) const
    {
        auto* p = parameter (t);
        REQUIRE (p != nullptr);
        return p->valueRange.convertTo0to1 (p->getCurrentExplicitValue());
    }

    static juce::String targetJson (const model::ParameterTarget& t)
    {
        return juce::JSON::toString (model::targetToJson (t), true);
    }

    /** An automation clip on a new lane, or on `lane`. */
    model::ClipId addClip (const model::ParameterTarget& t, model::Ticks start, model::Ticks length,
                           const juce::String& points, std::optional<model::LaneId> lane = {})
    {
        juce::String json = "{\"start\": " + juce::String (start) + ", \"length\": " + juce::String (length) +
                            ", \"target\": " + targetJson (t) + ", \"points\": " + points;
        if (lane)
            json << ", \"lane\": " << (int)lane->value;
        json << "}";
        const auto result = performWithResult ("clip.addAutomation", json);
        return model::ClipId { (uint32_t)(int)result["clip"] };
    }

    juce::var performWithResult (const juce::String& name, const juce::String& json)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (json), error);
        INFO (name << ": " << error);
        REQUIRE (command != nullptr);
        auto* raw = command.get();
        const auto result = session->getHistory().perform (std::move (command));
        INFO (result.getErrorMessage());
        REQUIRE (result.wasOk());
        return session->getHistory().canUndo() ? raw->getResult() : juce::var();
    }

    bool perform (const juce::String& name, const juce::String& json)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (json), error);
        INFO (error);
        return command != nullptr && session->getHistory().perform (std::move (command)).wasOk();
    }

    /** A note on every step of a pattern, placed over `bars` bars. */
    void playNotes (int bars)
    {
        for (int step = 0; step < 16; ++step)
            REQUIRE (rack->toggleStep (lead().id, step));
        for (int b = 0; b < bars; ++b)
            REQUIRE (playlist->placePattern (0, b * bar));
    }

    EngineHost host { { .openAudioDevice = false } };
    std::unique_ptr<Session> session;
    std::unique_ptr<ChannelRack> rack;
    std::unique_ptr<Playlist> playlist;
    std::unique_ptr<Mixer> mixer;
};

bool near (float a, float b, float tolerance = 0.01f) { return std::abs (a - b) < tolerance; }

const juce::String rampUp = R"([{"time": 0, "value": 0}, {"time": 15360, "value": 1}])"; // 0 -> 1 over 4 bars
} // namespace

//==============================================================================
TEST_CASE ("Mixer faders: automation plays in its clip; outside it, the fader as it is set", "[ui][automation-edge]")
{
    Fixture f;
    auto check = [&] (const model::ParameterTarget& target, const std::function<bool()>& moveByHand)
    {
        INFO ("target kind " << (int)target.kind);
        f.addClip (target, bar, bar, R"([{"time": 0, "value": 0.1}])");
        const auto before = f.knob (target);
        REQUIRE (! near (before, 0.1f, 0.05f));
        CHECK (near (f.at (target, 1.0), before));
        CHECK (near (f.at (target, 6.0), 0.1f));
        CHECK (near (f.at (target, 12.0), before));

        // Dragged outside the clip: after it, the new place counts; in it, still the clip.
        REQUIRE (moveByHand());
        settle();
        const auto moved = f.knob (target);
        CHECK (! near (moved, before));
        CHECK (near (f.at (target, 1.0), moved));
        CHECK (near (f.at (target, 12.0), moved));
        CHECK (near (f.at (target, 6.0), 0.1f));
    };

    model::ParameterTarget t;
    t.kind = Kind::insertVolume;
    t.insert = f.lead().targetInsert;
    check (t, [&] { return f.mixer->setInsertVolume (t.insert, -12.0); });
    t.kind = Kind::insertPan;
    check (t, [&] { return f.mixer->setInsertPan (t.insert, 0.5); });
    t = {};
    t.kind = Kind::channelVolume;
    t.channel = f.lead().id;
    check (t, [&] { return f.rack->setChannelVolume (t.channel, -12.0); });
    t.kind = Kind::channelPan;
    check (t, [&] { return f.rack->setChannelPan (t.channel, -0.5); });
    // The master's fader.
    t = {};
    t.kind = Kind::insertVolume;
    t.insert = model::masterInsertId;
    check (t, [&] { return f.mixer->setInsertVolume (model::masterInsertId, -6.0); });
}

TEST_CASE ("Moving, lengthening, cutting and copying an automation clip moves its sound with it",
           "[ui][automation-edge]")
{
    Fixture f;
    const auto t = f.cutoff();
    const auto knob = f.knob (t);
    // 0 -> 1 over one bar.
    const auto id = f.addClip (t, 0, bar, R"([{"time": 0, "value": 0}, {"time": 3840, "value": 1}])");
    const auto clip = [&] { return juce::String ((int)id.value); };
    CHECK (near (f.at (t, 2.0), 0.5f));

    SECTION ("Move")
    {
        REQUIRE (f.perform ("clip.set", "{\"clip\": " + clip() + ", \"start\": 7680}"));
        CHECK (near (f.at (t, 2.0), knob));
        CHECK (near (f.at (t, 10.0), 0.5f));
        CHECK (near (f.at (t, 14.0), knob));
    }
    SECTION ("Longer: the last value holds to the new end")
    {
        REQUIRE (f.perform ("clip.set", "{\"clip\": " + clip() + ", \"length\": 7680}"));
        CHECK (near (f.at (t, 6.0), 1.0f));
        CHECK (near (f.at (t, 9.0), knob));
    }
    SECTION ("Cut")
    {
        REQUIRE (f.perform ("clip.split", "{\"clip\": " + clip() + ", \"at\": 1920}"));
        CHECK (near (f.at (t, 1.0), 0.25f));
        CHECK (near (f.at (t, 2.0), 0.5f));
        CHECK (near (f.at (t, 3.0), 0.75f));
        CHECK (near (f.at (t, 8.0), knob));
    }
    SECTION ("Copy")
    {
        auto copy = model::clipToJson (*f.session->getProject().findClip (id));
        copy.getDynamicObject()->removeProperty ("id");
        copy.getDynamicObject()->setProperty ("start", (int)(4 * bar));
        auto params = juce::var (new juce::DynamicObject());
        params.getDynamicObject()->setProperty ("clip", copy);
        REQUIRE (f.perform ("clip.add", juce::JSON::toString (params)));
        CHECK (near (f.at (t, 2.0), 0.5f));
        CHECK (near (f.at (t, 18.0), 0.5f));
        CHECK (near (f.at (t, 10.0), 1.0f)); // held between the two
    }
}

TEST_CASE ("Muting or deleting a clip gives the knob back; undo and redo", "[ui][automation-edge]")
{
    Fixture f;
    const auto t = f.cutoff();
    REQUIRE (f.perform ("device.setParameters",
                        "{\"device\": " + juce::String ((int)f.filter().value) + ", \"values\": {\"cutoff\": 0.3}}"));
    settle(); // the plugin tells Tracktion
    const auto id = f.addClip (t, 0, 4 * bar, R"([{"time": 0, "value": 0.9}])");
    const auto clip = juce::String ((int)id.value);
    auto* parameter = f.parameter (t);
    auto& curve = parameter->getCurve();
    REQUIRE (curve.getNumPoints() > 0);
    CHECK (near (f.at (t, 2.0), 0.9f));

    // While it plays: the setting follows the clip.
    test::RealtimeRunner runner (f.host.getEngine(), { .outputChannels = 2, .maxSeconds = 6.0 });
    runner.start();
    f.session->play();
    runner.runUntil (0.4);
    settle (50);
    CHECK (near (parameter->getCurrentNormalisedValue(), 0.9f));

    SECTION ("Mute")
    {
        REQUIRE (f.perform ("clip.set", "{\"clip\": " + clip + ", \"muted\": true}"));
        CHECK (curve.getNumPoints() == 0);
        runner.runUntil (0.8);
        settle (50);
        CHECK (near (parameter->getCurrentNormalisedValue(), 0.3f)); // where the knob was left
    }
    SECTION ("Delete, undo, redo")
    {
        REQUIRE (f.perform ("clip.remove", "{\"clip\": " + clip + "}"));
        CHECK (curve.getNumPoints() == 0);
        runner.runUntil (0.8);
        settle (50);
        CHECK (near (parameter->getCurrentNormalisedValue(), 0.3f));

        REQUIRE (f.session->getHistory().undo());
        REQUIRE (f.session->getProject().findClip (id) != nullptr);
        CHECK (near (f.at (t, 2.0), 0.9f));
        runner.runUntil (1.2);
        settle (50);
        CHECK (near (parameter->getCurrentNormalisedValue(), 0.9f));

        REQUIRE (f.session->getHistory().redo());
        CHECK (f.session->getProject().findClip (id) == nullptr);
        CHECK (curve.getNumPoints() == 0);
        runner.runUntil (1.6);
        settle (50);
        CHECK (near (parameter->getCurrentNormalisedValue(), 0.3f));
    }
    f.session->stop();
    runner.stop();
}

TEST_CASE ("Deleting the automated device takes its clips; undo brings both back, working", "[ui][automation-edge]")
{
    Fixture f;
    SECTION ("An effect")
    {
        const auto device = f.filter();
        const auto id = f.addClip (f.cutoff(), 0, bar, R"([{"time": 0, "value": 0.9}])");
        REQUIRE (f.mixer->removeEffect (device));
        CHECK (f.session->getProject().findClip (id) == nullptr);
        CHECK (f.session->getAdapter().getDevicePlugin (device) == nullptr);
        settle();

        REQUIRE (f.session->getHistory().undo());
        REQUIRE (f.session->getProject().findClip (id) != nullptr);
        REQUIRE (f.parameter (Fixture::deviceParameter (device, "cutoff")) != nullptr);
        CHECK (near (f.at (Fixture::deviceParameter (device, "cutoff"), 2.0), 0.9f));
        REQUIRE (f.session->getHistory().redo());
        CHECK (f.session->getProject().findClip (id) == nullptr);
    }
    SECTION ("A channel with its instrument")
    {
        const auto device = f.synth();
        const auto target = Fixture::deviceParameter (device, "cutoff");
        const auto id = f.addClip (target, 0, bar, R"([{"time": 0, "value": 0.9}])");
        REQUIRE (f.perform ("channel.remove", "{\"channel\": " + juce::String ((int)f.lead().id.value) + "}"));
        CHECK (f.session->getProject().findClip (id) == nullptr);
        settle();
        REQUIRE (f.session->getHistory().undo());
        REQUIRE (f.session->getProject().findClip (id) != nullptr);
        CHECK (near (f.at (target, 2.0), 0.9f));
    }
}

TEST_CASE ("Replacing the automated device removes its clips: the new one's same-named knob is not moved",
           "[ui][automation-edge]")
{
    Fixture f;
    SECTION ("Replace effect")
    {
        const auto old = f.filter();
        const auto id = f.addClip (f.cutoff(), 0, bar, R"([{"time": 0, "value": 0.9}])");
        model::Device filter;
        filter.kind = model::Device::Kind::stock;
        filter.stockType = "spinus.filter";
        REQUIRE (f.mixer->replaceEffect (old, filter));
        CHECK (f.session->getProject().findClip (id) == nullptr);
        auto* now = f.parameter (f.cutoff());
        REQUIRE (now != nullptr);
        CHECK (now->getCurve().getNumPoints() == 0);
        REQUIRE (f.session->getHistory().undo());
        CHECK (f.filter() == old);
        CHECK (near (f.at (f.cutoff(), 2.0), 0.9f));
    }
    SECTION ("Replace instrument")
    {
        const auto old = f.synth();
        const auto id = f.addClip (Fixture::deviceParameter (old, "cutoff"), 0, bar, R"([{"time": 0, "value": 0.9}])");
        model::Device osc;
        osc.kind = model::Device::Kind::stock;
        osc.stockType = "spinus.3x-osc";
        REQUIRE (f.rack->replaceInstrument (f.lead().id, osc, "3x Osc"));
        CHECK (f.session->getProject().findClip (id) == nullptr);
        auto* now = f.parameter (Fixture::deviceParameter (f.synth(), "cutoff"));
        REQUIRE (now != nullptr);
        CHECK (now->getCurve().getNumPoints() == 0);
        REQUIRE (f.session->getHistory().undo());
        CHECK (f.synth() == old);
        CHECK (near (f.at (Fixture::deviceParameter (old, "cutoff"), 2.0), 0.9f));
    }
}

TEST_CASE ("Automation goes with an effect moved to another slot", "[ui][automation-edge]")
{
    Fixture f;
    const auto device = f.filter();
    const auto t = f.cutoff();
    f.addClip (t, 0, bar, R"([{"time": 0, "value": 0.9}])");
    REQUIRE (f.mixer->addEffect (f.lead().targetInsert, "spinus.eq"));

    REQUIRE (f.mixer->moveEffect (device, f.lead().targetInsert, 1));
    REQUIRE (f.leadInsert().effects.back().device.id == device);
    CHECK (near (f.at (t, 2.0), 0.9f));

    CHECK (! near (f.at (t, 8.0), 0.9f));
    // (To the Master or an insert further left: tests/ui/MixerTests.cpp, once that fix is in.)
}

TEST_CASE ("Automation, and a knob set outside its clips, are saved and opened again", "[ui][automation-edge]")
{
    juce::TemporaryFile folder (".spinus");
    const auto& projectFolder = folder.getFile();
    model::ParameterTarget cutoff, volume;
    {
        Fixture f;
        cutoff = f.cutoff();
        volume.kind = Kind::insertVolume;
        volume.insert = f.lead().targetInsert;
        REQUIRE (f.perform ("device.setParameters", "{\"device\": " + juce::String ((int)f.filter().value) +
                                                        ", \"values\": {\"cutoff\": 0.3}}"));
        REQUIRE (f.mixer->setInsertVolume (volume.insert, -12.0));
        f.addClip (cutoff, bar, bar, R"([{"time": 0, "value": 0.9}, {"time": 3840, "value": 0.5}])");
        f.addClip (volume, bar, bar, R"([{"time": 0, "value": 0.1}])");
        settle();
        CHECK (near (f.at (cutoff, 2.0), 0.3f));
        f.session->setProjectFolder (projectFolder);
        REQUIRE (project::saveProject (projectFolder, f.session->captureProjectData()).wasOk());
    }

    Fixture f;
    project::ProjectData data;
    REQUIRE (project::loadProject (projectFolder, data).wasOk());
    f.session->setProjectFolder (projectFolder);
    f.session->loadProject (std::move (data));
    f.session->setMode (Session::Mode::song);
    settle();
    CHECK (near (f.at (cutoff, 2.0), 0.3f));
    CHECK (near (f.at (cutoff, 4.0), 0.9f));
    CHECK (near (f.at (cutoff, 6.0), 0.7f));
    CHECK (near (f.at (cutoff, 12.0), 0.3f));
    const auto fader = te::decibelsToVolumeFaderPosition (-12.0f);
    CHECK (near (f.at (volume, 2.0), fader));
    CHECK (near (f.at (volume, 6.0), 0.1f));
    CHECK (near (f.at (volume, 12.0), fader));
    projectFolder.deleteRecursively();
}

TEST_CASE ("Export plays automation as playback does, from the first sample", "[ui][automation-edge][audio]")
{
    Fixture f;
    f.playNotes (3);
    // Silent (fader down) before and after a clip at full volume in bar 2.
    model::ParameterTarget volume;
    volume.kind = Kind::insertVolume;
    volume.insert = f.lead().targetInsert;
    REQUIRE (f.mixer->setInsertVolume (volume.insert, -100.0));
    f.addClip (volume, bar, bar,
               "[{\"time\": 0, \"value\": " + juce::String (te::decibelsToVolumeFaderPosition (0.0f)) + "}]");
    settle();

    const auto file = f.host.getEngine().getTemporaryFileManager().getTempDirectory().getChildFile (
        "automation-export-" + juce::Uuid().toString() + ".wav");
    engine::ExportSettings settings;
    settings.format = engine::ExportSettings::Format::wav32Float;
    std::optional<engine::ExportJob::Result> result;
    auto job = f.session->createExportJob (file, settings, [&] (const engine::ExportJob::Result& r) { result = r; });
    job->start();
    for (int i = 0; i < 1200 && ! result; ++i)
        settle (50);
    REQUIRE (result.has_value());
    REQUIRE (result.value_or (engine::ExportJob::Result {}).ok);

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (new juce::FileInputStream (file), true));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
    const auto sr = reader->sampleRate;
    reader.reset();
    file.deleteFile();
    auto exported = [&] (double from, double to)
    { return audio.getRMSLevel (0, (int)(from * sr), (int)((to - from) * sr)); };

    // Playback of the same.
    test::RealtimeRunner runner (f.host.getEngine(), { .outputChannels = 2, .maxSeconds = 8.0 });
    runner.start();
    runner.runUntil (0.2);
    const auto start = runner.getSecondsProcessed();
    f.session->play();
    runner.runUntil (start + 6.0);
    f.session->stop();
    runner.stop();
    const auto psr = runner.getParams().sampleRate;
    auto played = [&] (double from, double to)
    {
        return test::checks::rms (runner.getChannel (0), (int64_t)((start + from) * psr),
                                  (int64_t)((start + to) * psr));
    };

    // 120 BPM: bar 1 is 0-2 s, bar 2 2-4 s, bar 3 4-6 s.
    CHECK (exported (0.0, 0.05) < 0.0005f); // the very first moment: the fader's own value
    CHECK (exported (0.0, 1.9) < 0.0005f);
    CHECK (exported (2.1, 3.9) > 0.01f);
    CHECK (exported (4.1, 5.9) < 0.0005f);
    CHECK (played (0.0, 0.05) < 0.0005f);
    CHECK (played (0.0, 1.9) < 0.0005f);
    CHECK (played (2.1, 3.9) > 0.01f);
    CHECK (played (4.1, 5.9) < 0.0005f);
    CHECK (std::abs (juce::Decibels::gainToDecibels (exported (2.1, 3.9)) -
                     juce::Decibels::gainToDecibels (played (2.1, 3.9))) < 1.5f);
}

TEST_CASE ("Playback that starts, loops or jumps in the middle of a clip takes its value at once",
           "[ui][automation-edge][audio]")
{
    Fixture f;
    f.playNotes (4);
    const auto t = f.cutoff();
    f.addClip (t, 0, 4 * bar, rampUp);
    auto* parameter = f.parameter (t);
    REQUIRE (parameter != nullptr);
    auto& transport = f.session->getEdit().getTransport();

    test::RealtimeRunner runner (f.host.getEngine(), { .outputChannels = 2, .maxSeconds = 8.0 });
    runner.start();
    runner.runUntil (0.1);

    SECTION ("Start in the middle")
    {
        transport.setPosition (engine::ticksToTime (f.session->getEdit(), 6.0 * beat));
        f.session->play();
        const auto from = runner.getSecondsProcessed();
        runner.runUntil (from + 0.03);
        CHECK (near (parameter->getCurrentNormalisedValue(), 6.0f / 16.0f, 0.02f));
    }
    SECTION ("A loop that starts in the middle")
    {
        f.session->setLoopRegion (Session::Mode::song, Session::LoopRegion { 8 * beat, 10 * beat });
        transport.setPosition (engine::ticksToTime (f.session->getEdit(), 8.0 * beat));
        f.session->play();
        const auto from = runner.getSecondsProcessed();
        runner.runUntil (from + 0.03);
        CHECK (near (parameter->getCurrentNormalisedValue(), 8.0f / 16.0f, 0.02f));
        // Second pass, just after the loop's start (two beats take 1 s).
        runner.runUntil (from + 1.03);
        CHECK (near (parameter->getCurrentNormalisedValue(), 8.0f / 16.0f, 0.02f));
    }
    SECTION ("A jump while playing")
    {
        f.session->play();
        runner.runUntil (runner.getSecondsProcessed() + 0.3);
        transport.setPosition (engine::ticksToTime (f.session->getEdit(), 12.0 * beat));
        const auto from = runner.getSecondsProcessed();
        runner.runUntil (from + 0.03);
        CHECK (near (parameter->getCurrentNormalisedValue(), 12.0f / 16.0f, 0.02f));
    }
    f.session->stop();
    runner.stop();
}

namespace
{
/** The test effect in param-ids.clap (Drive, Tone, Mix, Level), installed. */
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

} // namespace

TEST_CASE ("A plugin's automated setting: its clip plays; outside it, its knob as set", "[ui][automation-edge]")
{
    Fixture f;
    REQUIRE (f.mixer->addEffect (f.lead().targetInsert, installParamIdsPlugin (f.host.getEngine())));
    const auto device = f.leadInsert().effects.back().device.id;
    const auto tone = Fixture::deviceParameter (device, "1"); // by position (its CLAP ID is 1000)
    REQUIRE (f.parameter (tone) != nullptr);
    CHECK (near (f.knob (tone), 0.2f));
    f.addClip (tone, bar, bar, R"([{"time": 0, "value": 0.9}])");
    CHECK (near (f.at (tone, 1.0), 0.2f));
    CHECK (near (f.at (tone, 6.0), 0.9f));
    CHECK (near (f.at (tone, 12.0), 0.2f));

    // Set outside the clip (as the AI, a script or a typed value does): before and after the clip.
    REQUIRE (f.perform ("device.setParameters",
                        "{\"device\": " + juce::String ((int)device.value) + ", \"values\": {\"1\": 0.6}}"));
    settle (200);
    CHECK (near (f.at (tone, 1.0), 0.6f));
    CHECK (near (f.at (tone, 12.0), 0.6f));
    CHECK (near (f.at (tone, 6.0), 0.9f));
    // (A knob dragged in a CLAP plugin's own window: tests/session/DeviceParametersTests.cpp, once that fix is in.)
}

TEST_CASE ("The Mixer's fader and pan knob move with their automation while it plays", "[ui][automation-edge][audio]")
{
    Fixture f;
    f.mixer->setSize (900, 500);
    f.mixer->setVisible (true);
    const auto insert = f.lead().targetInsert;
    model::ParameterTarget volume, pan;
    volume.kind = Kind::insertVolume;
    volume.insert = insert;
    pan = volume;
    pan.kind = Kind::insertPan;
    const auto low = te::decibelsToVolumeFaderPosition (-24.0f);
    f.addClip (volume, 0, bar, "[{\"time\": 0, \"value\": " + juce::String (low) + "}]");
    f.addClip (pan, 0, bar, R"([{"time": 0, "value": 0.0}])"); // all the way left

    test::RealtimeRunner runner (f.host.getEngine(), { .outputChannels = 2, .maxSeconds = 4.0 });
    runner.start();
    f.session->play();
    runner.runUntil (0.5);
    settle (200);
    // Some strip shows -24 dB and full left: the lead's.
    bool faderMoved = false, panMoved = false;
    std::function<void (juce::Component&)> find = [&] (juce::Component& c)
    {
        if (auto* s = dynamic_cast<juce::Slider*> (&c))
        {
            if (s->getHelpText().startsWith ("Insert volume") && std::abs (s->getValue() - -24.0) < 0.5)
                faderMoved = true;
            if (s->getHelpText().startsWith ("Insert pan") && std::abs (s->getValue() - -1.0) < 0.02)
                panMoved = true;
        }
        for (auto* child : c.getChildren())
            find (*child);
    };
    find (*f.mixer);
    CHECK (faderMoved);
    CHECK (panMoved);
    f.session->stop();
    runner.stop();
}

TEST_CASE ("A plugin's knob outside its clip stays put when another of its settings changes while the clip plays",
           "[ui][automation-edge][audio]")
{
    Fixture f;
    REQUIRE (f.mixer->addEffect (f.lead().targetInsert, installParamIdsPlugin (f.host.getEngine())));
    const auto device = f.leadInsert().effects.back().device.id;
    const auto tone = Fixture::deviceParameter (device, "1");
    REQUIRE (f.parameter (tone) != nullptr);
    f.addClip (tone, 0, bar, R"([{"time": 0, "value": 0.9}])");
    CHECK (near (f.at (tone, 8.0), 0.2f));

    test::RealtimeRunner runner (f.host.getEngine(), { .outputChannels = 2, .maxSeconds = 8.0 });
    runner.start();
    f.session->play();
    runner.runUntil (0.5);
    settle (100);
    CHECK (near (f.parameter (tone)->getCurrentNormalisedValue(), 0.9f));
    // Drive changed by a command (as the AI or a script does) while Tone's clip plays.
    REQUIRE (f.perform ("device.setParameters",
                        "{\"device\": " + juce::String ((int)device.value) + ", \"values\": {\"0\": 0.7}}"));
    runner.runUntil (runner.getSecondsProcessed() + 0.3);
    settle (200);
    CHECK (near (f.knob (tone), 0.2f));
    CHECK (near (f.at (tone, 8.0), 0.2f)); // after the clip: still Tone's knob, not the clip's value
    f.session->stop();
    runner.stop();
}

TEST_CASE ("A step setting (the Filter's Slope) follows its line: on a ramp it changes halfway, on a jump at the point",
           "[ui][automation-edge][audio]")
{
    // As in FL Studio: a line between two points that differ is followed and rounded to the nearest choice. To
    // change exactly on a point, the line jumps there (two points at the same place, or the Square shape).
    Fixture f;
    const auto slope = Fixture::deviceParameter (f.filter(), "slope"); // 12 dB, 24 dB
    auto* external = dynamic_cast<te::ExternalPlugin*> (f.session->getAdapter().getDevicePlugin (f.filter()));
    REQUIRE (external != nullptr);
    juce::AudioParameterChoice* choice = nullptr;
    for (auto* p : external->getAudioPluginInstance()->getParameters())
        if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (p); c != nullptr && c->paramID == "slope")
            choice = c;
    REQUIRE (choice != nullptr);

    // Bar 1: a ramp from 12 dB to 24 dB. Bar 2: 12 dB, then a jump to 24 dB on its third beat.
    f.addClip (slope, 0, bar, R"([{"time": 0, "value": 0}, {"time": 3840, "value": 1}])");
    f.addClip (slope, bar, bar, R"([{"time": 0, "value": 0}, {"time": 1919, "value": 0}, {"time": 1920, "value": 1}])");

    test::RealtimeRunner runner (f.host.getEngine(), { .outputChannels = 2, .maxSeconds = 6.0 });
    runner.start();
    auto indexAt = [&] (double beats)
    {
        f.session->stop();
        f.session->getEdit().getTransport().setPosition (engine::ticksToTime (f.session->getEdit(), beats * beat));
        f.session->play();
        runner.runUntil (runner.getSecondsProcessed() + 0.05);
        settle (20);
        return choice->getIndex();
    };
    CHECK (indexAt (0.5) == 0);
    CHECK (indexAt (1.6) == 0); // the ramp is below halfway
    CHECK (indexAt (2.2) == 1); // past halfway
    CHECK (indexAt (5.5) == 0); // the jump's bar: 12 dB up to its point
    CHECK (indexAt (6.1) == 1);
    f.session->stop();
    runner.stop();
}
