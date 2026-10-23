#include <catch2/catch_test_macros.hpp>

#include "../model/ExampleProject.h"
#include "TestDevices.h"
#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "commands/CommandHistory.h"
#include "commands/CommandRegistry.h"
#include "engine/EngineAdapter.h"
#include "engine/EngineHost.h"

#include <random>

using namespace spinus;
using namespace spinus::engine;
using namespace spinus::commands;
using spinus::test::makeExample;
using spinus::test::TestDevices;
namespace te = tracktion;

namespace
{

struct Fixture
{
    Fixture() : host ({ .openAudioDevice = false }), factory (host.getEngine()) {}

    std::unique_ptr<te::Edit> newEdit() { return te::Edit::createSingleTrackEdit (host.getEngine()); }

    /** What a brand-new adapter builds for this project, for comparison with incremental syncs. */
    juce::String describeFresh (const model::Project& project)
    {
        auto e = newEdit();
        EngineAdapter fresh (*e, factory);
        fresh.sync (project);
        return fresh.describe();
    }

    EngineHost host;
    TestDevices factory;
};

std::unique_ptr<Command> make (const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = CommandRegistry::get().create (name, juce::JSON::parse (params), error);
    INFO (error);
    REQUIRE (command != nullptr);
    return command;
}

bool contains (const juce::String& text, const juce::String& part)
{
    if (! text.contains (part))
        UNSCOPED_INFO ("looking for: " << part << "\nin:\n" << text);
    return text.contains (part);
}

/** Builds a random, usually valid command for the current project (invalid ones are just refused). */
std::unique_ptr<Command> randomCommand (const model::Project& p, std::mt19937& rng)
{
    auto pick = [&] (int n) { return n <= 0 ? 0 : std::uniform_int_distribution<int> (0, n - 1) (rng); };
    auto channelId = [&]
    { return p.channels.empty() ? 999 : (int)p.channels[(size_t)pick ((int)p.channels.size())].id.value; };
    auto insertId = [&]
    {
        const auto n = (int)p.mixer.inserts.size();
        const auto i = pick (n + 1);
        return i == n ? 1 : (int)p.mixer.inserts[(size_t)i].id.value;
    };
    auto patternId = [&]
    { return p.patterns.empty() ? 999 : (int)p.patterns[(size_t)pick ((int)p.patterns.size())].id.value; };
    auto num = [] (auto v) { return juce::String (v); };
    auto effectId = [&]
    {
        std::vector<int> ids;
        for (auto* i : { &p.mixer.master })
            for (auto& slot : i->effects)
                ids.push_back ((int)slot.device.id.value);
        for (auto& i : p.mixer.inserts)
            for (auto& slot : i.effects)
                ids.push_back ((int)slot.device.id.value);
        return ids.empty() ? 999 : ids[(size_t)pick ((int)ids.size())];
    };

    switch (pick (18))
    {
    case 0:
        return make ("channel.add", R"({"name": "Ch", "instrument": {"kind": "stock", "stockType": "test.tone"}})");
    case 1:
        return make ("channel.add", R"({"name": "To master", "insert": "master"})");
    case 2:
        return make ("channel.remove", "{\"channel\": " + num (channelId()) + "}");
    case 3:
        return make ("channel.set", "{\"channel\": " + num (channelId()) + ", \"volumeDb\": " + num (-pick (24)) +
                                        ", \"muted\": " + (pick (2) ? "true" : "false") + "}");
    case 4:
        return make ("channel.route", "{\"channel\": " + num (channelId()) + ", \"insert\": " + num (insertId()) + "}");
    case 5:
        return make ("insert.add", R"({"name": "Bus"})");
    case 6:
        return make ("insert.remove", "{\"insert\": " + num (insertId()) + "}");
    case 7:
        return make ("insert.setOutput",
                     "{\"insert\": " + num (insertId()) + ", \"target\": " + num (insertId()) + "}");
    case 8:
        return make ("insert.setSend", "{\"insert\": " + num (insertId()) + ", \"target\": " + num (insertId()) +
                                           ", \"levelDb\": " + num (-pick (30)) + "}");
    case 9:
        return make ("insert.removeSend",
                     "{\"insert\": " + num (insertId()) + ", \"target\": " + num (insertId()) + "}");
    case 10:
        return make ("insert.set", "{\"insert\": " + num (insertId()) + ", \"pan\": " + num (pick (3) - 1) +
                                       ", \"solo\": " + (pick (4) == 0 ? "true" : "false") + "}");
    case 11:
        return make ("insert.move", "{\"insert\": " + num (insertId()) +
                                        ", \"index\": " + num (pick ((int)p.mixer.inserts.size())) + "}");
    case 12:
    {
        juce::String notes;
        for (int i = 0, n = pick (5); i < n; ++i)
            notes << (i > 0 ? "," : "") << "{\"start\": " << pick (16) * 240
                  << ", \"length\": 240, \"pitch\": " << 36 + pick (40) << "}";
        return make ("pattern.setNotes", "{\"pattern\": " + num (patternId()) + ", \"channel\": " + num (channelId()) +
                                             ", \"notes\": [" + notes + "]}");
    }
    case 13:
        return make ("insert.addEffect", "{\"insert\": " + num (insertId()) + ", \"device\": {\"kind\": \"stock\", " +
                                             "\"stockType\": \"" +
                                             (pick (2) ? "spinus.reverb" : "bundled.lsp.compressor-stereo") + "\"}}");
    case 14:
        return make ("insert.removeEffect", "{\"device\": " + num (effectId()) + "}");
    case 15:
        return make ("insert.moveEffect", "{\"device\": " + num (effectId()) + ", \"insert\": " + num (insertId()) +
                                              ", \"index\": " + num (pick (3)) + "}");
    case 16:
        return make ("insert.setEffect",
                     "{\"device\": " + num (effectId()) + ", \"bypassed\": " + (pick (2) ? "true" : "false") + "}");
    default:
        return make ("project.setTempo", "{\"bpm\": " + num (90 + pick (80)) + "}");
    }
}

} // namespace

