#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "dsp/drums/StarterPack.h"
#include "engine/BundledDevices.h"
#include "engine/DeviceFactory.h"
#include "engine/EngineHost.h"
#include "model/Model.h"
#include "session/AgentCommandLine.h"
#include "session/ProjectDocument.h"
#include "session/Session.h"

using namespace spinus;

namespace
{
struct TempFolder
{
    TempFolder()
        : root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("spinus-cli-test-" + juce::Uuid().toString()))
    {
        root.createDirectory();
    }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root;
};

struct Fixture
{
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine(), makeDevices (host.getEngine()) };

    static std::unique_ptr<engine::DeviceFactory> makeDevices (tracktion::Engine& engine)
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerBundledDevices (*devices, engine, {});
        return devices;
    }

    /** Runs a script given as JSON text; the lines it said, parsed. */
    int run (const juce::String& script, std::vector<juce::var>& said, bool keepGoing = false,
             const juce::File& folder = {})
    {
        const auto steps = juce::JSON::parse (script);
        REQUIRE (steps.isArray());
        return cli::runSteps (session, steps, { .scriptFolder = folder, .keepGoing = keepGoing },
                              [&] (const juce::String& line) { said.push_back (juce::JSON::parse (line)); });
    }
};

/** A short song: drums and a 3x Osc bass on one pattern, twice on the Playlist. */
const char* const songScript = R"([
  {"command": "project.setTempo", "params": {"bpm": 120}},
  {"command": "channel.add", "params": {"name": "Drums", "instrument": {"kind": "stock", "stockType": "spinus.drum-machine"}}, "as": "drums"},
  {"command": "channel.add", "params": {"name": "Bass", "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}}, "as": "bass"},
  {"command": "device.loadPreset", "params": {"device": "$bass.device", "preset": "Reese Bass"}},
  {"command": "insert.addEffect", "params": {"insert": "$bass.insert", "device": {"kind": "stock", "stockType": "spinus.ducker"}}},
  {"command": "insert.setSidechain", "params": {"insert": "$bass.insert", "source": "$drums.insert"}},
  {"command": "pattern.add", "params": {"name": "Loop", "length": 3840}, "as": "loop"},
  {"command": "pattern.setNotes", "params": {"pattern": "$loop.pattern", "channel": "$drums.channel", "notes": [
    {"start": 0, "length": 240, "pitch": 60, "velocity": 0.9}, {"start": 960, "length": 240, "pitch": 61, "velocity": 0.8},
    {"start": 1920, "length": 240, "pitch": 60, "velocity": 0.9}, {"start": 2880, "length": 240, "pitch": 61, "velocity": 0.8}]}},
  {"command": "pattern.setNotes", "params": {"pattern": "$loop.pattern", "channel": "$bass.channel", "notes": [
    {"start": 0, "length": 1800, "pitch": 33, "velocity": 0.8}, {"start": 1920, "length": 1800, "pitch": 36, "velocity": 0.8}]}},
  {"command": "lane.add", "params": {"name": "Music"}, "as": "lane"},
  {"command": "clip.addPattern", "params": {"lane": "$lane.lane", "pattern": "$loop.pattern", "start": 0}},
  {"command": "clip.addPattern", "params": {"lane": "$lane.lane", "pattern": "$loop.pattern", "start": 3840}},
  {"command": "marker.add", "params": {"at": 0, "name": "Intro"}}
])";
} // namespace

