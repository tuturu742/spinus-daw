#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ExampleProject.h"
#include "commands/CommandHistory.h"
#include "commands/CommandRegistry.h"
#include "model/ModelJson.h"
#include "model/Tempo.h"
#include "model/Validation.h"

using namespace spinus::model;
using Catch::Approx;
using spinus::commands::CommandHistory;
using spinus::commands::CommandRegistry;

namespace
{
constexpr Ticks bar = 4 * ticksPerQuarterNote;

std::unique_ptr<spinus::commands::Command> make (const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = CommandRegistry::get().create (name, juce::JSON::parse (params), error);
    INFO (error);
    REQUIRE (command != nullptr);
    return command;
}

/** A project at 120 BPM with two audio clips at bar 3, one of each kind, 2 s long, with an offset and fades. */
Project twoClips()
{
    auto p = spinus::test::makeExample();
    p.settings.tempoBpm = 120.0;
    const auto lane = p.playlist.lanes.front().id;
    for (auto kind : { "time", "beat" })
    {
        AudioClip audio { "take.wav", {}, 480, 0.0f, 0.0f, "off", 96, 192 };
        audio.onTempoChange = kind;
        p.playlist.clips.push_back ({ p.allocateId<ClipId>(), lane, 2 * bar, 4 * ticksPerQuarterNote, false, audio });
    }
    return p;
}

const Clip& clipOf (const Project& p, const char* kind)
{
    for (auto& c : p.playlist.clips)
        if (auto* a = std::get_if<AudioClip> (&c.content);
            a != nullptr && a->file == "take.wav" && a->onTempoChange == kind)
            return c;
    FAIL ("no clip");
    return p.playlist.clips.front();
}
} // namespace

TEST_CASE ("The tempo map: seconds and ticks through tempo jumps", "[tempo]")
{
    ProjectSettings s;
    s.tempoBpm = 120.0;                     // a bar is 2 s
    s.tempoChanges = { { 2 * bar, 60.0 } }; // from bar 3, a bar is 4 s
    const TempoMap map (s);

    CHECK (map.seconds (0.0) == Approx (0.0));
    CHECK (map.seconds ((double)bar) == Approx (2.0));
    CHECK (map.seconds ((double)(2 * bar)) == Approx (4.0));
    CHECK (map.seconds ((double)(3 * bar)) == Approx (8.0));
    CHECK (map.ticks (8.0) == Approx ((double)(3 * bar)));
    CHECK (map.ticks (3.0) == Approx (1.5 * (double)bar));
    CHECK (map.bpmAt ((double)(2 * bar - 1)) == Approx (120.0));
    CHECK (map.bpmAt ((double)(2 * bar)) == Approx (60.0));

    CHECK (tempoChangesProblem ({ { bar, 90.0 }, { 2 * bar, 100.0 } }).isEmpty());
    CHECK (tempoChangesProblem ({ { 0, 90.0 } }).isNotEmpty());                       // at the start: that is tempoBpm
    CHECK (tempoChangesProblem ({ { 2 * bar, 90.0 }, { bar, 100.0 } }).isNotEmpty()); // out of order
    CHECK (tempoChangesProblem ({ { bar, 5.0 } }).isNotEmpty());                      // too slow
}

TEST_CASE ("A tempo change keeps a 'time' clip at its second and a 'beat' clip on its beat", "[tempo][commands]")
{
    CommandHistory history (twoClips());
    const auto before = history.getProject();

    // Slower from bar 2: the clips at bar 3 are later in seconds now, unless fixed in time.
    REQUIRE (
        history.perform (make ("project.setTempoChange", "{\"at\": " + juce::String (bar) + ", \"bpm\": 60}")).wasOk());
    const auto& p = history.getProject();
    REQUIRE (p.settings.tempoChanges.size() == 1);
    const TempoMap was (before.settings), now (p.settings);

    const auto& fixed = clipOf (p, "time");
    CHECK (now.seconds ((double)fixed.start) == Approx (4.0).margin (0.001)); // still 4 s in
    CHECK (now.seconds ((double)(fixed.start + fixed.length)) - now.seconds ((double)fixed.start) ==
           Approx (2.0).margin (0.001));
    CHECK (fixed.start < 2 * bar); // so now earlier in the bars

    const auto& onBeat = clipOf (p, "beat");
    CHECK (onBeat.start == 2 * bar); // on its bar
    CHECK (now.seconds ((double)(onBeat.start + onBeat.length)) - now.seconds ((double)onBeat.start) ==
           Approx (2.0).margin (0.001)); // plays as long as before

    // Offset and fades: as long in seconds at the clip's new tempo (half as many ticks at half the tempo).
    const auto& audio = std::get<AudioClip> (onBeat.content);
    CHECK (audio.offset == 240);
    CHECK (audio.fadeIn == 48);
    CHECK (audio.fadeOut == 96);

    // The song's own tempo follows the same rules.
    REQUIRE (history.perform (make ("project.setTempo", R"({"bpm": 90})")).wasOk());
    CHECK (TempoMap (history.getProject().settings).seconds ((double)clipOf (history.getProject(), "time").start) ==
           Approx (4.0).margin (0.001));

    // One undo step each, back to where they were.
    REQUIRE (history.undo());
    REQUIRE (history.undo());
    CHECK (history.getProject() == before);
    (void)was;
}

