#include <catch2/catch_test_macros.hpp>

#include "../engine/TestDevices.h"
#include "../model/ExampleProject.h"
#include "commands/CommandHistory.h"
#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "model/Freeze.h"
#include "model/ModelJson.h"
#include "project/ProjectFiles.h"
#include "session/Session.h"

using namespace spinus;
using namespace spinus::model;
namespace te = tracktion;

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

// IDs in makeExample(): inserts drums 2, reverb 4, bass 6; channels kick 7 (device 8), synth 9 (device 10).
constexpr ChannelId kick { 7 }, synth { 9 };
constexpr InsertId drums { 2 }, reverb { 4 }, bass { 6 };

//==============================================================================
/** A song with a 3x Osc lead (notes in two bars) on insert "Lead", whose Filter is automated, and a bass
    on its own insert with a Ducker following the lead's insert. */
struct Song
{
    Project project;
    ChannelId lead, low;
    InsertId leadInsert, lowInsert, busInsert;
    DeviceId leadSynth, filter, ducker;
    PatternId pattern;

    Song()
    {
        auto& p = project;
        p.settings.tempoBpm = 120.0; // one bar = 2 seconds

        Insert bus;
        bus.id = busInsert = p.allocateId<InsertId>();
        bus.name = "Bus";

        Insert a;
        a.id = leadInsert = p.allocateId<InsertId>();
        a.name = "Lead";
        a.output = bus.id;
        a.effects.push_back ({ Device { filter = p.allocateId<DeviceId>(), Device::Kind::stock, "spinus.filter" } });

        Insert b;
        b.id = lowInsert = p.allocateId<InsertId>();
        b.name = "Low";
        b.output = bus.id;
        b.volumeDb = -3.0f;
        b.effects.push_back ({ Device { ducker = p.allocateId<DeviceId>(), Device::Kind::stock, "spinus.ducker" } });
        b.sidechainSources.push_back (a.id);
        p.mixer.inserts = { bus, a, b };

        auto synthChannel = [&] (const char* name, InsertId target, DeviceId& device)
        {
            Channel c;
            c.id = p.allocateId<ChannelId>();
            c.name = name;
            c.instrument = Device { device = p.allocateId<DeviceId>(), Device::Kind::stock, "spinus.3x-osc" };
            c.targetInsert = target;
            return c;
        };
        DeviceId lowSynth;
        auto leadChannel = synthChannel ("Lead", a.id, leadSynth);
        leadChannel.volumeDb = -6.0f;
        leadChannel.pan = 0.5f;
        auto lowChannel = synthChannel ("Low", b.id, lowSynth);
        lead = leadChannel.id;
        low = lowChannel.id;
        p.channels = { leadChannel, lowChannel };

        Pattern pt;
        pt.id = pattern = p.allocateId<PatternId>();
        pt.length = 8 * ticksPerQuarterNote;
        for (int beat = 0; beat < 8; ++beat)
            pt.notes[lead].push_back ({ beat * ticksPerQuarterNote, 480, 60 + beat, 0.8f });
        pt.notes[low].push_back ({ 0, 8 * ticksPerQuarterNote, 36, 0.9f });
        p.patterns = { pt };

        Lane lane { p.allocateId<LaneId>(), "Track 1" };
        p.playlist.lanes = { lane };
        p.playlist.clips.push_back ({ p.allocateId<ClipId>(), lane.id, 0, pt.length, false, PatternClip { pt.id, 0 } });

        AutomationClip sweep;
        sweep.target = { ParameterTarget::Kind::deviceParameter, {}, {}, filter, "cutoff" };
        sweep.points = { { 0, 1.0f, 0.0f }, { pt.length, 0.2f, 0.0f } };
        Lane automationLane { p.allocateId<LaneId>(), "Cutoff" };
        p.playlist.lanes.push_back (automationLane);
        p.playlist.clips.push_back ({ p.allocateId<ClipId>(), automationLane.id, 0, pt.length, false, sweep });
    }
};

struct Fixture
{
    Fixture() : host ({ .openAudioDevice = false }), session (host.getEngine(), makeDevices (host.getEngine()))
    {
        session.setMode (Session::Mode::song);
    }

    static std::unique_ptr<engine::DeviceFactory> makeDevices (te::Engine& engine)
    {
        auto devices = std::make_unique<test::TestDevices> (engine);
        engine::registerSpinusDevices (*devices, engine);
        return devices;
    }

