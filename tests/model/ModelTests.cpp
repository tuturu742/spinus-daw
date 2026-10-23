#include <catch2/catch_test_macros.hpp>

#include "ExampleProject.h"
#include "model/ModelJson.h"
#include "model/Validation.h"

using namespace spinus::model;
using spinus::test::makeExample;

namespace
{

juce::File exampleFile() { return juce::File (SPINUS_TEST_FILES_DIR).getChildFile ("model/files/example-v1.json"); }

Project roundTrip (const Project& p)
{
    Project loaded;
    const auto result = fromJsonText (toJsonText (p), loaded);
    INFO (result.getErrorMessage());
    REQUIRE (result.wasOk());
    return loaded;
}

void expectInvalid (const Project& p, const juce::String& expectedProblem)
{
    const auto problems = validate (p);
    INFO ("problems: " << problems.joinIntoString ("; "));
    CHECK (problems.joinIntoString ("\n").contains (expectedProblem));

    Project loaded;
    CHECK (fromJsonText (toJsonText (p), loaded).failed());
}

} // namespace

//==============================================================================
TEST_CASE ("A new project has only the master insert and is valid", "[model]")
{
    Project p;
    CHECK (validate (p).isEmpty());
    CHECK (p.mixer.master.id == masterInsertId);
    CHECK (p.mixer.inserts.empty());
    CHECK (p.channels.empty());
    CHECK (roundTrip (p) == p);
}

TEST_CASE ("Every model object survives a JSON round trip", "[model]")
{
    const auto p = makeExample();
    INFO (validate (p).joinIntoString ("; "));
    REQUIRE (validate (p).isEmpty());

    const auto loaded = roundTrip (p);
    CHECK (loaded == p);
    CHECK (toJsonText (loaded) == toJsonText (p)); // deterministic output
}

TEST_CASE ("Schema version 1 files keep loading", "[model]")
{
    // The checked-in file pins the on-disk format. If this test fails after a deliberate format change,
    // bump the schema version and add a migration instead of editing the file.
    // Writing the file is only for creating it the first time: SPINUS_WRITE_MODEL_EXAMPLE=1 spinus_tests.
    if (juce::SystemStats::getEnvironmentVariable ("SPINUS_WRITE_MODEL_EXAMPLE", {}) == "1")
        REQUIRE (exampleFile().replaceWithText (toJsonText (makeExample()) + "\n", false, false, "\n"));

    REQUIRE (exampleFile().existsAsFile());

    Project loaded;
    const auto result = fromJsonText (exampleFile().loadFileAsString(), loaded);
    INFO (result.getErrorMessage());
    REQUIRE (result.wasOk());
    CHECK (loaded == makeExample());
}

TEST_CASE ("IDs are unique, stable and never reused", "[model]")
{
    Project p;
    const auto a = p.allocateId<ChannelId>();
    const auto b = p.allocateId<InsertId>();
    CHECK (a.value != b.value);
    CHECK (a.value != masterInsertId.value);

    // Deleting an object does not give its ID back.
    auto example = makeExample();
    const auto before = example.nextId;
    const auto removed = example.channels.back().id;
    example.channels.pop_back();
    example.patterns[0].notes.erase (removed);
    CHECK (validate (example).isEmpty());
    CHECK (example.allocateId<ChannelId>().value == before);
}

