#include <catch2/catch_test_macros.hpp>

#include "../model/ExampleProject.h"
#include "commands/CommandHistory.h"
#include "commands/CommandRegistry.h"
#include "model/Automation.h"
#include "model/ModelJson.h"
#include "model/Validation.h"

using namespace spinus::commands;
using spinus::test::makeExample;

namespace
{

// IDs in makeExample(): inserts drums 2 (compressor device 3), reverb 4 (plugin device 5), bass 6;
// channels kick 7 (device 8) and synth 9 (device 10); pattern 11; lanes 12, 13; clips 14-17.
constexpr int drums = 2, reverb = 4, bass = 6, kick = 7, synth = 9, beat = 11;

juce::var json (const juce::String& text)
{
    auto v = juce::JSON::parse (text);
    REQUIRE (! v.isVoid());
    return v;
}

std::unique_ptr<Command> make (const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = CommandRegistry::get().create (name, json (params), error);
    INFO (error);
    REQUIRE (command != nullptr);
    return command;
}

juce::String createError (const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = CommandRegistry::get().create (name, json (params), error);
    CHECK (command == nullptr);
    return error;
}

juce::File testFile (const juce::String& path) { return juce::File (SPINUS_TEST_FILES_DIR).getChildFile (path); }

/** One valid example call per command; the generic undo/redo test runs all of them. Every registered
    command must appear here (checked below). */
const std::vector<std::pair<juce::String, juce::String>>& exampleCalls()
{
    static const std::vector<std::pair<juce::String, juce::String>> calls {
        { "project.setTempo", R"({"bpm": 128})" },
        { "project.setKey", R"({"tonic": 2, "scale": "dorian"})" },
        { "channel.add", R"({"name": "Hats", "instrument": {"kind": "stock", "stockType": "spinus.sampler"}})" },
        { "channel.remove", R"({"channel": 9})" },
        { "channel.replaceInstrument",
          R"({"channel": 9, "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}})" },
        { "channel.set",
          R"({"channel": 7, "name": "Big kick", "volumeDb": -6, "muted": true, "colour": "#ff112233"})" },
        { "channel.route", R"({"channel": 7, "insert": 6})" },
        { "channel.move", R"({"channel": 9, "index": 0})" },
        { "channel.freeze", R"({"channel": 7})" },
        { "insert.add", R"({"name": "Vocals", "index": 0})" },
        { "insert.addBus", R"({"name": "Low end", "inserts": [2, 6]})" },
        { "insert.remove", R"({"insert": 2})" },
        { "insert.set", R"({"insert": 1, "volumeDb": -1.5, "stereoWidth": 0.5})" },
        { "insert.move", R"({"insert": 6, "index": 0})" },
        { "insert.freeze", R"({"insert": 6, "frozen": true})" },
        { "insert.setOutput", R"({"insert": 6, "target": 4})" }, // 6 follows 2 as a sidechain: not into 2
        { "insert.setSend", R"({"insert": 6, "target": 4, "levelDb": -9})" },
        { "insert.setSidechain", R"({"insert": 4, "source": 6})" },
        { "insert.setNotesFrom", R"({"insert": 6, "channel": 9})" },
        { "insert.removeSend", R"({"insert": 2, "target": 4})" },
        { "insert.addEffect",
          R"({"insert": 6, "device": {"kind": "stock", "stockType": "bundled.lsp.compressor-stereo"}, "index": 0})" },
        { "insert.removeEffect", R"({"device": 3})" },
        { "insert.moveEffect", R"({"device": 3, "insert": 6, "index": 0})" },
        { "insert.setEffect", R"({"device": 5, "bypassed": true})" },
        { "device.setParameters", R"({"device": 3, "values": {"ratio": 0.5}})" },
        { "device.loadPreset", R"({"device": 8, "preset": "Reese Bass"})" }, // the kick made a 3x Osc below
        { "pattern.add", R"({"name": "Fill", "length": 3840})" },
        { "pattern.remove", R"({"pattern": 11})" },
        { "lane.add", R"({"name": "Drums"})" },
        { "lane.remove", R"({"lane": 12})" },
        { "lane.set", R"({"lane": 12, "name": "Beat", "muted": true})" },
        { "clip.addPattern", R"({"lane": 13, "pattern": 11, "start": 7680})" },
        { "clip.set", R"({"clip": 14, "lane": 13, "start": 1920, "length": 3840})" },
        { "clip.remove", R"({"clip": 15})" },
        { "clip.split", R"({"clip": 16, "at": 1000})" },
        { "clip.makeUnique", R"({"clip": 15})" },
        { "clip.add", R"({"clip": {"type": "pattern", "pattern": 11, "lane": 13, "start": 7680, "length": 3840,
              "offset": 960, "muted": true}})" },
        { "marker.add", R"({"at": 7680, "name": "Bridge"})" },
        { "marker.set", R"({"marker": 23, "name": "Drop", "at": 1920})" },
        { "marker.remove", R"({"marker": 23})" },
        { "clip.addAudio", R"({"lane": 13, "file": "drums/loop.wav", "start": 3840, "length": 7680})" },
        { "clip.addAudio",
          R"({"lane": 12, "file": "/home/me/vox.flac", "start": 0, "length": 960, "channel": 7, "gainDb": -3})" },
        { "clip.addTake", R"({"channel": 7, "file": "take2.wav", "start": 0, "length": 960})" },
        { "clip.useTake", R"({"clip": 24})" },
        { "clip.setAudio", R"({"clip": 16, "gainDb": -6, "fadeIn": 240})" },
        { "clip.setTune",
          R"({"clip": 16, "notes": [{"start": 0.5, "end": 1.0, "pitch": 60.4, "shift": -0.4, "drift": 0.2},
              {"start": 1.2, "end": 1.8, "pitch": 63.1, "vibrato": 0.5}], "description": "Tune notes"})" },
        { "clip.addAutomation",
          R"({"start": 0, "length": 3840, "laneName": "Drums volume", "target": {"kind": "insertVolume", "insert": 2},
              "points": [{"time": 0, "value": 0.2}, {"time": 3840, "value": 0.9, "curve": 0.3}]})" },
        { "clip.addAutomation",
          R"({"lane": 13, "start": 960, "length": 1920, "target": {"kind": "deviceParameter", "device": 5, "parameterId": "size"},
              "points": [{"time": 0, "value": 1}]})" },
        { "clip.setPoints", R"({"clip": 17, "points": [{"time": 960, "value": 0.25}, {"time": 0, "value": 0.5}]})" },
        { "pattern.setLength", R"({"pattern": 11, "length": 7680})" },
        { "pattern.setTimeSignature", R"({"pattern": 11, "numerator": 7, "denominator": 8})" },
        { "pattern.setTripletSteps", R"({"pattern": 11, "triplets": true})" },
        { "pattern.set", R"({"pattern": 11, "name": "Chorus drums", "colour": "#ffd65c5c"})" },
        { "pattern.clone", R"({"pattern": 11})" },
        { "pattern.setSwing", R"({"pattern": 11, "swing": 0.6})" },
        { "project.setTimeSignature", R"({"numerator": 6, "denominator": 8})" },
        { "pattern.setNotes",
          R"({"pattern": 11, "channel": 9, "notes": [{"start": 0, "length": 480, "pitch": 48, "velocity": 0.6}]})" },
        // The example's launcher: track 18, scenes 19 and 20, pattern clip 21 (18/19), audio clip 22 (18/20).
        { "launcher.addTrack", R"({"name": "Bass jam"})" },
        { "launcher.removeTrack", R"({"track": 18})" },
        { "launcher.setTrack", R"({"track": 18, "name": "Keys", "colour": "#ff336699"})" },
        { "launcher.addScene", R"({"name": "Bridge", "index": 1})" },
        { "launcher.removeScene", R"({"scene": 20})" },
        { "launcher.setScene", R"({"scene": 19, "name": "Intro"})" },
        { "launcher.addPattern", R"({"track": 18, "scene": 20, "pattern": 11, "length": 3840})" },
        { "launcher.addAudio", R"({"track": 18, "scene": 19, "file": "loops/pad.wav", "length": 7680})" },
        { "launcher.addAudio", R"({"track": 18, "scene": 19, "file": "pad.wav", "length": 960, "channel": 7})" },
        { "launcher.setClip", R"({"clip": 21, "scene": 20, "loop": false})" },
        { "launcher.removeClip", R"({"clip": 22})" },
        { "launcher.setQuantize", R"({"quantize": "none"})" },
    };
    return calls;
}

} // namespace