//==============================================================================
TEST_CASE ("The example project compiles into the expected Edit", "[engine]")
{
    Fixture f;
    auto project = makeExample();
    // Give the bass insert an effect so its sidechain (from the drums insert) is visible.
    project.mixer.inserts[2].effects.push_back ({ model::Device { project.allocateId<model::DeviceId>(),
                                                                  model::Device::Kind::stock,
                                                                  "bundled.lsp.compressor-stereo",
                                                                  {},
                                                                  false },
                                                  1.0f });

    auto edit = f.newEdit();
    EngineAdapter adapter (*edit, f.factory);
    adapter.sync (project);
    const auto text = adapter.describe();

    CHECK (contains (text, "insert 2 'Drums': out=master vol=0.00 pan=0.00 mute=0 solo=0 effects=[Compressor] "
                           "sends=[insert 4:-12.00] return=no"));
    // The reverb plugin is not installed in tests: its slot stays empty, everything else works.
    CHECK (contains (text,
                     "insert 4 'Reverb': out=master vol=0.00 pan=0.00 mute=0 solo=0 effects=[] sends=[] return=yes"));
    CHECK (contains (text, "insert 6 'Bass': out=master"));
    CHECK (contains (text, "sidechain=[insert 2]"));
    CHECK (contains (text, "channel 7 'Kick': out=insert 2"));
    CHECK (contains (text, "channel 9 'Surge': out=insert 6 vol=-3.00 pan=0.25"));

    // Two placements of the 4-bar pattern (3/4 time: 12 beats per pattern, 16 beats per 3840*4 ticks).
    CHECK (adapter.getNumMidiClips() == 4);
    CHECK (contains (text, "midi clip 14 channel 7 on 'Kick': 0.000-16.000 muted=0 notes=[36@0.000+0.250v127 "
                           "36@1.000+0.250v127 36@2.000+0.250v127 36@3.000+0.250v127]"));
    // The second placement starts one beat into the pattern (offset 960).
    CHECK (contains (text, "midi clip 15 channel 7 on 'Kick': 16.000-32.000 muted=0 notes=[36@0.000+0.250v127 "
                           "36@1.000+0.250v127 36@2.000+0.250v127]"));
    CHECK (contains (text, "midi clip 14 channel 9 on 'Surge': 0.000-16.000 muted=0 notes=[45@0.000+2.000v95]"));
    CHECK (juce::approximatelyEqual (edit->tempoSequence.getTempo (0)->getBpm(), 140.0));
}