TEST_CASE ("Tempo changes are moved, changed and removed as commands, and bad ones refused", "[tempo][commands]")
{
    CommandHistory history (twoClips());
    REQUIRE (history.perform (make ("project.setTempoChange", "{\"at\": " + juce::String (bar) + ", \"bpm\": 100}"))
                 .wasOk());
    REQUIRE (history
                 .perform (make ("project.setTempoChange", "{\"at\": " + juce::String (2 * bar) +
                                                               ", \"bpm\": 140, \"from\": " + juce::String (bar) + "}"))
                 .wasOk());
    REQUIRE (history.getProject().settings.tempoChanges == std::vector<TempoChange> { { 2 * bar, 140.0 } });

    CHECK (history.perform (make ("project.removeTempoChange", "{\"at\": 5}")).failed());
    CHECK (history.perform (make ("project.setTempoChange", "{\"at\": 5, \"bpm\": 100, \"from\": 7}")).failed());
    juce::String error;
    CHECK (CommandRegistry::get().create ("project.setTempoChange", juce::JSON::parse (R"({"at": 0, "bpm": 100})"),
                                          error) == nullptr);

    REQUIRE (history.perform (make ("project.removeTempoChange", "{\"at\": " + juce::String (2 * bar) + "}")).wasOk());
    CHECK (history.getProject().settings.tempoChanges.empty());
}

TEST_CASE ("Tempo changes and each clip's choice are saved; older projects get the default", "[tempo][model]")
{
    auto p = twoClips();
    p.settings.tempoChanges = { { bar, 100.0 }, { 3 * bar, 140.0 } };
    Project read;
    REQUIRE (fromJson (toJson (p), read).wasOk());
    CHECK (read.settings.tempoChanges == p.settings.tempoChanges);
    CHECK (std::get<AudioClip> (clipOf (read, "time").content).onTempoChange == "time");
    CHECK (std::get<AudioClip> (clipOf (read, "beat").content).onTempoChange == "beat");

    // Saved before the choice existed: a long clip stays at its second, a short one on its beat.
    auto json = toJson (twoClips());
    auto* clips = json["playlist"]["clips"].getArray();
    REQUIRE (clips != nullptr);
    for (auto& c : *clips)
        if (auto* o = c.getDynamicObject(); o != nullptr && o->getProperty ("type") == "audio")
        {
            o->removeProperty ("onTempoChange");
            if (o->getProperty ("file") == "take.wav" && (int)o->getProperty ("start") == (int)(2 * bar))
                o->setProperty ("length", 57600); // 30 s at 120 BPM
        }
    Project old;
    REQUIRE (fromJson (json, old).wasOk());
    for (auto& c : old.playlist.clips)
        if (auto* a = std::get_if<AudioClip> (&c.content))
            CHECK (a->onTempoChange == (TempoMap (old.settings).seconds ((double)c.length) > 20.0 ? "time" : "beat"));

    // A list that cannot be is refused, or left out when salvaging.
    auto bad = toJson (p);
    bad["settings"].getDynamicObject()->setProperty (
        "tempoChanges", juce::JSON::parse (R"([{"at": 0, "bpm": 100}, {"at": 3840, "bpm": 5}])"));
    CHECK (fromJson (bad, old).failed());
    juce::StringArray skipped;
    REQUIRE (fromJsonSalvaging (bad, old, skipped).wasOk());
    CHECK (old.settings.tempoChanges.empty());
    CHECK (skipped.joinIntoString ("\n").contains ("tempo change"));
}