TEST_CASE ("Command line: references name earlier results by label or step", "[cli]")
{
    std::map<juce::String, juce::var> results;
    results["bass"] = juce::JSON::parse (R"({"channel": 5, "insert": 7})");
    results["1"] = juce::JSON::parse (R"({"pattern": 9})");
    results["count"] = 3;

    juce::String error;
    const auto resolved = cli::resolveReferences (
        juce::JSON::parse (R"({"channel": "$bass.channel", "list": ["$1.pattern", "$count"], "text": "$ not one"})"),
        results, error);
    CHECK (error.isEmpty());
    CHECK ((int)resolved["channel"] == 5);
    CHECK ((int)resolved["list"][0] == 9);
    CHECK ((int)resolved["list"][1] == 3);

    const auto missing = cli::resolveReferences (juce::var ("$pad.channel"), results, error);
    CHECK (error.contains ("$pad.channel"));
    CHECK (missing.toString() == "$pad.channel");
}

TEST_CASE ("Command line: a script's steps run as commands, and a failed step stops it", "[cli]")
{
    Fixture f;
    std::vector<juce::var> said;
    CHECK (f.run (R"([
        {"command": "channel.add", "params": {"name": "Lead", "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}}, "as": "lead"},
        {"command": "channel.set", "params": {"channel": "$lead.channel", "volumeDb": -6}},
        {"command": "no.such.command", "params": {}},
        {"command": "channel.add", "params": {"name": "After"}}
    ])",
                  said) == 2);
    REQUIRE (said.size() == 3);
    CHECK ((bool)said[0]["ok"]);
    CHECK ((int)said[0]["result"]["channel"] > 0);
    CHECK ((bool)said[1]["ok"]);
    CHECK_FALSE ((bool)said[2]["ok"]);
    CHECK (said[2]["error"].toString().isNotEmpty());
    REQUIRE (f.session.getProject().channels.size() == 1); // "After" did not run
    CHECK (f.session.getProject().channels[0].volumeDb == Catch::Approx (-6.0));
    CHECK (f.session.getHistory().getUndoDescriptions().size() == 2); // one undo step per command

    said.clear();
    CHECK (
        f.run (
            R"([{"command": "channel.add", "params": {"nope": 1}}, {"command": "channel.add", "params": {"name": "After"}}])",
            said, true) == 2);
    REQUIRE (said.size() == 2);
    CHECK_FALSE ((bool)said[0]["ok"]);
    CHECK ((bool)said[1]["ok"]); // --keep-going
    CHECK (f.session.getProject().channels.size() == 2);

    said.clear();
    CHECK (f.run (R"([{"command": "channel.set", "params": {"channel": "$nobody.channel"}}])", said) == 2);
    CHECK (said[0]["error"].toString().contains ("$nobody.channel"));
    CHECK (cli::runSteps (f.session, juce::var ("not a list"), {}, [] (const juce::String&) {}) == 1);
}

TEST_CASE ("Command line: check sums up the project, and names what is missing", "[cli]")
{
    Fixture f;
    std::vector<juce::var> said;
    REQUIRE (f.run (songScript, said) == 0);
    REQUIRE (f.run (R"([{"command": "lane.add", "params": {"name": "Audio"}, "as": "a"},
                        {"command": "clip.addAudio", "params": {"lane": "$a.lane", "file": "/no/such/loop.wav", "start": 0, "length": 3840}}])",
                    said) == 0);

    const auto summary = cli::summarize (f.session, {});
    CHECK ((double)summary["tempoBpm"] == Catch::Approx (120.0));
    CHECK ((double)summary["bars"] == Catch::Approx (2.0));
    CHECK (summary["channels"].size() == 3); // drums, bass and the audio clip's channel
    CHECK (summary["channels"][0]["instrument"]["type"].toString() == "spinus.drum-machine");
    CHECK (summary["channels"][0]["instrument"]["kit"].toString() == "Spinus Synth Kit");
    CHECK ((int)summary["clips"]["pattern"] == 2);
    CHECK (summary["markers"][0]["name"].toString() == "Intro");
    bool sidechained = false;
    for (auto& insert : *summary["inserts"].getArray())
        sidechained = sidechained || insert["sidechainFrom"].size() == 1;
    CHECK (sidechained);
    REQUIRE (summary["problems"].size() == 1);
    CHECK (summary["problems"][0].toString().contains ("loop.wav"));
}