//==============================================================================
TEST_CASE ("Every command performs, undoes exactly and redoes", "[commands]")
{
    for (auto& [name, params] : exampleCalls())
    {
        INFO (name << " " << params);
        auto project = makeExample();
        project.playlist.markers = { { project.allocateId<MarkerId>(), 3840, "Chorus" } }; // ID 23
        // ID 24: a muted second take of the vocals (clip 16), on lane 12, for clip.useTake.
        project.playlist.clips.push_back ({ project.allocateId<ClipId>(), LaneId { 12 }, 0, 960, true,
                                            std::get<AudioClip> (project.findClip (ClipId { 16 })->content) });
        if (name == "device.loadPreset")
            project.channels[0].instrument.value().stockType = "spinus.3x-osc"; // only 3x Osc has built-in presets
        CommandHistory history (project);
        const auto before = history.getProject();

        int notifications = 0;
        history.addListener (
            [&] (const ChangeSet& changes)
            {
                ++notifications;
                CHECK (! changes.isEmpty());
            });

        const auto result = history.perform (make (name, params));
        INFO (result.getErrorMessage());
        REQUIRE (result.wasOk());
        CHECK (validate (history.getProject()).isEmpty());

        const auto after = history.getProject();
        CHECK (! (after == before));
        CHECK (history.getUndoDescriptions().size() == 1);
        CHECK (history.getUndoDescriptions()[0].isNotEmpty());

        REQUIRE (history.undo());
        auto undone = history.getProject();
        undone.nextId = before.nextId; // allocated IDs are never given back
        CHECK (undone == before);

        REQUIRE (history.redo());
        CHECK (history.getProject() == after);
        CHECK (notifications == 3);
    }
}

TEST_CASE ("Every registered command has an example call", "[commands]")
{
    for (auto& info : CommandRegistry::get().getCommands())
    {
        INFO (info.name);
        CHECK (
            std::any_of (exampleCalls().begin(), exampleCalls().end(), [&] (auto& c) { return c.first == info.name; }));
        CHECK (info.summary.isNotEmpty());
    }
}

TEST_CASE ("Adding a channel creates and routes its own insert; redo reuses the IDs", "[commands]")
{
    CommandHistory history (makeExample());
    auto command = make ("channel.add", R"({"name": "Hats"})");
    auto* raw = command.get();
    REQUIRE (history.perform (std::move (command)).wasOk());

    const auto result = raw->getResult();
    const auto channelId = ChannelId { (uint32_t)(int)result["channel"] };
    const auto insertId = InsertId { (uint32_t)(int)result["insert"] };
    const auto* channel = history.getProject().findChannel (channelId);
    REQUIRE (channel != nullptr);
    CHECK (channel->targetInsert == insertId);
    CHECK (history.getProject().findInsert (insertId)->name == "Hats");

    history.undo();
    CHECK (history.getProject().findChannel (channelId) == nullptr);
    CHECK (history.getProject().findInsert (insertId) == nullptr);

    history.redo();
    CHECK (history.getProject().findChannel (channelId) != nullptr);
    CHECK (history.getProject().findChannel (channelId)->targetInsert == insertId);

    // A channel can also go straight to master or an existing insert.
    REQUIRE (history.perform (make ("channel.add", R"({"name": "Pad", "insert": "master"})")).wasOk());
    CHECK (history.getProject().channels.back().targetInsert == masterInsertId);
    REQUIRE (history.perform (make ("channel.add", R"({"name": "Bass 2", "insert": 6})")).wasOk());
    CHECK (history.getProject().channels.back().targetInsert == InsertId { bass });
}

TEST_CASE ("Removing a channel removes its notes and automation; undo brings them back", "[commands]")
{
    auto project = makeExample();
    AutomationClip volume;
    volume.target = { ParameterTarget::Kind::channelVolume, ChannelId { synth }, {}, {}, {} };
    volume.points = { { 0, 0.5f, 0.0f } };
    project.playlist.clips.push_back ({ project.allocateId<ClipId>(), LaneId { 12 }, 0, 960, false, volume });

    CommandHistory history (project);
    REQUIRE (history.perform (make ("channel.remove", R"({"channel": 9})")).wasOk());
    CHECK (! history.getProject().patterns[0].notes.contains (ChannelId { synth }));
    CHECK (history.getProject().playlist.clips.size() == project.playlist.clips.size() - 1);

    history.undo();
    auto restored = history.getProject();
    CHECK (restored == project);
}

TEST_CASE ("Replacing an instrument keeps the channel and its notes; undo brings the old one back", "[commands]")
{
    auto project = makeExample();
    // Automation of the old instrument (Surge XT, device 10) and of the channel's volume.
    AutomationClip cutoff, volume;
    cutoff.target = { ParameterTarget::Kind::deviceParameter, {}, {}, DeviceId { 10 }, "cutoff" };
    cutoff.points = { { 0, 0.5f, 0.0f } };
    volume.target = { ParameterTarget::Kind::channelVolume, ChannelId { synth }, {}, {}, {} };
    volume.points = { { 0, 0.5f, 0.0f } };
    project.playlist.clips.push_back ({ project.allocateId<ClipId>(), LaneId { 12 }, 0, 960, false, cutoff });
    project.playlist.clips.push_back ({ project.allocateId<ClipId>(), LaneId { 12 }, 0, 960, false, volume });
    project.channels[1].colour = 0xff112233;
    project.channels[1].group = "Synths";
    project.channels[1].frozen = true;

    CommandHistory history (project);
    auto command = make ("channel.replaceInstrument",
                         R"({"channel": 9, "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}})");
    auto* raw = command.get();
    REQUIRE (history.perform (std::move (command)).wasOk());
    CHECK (history.getUndoDescriptions()[0] == "Replace instrument of 'Surge' with 3x Osc");

    const auto newDevice = DeviceId { (uint32_t)(int)raw->getResult()["device"] };
    CHECK ((int)raw->getResult()["channel"] == synth);
    const auto& p = history.getProject();
    const auto* channel = p.findChannel (ChannelId { synth });
    REQUIRE (channel != nullptr);
    REQUIRE (channel->instrument.has_value());
    CHECK (channel->instrument.value_or (Device {}).stockType == "spinus.3x-osc");
    CHECK (channel->instrument.value_or (Device {}).id == newDevice);
    CHECK (newDevice != DeviceId { 10 });
    // Everything else stays: its name (not the old instrument's), colour, group, insert, volume, pan, notes.
    CHECK (channel->name == "Surge");
    CHECK (channel->colour == 0xff112233);
    CHECK (channel->group == "Synths");
    CHECK (channel->targetInsert == InsertId { bass });
    CHECK (juce::approximatelyEqual (channel->volumeDb, -3.0f));
    CHECK (juce::approximatelyEqual (channel->pan, 0.25f));
    CHECK (! channel->frozen); // the new instrument plays live
    CHECK (p.patterns[0].notes.at (ChannelId { synth }) == project.patterns[0].notes.at (ChannelId { synth }));
    CHECK (p.findInsert (InsertId { bass })->name == "Bass");
    // The old instrument's automation went; the channel's volume automation stayed.
    CHECK (p.playlist.clips.size() == project.playlist.clips.size() - 1);
    CHECK (std::none_of (p.playlist.clips.begin(), p.playlist.clips.end(),
                         [] (const Clip& c)
                         {
                             auto* a = std::get_if<AutomationClip> (&c.content);
                             return a != nullptr && a->target.device == DeviceId { 10 };
                         }));
    CHECK (validate (p).isEmpty());

    REQUIRE (history.undo());
    auto undone = history.getProject();
    undone.nextId = project.nextId;
    CHECK (undone == project);

    REQUIRE (history.redo());
    CHECK (history.getProject().findChannel (ChannelId { synth })->instrument.value_or (Device {}).id ==
           newDevice); // same ID again
}

