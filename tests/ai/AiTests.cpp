#include <catch2/catch_test_macros.hpp>

#include "core/ChildProcess.h"
#include "ai/AiService.h"
#include "ai/Proposal.h"
#include "commands/CommandRegistry.h"
#include "model/ModelJson.h"

#include <csignal>

using namespace spinus;
using namespace spinus::ai;

namespace
{
struct TempFolder
{
    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-ai-tests") + "-" + juce::Uuid().toString());
};

model::Project example()
{
    model::Project p;
    const auto file = juce::File (SPINUS_TEST_FILES_DIR).getChildFile ("model/files/example-v1.json");
    REQUIRE (model::fromJsonText (file.loadFileAsString(), p).wasOk());
    return p;
}

juce::var proposal (const juce::String& json) { return juce::JSON::parse (json); }

/** Runs the message loop until `done` is set or the time is up. */
bool waitFor (const std::function<bool()>& done, int ms = 20000)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32)ms;
    while (! done() && juce::Time::getMillisecondCounter() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    return done();
}
} // namespace

TEST_CASE ("A proposal is checked on a copy, with references to earlier steps, and applied as one undo step", "[ai]")
{
    commands::CommandHistory history (example());
    const auto before = model::toJsonText (history.getProject());

    auto p = Proposal::prepare (proposal (R"({"summary": "A pad with a filter", "steps": [
        {"command": "channel.add", "parameters": {"name": "Pad", "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}}},
        {"command": "insert.addEffect", "parameters": {"insert": "$1.insert", "device": {"kind": "stock", "stockType": "spinus.filter"}}}
    ]})"),
                                history);
    INFO (p.error);
    REQUIRE (p.canApply());
    CHECK (model::toJsonText (history.getProject()) == before); // checking changed nothing
    REQUIRE (p.steps.size() == 2);
    CHECK (p.steps[0].description.contains ("Pad"));
    CHECK (p.steps[1].parameters["insert"].isInt()); // the reference was filled in

    REQUIRE (p.apply (history).wasOk());
    const auto& project = history.getProject();
    const auto& pad = project.channels.back();
    CHECK (pad.name == "Pad");
    CHECK (project.findInsert (pad.targetInsert)->effects.size() == 1);
    CHECK (history.getUndoDescriptions().strings.getLast() == "Assistant: A pad with a filter");

    history.undo(); // one step takes it all back (IDs are never reused, so only nextId moved on)
    auto undone = history.getProject();
    undone.nextId = example().nextId;
    CHECK (model::toJsonText (undone) == before);
}

TEST_CASE ("A proposal that cannot be done says which step and why", "[ai]")
{
    commands::CommandHistory history (example());

    auto badReference = Proposal::prepare (proposal (R"({"summary": "x", "steps": [
        {"command": "insert.addEffect", "parameters": {"insert": "$3.insert", "device": {"kind": "stock", "stockType": "spinus.filter"}}}]})"),
                                           history);
    CHECK (! badReference.canApply());
    CHECK (badReference.error.contains ("Step 1"));
    CHECK (badReference.error.contains ("$3.insert"));

    auto failing = Proposal::prepare (proposal (R"({"summary": "x", "steps": [
        {"command": "project.setTempo", "parameters": {"bpm": 128}},
        {"command": "channel.remove", "parameters": {"channel": 99999}}]})"),
                                      history);
    CHECK (! failing.canApply());
    CHECK (failing.error.startsWith ("Step 2 (channel.remove)"));
    CHECK (! failing.apply (history).wasOk());
    CHECK (history.getUndoDescriptions().isEmpty()); // nothing applied, not even step 1

    CHECK (! Proposal::prepare (proposal (R"({"summary": "x", "steps": []})"), history).canApply());
}

TEST_CASE ("A proposal is checked again when the project changed before Apply", "[ai]")
{
    commands::CommandHistory history (example());
    auto p = Proposal::prepare (proposal (R"({"summary": "Bass", "steps": [
        {"command": "channel.add", "parameters": {"name": "Bass"}},
        {"command": "channel.set", "parameters": {"channel": "$1.channel", "volumeDb": -6}}]})"),
                                history);
    REQUIRE (p.canApply());

    // The user adds a channel meanwhile: the IDs the proposal expected are taken.
    juce::String error;
    REQUIRE (history
                 .perform (commands::CommandRegistry::get().create ("channel.add",
                                                                    juce::JSON::parse (R"({"name": "Other"})"), error))
                 .wasOk());
    REQUIRE (p.apply (history).wasOk());
    const auto& channels = history.getProject().channels;
    CHECK (channels.back().name == "Bass");
    CHECK (juce::approximatelyEqual (channels.back().volumeDb, -6.0f));
    CHECK (channels[channels.size() - 2].name == "Other");
    CHECK (juce::exactlyEqual (channels[channels.size() - 2].volumeDb, 0.0f));
}