TEST_CASE ("Validation catches broken references and limits", "[model]")
{
    SECTION ("channel routed to a missing insert")
    {
        auto p = makeExample();
        p.channels[0].targetInsert = InsertId { 999 };
        expectInvalid (p, "targets a missing insert");
    }

    SECTION ("duplicate ID")
    {
        auto p = makeExample();
        p.patterns[0].id = PatternId { p.channels[0].id.value };
        expectInvalid (p, "reuses ID");
    }

    SECTION ("ID not below nextId")
    {
        auto p = makeExample();
        p.nextId = 3;
        expectInvalid (p, "not below nextId");
    }

    SECTION ("routing cycle through a send")
    {
        auto p = makeExample();
        auto& reverb = p.mixer.inserts[1];
        reverb.output = p.mixer.inserts[0].id; // reverb -> drums, while drums sends to reverb
        expectInvalid (p, "routing cycle");
    }

    SECTION ("any number of effects is fine")
    {
        auto p = makeExample();
        auto& insert = p.mixer.inserts[2];
        for (int i = 0; i < 50; ++i)
            insert.effects.push_back (
                { Device { p.allocateId<DeviceId>(), Device::Kind::stock, "spinus.utility", {}, false }, 1.0f });
        CHECK (validate (p).isEmpty());
    }

    SECTION ("invalid note")
    {
        auto p = makeExample();
        p.patterns[0].notes.begin()->second[0].pitch = 128;
        expectInvalid (p, "invalid note");
    }

    SECTION ("clip plays a missing pattern")
    {
        auto p = makeExample();
        std::get<PatternClip> (p.playlist.clips[0].content).pattern = PatternId { 999 };
        expectInvalid (p, "missing pattern");
    }

    SECTION ("automation of a device that no longer exists")
    {
        auto p = makeExample();
        p.mixer.inserts[1].effects.clear();
        expectInvalid (p, "automates a missing target");
    }

    SECTION ("master with an output")
    {
        auto p = makeExample();
        p.mixer.master.output = p.mixer.inserts[0].id;
        expectInvalid (p, "master insert cannot have an output");
    }
}

TEST_CASE ("Routing cycle check for new connections", "[model]")
{
    const auto p = makeExample();
    const auto drums = p.mixer.inserts[0].id, reverb = p.mixer.inserts[1].id, bass = p.mixer.inserts[2].id;

    CHECK (wouldCreateRoutingCycle (p, drums, drums));
    CHECK (wouldCreateRoutingCycle (p, reverb, drums));        // drums already sends to reverb
    CHECK (wouldCreateRoutingCycle (p, masterInsertId, bass)); // everything ends at master
    CHECK_FALSE (wouldCreateRoutingCycle (p, bass, reverb));
    CHECK_FALSE (wouldCreateRoutingCycle (p, drums, bass));
}

TEST_CASE ("Loading rejects files Spinus cannot read safely", "[model]")
{
    Project p;

    SECTION ("newer schema")
    {
        const auto result = fromJsonText (R"({"schemaVersion": 99})", p);
        CHECK (result.getErrorMessage().contains ("newer version of Spinus"));
    }

    SECTION ("no schema version")
    {
        CHECK (fromJsonText (R"({"settings": {}})", p).getErrorMessage().contains ("no schemaVersion"));
    }

    SECTION ("not JSON")
    {
        CHECK (fromJsonText ("{ this is not json", p).getErrorMessage().contains ("not valid JSON"));
    }

    SECTION ("wrong value type")
    {
        auto json = toJson (makeExample());
        json["settings"].getDynamicObject()->setProperty ("tempoBpm", "fast");
        CHECK (fromJson (json, p).getErrorMessage().contains ("'tempoBpm' must be a number"));
    }

    SECTION ("unknown clip type")
    {
        auto json = toJson (makeExample());
        json["playlist"]["clips"][0].getDynamicObject()->setProperty ("type", "hologram");
        CHECK (fromJson (json, p).getErrorMessage().contains ("unknown type 'hologram'"));
    }
}