TEST_CASE ("A channel named after its instrument takes the new instrument's name", "[commands]")
{
    auto project = makeExample();
    project.channels[1].name = "Surge XT"; // as added from the menu
    project.findInsert (InsertId { bass })->name = "Surge XT";

    CommandHistory history (project);
    REQUIRE (history
                 .perform (make ("channel.replaceInstrument",
                                 R"({"channel": 9, "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}})"))
                 .wasOk());
    CHECK (history.getProject().findChannel (ChannelId { synth })->name == "3x Osc");
    CHECK (history.getProject().findInsert (InsertId { bass })->name == "3x Osc");

    // A name given is used; a mixer insert other channels play into keeps its name.
    REQUIRE (history.perform (make ("channel.route", R"({"channel": 7, "insert": 6})")).wasOk());
    REQUIRE (history
                 .perform (make ("channel.replaceInstrument",
                                 R"({"channel": 9, "name": "Lead", "instrument": {"kind": "plugin", "plugin":
                                     {"format": "CLAP", "name": "Some Synth", "fileOrIdentifier": "/x.clap"}}})"))
                 .wasOk());
    CHECK (history.getProject().findChannel (ChannelId { synth })->name == "Lead");
    CHECK (history.getProject().findChannel (ChannelId { synth })->instrument.value_or (Device {}).plugin.name ==
           "Some Synth");
    CHECK (history.getProject().findInsert (InsertId { bass })->name == "3x Osc");

    // Refused: an audio track, a channel that isn't there, no instrument.
    REQUIRE (history.perform (make ("channel.add", R"({"name": "Vox", "audioTrack": true})")).wasOk());
    const auto vox = history.getProject().channels.back().id;
    CHECK (history
               .perform (make ("channel.replaceInstrument",
                               "{\"channel\": " + juce::String (vox.value) +
                                   ", \"instrument\": {\"kind\": \"stock\", \"stockType\": \"spinus.3x-osc\"}}"))
               .getErrorMessage()
               .contains ("audio track"));
    CHECK (history
               .perform (make ("channel.replaceInstrument",
                               R"({"channel": 999, "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}})"))
               .failed());
    CHECK (createError ("channel.replaceInstrument", R"({"channel": 9})").contains ("instrument"));
}

TEST_CASE ("Removing an insert sends its inputs to master and drops sends to it", "[commands]")
{
    CommandHistory history (makeExample());
    REQUIRE (history.perform (make ("insert.remove", R"({"insert": 4})")).wasOk()); // reverb

    const auto& p = history.getProject();
    CHECK (p.findInsert (InsertId { reverb }) == nullptr);
    CHECK (p.findInsert (InsertId { drums })->sends.empty());
    // The reverb plugin's automation clip went with it.
    CHECK (std::none_of (p.playlist.clips.begin(), p.playlist.clips.end(),
                         [] (auto& c) { return std::holds_alternative<AutomationClip> (c.content); }));

    REQUIRE (history.perform (make ("insert.remove", R"({"insert": 2})")).wasOk()); // drums
    CHECK (history.getProject().findChannel (ChannelId { kick })->targetInsert == masterInsertId);
    CHECK (history.getProject().findInsert (InsertId { bass })->sidechainSources.empty());

    CHECK (history.perform (make ("insert.remove", R"({"insert": 1})")).getErrorMessage().contains ("master"));
}

TEST_CASE ("Effects: any number per insert, move between inserts, removal takes the automation", "[commands]")
{
    CommandHistory history (makeExample());
    const auto compressor = R"({"kind": "stock", "stockType": "bundled.lsp.compressor-stereo"})";

    // No limit on effects per insert (FL's 10 is a common complaint): 30 on the bass insert.
    std::vector<int> added;
    for (int i = 0; i < 30; ++i)
    {
        auto command = make ("insert.addEffect", juce::String (R"({"insert": 6, "device": )") + compressor + "}");
        auto* raw = command.get();
        REQUIRE (history.perform (std::move (command)).wasOk());
        added.push_back ((int)raw->getResult()["device"]);
    }
    CHECK (history.getProject().findInsert (InsertId { bass })->effects.size() == 30);
    CHECK (history
               .perform (make ("insert.addEffect",
                               R"({"insert": 6, "device": {"kind": "stock", "stockType": "x"}, "index": 31})"))
               .getErrorMessage()
               .contains ("index out of range"));
    REQUIRE (
        history.perform (make ("insert.moveEffect", "{\"device\": " + juce::String (added.back()) + ", \"index\": 0}"))
            .wasOk());
    CHECK (history.getProject().findInsert (InsertId { bass })->effects[0].device.id.value == (uint32_t)added.back());

    // Move the drums compressor to the reverb insert, before the reverb.
    REQUIRE (history.perform (make ("insert.moveEffect", R"({"device": 3, "insert": 4, "index": 0})")).wasOk());
    {
        const auto& p = history.getProject();
        CHECK (p.findInsert (InsertId { drums })->effects.empty());
        REQUIRE (p.findInsert (InsertId { reverb })->effects.size() == 2);
        CHECK (p.findInsert (InsertId { reverb })->effects[0].device.id == DeviceId { 3 });
    }

    // Removing the reverb plugin removes its automation clip.
    REQUIRE (history.perform (make ("insert.removeEffect", R"({"device": 5})")).wasOk());
    {
        const auto& p = history.getProject();
        CHECK (history.getUndoDescriptions().strings.getLast() == "Remove effect 'Dragonfly Hall Reverb'");
        CHECK (std::none_of (p.playlist.clips.begin(), p.playlist.clips.end(),
                             [] (auto& c) { return std::holds_alternative<AutomationClip> (c.content); }));
    }

    CHECK (history.perform (make ("insert.removeEffect", R"({"device": 5})")).getErrorMessage().contains ("no effect"));
    CHECK (history.perform (make ("insert.setEffect", R"({"device": 8, "bypassed": true})")).failed()); // instrument

    REQUIRE (history.perform (make ("insert.setEffect", R"({"device": 3, "bypassed": true})")).wasOk());
    CHECK (history.getUndoDescriptions().strings.getLast() ==
           "Bypass 'bundled.lsp.compressor-stereo'"); // not bundled: no display name
    CHECK (history.getProject().findInsert (InsertId { reverb })->effects[0].device.bypassed);
}

TEST_CASE ("Routing that would loop is refused", "[commands]")
{
    CommandHistory history (makeExample());

    // drums (2) sends to reverb (4): reverb -> drums would loop.
    CHECK (history.perform (make ("insert.setOutput", R"({"insert": 4, "target": 2})"))
               .getErrorMessage()
               .contains ("feedback loop"));
    CHECK (history.perform (make ("insert.setSend", R"({"insert": 4, "target": 2, "levelDb": 0})"))
               .getErrorMessage()
               .contains ("feedback loop"));
    CHECK (history.perform (make ("insert.setSend", R"({"insert": 2, "target": 2, "levelDb": 0})")).failed());
    CHECK (
        history.perform (make ("insert.setOutput", R"({"insert": 1, "target": 2})")).failed()); // master has no output

    // Re-pointing an output that is already there is fine.
    CHECK (history.perform (make ("insert.setOutput", R"({"insert": 2, "target": 1})")).wasOk());
    CHECK (! history.canRedo());
    CHECK (history.getUndoDescriptions().size() == 1);
}

TEST_CASE ("Bad parameters give readable errors and change nothing", "[commands]")
{
    CHECK (createError ("channel.teleport", "{}").contains ("unknown command"));
    CHECK (createError ("project.setTempo", R"({"bpm": 5})").contains ("'bpm' must be a number from 20"));
    CHECK (createError ("project.setTempo", "{}").contains ("missing 'bpm'"));
    CHECK (createError ("channel.set", R"({"channel": 7, "volumme": 3})").contains ("unknown parameter 'volumme'"));
    CHECK (createError ("channel.set", R"({"channel": "seven"})").contains ("positive integer ID"));
    CHECK (createError ("channel.set", R"({"channel": 7, "colour": "red"})").contains ("colour like"));
    CHECK (
        createError ("pattern.setNotes", R"({"pattern": 11, "channel": 7, "notes": 3})").contains ("must be an array"));

    // Valid parameters that do not fit this project fail at perform() time, leaving it unchanged.
    CommandHistory history (makeExample());
    const auto before = history.getProject();
    CHECK (history.perform (make ("channel.route", R"({"channel": 7, "insert": 99})"))
               .getErrorMessage()
               .contains ("does not exist"));
    CHECK (history
               .perform (make ("pattern.setNotes",
                               R"({"pattern": 11, "channel": 7, "notes": [{"start": 0, "length": 0, "pitch": 60}]})"))
               .getErrorMessage()
               .contains ("invalid note"));
    CHECK (history.getProject() == before);
    CHECK (! history.canUndo());
}