TEST_CASE ("AI is off until it is turned on, and remembers", "[ai]")
{
    TempFolder t;
    {
        AiService service (t.root);
        CHECK (! service.isEnabled());
        CHECK (service.getState() == AiService::State::off);
        juce::String error;
        bool replied = false;
        service.call ("ai.v1.hello", {},
                      [&] (const juce::var&, const juce::String& e)
                      {
                          error = e;
                          replied = true;
                      });
        REQUIRE (waitFor ([&] { return replied; }));
        CHECK (error == "AI is off. Turn it on in Options > AI.");
        service.setEnabled (true);
    }
    AiService again (t.root);
    CHECK (again.isEnabled());
    CHECK (again.getState() == AiService::State::stopped); // not started until used
    again.setEnabled (false);
}

TEST_CASE ("The AI service starts on first use, answers, and starts again after a crash", "[ai]")
{
    const auto installation = AiService::findInstallation();
    if (installation.python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");

    TempFolder t;
    AiService service (t.root);
    service.setEnabled (true);

    auto hello = [&]
    {
        juce::var result;
        juce::String error;
        bool replied = false;
        service.call ("ai.v1.hello", {},
                      [&] (const juce::var& r, const juce::String& e)
                      {
                          result = r;
                          error = e;
                          replied = true;
                      });
        REQUIRE (waitFor ([&] { return replied; }));
        INFO (error);
        REQUIRE (error.isEmpty());
        return result;
    };

    const auto first = hello();
    CHECK ((int)first["protocol"] == 1);
    CHECK ((int)first["manualSections"] > 20);
    CHECK (service.getState() == AiService::State::running);

    // A connection to a local server: saved by the service, without a key.
    bool saved = false;
    service.call ("ai.v1.connections.save", juce::JSON::parse (R"({"name": "Ollama", "provider": "local"})"),
                  [&] (const juce::var& r, const juce::String& e)
                  {
                      CHECK (e.isEmpty());
                      CHECK (r["id"].toString().isNotEmpty());
                      saved = true;
                  });
    REQUIRE (waitFor ([&] { return saved; }));
    CHECK (t.root.getChildFile ("ai/connections.json").loadFileAsString().contains ("Ollama"));

    // The service crashes: Spinus notices, and the next call starts it again. Only this test's service (its
    // folder is in the command line): tests running at the same time have services of their own.
    const auto pids = juce::StringArray::fromLines (
        [&t]
        {
            spinus::core::ChildProcess p;
            p.start (juce::StringArray { "pgrep", "-f", "spinus_ai.*" + t.root.getFileName() });
            return p.readAllProcessOutput();
        }());
    for (auto& pid : pids)
        if (pid.trim().isNotEmpty())
            ::kill (pid.getIntValue(), SIGKILL);
    REQUIRE (waitFor ([&] { return service.getState() == AiService::State::stopped; }));
    CHECK ((int)hello()["protocol"] == 1);

    // Turning AI off stops it.
    service.setEnabled (false);
    CHECK (service.getState() == AiService::State::off);
}

TEST_CASE ("The AI service takes long requests, such as every Surge XT preset the Analyzer might use", "[ai]")
{
    const auto installation = AiService::findInstallation();
    if (installation.python == juce::File())
        SKIP ("the AI service's Python environment is not set up (uv sync in ai/)");

    TempFolder t;
    AiService service (t.root);
    service.setEnabled (true);
    juce::Array<juce::var> presets;
    for (int i = 0; i < 5000; ++i)
    {
        auto* preset = new juce::DynamicObject();
        preset->setProperty ("id", "/home/someone/.local/share/Surge XT/patches_factory/Category/Preset " +
                                       juce::String (i) + ".fxp");
        preset->setProperty ("version", "1");
        presets.add (juce::var (preset));
    }
    auto* params = new juce::DynamicObject();
    params->setProperty ("library", "surge-xt");
    params->setProperty ("presets", presets);
    juce::var result;
    juce::String error;
    bool replied = false;
    service.call ("ai.v1.presets.missing", juce::var (params),
                  [&] (const juce::var& r, const juce::String& e)
                  {
                      result = r;
                      error = e;
                      replied = true;
                  });
    REQUIRE (waitFor ([&] { return replied; }));
    INFO (error);
    REQUIRE (error.isEmpty());
    CHECK (result["missing"].size() == 5000);
}

TEST_CASE ("The AI service knows what each command returns, for references between steps", "[ai]")
{
    // RESULT_FIELDS in ai/spinus_ai/project_tools.py tells the model what "$<step>.<field>" can name.
    const auto python =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("ai/spinus_ai/project_tools.py");
    std::map<juce::String, juce::StringArray> table;
    bool inTable = false;
    for (auto& line : juce::StringArray::fromLines (python.loadFileAsString()))
    {
        if (line.startsWith ("RESULT_FIELDS"))
            inTable = true;
        else if (inTable && line.startsWith ("}"))
            break;
        else if (inTable && line.trim().startsWith ("\""))
        {
            const auto name =
                line.fromFirstOccurrenceOf ("\"", false, false).upToFirstOccurrenceOf ("\"", false, false);
            juce::StringArray fields;
            fields.addTokens (line.fromFirstOccurrenceOf ("(", false, false).upToLastOccurrenceOf (")", false, false),
                              ",", "\"");
            fields.trim();
            fields.removeEmptyStrings();
            fields.removeString ("\"\"");
            for (auto& f : fields)
                f = f.unquoted();
            table[name] = fields;
        }
    }
    REQUIRE (table.size() == 18);

    const std::map<juce::String, juce::String> calls {
        { "channel.add", R"({"name": "Hats", "instrument": {"kind": "stock", "stockType": "spinus.sampler"}})" },
        { "insert.add", R"({"name": "Vocals"})" },
        { "insert.addEffect", R"({"insert": 6, "device": {"kind": "stock", "stockType": "spinus.filter"}})" },
        { "insert.addBus", R"({"name": "Drums", "inserts": [6]})" },
        { "pattern.add", R"({"name": "Fill"})" },
        { "pattern.clone", R"({"pattern": 11})" },
        { "marker.add", R"({"at": 3840, "name": "Bridge"})" },
        { "lane.add", R"({"name": "Drums"})" },
        { "clip.addPattern", R"({"lane": 13, "pattern": 11, "start": 7680})" },
        { "clip.add", R"({"clip": {"type": "pattern", "lane": 13, "pattern": 11, "start": 15360, "length": 3840}})" },
        { "clip.split", R"({"clip": 14, "at": 1920})" },
        { "clip.makeUnique", R"({"clip": 14})" },
        { "clip.addAudio", R"({"lane": 13, "file": "drums/loop.wav", "start": 3840, "length": 7680})" },
        { "clip.addAutomation",
          R"({"start": 0, "length": 3840, "laneName": "Drums volume", "target": {"kind": "insertVolume", "insert": 2},
              "points": [{"time": 0, "value": 0.2}]})" },
        { "launcher.addTrack", R"({"name": "Jam"})" },
        { "launcher.addScene", R"({"name": "Drop"})" },
        { "launcher.addPattern", R"({"track": 18, "scene": 20, "pattern": 11})" },
        { "launcher.addAudio", R"({"track": 18, "scene": 19, "file": "pad.wav", "length": 3840})" },
    };
    for (auto& [name, fields] : table)
    {
        INFO (name);
        REQUIRE (calls.contains (name));
        commands::CommandHistory history (example());
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (calls.at (name)), error);
        INFO (error);
        REQUIRE (command != nullptr);
        auto* raw = command.get();
        const auto performed = history.perform (std::move (command));
        INFO (performed.getErrorMessage());
        REQUIRE (performed.wasOk());
        juce::StringArray returned;
        const auto result = raw->getResult();
        if (auto* o = result.getDynamicObject())
            for (auto& p : o->getProperties())
                returned.add (p.name.toString());
        returned.sort (false);
        auto expected = fields;
        expected.sort (false);
        CHECK (returned == expected);
    }
}