TEST_CASE ("Incremental syncs match a fresh build after random edits and undo", "[engine]")
{
    Fixture f;
    auto edit = f.newEdit();
    EngineAdapter adapter (*edit, f.factory);

    CommandHistory history (makeExample());
    history.addListener ([&] (const ChangeSet&) { adapter.sync (history.getProject()); });
    adapter.sync (history.getProject());

    std::mt19937 rng (42); // NOLINT(bugprone-random-generator-seed): a fixed seed keeps failures reproducible
    int performed = 0;

    for (int step = 0; step < 300; ++step)
    {
        if (std::uniform_int_distribution<int> (0, 5) (rng) == 0)
            history.undo();
        else if (history.perform (randomCommand (history.getProject(), rng)).wasOk())
            ++performed;

        if (step % 25 == 24)
        {
            INFO ("after step " << step);
            REQUIRE (adapter.describe() == f.describeFresh (history.getProject()));
        }
    }

    CHECK (performed > 150);

    while (history.undo())
    {
    }

    CHECK (adapter.describe() == f.describeFresh (makeExample()));
}

TEST_CASE ("The master plays every effect on it, beyond the first", "[engine]")
{
    // Tracktion allows 4 plugins on the master by default; Spinus's own around each device and the master's
    // volume count too, so only one effect played and the rest were dropped without a word (the showcase song).
    Fixture f;
    auto edit = f.newEdit();
    EngineAdapter adapter (*edit, f.factory);
    CommandHistory history;
    history.addListener ([&] (const ChangeSet&) { adapter.sync (history.getProject()); });

    std::vector<model::DeviceId> effects;
    for (auto* type : { "spinus.reverb", "bundled.lsp.compressor-stereo", "spinus.reverb",
                        "bundled.lsp.compressor-stereo", "spinus.reverb", "bundled.lsp.compressor-stereo" })
    {
        auto add = make ("insert.addEffect",
                         juce::String (R"({"insert": 1, "device": {"kind": "stock", "stockType": ")") + type + "\"}}");
        auto* command = add.get();
        REQUIRE (history.perform (std::move (add)).wasOk());
        effects.push_back (model::DeviceId { (uint32_t)(int)command->getResult()["device"] });
    }
    edit->dispatchPendingUpdatesSynchronously();

    auto& master = edit->getMasterPluginList();
    for (auto id : effects)
    {
        auto* plugin = adapter.getDevicePlugin (id);
        REQUIRE (plugin != nullptr);
        CHECK (master.indexOf (plugin) >= 0);
    }
}