    void perform (const juce::String& name, const juce::String& params)
    {
        const auto result = session.getHistory().perform (make (name, params));
        INFO (result.getErrorMessage());
        REQUIRE (result.wasOk());
    }

    /** Waits for the Freezer to finish rendering. */
    void waitForFreeze()
    {
        for (int i = 0; i < 600 && session.getFreezer().isRendering(); ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        REQUIRE (! session.getFreezer().isRendering());
        session.getEdit().dispatchPendingUpdatesSynchronously();
    }

    /** The session's Edit rendered offline, as it plays. */
    juce::AudioBuffer<float> render (double seconds = 5.0)
    {
        session.getEdit().dispatchPendingUpdatesSynchronously();
        const auto file = juce::File::createTempFile (".wav");
        juce::WavAudioFormat wav;
        te::Renderer::Parameters params (session.getEdit());
        params.destFile = file;
        params.tracksToDo = te::toBitSet (te::getAllTracks (session.getEdit()));
        params.audioFormat = &wav;
        params.bitDepth = 32;
        // The rate playback runs at, as Freeze renders (without a device: the engine's default).
        params.sampleRateForAudio = session.getEdit().engine.getDeviceManager().getSampleRate();
        params.blockSizeForAudio = 512;
        params.time = { te::TimePosition(), te::TimePosition::fromSeconds (seconds) };
        params.usePlugins = true;
        params.useMasterPlugins = true;
        params.shouldNormalise = false;
        params.ditheringEnabled = false;
        params.canRenderInMono = false;
        params.checkNodesForAudio = false;

        const auto rendered = te::Renderer::renderToFile ("freeze test", params);
        REQUIRE (rendered.existsAsFile());
        std::unique_ptr<juce::AudioFormatReader> reader (
            wav.createReaderFor (new juce::FileInputStream (rendered), true));
        REQUIRE (reader != nullptr);
        juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->lengthInSamples);
        reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
        reader.reset();
        rendered.deleteFile();
        return audio;
    }

    EngineHost host;
    Session session;
};

/** The largest difference between two renders, and the louder one's peak (to show it is not silence). */
std::pair<float, float> compare (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    REQUIRE (a.getNumChannels() == b.getNumChannels());
    const auto n = std::min (a.getNumSamples(), b.getNumSamples());
    float difference = 0.0f, peak = 0.0f;
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = 0; i < n; ++i)
        {
            difference = std::max (difference, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
            peak = std::max (peak, std::abs (a.getSample (ch, i)));
        }
    return { difference, peak };
}

} // namespace

//==============================================================================
TEST_CASE ("A channel's freeze plan holds only what its instrument plays", "[freeze]")
{
    const auto p = test::makeExample();
    const auto plan = makeFreezePlan (p, kick, std::nullopt);

    REQUIRE (plan.project.channels.size() == 1);
    const auto& c = plan.project.channels.front();
    CHECK (c.id == kick);
    CHECK (c.targetInsert == masterInsertId); // straight out, before the channel's own fader
    CHECK (plan.project.mixer.inserts.empty());
    CHECK (plan.project.mixer.master.effects.empty());
    CHECK (! plan.silent);

    // Both pattern clips play the kick; the audio clip and the reverb's automation are not the kick's.
    CHECK (plan.project.playlist.clips.size() == 2);
    REQUIRE (plan.project.patterns.size() == 1);
    CHECK (plan.project.patterns.front().notes.size() == 1);
    CHECK (plan.project.patterns.front().notes.contains (kick));

    // The same sound gives the same key, whatever else changes.
    const auto key = freezeKey (plan, 48000.0);
    auto changed = p;
    changed.channels[0].name = "Big kick";
    changed.channels[0].volumeDb = -6.0f; // the channel's fader stays live
    changed.channels[1].instrument.reset();
    changed.patterns[0].notes[synth].clear();
    changed.mixer.inserts[0].effects.clear();
    changed.nextId += 10;
    CHECK (freezeKey (makeFreezePlan (changed, kick, std::nullopt), 48000.0) == key);

    // What the kick plays changes it, and so do the tempo and the sample rate.
    changed.patterns[0].notes[kick][0].velocity = 0.5f;
    CHECK (freezeKey (makeFreezePlan (changed, kick, std::nullopt), 48000.0) != key);
    auto faster = p;
    faster.settings.tempoBpm = 150.0;
    CHECK (freezeKey (makeFreezePlan (faster, kick, std::nullopt), 48000.0) != key);
    CHECK (freezeKey (plan, 44100.0) != key);

    // Pattern mode renders the pattern alone; a pattern without the channel's notes is silent.
    const auto patternPlan = makeFreezePlan (p, kick, PatternId { 11 });
    CHECK (patternPlan.project.playlist.clips.empty());
    CHECK (! patternPlan.silent);
    CHECK (freezeKey (patternPlan, 48000.0) != key);
    auto noKick = p;
    noKick.patterns[0].notes.erase (kick);
    CHECK (makeFreezePlan (noKick, kick, PatternId { 11 }).silent);
    CHECK (makeFreezePlan (noKick, kick, std::nullopt).silent);
}