TEST_CASE ("Command line: describe lists a device's settings, presets, pads and kits", "[cli]")
{
    Fixture f;
    const auto osc = cli::describe (f.session, "spinus.3x-osc");
    CHECK_FALSE (osc.hasProperty ("error"));
    CHECK ((bool)osc["instrument"]);
    CHECK (osc["presets"].size() > 10);
    REQUIRE (osc["parameters"].size() > 10);
    bool cutoff = false;
    for (auto& p : *osc["parameters"].getArray())
        if (p["id"].toString() == "cutoff")
        {
            cutoff = true;
            CHECK (p["at"]["0.00"].toString().isNotEmpty());
            CHECK (p["at"]["1.00"].toString() != p["at"]["0.00"].toString());
        }
    CHECK (cutoff);

    const auto drums = cli::describe (f.session, "spinus.drum-machine");
    REQUIRE (drums["pads"].size() == 16);
    CHECK ((int)drums["pads"][0]["note"] == 60);
    CHECK (drums["pads"][0]["noteName"].toString() == "C5");
    CHECK (drums["kits"][0]["name"].toString() == "Spinus Synth Kit");

    const auto eq = cli::describe (f.session, "spinus.eq");
    CHECK_FALSE ((bool)eq["instrument"]);
    CHECK (cli::describe (f.session, "spinus.nothing").hasProperty ("error"));
    CHECK (cli::describableTypes (f.session).contains ("spinus.limiter"));

    // Made for a moment only.
    CHECK (f.session.getProject().channels.empty());
    CHECK (f.session.getProject().mixer.master.effects.empty());
}

TEST_CASE ("Command line: a script's song is saved, checked and rendered with sound", "[cli][audio]")
{
    TempFolder t;
    const auto folder = t.root.getChildFile ("Song");
    {
        Fixture f;
        ProjectDocument document (f.session, t.root.getChildFile ("settings"));
        std::vector<juce::var> said;
        REQUIRE (f.run (songScript, said) == 0);
        if (const auto pack = dsp::starter::findPack(); pack != juce::File())
        {
            said.clear();
            CHECK (f.run (R"([{"action": "loadKit", "params": {"device": 3, "kit": "magpie"}}])", said) == 0);
            CHECK (cli::summarize (f.session, {})["channels"][0]["instrument"]["kit"].toString() == "Magpie");
        }
        said.clear();
        CHECK (f.run (R"([{"action": "loadKit", "params": {"device": 3, "kit": "No Such Kit"}}])", said) == 2);
        CHECK (said[0]["error"].toString().contains ("Spinus Synth Kit"));
        REQUIRE (document.saveAs (folder).wasOk());
    }

    Fixture f;
    ProjectDocument document (f.session, t.root.getChildFile ("settings"));
    REQUIRE (document.open (folder).wasOk());
    CHECK (cli::summarize (f.session, document.getFolder())["problems"].size() == 0);

    std::vector<juce::String> said;
    const auto out = t.root.getChildFile ("song.wav");
    REQUIRE (cli::render (f.session, document.getFolder().getChildFile ("audio"), out, t.root.getChildFile ("stems"),
                          1.0, [&] (const juce::String& line) { said.push_back (line); }) == 0);
    REQUIRE (said.size() == 1);
    const auto report = juce::JSON::parse (said[0]);
    CHECK ((bool)report["ok"]);
    INFO (said[0]);
    CHECK ((double)report["mix"]["seconds"] > 4.0);
    CHECK ((double)report["mix"]["lufs"] > -40.0); // not silent
    CHECK ((double)report["mix"]["soundingSeconds"] > 2.0);
    CHECK_FALSE (report["mix"].hasProperty ("unencodedTruePeakDb")); // a WAV is not encoded
    // A stem per insert with sound, straight in the folder ("NN <insert>.wav"), and no copy of the mix there.
    REQUIRE (report["stems"].size() >= 2);
    const auto stems = t.root.getChildFile ("stems").findChildFiles (juce::File::findFiles, false, "*.wav");
    CHECK (stems.size() == report["stems"].size());
    for (auto& stem : stems)
    {
        CHECK (stem.getFileName().substring (0, 2).containsOnly ("0123456789"));
        CHECK_FALSE (stem.getFileNameWithoutExtension().equalsIgnoreCase ("Mix"));
    }
    CHECK (t.root.getChildFile ("stems").findChildFiles (juce::File::findDirectories, false).isEmpty());

    // Lossy: the encoded file's peak, and the render's before encoding.
    said.clear();
    const auto opus = t.root.getChildFile ("song.opus");
    REQUIRE (cli::render (f.session, document.getFolder().getChildFile ("audio"), opus, {}, 1.0,
                          [&] (const juce::String& line) { said.push_back (line); }) == 0);
    const auto lossy = juce::JSON::parse (said.back());
    INFO (said.back());
    CHECK (opus.existsAsFile());
    CHECK (lossy["mix"].hasProperty ("unencodedTruePeakDb"));
}