TEST_CASE ("Effects are added, reordered and moved in place; undoing a removal restores their settings", "[engine]")
{
    Fixture f;
    auto edit = f.newEdit();
    EngineAdapter adapter (*edit, f.factory);

    CommandHistory history (makeExample());
    history.addListener ([&] (const ChangeSet&) { adapter.sync (history.getProject()); });
    adapter.sync (history.getProject());

    // Drums (2) has the compressor (device 3). Add a reverb before it.
    auto add = make ("insert.addEffect", R"({"insert": 2, "device": {"kind": "stock", "stockType": "spinus.reverb"},
                                             "index": 0})");
    auto* addCommand = add.get();
    REQUIRE (history.perform (std::move (add)).wasOk());
    const auto reverbId = model::DeviceId { (uint32_t)(int)addCommand->getResult()["device"] };

    auto* compressor = adapter.getDevicePlugin (model::DeviceId { 3 });
    auto* reverb = adapter.getDevicePlugin (reverbId);
    REQUIRE (compressor != nullptr);
    REQUIRE (reverb != nullptr);

    auto& list = adapter.getInsertTrack (model::InsertId { 2 })->pluginList;
    CHECK (list.indexOf (reverb) < list.indexOf (compressor));

    // Reordering keeps the same plugin objects (no reload while playing).
    REQUIRE (history.perform (make ("insert.moveEffect", R"({"device": 3, "index": 0})")).wasOk());
    CHECK (adapter.getDevicePlugin (model::DeviceId { 3 }) == compressor);
    CHECK (list.indexOf (compressor) < list.indexOf (reverb));

    REQUIRE (history.perform (make ("insert.setEffect", R"({"device": 3, "bypassed": true})")).wasOk());
    CHECK (! compressor->isEnabled());

    // Change a compressor setting, remove the compressor, undo: the setting comes back.
    auto* parameter = compressor->getAutomatableParameter (0).get();
    REQUIRE (parameter != nullptr);
    const auto original = parameter->getCurrentValue();
    parameter->setParameter (0.123f, juce::sendNotificationSync);
    const auto value = parameter->getCurrentValue();
    REQUIRE (! juce::approximatelyEqual (value, original));

    REQUIRE (history.perform (make ("insert.removeEffect", R"({"device": 3})")).wasOk());
    CHECK (adapter.getDevicePlugin (model::DeviceId { 3 }) == nullptr);
    REQUIRE (history.undo());

    auto* restored = adapter.getDevicePlugin (model::DeviceId { 3 });
    REQUIRE (restored != nullptr);
    CHECK (juce::approximatelyEqual (restored->getAutomatableParameter (0)->getCurrentValue(), value));

    // Moving to another insert keeps the settings too.
    REQUIRE (history.perform (make ("insert.moveEffect", R"({"device": 3, "insert": 6, "index": 0})")).wasOk());
    auto* moved = adapter.getDevicePlugin (model::DeviceId { 3 });
    REQUIRE (moved != nullptr);
    CHECK (adapter.getInsertTrack (model::InsertId { 6 })->pluginList.indexOf (moved) >= 0);
    CHECK (juce::approximatelyEqual (moved->getAutomatableParameter (0)->getCurrentValue(), value));
    CHECK (adapter.describe() == f.describeFresh (history.getProject()));
}

TEST_CASE ("Editing one channel's notes leaves other channels' clips alone", "[engine]")
{
    Fixture f;
    auto edit = f.newEdit();
    EngineAdapter adapter (*edit, f.factory);
    // The example's pattern one bar long, with its notes in it, so that the edit below keeps its length.
    auto example = makeExample();
    auto& pattern = example.patterns.front();
    const auto bar = example.settings.timeSignature.barTicks();
    for (auto& [channel, notes] : pattern.notes)
        std::erase_if (notes, [bar] (auto& n) { return n.start + n.length > bar; });
    pattern.length = example.fittedLength (pattern);
    for (auto& clip : example.playlist.clips)
        if (std::holds_alternative<model::PatternClip> (clip.content))
            clip.length = pattern.length;
    CommandHistory history (example);
    history.addListener ([&] (const ChangeSet&) { adapter.sync (history.getProject()); });
    adapter.sync (history.getProject());

    auto synthTrack = adapter.getChannelTrack (model::ChannelId { 9 });
    auto clipsBefore = synthTrack->getClips();
    REQUIRE (clipsBefore.size() == 2);
    const auto synthSequenceState = clipsBefore[0]->state.toXmlString();

    REQUIRE (
        history
            .perform (make ("pattern.setNotes",
                            R"({"pattern": 11, "channel": 7, "notes": [{"start": 0, "length": 960, "pitch": 38}]})"))
            .wasOk());

    CHECK (synthTrack->getClips() == clipsBefore);
    CHECK (clipsBefore[0]->state.toXmlString() == synthSequenceState);
    CHECK (contains (adapter.describe(), "midi clip 14 channel 7 on 'Kick': 0.000-" +
                                             juce::String ((double)pattern.length / 960.0, 3) +
                                             " muted=0 notes=[38@0.000+1.000v102]"));

    // Clearing a channel's notes removes its clips.
    REQUIRE (history.perform (make ("pattern.setNotes", R"({"pattern": 11, "channel": 7, "notes": []})")).wasOk());
    CHECK (adapter.getNumMidiClips() == 2);
}