TEST_CASE ("An insert can be frozen unless something inside it also plays elsewhere", "[freeze]")
{
    auto p = test::makeExample();

    // Bass follows Drums as a sidechain: Drums' own output still plays when frozen, so that is fine.
    CHECK (whyCannotFreeze (p, drums).isEmpty());
    CHECK (whyCannotFreeze (p, bass).isEmpty());
    // Drums sends to Reverb but goes out to the master: freezing Reverb would lose that.
    CHECK (whyCannotFreeze (p, reverb).contains ("'Drums' plays into Reverb and also into"));
    CHECK (whyCannotFreeze (p, masterInsertId).isNotEmpty());

    // Something that another insert follows as a sidechain cannot disappear inside a frozen one.
    p.mixer.inserts[0].output = bass; // Drums now plays into Bass, which follows Drums
    p.mixer.inserts[0].sends.clear();
    CHECK (whyCannotFreeze (p, bass).isEmpty()); // the sidechain is inside: rendered with it
    p.mixer.inserts[1].sidechainSources = { drums };
    CHECK (whyCannotFreeze (p, bass).contains ("'Reverb' follows 'Drums' as a sidechain"));
}

TEST_CASE ("An insert's freeze plan plays what flows into it; what feeds its sidechains is unheard", "[freeze]")
{
    const auto p = test::makeExample();
    const auto plan = makeFreezePlan (p, bass, std::nullopt);

    // The synth plays into Bass; the kick plays into Drums, which Bass follows as a sidechain.
    CHECK (plan.project.channels.size() == 2);
    CHECK (plan.unheard == std::set<InsertId> { drums });
    REQUIRE (plan.project.findInsert (bass) != nullptr);
    CHECK (plan.project.findInsert (bass)->output == masterInsertId);
    CHECK (plan.project.findInsert (reverb) == nullptr);
    CHECK (plan.project.findInsert (drums)->sends.empty()); // the reverb is not part of it
}

TEST_CASE ("Freeze commands: one undo step each; an insert that cannot be frozen is refused", "[freeze][commands]")
{
    commands::CommandHistory history (test::makeExample());

    REQUIRE (history.perform (make ("channel.freeze", R"({"channel": 7})")).wasOk());
    CHECK (history.getProject().findChannel (kick)->frozen);
    CHECK (history.getUndoDescriptions()[history.getUndoDescriptions().size() - 1] == "Freeze channel 'Kick'");

    REQUIRE (history.perform (make ("insert.freeze", R"({"insert": 2})")).wasOk());
    CHECK (history.getProject().findInsert (drums)->frozen);

    // Drums plays the kick's sound now: the kick's own freeze waits inside it.
    const auto targets = getEffectiveFreezeTargets (history.getProject());
    REQUIRE (targets.size() == 1);
    CHECK (targets.front() == FreezeTarget { drums });

    const auto refused = history.perform (make ("insert.freeze", R"({"insert": 4})"));
    CHECK (refused.failed());
    CHECK (refused.getErrorMessage().contains ("cannot be frozen"));
    CHECK (history.perform (make ("insert.freeze", R"({"insert": 1})")).failed());

    REQUIRE (history.perform (make ("insert.freeze", R"({"insert": 2, "frozen": false})")).wasOk());
    CHECK (! history.getProject().findInsert (drums)->frozen);
    history.undo();
    history.undo();
    history.undo();
    CHECK (history.getProject() == test::makeExample());
}

TEST_CASE ("Frozen channels and inserts are saved in the project", "[freeze][model]")
{
    auto p = test::makeExample();
    p.channels[0].frozen = true;
    p.mixer.inserts[2].frozen = true;

    Project loaded;
    REQUIRE (fromJsonText (toJsonText (p), loaded).wasOk());
    CHECK (loaded.channels[0].frozen);
    CHECK (! loaded.channels[1].frozen);
    CHECK (loaded.mixer.inserts[2].frozen);
    CHECK (! toJsonText (test::makeExample()).contains ("frozen")); // left out when not frozen
}