TEST_CASE ("The project harness's edits apply in Spinus as one undo step", "[ai]")
{
    // tests/ai/files/harness-proposal.json: what ai/spinus_ai/project_harness.py makes of a part added and written,
    // a shared pattern's notes changed, a bridge inserted, automation, a send and ducking, on the example project
    // (ai/tests/test_project_harness.py keeps it up to date).
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("tests/ai/files/harness-proposal.json");
    const auto json = juce::JSON::parse (file.loadFileAsString());
    REQUIRE (json.isObject());
    commands::CommandHistory history (example());
    const auto markersBefore = history.getProject().playlist.markers.size();
    auto proposal = ai::Proposal::prepare (json, history);
    INFO (proposal.error);
    REQUIRE (proposal.canApply());
    REQUIRE (proposal.apply (history).wasOk());
    const auto& project = history.getProject();
    CHECK (history.getUndoDescriptions().size() == 1);
    CHECK (project.channels.size() == 3);
    CHECK (project.channels.back().name == "Pads");
    CHECK (project.playlist.markers.size() == markersBefore + 1);
    CHECK (project.playlist.markers.back().name == "Bridge");
    const auto* reverb = project.findInsert (model::InsertId { 4 });
    const auto* kick = project.findInsert (model::InsertId { 2 });
    REQUIRE (kick != nullptr);
    REQUIRE (reverb != nullptr);
    CHECK (kick->sends.size() == 1);
    CHECK (history.undo());
    auto undone = history.getProject();
    undone.nextId = example().nextId; // IDs are not handed out again after an undo
    CHECK (undone == example());
}