TEST_CASE ("Joined commands (one fader drag) are one undo step", "[commands]")
{
    CommandHistory history (makeExample());
    const auto before = history.getProject();
    int notifications = 0;
    history.addListener ([&] (const ChangeSet&) { ++notifications; });

    // The first move of a drag is a normal step; the rest join it. Each is applied (and heard) at once.
    REQUIRE (history.perform (make ("insert.set", R"({"insert": 2, "volumeDb": -1})")).wasOk());
    REQUIRE (history.performJoined (make ("insert.set", R"({"insert": 2, "volumeDb": -2})")).wasOk());
    REQUIRE (history.performJoined (make ("insert.set", R"({"insert": 2, "volumeDb": -3})")).wasOk());
    CHECK (notifications == 3);
    CHECK (juce::approximatelyEqual (history.getProject().findInsert (InsertId { drums })->volumeDb, -3.0f));
    CHECK (history.getUndoDescriptions() == juce::StringArray { "Change insert volume" });
    const auto after = history.getProject();

    // A second drag is its own step.
    REQUIRE (history.perform (make ("insert.set", R"({"insert": 2, "volumeDb": -6})")).wasOk());
    CHECK (history.getUndoDescriptions().size() == 2);
    // A different command never joins.
    REQUIRE (history.performJoined (make ("project.setTempo", R"({"bpm": 100})")).wasOk());
    CHECK (history.getUndoDescriptions().size() == 3);

    history.undo();
    history.undo();
    CHECK (history.getProject() == after);
    history.undo();
    CHECK (history.getProject() == before);
    REQUIRE (history.redo());
    CHECK (history.getProject() == after);
}

TEST_CASE ("A transaction is one undo step and applies all or nothing", "[commands]")
{
    juce::String error;
    auto transaction = CommandRegistry::get().createTransaction (json (R"({
        "description": "AI: add a bass line",
        "commands": [
            {"command": "pattern.add", "parameters": {"name": "Bass line"}},
            {"command": "channel.set", "parameters": {"channel": 9, "volumeDb": -4}},
            {"command": "insert.setSend", "parameters": {"insert": 6, "target": 4, "levelDb": -12}}
        ]})"),
                                                                 error);
    INFO (error);
    REQUIRE (transaction != nullptr);

    CommandHistory history (makeExample());
    const auto before = history.getProject();
    REQUIRE (history.perform (std::move (transaction)).wasOk());
    CHECK (history.getUndoDescriptions() == juce::StringArray { "AI: add a bass line" });
    CHECK (history.getProject().patterns.size() == 2);

    history.undo();
    auto undone = history.getProject();
    undone.nextId = before.nextId;
    CHECK (undone == before);

    // The last step fails (feedback loop): the earlier steps are rolled back.
    auto failing = CommandRegistry::get().createTransaction (json (R"({
        "commands": [
            {"command": "channel.set", "parameters": {"channel": 9, "volumeDb": -4}},
            {"command": "insert.setOutput", "parameters": {"insert": 4, "target": 2}}
        ]})"),
                                                             error);
    REQUIRE (failing != nullptr);
    const auto result = history.perform (std::move (failing));
    CHECK (result.getErrorMessage().contains ("step 2 (insert.setOutput)"));
    auto unchanged = history.getProject();
    unchanged.nextId = before.nextId;
    CHECK (unchanged == before);

    CHECK (CommandRegistry::get().createTransaction (json (R"({"commands": []})"), error) == nullptr);
}

TEST_CASE ("Changes report what was touched", "[commands]")
{
    CommandHistory history (makeExample());
    ChangeSet last;
    history.addListener ([&] (const ChangeSet& c) { last = c; });

    history.perform (make ("pattern.setNotes", R"({"pattern": 11, "channel": 7, "notes": []})"));
    CHECK (last.patterns == std::set<PatternId> { PatternId { beat } });
    CHECK (last.channels.empty());

    history.perform (make ("insert.move", R"({"insert": 6, "index": 0})"));
    CHECK (last.insertOrder);

    history.perform (make ("insert.remove", R"({"insert": 2})"));
    CHECK (last.inserts.contains (InsertId { drums }));
    CHECK (last.inserts.contains (InsertId { bass }));   // lost its sidechain source
    CHECK (last.channels.contains (ChannelId { kick })); // rerouted to master
    CHECK (last.insertOrder);

    history.reset (makeExample());
    CHECK (last.everything);
    CHECK (! history.canUndo());
}

TEST_CASE ("docs/dev/commands.schema.json matches the registry", "[commands]")
{
    // Regenerate after changing commands: SPINUS_WRITE_COMMAND_SCHEMA=1 spinus_tests "[commands]"
    const auto schema = juce::JSON::toString (CommandRegistry::get().getSchema(),
                                              juce::JSON::FormatOptions().withSpacing (juce::JSON::Spacing::multiLine))
                            .replace ("\r\n", "\n") +
                        "\n";
    const auto file = testFile ("../docs/dev/commands.schema.json");

    if (juce::SystemStats::getEnvironmentVariable ("SPINUS_WRITE_COMMAND_SCHEMA", {}) == "1")
        REQUIRE (file.replaceWithText (schema, false, false, "\n"));

    INFO ("Out of date: regenerate with SPINUS_WRITE_COMMAND_SCHEMA=1 spinus_tests \"[commands]\"");
    CHECK (file.loadFileAsString().replace ("\r\n", "\n") == schema); // a Windows checkout may have CRLF
}

TEST_CASE ("An audio clip plays through its own new channel, or an existing one", "[commands]")
{
    CommandHistory history (makeExample());
    REQUIRE (history
                 .perform (make ("clip.addAudio",
                                 R"({"lane": 13, "file": "drums/Amen Break.wav", "start": 0, "length": 3840})"))
                 .wasOk());
    const auto& p = history.getProject();
    const auto& clip = p.playlist.clips.back();
    const auto* audio = std::get_if<spinus::model::AudioClip> (&clip.content);
    REQUIRE (audio != nullptr);
    CHECK (audio->file == "drums/Amen Break.wav");
    CHECK (audio->stretchMode == "off");
    const auto* channel = p.findChannel (audio->channel);
    REQUIRE (channel != nullptr);
    CHECK (channel->name == "Amen Break");
    CHECK (! channel->instrument.has_value());
    REQUIRE (p.findInsert (channel->targetInsert) != nullptr);
    CHECK (p.findInsert (channel->targetInsert)->name == "Amen Break");
    CHECK (history.getUndoDescriptions().strings.getLast() == "Add audio clip 'Amen Break'");

    // Removing the channel removes its audio clips too (undo brings both back).
    const auto clips = p.playlist.clips.size();
    REQUIRE (
        history.perform (make ("channel.remove", "{\"channel\": " + juce::String ((int)audio->channel.value) + "}"))
            .wasOk());
    CHECK (p.playlist.clips.size() == clips - 1);
    REQUIRE (history.undo());
    CHECK (p.playlist.clips.size() == clips);

    // Refused: an unknown channel or lane, an empty file, nothing to change.
    juce::String error;
    auto& registry = CommandRegistry::get();
    CHECK (history
               .perform (
                   make ("clip.addAudio", R"({"lane": 13, "file": "a.wav", "start": 0, "length": 10, "channel": 999})"))
               .failed());
    CHECK (history.perform (make ("clip.addAudio", R"({"lane": 999, "file": "a.wav", "start": 0, "length": 10})"))
               .failed());
    CHECK (registry.create ("clip.setAudio", juce::JSON::parse (R"({"clip": 16})"), error) == nullptr);
    CHECK (history.perform (make ("clip.setAudio", R"({"clip": 14, "gainDb": 1})")).failed()); // a pattern clip
}