//==============================================================================
TEST_CASE ("A frozen channel plays its rendered audio instead of its plugin, and sounds the same", "[freeze]")
{
    Fixture f;
    Song song;
    f.session.getHistory().reset (song.project);
    const auto live = f.render();

    f.perform ("channel.freeze", "{\"channel\": " + juce::String (song.lead.value) + "}");
    CHECK (f.session.getFreezer().getStatus (song.lead).state == Freezer::State::rendering);
    // Until its audio is ready the plugin keeps playing.
    CHECK (f.session.getAdapter().getDevicePlugin (song.leadSynth) != nullptr);

    f.waitForFreeze();
    CHECK (f.session.getFreezer().getStatus (song.lead).state == Freezer::State::frozen);
    CHECK (f.session.getFreezer().getRenderCount() == 1);
    CHECK (f.session.getAdapter().getDevicePlugin (song.leadSynth) == nullptr); // unloaded
    CHECK (f.session.getAdapter().getMissingDevices().empty());                 // not "missing"
    CHECK (f.session.isDeviceFrozen (song.leadSynth));
    CHECK (! f.session.showDeviceWindow (song.leadSynth));
    const auto text = f.session.getAdapter().describe();
    INFO (text);
    CHECK (text.contains ("frozen channel " + juce::String (song.lead.value) + " on 'Lead'"));
    CHECK (! text.contains ("midi clip " + juce::String (song.lead.value + 0))); // no notes played live

    const auto frozen = f.render();
    const auto [difference, peak] = compare (live, frozen);
    CHECK (peak > 0.05f);
    CHECK (difference < 0.001f);

    // Changing the channel's volume does not render again: the fader stays live.
    f.perform ("channel.set", "{\"channel\": " + juce::String (song.lead.value) + ", \"volumeDb\": -12}");
    CHECK (! f.session.getFreezer().isRendering());

    // Unfreeze: the plugin comes back with its settings, and the audio file goes.
    const auto files = f.session.getFreezer().getCacheFolder().findChildFiles (juce::File::findFiles, false, "*.wav");
    CHECK (files.size() == 1);
    f.perform ("channel.freeze", "{\"channel\": " + juce::String (song.lead.value) + ", \"frozen\": false}");
    CHECK (f.session.getAdapter().getDevicePlugin (song.leadSynth) != nullptr);
    CHECK (f.session.getFreezer().getStatus (song.lead).state == Freezer::State::live);
    for (auto& file : files)
        CHECK (! file.existsAsFile());
}

TEST_CASE ("A frozen insert plays everything into it through its effects, and its Ducker still follows", "[freeze]")
{
    Fixture f;
    Song song;
    f.session.getHistory().reset (song.project);
    const auto live = f.render();

    // Bus has the lead and the low parts in it, with the Filter's automation and the Ducker.
    f.perform ("insert.freeze", "{\"insert\": " + juce::String (song.busInsert.value) + "}");
    f.waitForFreeze();
    CHECK (f.session.getFreezer().getStatus (song.busInsert).state == Freezer::State::frozen);
    for (auto device : { song.leadSynth, song.filter, song.ducker })
        CHECK (f.session.getAdapter().getDevicePlugin (device) == nullptr);

    const auto frozen = f.render();
    const auto [difference, peak] = compare (live, frozen);
    CHECK (peak > 0.05f);
    CHECK (difference < 0.001f);

    // Only the low part's insert: the lead's insert plays unheard into its Ducker while rendering, and
    // live it keeps playing (it is not inside Low).
    f.perform ("insert.freeze", "{\"insert\": " + juce::String (song.busInsert.value) + ", \"frozen\": false}");
    f.perform ("insert.freeze", "{\"insert\": " + juce::String (song.lowInsert.value) + "}");
    f.waitForFreeze();
    CHECK (f.session.getAdapter().getDevicePlugin (song.ducker) == nullptr);
    CHECK (f.session.getAdapter().getDevicePlugin (song.leadSynth) != nullptr);
    const auto [lowDifference, lowPeak] = compare (live, f.render());
    CHECK (lowPeak > 0.05f);
    CHECK (lowDifference < 0.001f);
}