TEST_CASE ("In pattern mode, edits and another pattern leave the Playlist's clips alone", "[engine]")
{
    // With many instruments, rewriting every clip of the song on every edit or pattern opened would freeze
    // Spinus: only what changed is rewritten, and pattern mode only mutes the Playlist's clips.
    Fixture f;
    auto edit = f.newEdit();
    EngineAdapter adapter (*edit, f.factory);
    // The example's pattern one bar long, with its notes in it, so that the edit below keeps its length.
    auto example = makeExample();
    auto& pattern = example.patterns.front();
    const auto bar = example.settings.timeSignature.barTicks();
    for (auto& [channel, notes] : pattern.notes)
        std::erase_if (notes, [bar] (auto& n) { return n.start + n.length > bar; });
    pattern.length = example.fittedLength (pattern);
    for (auto& clip : example.playlist.clips)
        if (std::holds_alternative<model::PatternClip> (clip.content))
            clip.length = pattern.length;
    CommandHistory history (example);
    auto sync = [&] { adapter.sync (history.getProject()); };
    history.addListener ([&] (const ChangeSet&) { sync(); });
    REQUIRE (history.perform (make ("pattern.add", R"({"name": "Other", "length": 3840})")).wasOk());
    const auto other = (int)history.getProject().patterns.back().id.value;
    REQUIRE (history
                 .perform (make ("pattern.setNotes",
                                 "{\"pattern\": " + juce::String (other) +
                                     R"(, "channel": 9, "notes": [{"start": 0, "length": 240, "pitch": 60}]})"))
                 .wasOk());
    const auto first = history.getProject().patterns.front().id;
    adapter.setPlayMode (EngineAdapter::PlayMode::pattern, first);
    sync();

    /** Counts the changes made to a clip's notes (also when they are written again as they were). */
    struct Changes final : juce::ValueTree::Listener
    {
        explicit Changes (juce::ValueTree s) : state (std::move (s)) { state.addListener (this); }
        ~Changes() override { state.removeListener (this); }
        void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { ++count; }
        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { ++count; }
        void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { ++count; }
        juce::ValueTree state;
        int count = 0;
    };

    auto* synth = adapter.getChannelTrack (model::ChannelId { 9 });
    REQUIRE (synth->getClips().size() == 3); // the Playlist's two clips of the pattern, and its preview
    std::vector<std::unique_ptr<Changes>> playlistClips;
    std::unique_ptr<Changes> preview;
    for (auto* clip : synth->getClips())
    {
        if (dynamic_cast<te::MidiClip*> (clip) != nullptr && clip->isMuted())
            playlistClips.push_back (std::make_unique<Changes> (clip->state));
        else
            preview = std::make_unique<Changes> (clip->state);
    }
    REQUIRE (playlistClips.size() == 2); // pattern mode mutes the Playlist
    REQUIRE (preview != nullptr);

    // Another channel's notes: nothing of this channel changes.
    REQUIRE (
        history
            .perform (make ("pattern.setNotes",
                            R"({"pattern": 11, "channel": 7, "notes": [{"start": 0, "length": 960, "pitch": 38}]})"))
            .wasOk());
    for (auto& clip : playlistClips)
        CHECK (clip->count == 0);
    CHECK (preview->count == 0);

    // Another pattern: only the preview changes.
    adapter.setPlayMode (EngineAdapter::PlayMode::pattern, model::PatternId { (uint32_t)other });
    sync();
    for (auto& clip : playlistClips)
        CHECK (clip->count == 0);
    CHECK (contains (adapter.describe(), "notes=[60@0.000]"));

    // Song mode: the Playlist's clips play again, with their notes as they were.
    adapter.setPlayMode (EngineAdapter::PlayMode::song, model::PatternId { (uint32_t)other });
    sync();
    CHECK (adapter.describe() == f.describeFresh (history.getProject()));
}