TEST_CASE ("Automation clips: new lanes, sorted points, refusals", "[commands]")
{
    CommandHistory history (makeExample());
    const auto& p = history.getProject();
    const auto lanes = p.playlist.lanes.size();
    REQUIRE (history
                 .perform (make ("clip.addAutomation", R"({"start": 0, "length": 3840, "laneName": "Kick volume",
        "target": {"kind": "channelVolume", "channel": 7}, "points": [{"time": 3840, "value": 1}, {"time": 0, "value": 0}]})"))
                 .wasOk());
    REQUIRE (p.playlist.lanes.size() == lanes + 1);
    CHECK (p.playlist.lanes.back().name == "Kick volume");
    const auto& clip = p.playlist.clips.back();
    CHECK (clip.lane == p.playlist.lanes.back().id);
    const auto& automation = std::get<spinus::model::AutomationClip> (clip.content);
    REQUIRE (automation.points.size() == 2);
    CHECK (automation.points[0].time == 0); // sorted

    // One undo removes the clip and its new lane.
    REQUIRE (history.undo());
    CHECK (p.playlist.lanes.size() == lanes);

    CHECK (
        history
            .perform (make (
                "clip.addAutomation",
                R"({"start": 0, "length": 10, "target": {"kind": "channelVolume", "channel": 999}, "points": [{"time": 0, "value": 0}]})"))
            .failed());
    CHECK (history
               .perform (
                   make ("clip.addAutomation",
                         R"({"start": 0, "length": 10, "target": {"kind": "insertPan", "insert": 2}, "points": []})"))
               .failed());
    CHECK (
        history
            .perform (make (
                "clip.addAutomation",
                R"({"start": 0, "length": 10, "target": {"kind": "insertPan", "insert": 2}, "points": [{"time": 0, "value": 2}]})"))
            .failed());
    CHECK (history.perform (make ("clip.setPoints", R"({"clip": 14, "points": [{"time": 0, "value": 0}]})"))
               .failed()); // a pattern clip
}

TEST_CASE ("An insert's effects can take a channel's notes; removing the channel stops it", "[commands]")
{
    CommandHistory history (makeExample());
    REQUIRE (history.perform (make ("insert.setNotesFrom", R"({"insert": 6, "channel": 9})")).wasOk());
    CHECK (history.getProject().findInsert (InsertId { bass })->notesFrom == ChannelId { synth });

    // Saved in the project.
    Project loaded;
    REQUIRE (fromJsonText (toJsonText (history.getProject()), loaded).wasOk());
    CHECK (loaded.findInsert (InsertId { bass })->notesFrom == ChannelId { synth });
    CHECK (! toJsonText (makeExample()).contains ("notesFrom"));

    CHECK (history.perform (make ("insert.setNotesFrom", R"({"insert": 1, "channel": 9})")).failed()); // master
    CHECK (history.perform (make ("insert.setNotesFrom", R"({"insert": 6, "channel": 99})")).failed());

    REQUIRE (history.perform (make ("channel.remove", R"({"channel": 9})")).wasOk());
    CHECK (! history.getProject().findInsert (InsertId { bass })->notesFrom);
    CHECK (validate (history.getProject()).isEmpty());
    history.undo();
    CHECK (history.getProject().findInsert (InsertId { bass })->notesFrom == ChannelId { synth });

    REQUIRE (history.perform (make ("insert.setNotesFrom", R"({"insert": 6})")).wasOk());
    CHECK (! history.getProject().findInsert (InsertId { bass })->notesFrom);
}

TEST_CASE ("Spinus Tune notes: kept in order, saved, and overlapping ones refused", "[commands]")
{
    CommandHistory history (makeExample());
    REQUIRE (history
                 .perform (make ("clip.setTune", R"({"clip": 16, "notes": [
                     {"start": 2.0, "end": 2.5, "pitch": 67.0, "shift": 1},
                     {"start": 0.5, "end": 1.0, "pitch": 60.4, "shift": -0.4}]})"))
                 .wasOk());
    const auto& tune =
        std::get<spinus::model::AudioClip> (history.getProject().findClip (spinus::model::ClipId { 16 })->content).tune;
    REQUIRE (tune.size() == 2);
    CHECK (juce::approximatelyEqual (tune[0].start, 0.5));
    CHECK (juce::approximatelyEqual (tune[1].shift, 1.0f));

    Project loaded;
    REQUIRE (fromJsonText (toJsonText (history.getProject()), loaded).wasOk());
    CHECK (loaded == history.getProject());
    CHECK (! toJsonText (makeExample()).contains ("\"tune\""));

    CHECK (history
               .perform (make ("clip.setTune", R"({"clip": 16, "notes": [
        {"start": 0.5, "end": 1.0, "pitch": 60}, {"start": 0.9, "end": 1.5, "pitch": 62}]})"))
               .failed());
    CHECK (history.perform (make ("clip.setTune", R"({"clip": 14, "notes": []})")).failed()); // a pattern clip
    CHECK (createError ("clip.setTune", R"({"clip": 16, "notes": [{"start": 1.0, "end": 0.5, "pitch": 60}]})")
               .contains ("start < end"));
}

TEST_CASE ("The song's key: set, cleared and refused when unknown", "[commands]")
{
    CommandHistory history (makeExample()); // in A minor
    REQUIRE (history.perform (make ("project.setKey", R"({"tonic": 7, "scale": "major"})")).wasOk());
    CHECK (history.getProject().settings.key == spinus::model::Key { 7, "major" });
    CHECK (history.getUndoDescriptions()[0] == "Set key to G major");
    REQUIRE (history.perform (make ("project.setKey", "{}")).wasOk());
    CHECK (! history.getProject().settings.key);
    history.undo();
    CHECK (history.getProject().settings.key == spinus::model::Key { 7, "major" });
    CHECK (createError ("project.setKey", R"({"tonic": 12})").isNotEmpty());
    CHECK (createError ("project.setKey", R"({"scale": "lydian dominant"})").contains ("unknown scale"));
}

TEST_CASE ("pattern.setLength changes a pattern's length, one undo step", "[commands]")
{
    CommandHistory history (makeExample());
    const auto before = history.getProject().findPattern (PatternId { 11 })->length;
    REQUIRE (history.perform (make ("pattern.setLength", R"({"pattern": 11, "length": 11520})")).wasOk());
    CHECK (history.getProject().findPattern (PatternId { 11 })->length == 11520);
    CHECK (history.getUndoDescriptions().strings.getLast() == "Change pattern length");
    history.undo();
    CHECK (history.getProject().findPattern (PatternId { 11 })->length == before);
    CHECK (history.perform (make ("pattern.setLength", R"({"pattern": 99, "length": 3840})")).failed());
    CHECK (createError ("pattern.setLength", R"({"pattern": 11, "length": 0})").isNotEmpty());
}

TEST_CASE ("Time signatures: the song's, a pattern's own, and back to the song's", "[commands]")
{
    CommandHistory history (makeExample());
    REQUIRE (history.perform (make ("project.setTimeSignature", R"({"numerator": 5, "denominator": 4})")).wasOk());
    CHECK (history.getProject().settings.timeSignature == TimeSignature { 5, 4 });
    CHECK (history.getProject().getTimeSignature (PatternId { 11 }) == TimeSignature { 5, 4 });

    REQUIRE (history.perform (make ("pattern.setTimeSignature", R"({"pattern": 11, "numerator": 7, "denominator": 8})"))
                 .wasOk());
    CHECK (history.getProject().getTimeSignature (PatternId { 11 }) == TimeSignature { 7, 8 });
    CHECK (history.getProject().getTimeSignature (PatternId { 11 }).barTicks() == 3360);
    REQUIRE (history.perform (make ("pattern.setTimeSignature", R"({"pattern": 11})")).wasOk());
    CHECK (history.getProject().getTimeSignature (PatternId { 11 }) == TimeSignature { 5, 4 });
    history.undo();
    history.undo();
    history.undo();
    CHECK (history.getProject() == makeExample());

    CHECK (createError ("project.setTimeSignature", R"({"numerator": 4, "denominator": 3})").isNotEmpty());
    CHECK (createError ("pattern.setTimeSignature", R"({"pattern": 11, "numerator": 4})").isNotEmpty());
}