TEST_CASE ("Command line: --help and unknown options are told apart from projects", "[cli]")
{
    CHECK (cli::usage().contains ("--run"));
    CHECK (cli::usage().contains ("--render"));
    CHECK (cli::usage().contains ("command-line.md"));
    CHECK (cli::unknownOption ({ "--help" }).isEmpty());
    CHECK (cli::unknownOption ({ "-h" }).isEmpty());
    CHECK (cli::unknownOption ({ "Song/Song.spn" }).isEmpty());
    CHECK (cli::unknownOption ({ "--render", "Song", "--out", "mix.opus", "--stems", "s", "--tail", "3" }).isEmpty());
    CHECK (cli::unknownOption ({ "--run", "song.json", "--keep-going", "--save", "Out" }).isEmpty());
    CHECK (cli::unknownOption ({ "--tail", "--weird-but-a-value" }).isEmpty()); // the value of --tail
    CHECK (cli::unknownOption ({ "--hepl" }) == "--hepl");
    CHECK (cli::unknownOption ({ "--check", "Song", "--verbose" }) == "--verbose");
    CHECK (cli::unknownOption ({ "\"--list\"" }) == "--list");
}

TEST_CASE ("Command line: describe lists every choice of a choice setting", "[cli]")
{
    Fixture f;
    const auto eq = cli::describe (f.session, "spinus.eq");
    juce::StringArray choices;
    bool numbersHaveNone = true;
    for (auto& p : *eq["parameters"].getArray())
    {
        if (p["name"].toString().containsIgnoreCase ("type") && choices.isEmpty())
            for (auto& c : *p["choices"].getArray())
                choices.add (c.toString());
        if (p["name"].toString().containsIgnoreCase ("freq") && p.hasProperty ("choices"))
            numbersHaveNone = false;
    }
    INFO (choices.joinIntoString (", "));
    CHECK (choices.contains ("Low cut")); // between the 0, 0.25... samples, so they missed it
    CHECK (choices.contains ("Bell"));
    CHECK (numbersHaveNone);

    // A wrong name says what the choices are.
    std::vector<juce::var> said;
    CHECK (
        f.run (
            R"([{"command": "insert.addEffect", "params": {"insert": 1, "device": {"kind": "stock", "stockType": "spinus.eq"}}, "as": "eq"},
                      {"command": "device.setParameters", "params": {"device": "$eq.device", "text": {"Band 1 type": "High pass"}}}])",
            said) == 2);
    CHECK (said.back()["error"].toString().contains ("Low cut"));

    const auto missing = cli::notInstalled (f.session);
    CHECK (missing["notInstalled"].isArray());
    CHECK (missing["hint"].toString().contains ("Get free plugins"));
}