TEST_CASE ("A part of thousands of notes is written quickly and exactly", "[engine]")
{
    // Tracktion looks notes up by searching its list, so writing (and replacing) a song-long part note by note took
    // time growing with the square of its notes.
    Fixture f;
    auto edit = f.newEdit();
    EngineAdapter adapter (*edit, f.factory);
    CommandHistory history (makeExample());
    history.addListener ([&] (const ChangeSet&) { adapter.sync (history.getProject()); });
    // Pattern mode: the pattern plays whole (the Playlist's clips show a bar or two of it).
    const auto pattern = history.getProject().patterns.front().id;
    adapter.setPlayMode (EngineAdapter::PlayMode::pattern, pattern);
    adapter.sync (history.getProject());

    auto notesFrom = [] (int count, int pitch)
    {
        juce::MemoryOutputStream text;
        text << "{\"pattern\": 11, \"channel\": 9, \"notes\": [";
        for (int i = 0; i < count; ++i)
            text << (i > 0 ? "," : "") << "{\"start\": " << i * 120
                 << ", \"length\": 100, \"pitch\": " << pitch + i % 12
                 << ", \"velocity\": 0.5, \"releaseVelocity\": 0.25}";
        text << "]}";
        return text.toString();
    };

    constexpr int count = 20000;
    std::vector<std::unique_ptr<Command>> commands;
    for (int pitch : { 40, 50, 60 })
        commands.push_back (make ("pattern.setNotes", notesFrom (count, pitch)));
    const auto start = juce::Time::getMillisecondCounterHiRes();
    for (auto& command : commands)
        REQUIRE (history.perform (std::move (command)).wasOk());
    const auto ms = juce::Time::getMillisecondCounterHiRes() - start;
    INFO (ms << " ms");
#if JUCE_DEBUG
    CHECK (ms < 20000.0);
#else
    CHECK (ms < 2500.0);
#endif

    // Note for note as written (release velocity included).
    const auto& notes = adapter.getChannelTrack (model::ChannelId { 9 })->getClips();
    te::MidiClip* preview = nullptr;
    for (auto* clip : notes)
        if (auto* midi = dynamic_cast<te::MidiClip*> (clip); midi != nullptr && ! midi->isMuted())
            preview = midi;
    REQUIRE (preview != nullptr);
    REQUIRE (preview->getSequence().getNumNotes() == count);
    const auto& sorted = preview->getSequence().getNotes();
    CHECK (sorted.getFirst()->getNoteNumber() == 60);
    CHECK (sorted.getLast()->getNoteNumber() == 60 + (count - 1) % 12);
    CHECK (std::abs (sorted.getLast()->getStartBeat().inBeats() - (count - 1) * 120 / 960.0) < 1.0e-6);
    CHECK (sorted.getFirst()->getVelocity() == 64);
    CHECK (sorted.getFirst()->getNoteOffVelocity() == 32);

    // In song mode the same as a fresh build; and undone the same way.
    adapter.setPlayMode (EngineAdapter::PlayMode::song, pattern);
    adapter.sync (history.getProject());
    CHECK (adapter.describe() == f.describeFresh (history.getProject()));
    REQUIRE (history.undo());
    REQUIRE (history.undo());
    REQUIRE (history.undo());
    CHECK (adapter.describe() == f.describeFresh (history.getProject()));
}