TEST_CASE ("clip.split cuts a clip in two that sound as the whole one did", "[commands]")
{
    CommandHistory history (makeExample());
    auto split = [&] (int clip, int at)
    {
        auto command =
            make ("clip.split", "{\"clip\": " + juce::String (clip) + ", \"at\": " + juce::String (at) + "}");
        auto* raw = command.get();
        REQUIRE (history.perform (std::move (command)).wasOk());
        return ClipId { (uint32_t)(int)raw->getResult()["clip"] };
    };
    const auto& clips = [&]() -> const std::vector<Clip>& { return history.getProject().playlist.clips; };

    // A pattern clip: the second part plays on from the cut, through its offset.
    const auto patternPart = split (15, 15360 + 1000); // the example's pattern is 4 bars
    const auto& first = *history.getProject().findClip (ClipId { 15 });
    const auto& second = *history.getProject().findClip (patternPart);
    CHECK (first.length == 1000);
    CHECK (second.start == 15360 + 1000);
    CHECK (second.length == 15360 - 1000);
    CHECK (std::get<PatternClip> (second.content).offset == 960 + 1000);
    CHECK (second.lane == first.lane);

    // An audio clip: offset too, and the fades stay at the outer ends.
    const auto audioPart = split (16, 2000);
    const auto& audioFirst = std::get<AudioClip> (history.getProject().findClip (ClipId { 16 })->content);
    const auto& audioSecond = std::get<AudioClip> (history.getProject().findClip (audioPart)->content);
    CHECK (audioSecond.offset == 2000);
    CHECK (audioFirst.fadeIn == 96);
    CHECK (audioFirst.fadeOut == 0);
    CHECK (audioSecond.fadeIn == 0);
    CHECK (audioSecond.fadeOut == 480);

    // An automation clip: both parts give the same values as the whole clip did.
    const auto whole = std::get<AutomationClip> (makeExample().findClip (ClipId { 17 })->content).points;
    const auto automationPart = split (17, 1500);
    const auto& a = std::get<AutomationClip> (history.getProject().findClip (ClipId { 17 })->content).points;
    const auto& b = std::get<AutomationClip> (history.getProject().findClip (automationPart)->content).points;
    for (int t = 0; t < 3840; t += 120)
    {
        const auto value =
            t < 1500 ? spinus::model::automationValueAt (a, t) : spinus::model::automationValueAt (b, t - 1500);
        CHECK (std::abs (value - spinus::model::automationValueAt (whole, t)) < 0.001f);
    }
    CHECK (clips().size() == makeExample().playlist.clips.size() + 3);

    CHECK (history.perform (make ("clip.split", R"({"clip": 14, "at": 15360})")).failed()); // at its end
    CHECK (history.perform (make ("clip.split", R"({"clip": 99, "at": 100})")).failed());
    history.undo();
    history.undo();
    history.undo();
    CHECK (history.getProject().playlist == makeExample().playlist); // IDs are not handed back
}

TEST_CASE ("A pattern's steps can be triplets, saved with the project", "[commands]")
{
    CommandHistory history (makeExample());
    CHECK (history.getProject().findPattern (PatternId { 11 })->getStepTicks() == 240);
    REQUIRE (history.perform (make ("pattern.setTripletSteps", R"({"pattern": 11, "triplets": true})")).wasOk());
    const auto& pattern = *history.getProject().findPattern (PatternId { 11 });
    CHECK (pattern.tripletSteps);
    CHECK (pattern.getStepTicks() == 160);

    Project reloaded;
    REQUIRE (spinus::model::fromJsonText (spinus::model::toJsonText (history.getProject()), reloaded).wasOk());
    CHECK (reloaded == history.getProject());

    history.undo();
    CHECK (history.getProject() == makeExample());
    CHECK (createError ("pattern.setTripletSteps", R"({"pattern": 11})").isNotEmpty());
    CHECK (history.perform (make ("pattern.setTripletSteps", R"({"pattern": 99, "triplets": true})")).failed());
}

TEST_CASE ("Swing: saved with the pattern, and which notes it delays", "[commands]")
{
    CommandHistory history (makeExample());
    REQUIRE (history.perform (make ("pattern.setSwing", R"({"pattern": 11, "swing": 0.5})")).wasOk());
    const auto& pattern = *history.getProject().findPattern (PatternId { 11 });
    CHECK (pattern.getSwingDelay (0) == 0);
    CHECK (pattern.getSwingDelay (240) == 60); // a second sixteenth: a quarter step later at 50%
    CHECK (pattern.getSwingDelay (480) == 0);
    CHECK (pattern.getSwingDelay (250) == 0); // off the grid: as it is
    Project reloaded;
    REQUIRE (spinus::model::fromJsonText (spinus::model::toJsonText (history.getProject()), reloaded).wasOk());
    CHECK (reloaded == history.getProject());
    CHECK (createError ("pattern.setSwing", R"({"pattern": 11, "swing": 1.5})").isNotEmpty());
    history.undo();
    CHECK (history.getProject() == makeExample());
}

TEST_CASE ("Markers: named points on the Playlist, in time order, saved with the project", "[commands]")
{
    CommandHistory history (makeExample());
    REQUIRE (history.perform (make ("marker.add", R"({"at": 7680})")).wasOk());
    REQUIRE (history.perform (make ("marker.add", R"({"at": 3840, "name": " Chorus "})")).wasOk());
    const auto& markers = history.getProject().playlist.markers;
    REQUIRE (markers.size() == 2);
    CHECK (markers[0].name == "Chorus"); // in time order
    CHECK (markers[1].name == "Marker 1");
    REQUIRE (
        history.perform (make ("marker.set", "{\"marker\": " + juce::String (markers[0].id.value) + ", \"at\": 15360}"))
            .wasOk());
    CHECK (history.getProject().playlist.markers[1].name == "Chorus"); // moved after the other

    Project reloaded;
    REQUIRE (spinus::model::fromJsonText (spinus::model::toJsonText (history.getProject()), reloaded).wasOk());
    CHECK (reloaded == history.getProject());
    // A project without markers is written as before: no "markers" at all.
    CHECK (! spinus::model::toJsonText (makeExample()).contains ("markers"));

    REQUIRE (
        history.perform (make ("marker.remove", "{\"marker\": " + juce::String (markers[0].id.value) + "}")).wasOk());
    CHECK (history.getProject().playlist.markers.size() == 1);
    CHECK (history.perform (make ("marker.remove", R"({"marker": 999})")).failed());
    CHECK (
        history
            .perform (make ("marker.set", "{\"marker\": " + juce::String (markers[0].id.value) + ", \"name\": \" \"}"))
            .failed());
    CHECK (createError ("marker.add", R"({"name": "x"})").isNotEmpty());
}

TEST_CASE ("A pattern is cloned, and a clip made unique gets a copy of its own", "[commands]")
{
    CommandHistory history (makeExample());
    const auto& source = *history.getProject().findPattern (PatternId { 11 });
    const auto sourceName = source.name;
    auto clone = make ("pattern.clone", R"({"pattern": 11})");
    auto* raw = clone.get();
    REQUIRE (history.perform (std::move (clone)).wasOk());
    const PatternId copyId { (uint32_t)(int)raw->getResult()["pattern"] };
    const auto* copy = history.getProject().findPattern (copyId);
    REQUIRE (copy != nullptr);
    CHECK (copy->name == sourceName + " 2");
    CHECK (copy->notes == history.getProject().findPattern (PatternId { 11 })->notes);
    CHECK (copy->length == history.getProject().findPattern (PatternId { 11 })->length);
    CHECK (history.getProject().patterns[1].id == copyId); // right after the original
    // A copy of the copy counts on.
    REQUIRE (history.perform (make ("pattern.clone", "{\"pattern\": " + juce::String (copyId.value) + "}")).wasOk());
    CHECK (history.getProject().patterns[2].name == sourceName + " 3");
    history.undo();
    history.undo();
    CHECK (history.getProject().patterns == makeExample().patterns); // IDs are not handed back

    // The example's first two playlist clips both play pattern 11: making the second unique leaves the first.
    std::vector<ClipId> clips;
    for (auto& clip : history.getProject().playlist.clips)
        if (auto* pc = std::get_if<PatternClip> (&clip.content); pc != nullptr && pc->pattern == PatternId { 11 })
            clips.push_back (clip.id);
    REQUIRE (clips.size() >= 2);
    auto unique = make ("clip.makeUnique", "{\"clip\": " + juce::String (clips[1].value) + "}");
    auto* rawUnique = unique.get();
    REQUIRE (history.perform (std::move (unique)).wasOk());
    const PatternId own { (uint32_t)(int)rawUnique->getResult()["pattern"] };
    CHECK (std::get<PatternClip> (history.getProject().findClip (clips[1])->content).pattern == own);
    CHECK (std::get<PatternClip> (history.getProject().findClip (clips[0])->content).pattern == PatternId { 11 });
    CHECK (std::get<PatternClip> (history.getProject().findClip (clips[1])->content).offset == 960); // kept
    CHECK (history.getUndoDescriptions().strings.getLast().startsWith ("Make clip unique"));
    history.undo();
    CHECK (history.getProject().patterns == makeExample().patterns);
    CHECK (history.getProject().playlist == makeExample().playlist);

    CHECK (history.perform (make ("pattern.clone", R"({"pattern": 99})")).failed());
    CHECK (history.perform (make ("clip.makeUnique", R"({"clip": 99})")).failed());
}