TEST_CASE ("The clip launcher is saved and checked", "[model][launcher]")
{
    const auto example = makeExample();
    const auto loaded = roundTrip (example);
    CHECK (loaded.launcher == example.launcher);
    CHECK (loaded.launcher.quantize == LaunchQuantize::beat);
    CHECK (! loaded.launcher.clips[1].loop);

    // A project without one writes no launcher node.
    CHECK (! toJsonText (Project()).contains ("launcher"));

    auto p = makeExample();
    p.launcher.clips[1].scene = p.launcher.clips[0].scene;
    expectInvalid (p, "shares its slot with another clip");

    p = makeExample();
    p.launcher.clips[0].scene = SceneId { 9999 };
    expectInvalid (p, "is in a missing track or scene");

    p = makeExample();
    p.launcher.clips[0].content = PatternClip { PatternId { 9999 }, 0 };
    expectInvalid (p, "plays a missing pattern");

    p = makeExample();
    p.launcher.clips[0].length = 0;
    expectInvalid (p, "has no length");

    p = makeExample();
    p.launcher.scenes[1].id = p.launcher.scenes[0].id;
    expectInvalid (p, "reuses ID");

    // In older files the launcher node was reserved and kept as it was; such a file opens with an empty launcher.
    {
        auto old = toJson (makeExample());
        old.getDynamicObject()->setProperty ("launcher", juce::JSON::parse (R"({"scenes": [], "future": true})"));
        Project opened;
        REQUIRE (fromJson (old, opened).wasOk());
        CHECK (opened.launcher.isEmpty());
    }

    auto json = toJson (makeExample());
    json["launcher"].getDynamicObject()->setProperty ("quantize", "sometimes");
    Project bad;
    CHECK (fromJson (json, bad).getErrorMessage().contains ("unknown quantize 'sometimes'"));
}

TEST_CASE ("A pattern's own time signature is saved only when it has one, and must be valid", "[model]")
{
    auto p = makeExample();
    CHECK (! toJsonText (p).contains ("\"timeSignature\": {\"numerator\": 7")); // none: not written
    p.patterns[0].timeSignature = TimeSignature { 7, 8 };
    CHECK (roundTrip (p) == p);
    CHECK (roundTrip (p).getTimeSignature (p.patterns[0].id) == TimeSignature { 7, 8 });

    p.patterns[0].timeSignature = TimeSignature { 4, 3 };
    expectInvalid (p, "invalid time signature");
}

TEST_CASE ("An audio track's recording input and arm are saved, and left out when unset", "[model]")
{
    Project p;
    Channel track;
    track.id = p.allocateId<ChannelId>();
    track.name = "Snare mic";
    track.audioTrack = true;
    track.recordInput = 3;
    track.recordStereo = true;
    track.recordArmed = true;
    p.channels = { track };
    Project loaded;
    REQUIRE (fromJsonText (toJsonText (p), loaded).wasOk());
    CHECK (loaded == p);

    p.channels[0].recordInput = 0;
    p.channels[0].recordStereo = p.channels[0].recordArmed = false;
    CHECK (! toJsonText (p).contains ("record"));
}

TEST_CASE ("A song of many instruments becomes project text quickly, with plain line breaks", "[model]")
{
    // Line breaks were changed after writing, copying the whole text once per line: saving a song the Analyzer
    // made into many instruments never finished (its plugin states were written, its project file never was).
    auto p = makeExample();
    auto& pattern = p.patterns.front();
    for (int c = 0; c < 50; ++c)
    {
        Channel channel;
        channel.id = p.allocateId<ChannelId>();
        channel.name = "Part " + juce::String (c + 1);
        auto& notes = pattern.notes[channel.id];
        for (int i = 0; i < 2000; ++i)
            notes.push_back ({ (Ticks)i * 240, 120, 36 + (i + c) % 48, 0.75f, 0.5f, 0.0f, 0.0f, {} });
        p.channels.push_back (channel);
    }
    pattern.length = p.fittedLength (pattern);
    REQUIRE (validate (p).isEmpty());

    const auto start = juce::Time::getMillisecondCounterHiRes();
    const auto text = toJsonText (p);
    const auto ms = juce::Time::getMillisecondCounterHiRes() - start;
    INFO (ms << " ms");
#if JUCE_DEBUG
    CHECK (ms < 30000.0);
#else
    CHECK (ms < 5000.0);
#endif
    CHECK (! text.containsChar ('\r'));
    CHECK (text.contains ("{\n"));

    Project loaded;
    REQUIRE (fromJsonText (text, loaded).wasOk());
    CHECK (loaded == p);
}