TEST_CASE ("Audio flows channel -> insert -> master, and mute and routing take effect", "[engine]")
{
    // Paced in real time: each change gets 0.5 s for Tracktion's asynchronous graph rebuild before the
    // level is measured. Unpaced, a slow CI machine renders past the change before it applies.
    Fixture f;
    test::RealtimeRunner runner (f.host.getEngine(),
                                 { .outputChannels = 2, .maxSeconds = 8.0, .paceInRealTime = true });
    auto edit = f.newEdit();
    EngineAdapter adapter (*edit, f.factory);

    CommandHistory history;
    history.addListener ([&] (const ChangeSet&) { adapter.sync (history.getProject()); });
    REQUIRE (history
                 .perform (make ("channel.add",
                                 R"({"name": "Tone", "instrument": {"kind": "stock", "stockType": "test.tone"}})"))
                 .wasOk());
    REQUIRE (history.perform (make ("insert.add", R"({"name": "Spare"})")).wasOk());

    edit->dispatchPendingUpdatesSynchronously();
    edit->getTransport().ensureContextAllocated();
    edit->getTransport().play (false);
    runner.start();

    auto levelOver = [&] (double from, double to)
    {
        runner.runUntil (to);
        const auto sr = runner.getParams().sampleRate;
        return test::checks::rms (runner.getChannel (0), (int64_t)(from * sr), (int64_t)(to * sr));
    };

    CHECK (levelOver (0.5, 1.0) > 0.05f);

    REQUIRE (
        history.perform (make ("insert.set", R"({"insert": 4, "muted": true})")).wasOk()); // the channel's own insert
    CHECK (levelOver (1.5, 2.0) < 1.0e-4f);

    REQUIRE (history.perform (make ("channel.route", R"({"channel": 2, "insert": 5})")).wasOk()); // to the spare insert
    CHECK (levelOver (2.5, 3.0) > 0.05f);

    REQUIRE (history.perform (make ("insert.remove", R"({"insert": 5})")).wasOk()); // falls back to master
    CHECK (levelOver (3.5, 4.0) > 0.05f);

    history.undo(); // spare insert back, channel routed to it again
    CHECK (levelOver (4.5, 5.0) > 0.05f);
    CHECK (contains (adapter.describe(), "channel 2 'Tone': out=insert 5"));

    runner.stop();
    edit->getTransport().stop (false, true);
}

TEST_CASE ("Muting the master silences the output after its effects, whatever its fader; undo brings it back",
           "[engine]")
{
    Fixture f;
    test::RealtimeRunner runner (f.host.getEngine(),
                                 { .outputChannels = 2, .maxSeconds = 6.0, .paceInRealTime = true });
    auto edit = f.newEdit();
    EngineAdapter adapter (*edit, f.factory);

    CommandHistory history;
    history.addListener ([&] (const ChangeSet&) { adapter.sync (history.getProject()); });
    REQUIRE (history
                 .perform (make ("channel.add",
                                 R"({"name": "Tone", "instrument": {"kind": "stock", "stockType": "test.tone"}})"))
                 .wasOk());
    // An effect on the master: the mute comes after it.
    REQUIRE (history
                 .perform (make ("insert.addEffect",
                                 R"({"insert": 1, "device": {"kind": "stock", "stockType": "spinus.reverb"}})"))
                 .wasOk());

    edit->dispatchPendingUpdatesSynchronously();
    edit->getTransport().ensureContextAllocated();
    edit->getTransport().play (false);
    runner.start();

    auto levelOver = [&] (double from, double to)
    {
        runner.runUntil (to);
        const auto sr = runner.getParams().sampleRate;
        return test::checks::rms (runner.getChannel (0), (int64_t)(from * sr), (int64_t)(to * sr));
    };

    CHECK (levelOver (0.5, 1.0) > 0.01f);
    CHECK (contains (adapter.describe(), "master: vol=0.00 pan=0.00 mute=0 effects=["));

    REQUIRE (history.perform (make ("insert.set", R"({"insert": 1, "muted": true})")).wasOk());
    CHECK (history.getProject().mixer.master.muted);
    CHECK (contains (adapter.describe(), "master: vol=0.00 pan=0.00 mute=1 effects=["));
    CHECK (levelOver (1.5, 2.0) < 1.0e-5f);

    // The master's fader moved (as its automation would move it): still silent.
    REQUIRE (history.perform (make ("insert.set", R"({"insert": 1, "volumeDb": 3.0})")).wasOk());
    CHECK (levelOver (2.5, 3.0) < 1.0e-5f);

    history.undo(); // the volume
    history.undo(); // the mute
    CHECK (! history.getProject().mixer.master.muted);
    CHECK (contains (adapter.describe(), "mute=0"));
    CHECK (levelOver (3.5, 4.0) > 0.01f);

    runner.stop();
    edit->getTransport().stop (false, true);
}