TEST_CASE ("clip.add places a whole clip as the project file writes it", "[commands]")
{
    CommandHistory history (makeExample());
    for (const auto& source : makeExample().playlist.clips)
    {
        auto json = spinus::model::clipToJson (source);
        json.getDynamicObject()->setProperty ("start", 96000);
        auto add = make ("clip.add", "{\"clip\": " + juce::JSON::toString (json, true) + "}");
        auto* rawAdd = add.get();
        REQUIRE (history.perform (std::move (add)).wasOk());
        const ClipId made { (uint32_t)(int)rawAdd->getResult()["clip"] };
        REQUIRE (made != source.id);

        const auto* placed = history.getProject().findClip (made);
        REQUIRE (placed != nullptr);
        CHECK (placed->lane == source.lane);
        CHECK (placed->start == 96000);
        CHECK (placed->length == source.length);
        CHECK (placed->muted == source.muted);
        CHECK (placed->content == source.content);
        CHECK (*history.getProject().findClip (source.id) == source); // the original stays
        CHECK (history.getUndoDescriptions().strings.getLast() == "Add clip");
    }
    while (history.canUndo())
        history.undo();
    CHECK (history.getProject().playlist == makeExample().playlist);

    CHECK (history
               .perform (make ("clip.add", R"({"clip": {"type": "pattern", "pattern": 11, "lane": 999,
        "start": 0, "length": 960}})"))
               .failed());
    CHECK (history
               .perform (make ("clip.add", R"({"clip": {"type": "pattern", "pattern": 999, "lane": 13,
        "start": 0, "length": 960}})"))
               .failed());
    juce::String error;
    CHECK (CommandRegistry::get().create ("clip.add",
                                          json (R"({"clip": {"type": "song", "lane": 13, "start": 0, "length": 960}})"),
                                          error) == nullptr);
    CHECK (error.contains ("unknown type"));
}