TEST_CASE ("Editing what a frozen channel plays renders it again; until then the old audio plays", "[freeze]")
{
    Fixture f;
    Song song;
    f.session.getHistory().reset (song.project);
    f.perform ("channel.freeze", "{\"channel\": " + juce::String (song.lead.value) + "}");
    f.waitForFreeze();
    const auto before = f.session.getAdapter().describe();

    f.perform ("pattern.setNotes", "{\"pattern\": " + juce::String (song.pattern.value) +
                                       ", \"channel\": " + juce::String (song.lead.value) +
                                       ", \"notes\": [{\"start\": 0, \"length\": 960, \"pitch\": 72}]}");
    CHECK (f.session.getFreezer().getStatus (song.lead).state == Freezer::State::rendering);
    CHECK (f.session.getAdapter().getDevicePlugin (song.leadSynth) == nullptr); // no plugin reload
    CHECK (f.session.getAdapter().describe() == before);                        // the old audio meanwhile

    f.waitForFreeze();
    CHECK (f.session.getFreezer().getRenderCount() == 2);
    CHECK (f.session.getAdapter().describe() != before);
    // The replaced audio is deleted.
    CHECK (f.session.getFreezer().getCacheFolder().findChildFiles (juce::File::findFiles, false, "*.wav").size() == 1);

    // Pattern mode plays the current pattern on its own, rendered for it.
    f.session.setMode (Session::Mode::pattern);
    f.waitForFreeze();
    CHECK (f.session.getFreezer().getRenderCount() == 3);
    CHECK (f.session.getAdapter().describe().contains ("frozen channel " + juce::String (song.lead.value) +
                                                       " on 'Lead': file="));
    CHECK (f.session.getAdapter().describe().contains (" 0.000-8.000"));
}

TEST_CASE ("A saved frozen project opens frozen, without loading the plugin or rendering again", "[freeze]")
{
    juce::TemporaryFile folder (".spinus");
    const auto& projectFolder = folder.getFile();
    Song song;

    {
        Fixture f;
        f.session.setProjectFolder (projectFolder);
        f.session.getHistory().reset (song.project);
        f.perform ("channel.freeze", "{\"channel\": " + juce::String (song.lead.value) + "}");
        f.waitForFreeze();
        REQUIRE (project::saveProject (projectFolder, f.session.captureProjectData()).wasOk());
        CHECK (projectFolder.getChildFile ("cache/freeze").findChildFiles (juce::File::findFiles, false).size() == 1);
    }

    Fixture f;
    project::ProjectData data;
    REQUIRE (project::loadProject (projectFolder, data).wasOk());
    CHECK (data.project.findChannel (song.lead)->frozen);
    CHECK (data.deviceStates.contains (song.leadSynth)); // its settings, for when it is unfrozen
    f.session.setProjectFolder (projectFolder);
    f.session.loadProject (std::move (data));
    f.session.setMode (Session::Mode::song);

    CHECK (! f.session.getFreezer().isRendering());
    CHECK (f.session.getFreezer().getRenderCount() == 0);
    CHECK (f.session.getFreezer().getStatus (song.lead).state == Freezer::State::frozen);
    CHECK (f.session.getAdapter().getDevicePlugin (song.leadSynth) == nullptr);
    CHECK (f.session.getAdapter().describe().contains ("frozen channel"));

    f.perform ("channel.freeze", "{\"channel\": " + juce::String (song.lead.value) + ", \"frozen\": false}");
    CHECK (f.session.getAdapter().getDevicePlugin (song.leadSynth) != nullptr);
    projectFolder.deleteRecursively();
}

TEST_CASE ("A frozen channel whose plugin is missing here keeps playing live and says why", "[freeze]")
{
    Fixture f;
    auto p = test::makeExample(); // its reverb is a plugin that tests do not have
    p.playlist.clips.clear();
    Lane lane { p.allocateId<LaneId>(), "Lane" };
    p.playlist.lanes = { lane };
    p.playlist.clips.push_back (
        { p.allocateId<ClipId>(), lane.id, 0, 3840, false, PatternClip { PatternId { 11 }, 0 } });
    p.mixer.inserts[1].sends.clear();
    p.mixer.inserts[0].sends.clear();
    p.channels[0].targetInsert = reverb;
    f.session.getHistory().reset (p);

    f.perform ("insert.freeze", R"({"insert": 4})");
    f.waitForFreeze();
    const auto status = f.session.getFreezer().getStatus (reverb);
    CHECK (status.state == Freezer::State::failed);
    CHECK (status.message.contains ("Dragonfly Hall Reverb"));
    CHECK (f.session.getAdapter().getDevicePlugin (DeviceId { 8 }) != nullptr); // the kick still plays
}