TEST_CASE ("Command line: automation points given as text, in the control's units", "[cli]")
{
    Fixture f;
    std::vector<juce::var> said;
    REQUIRE (f.run (R"([
        {"command": "channel.add", "params": {"name": "Pad", "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}}, "as": "pad"},
        {"command": "insert.addEffect", "params": {"insert": "$pad.insert", "device": {"kind": "stock", "stockType": "spinus.filter"}}, "as": "filter"},
        {"command": "clip.addAutomation", "params": {"laneName": "Fade", "start": 0, "length": 3840,
          "target": {"kind": "channelVolume", "channel": "$pad.channel"},
          "points": [{"time": 0, "text": "-6 dB"}, {"time": 3840, "value": 0.2}]}, "as": "fade"},
        {"command": "clip.addAutomation", "params": {"laneName": "Sweep", "start": 0, "length": 3840,
          "target": {"kind": "deviceParameter", "device": "$filter.device", "parameterId": "cutoff"},
          "points": [{"time": 0, "text": "200 Hz"}, {"time": 3840, "text": "5 kHz"}]}, "as": "sweep"},
        {"command": "clip.setPoints", "params": {"clip": "$fade.clip", "points": [{"time": 0, "text": "-12 dB"}]}}
    ])",
                    said) == 0);
    const auto& clips = f.session.getProject().playlist.clips;
    REQUIRE (clips.size() == 2);
    const auto& fade = std::get<model::AutomationClip> (clips[0].content);
    REQUIRE (fade.points.size() == 1);
    CHECK (f.session.describeTargetValue (fade.target, fade.points[0].value).contains ("-12"));
    const auto& sweep = std::get<model::AutomationClip> (clips[1].content);
    REQUIRE (sweep.points.size() == 2);
    CHECK (sweep.points[0].value < sweep.points[1].value);
    CHECK (f.session.describeTargetValue (sweep.target, sweep.points[1].value).containsIgnoreCase ("k"));

    said.clear();
    REQUIRE (f.run (R"([{"command": "clip.addAutomation", "params": {"start": 0, "length": 960,
                         "target": {"kind": "channelPan", "channel": 2}, "points": [{"time": 0, "text": "30% L"}]}}])",
                    said) == 0);
    const auto& pan = std::get<model::AutomationClip> (f.session.getProject().playlist.clips.back().content);
    CHECK (pan.points[0].value == Catch::Approx (0.35f)); // 30% left of the middle
    said.clear();
    CHECK (f.run (R"([{"command": "clip.setPoints", "params": {"clip": 999, "points": [{"time": 0, "text": "30%"}]}}])",
                  said) == 2); // no such clip: the command says so

    said.clear();
    CHECK (f.run (R"([{"command": "clip.addAutomation", "params": {"start": 0, "length": 960,
                       "target": {"kind": "channelVolume", "channel": 2}, "points": [{"time": 0, "text": "loud"}]}}])",
                  said) == 2);
    CHECK (said.back()["error"].toString().contains ("loud"));
}

TEST_CASE ("Command line: check says which channels nothing plays", "[cli]")
{
    Fixture f;
    std::vector<juce::var> said;
    REQUIRE (f.run (songScript, said) == 0);
    REQUIRE (
        f.run (
            R"([{"command": "channel.add", "params": {"name": "Forgotten", "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}}}])",
            said) == 0);
    const auto summary = cli::summarize (f.session, {});
    REQUIRE (summary["channels"].size() == 3);
    CHECK ((bool)summary["channels"][0]["used"]);
    CHECK ((bool)summary["channels"][1]["used"]);
    CHECK_FALSE ((bool)summary["channels"][2]["used"]);
    REQUIRE (summary["unusedChannels"].size() == 1);
    CHECK (summary["unusedChannels"][0].toString() == "Forgotten");
    CHECK (summary["problems"].size() == 0); // a warning, not a problem
}