TEST_CASE ("Patterns and playlist tracks get names and colours", "[commands]")
{
    CommandHistory history (makeExample());
    REQUIRE (history.perform (make ("pattern.set", R"({"pattern": 11, "name": "  Chorus  ", "colour": "#ffd65c5c"})"))
                 .wasOk());
    CHECK (history.getProject().findPattern (PatternId { 11 })->name == "Chorus");
    CHECK (history.getProject().findPattern (PatternId { 11 })->colour == 0xffd65c5cu);
    CHECK (history.getUndoDescriptions().strings.getLast() == "Rename pattern to 'Chorus'");
    REQUIRE (history.perform (make ("lane.set", R"({"lane": 13, "colour": "#ff5cd6a0"})")).wasOk());
    CHECK (history.getProject().findLane (LaneId { 13 })->colour == 0xff5cd6a0u);
    CHECK (history.getUndoDescriptions().strings.getLast() == "Colour playlist track");

    Project reloaded;
    REQUIRE (spinus::model::fromJsonText (spinus::model::toJsonText (history.getProject()), reloaded).wasOk());
    CHECK (reloaded == history.getProject());

    history.undo();
    history.undo();
    CHECK (history.getProject() == makeExample());
    CHECK (createError ("pattern.set", R"({"pattern": 11})").isNotEmpty());
    CHECK (history.perform (make ("pattern.set", R"({"pattern": 11, "name": " "})")).failed());
    CHECK (history.perform (make ("pattern.set", R"({"pattern": 99, "name": "x"})")).failed());
}

TEST_CASE ("device.setParameters sets an effect's or instrument's settings, and undo takes them back", "[commands]")
{
    CommandHistory history (makeExample());
    // The drums bus's compressor (device 3).
    REQUIRE (history.perform (make ("device.setParameters", R"({"device": 3, "values": {"ratio": 0.25}})")).wasOk());
    REQUIRE (history.perform (make ("device.setParameters", R"({"device": 3, "values": {"ratio": 0.5, "knee": 1}})"))
                 .wasOk());
    const auto* insert = history.getProject().findInsert (spinus::model::InsertId { 2 });
    REQUIRE (insert != nullptr);
    const auto& parameters = insert->effects[0].device.parameters;
    CHECK (parameters.size() == 2);
    CHECK (juce::approximatelyEqual (parameters.at ("ratio"), 0.5f));
    CHECK (history.getUndoDescriptions().strings.getLast().startsWith ("Change 2 settings of"));

    REQUIRE (history.undo());
    CHECK (parameters.size() == 1); // "knee" was new: gone again
    CHECK (juce::approximatelyEqual (parameters.at ("ratio"), 0.25f));
    REQUIRE (history.undo());
    CHECK (parameters.empty());

    // Instruments too; saved in project.json and checked when loading.
    const auto instrument = history.getProject().channels[0].instrument.value_or (spinus::model::Device {}).id;
    REQUIRE (history
                 .perform (make ("device.setParameters", R"({"device": )" + juce::String (instrument.value) +
                                                             R"(, "values": {"cutoff": 0.3}})"))
                 .wasOk());
    const auto json = spinus::model::toJson (history.getProject());
    spinus::model::Project loaded;
    REQUIRE (spinus::model::fromJson (json, loaded).wasOk());
    CHECK (loaded == history.getProject());
    auto broken = history.getProject();
    for (auto& c : broken.channels)
        if (c.instrument && ! c.instrument->parameters.empty())
            c.instrument->parameters["cutoff"] = 2.0f;
    CHECK (spinus::model::validate (broken).joinIntoString (" ").contains ("outside 0 to 1"));

    CHECK (createError ("device.setParameters", R"({"device": 3, "values": {"ratio": 1.5}})").isNotEmpty());
    CHECK (createError ("device.setParameters", R"({"device": 3, "values": {}})").isNotEmpty());
    CHECK (createError ("device.setParameters", R"({"device": 3})").isNotEmpty());
    CHECK (! history.perform (make ("device.setParameters", R"({"device": 999, "values": {"x": 0}})")).wasOk());
}

TEST_CASE ("Clip launcher: tracks, scenes and slots, one clip per slot", "[commands][launcher]")
{
    CommandHistory history (makeExample());
    const auto& launcher = history.getProject().launcher;

    // A pattern into a filled slot replaces the clip there.
    auto put = make ("launcher.addPattern", R"({"track": 18, "scene": 20, "pattern": 11})");
    auto* command = put.get();
    REQUIRE (history.perform (std::move (put)).wasOk());
    const auto clip = (int)command->getResult()["clip"];
    CHECK (launcher.clips.size() == 2);
    const auto* placed = launcher.findClip (spinus::model::LauncherTrackId { 18 }, spinus::model::SceneId { 20 });
    REQUIRE (placed != nullptr);
    CHECK ((int)placed->id.value == clip);
    CHECK (placed->length == history.getProject().findPattern (spinus::model::PatternId { 11 })->length);
    CHECK (std::holds_alternative<spinus::model::PatternClip> (placed->content));

    // Audio gets its own channel and insert unless given one; redo makes the same IDs.
    auto audio = make ("launcher.addAudio", R"({"track": 18, "scene": 19, "file": "loops/pad.wav", "length": 7680})");
    command = audio.get();
    REQUIRE (history.perform (std::move (audio)).wasOk());
    const auto made = command->getResult();
    CHECK (history.getProject().findChannel (spinus::model::ChannelId { (uint32_t)(int)made["channel"] })->name ==
           "pad");
    REQUIRE (history.undo());
    REQUIRE (history.redo());
    CHECK (history.getProject().findChannel (spinus::model::ChannelId { (uint32_t)(int)made["channel"] }) != nullptr);

    // Removing a channel takes the audio launcher clips that play through it.
    REQUIRE (history.perform (make ("channel.remove", R"({"channel": )" + made["channel"].toString() + "}")).wasOk());
    CHECK (launcher.findClip (spinus::model::LauncherTrackId { 18 }, spinus::model::SceneId { 19 }) == nullptr);
    REQUIRE (history.undo());

    // Moving onto a filled slot replaces it; removing a scene takes its clips.
    REQUIRE (history.perform (make ("launcher.setClip", R"({"clip": )" + juce::String (clip) + R"(, "scene": 19})"))
                 .wasOk());
    CHECK (launcher.clips.size() == 1);
    REQUIRE (history.perform (make ("launcher.removeScene", R"({"scene": 19})")).wasOk());
    CHECK (launcher.clips.empty());
    REQUIRE (history.undo());

    // Removing a pattern takes the launcher clips that play it.
    REQUIRE (history.perform (make ("pattern.remove", R"({"pattern": 11})")).wasOk());
    CHECK (launcher.clips.empty());
    REQUIRE (history.undo());

    // Refused: missing slots, patterns and clips, a bad quantize, nothing to change.
    CHECK (! history.perform (make ("launcher.addPattern", R"({"track": 18, "scene": 999, "pattern": 11})")).wasOk());
    CHECK (! history.perform (make ("launcher.addPattern", R"({"track": 18, "scene": 19, "pattern": 999})")).wasOk());
    CHECK (! history.perform (make ("launcher.removeClip", R"({"clip": 999})")).wasOk());
    CHECK (createError ("launcher.setQuantize", R"({"quantize": "sometimes"})").isNotEmpty());
    CHECK (createError ("launcher.setClip", R"({"clip": 21})").isNotEmpty());
    CHECK (createError ("launcher.setTrack", R"({"track": 18, "colour": "red"})").isNotEmpty());

    // What changed is reported as the launcher.
    REQUIRE (history.perform (make ("launcher.setQuantize", R"({"quantize": "beat"})")).wasOk());
    CHECK (history.getProject().launcher.quantize == spinus::model::LaunchQuantize::beat);
}

TEST_CASE ("A pattern is as long as its notes; its Playlist clips follow unless resized by hand", "[commands]")
{
    auto project = makeExample(); // 3/4; pattern 11 is placed by clips 14-17
    CommandHistory history (project);
    auto clipLength = [&] (int id) { return history.getProject().findClip (ClipId { (juce::uint32)id })->length; };
    auto patternLength = [&] { return history.getProject().findPattern (PatternId { beat })->length; };
    const auto bar = history.getProject().settings.timeSignature.barTicks();

    // Fit to the notes: one bar. One clip made longer by hand first.
    REQUIRE (
        history
            .perform (make ("pattern.setNotes",
                            R"({"pattern": 11, "channel": 7, "notes": [{"start": 0, "length": 240, "pitch": 36}]})"))
            .wasOk());
    REQUIRE (history.perform (make ("pattern.setNotes", R"({"pattern": 11, "channel": 9, "notes": []})")).wasOk());
    REQUIRE (patternLength() == bar);
    std::vector<int> clips;
    for (auto& c : history.getProject().playlist.clips)
        if (auto* pc = std::get_if<PatternClip> (&c.content); pc != nullptr && pc->pattern == PatternId { beat })
            clips.push_back ((int)c.id.value);
    REQUIRE (clips.size() >= 2);
    REQUIRE (history
                 .perform (make ("clip.set", "{\"clip\": " + juce::String (clips[1]) +
                                                 ", \"length\": " + juce::String (3 * bar) + "}"))
                 .wasOk());
    const auto before = history.getProject();

    // A note in the second bar: two bars, and so are its clips, except the one resized by hand.
    REQUIRE (
        history
            .perform (make ("pattern.setNotes", "{\"pattern\": 11, \"channel\": 7, \"notes\": [{\"start\": 0, "
                                                "\"length\": 240, \"pitch\": 36}, {\"start\": " +
                                                    juce::String (bar + 240) + ", \"length\": 240, \"pitch\": 36}]}"))
            .wasOk());
    CHECK (patternLength() == 2 * bar);
    CHECK (clipLength (clips[0]) == 2 * bar);
    CHECK (clipLength (clips[1]) == 3 * bar);

    // Undo puts the pattern and its clips back.
    REQUIRE (history.undo());
    CHECK (history.getProject() == before);

    // 4/4 for the pattern: one 4/4 bar, the clip too.
    REQUIRE (history.perform (make ("pattern.setTimeSignature", R"({"pattern": 11, "numerator": 4, "denominator": 4})"))
                 .wasOk());
    CHECK (patternLength() == 3840);
    CHECK (clipLength (clips[0]) == 3840);
    REQUIRE (history.undo());
    CHECK (history.getProject() == before);
}

TEST_CASE ("The newest undo steps can be joined into one", "[commands]")
{
    CommandHistory history;
    auto add = [&history] (const juce::String& name)
    {
        juce::String error;
        auto c = CommandRegistry::get().create (
            "channel.add", juce::JSON::parse ("{\"name\": \"" + name + "\", \"audioTrack\": true}"), error);
        REQUIRE (c != nullptr);
        REQUIRE (history.perform (std::move (c)).wasOk());
    };
    add ("A");
    add ("B");
    add ("C");
    CHECK (history.getProject().channels[1].audioTrack);
    CHECK (! history.joinLastSteps (4, "Too many"));
    REQUIRE (history.joinLastSteps (2, "Add B and C"));
    CHECK (history.getUndoDescriptions() == juce::StringArray { "Add audio track 'A'", "Add B and C" });
    REQUIRE (history.undo());
    CHECK (history.getProject().channels.size() == 1);
    REQUIRE (history.redo());
    CHECK (history.getProject().channels.size() == 3);
}

TEST_CASE ("device.setParameters reads text for Spinus devices; for plugins only with a Session", "[commands]")
{
    CommandHistory history (makeExample());
    auto add =
        make ("insert.addEffect", R"({"insert": 2, "device": {"kind": "stock", "stockType": "spinus.compressor"}})");
    auto* raw = add.get();
    REQUIRE (history.perform (std::move (add)).wasOk());
    const auto compressor = juce::String ((int)raw->getResult()["device"]);

    REQUIRE (history
                 .perform (make (
                     "device.setParameters",
                     R"({"device": )" + compressor +
                         R"(, "values": {"knee": 0.5}, "text": {"Ratio": "4:1", "attack": "15 ms", "mix": "50"}})"))
                 .wasOk());
    const auto* insert = history.getProject().findInsert (spinus::model::InsertId { 2 });
    REQUIRE (insert != nullptr);
    const auto& parameters = insert->effects.back().device.parameters;
    CHECK (parameters.size() == 4);
    CHECK (std::abs (parameters.at ("mix") - 0.5f) < 1.0e-4f);
    CHECK (std::abs (parameters.at ("knee") - 0.5f) < 1.0e-4f);
    CHECK (parameters.at ("ratio") > 0.0f);
    CHECK (parameters.at ("ratio") < 1.0f);

    // The LSP compressor (device 3) is a plugin: without a Session to open it, values from 0 to 1 only
    // (DeviceParametersTests reads plugins' text).
    auto lsp = make ("device.setParameters", R"({"device": 3, "text": {"ratio": "4:1"}})");
    const auto result = history.perform (std::move (lsp));
    CHECK (result.failed());
    CHECK (result.getErrorMessage().contains ("'values'"));
    CHECK (createError ("device.setParameters", R"({"device": 3, "text": "4:1"})").isNotEmpty());
}
