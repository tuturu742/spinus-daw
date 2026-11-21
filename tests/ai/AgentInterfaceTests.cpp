#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ai/AgentInterface.h"
#include "ai/ProjectContext.h"
#include "session/AnalysisForAgents.h"
#include "session/EffectProfiles.h"
#include "session/InstrumentProfiles.h"
#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/DeviceFactory.h"
#include "engine/EngineHost.h"
#include "engine/EngineAdapter.h"
#include "dsp/drums/DrumMachineProcessor.h"
#include "dsp/sampler/SamplerProcessor.h"
#include "session/Session.h"
#include "ui/AgentInterfacePage.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <map>
#include <array>
#include <thread>

#if ! JUCE_WINDOWS
#include <sys/stat.h>
#endif

using namespace spinus;

namespace
{
struct TempFolder
{
    TempFolder()
        : root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("spinus-agents-test-" + juce::Uuid().toString()))
    {
        root.createDirectory();
    }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root;
};

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> properties)
{
    auto* o = new juce::DynamicObject();
    for (auto& [name, value] : properties)
        o->setProperty (name, value);
    return juce::var (o);
}

/** A Spinus with the live interface, and a stand-in for the AI service's project harness: one tool, fake_tempo,
    whose "edit" sets the tempo to 140. */
struct Fixture
{
    TempFolder folder;
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine(), makeDevices (host.getEngine()) };
    std::unique_ptr<ai::AgentInterface> agents;
    juce::var lastContext;
    bool music = true; // AI on, for the harness's stand-in
    // A stand-in for the project as files: the open project's folder, the Browser's projects, unsaved changes.
    juce::File openFolder = folder.root.getChildFile ("Song");
    juce::Array<juce::File> listed;
    juce::Array<juce::File> roots { folder.root.getChildFile ("Projects"), folder.root.getChildFile ("Mine") };
    bool unsaved = false;
    juce::File savedTo { "/nowhere" };
    juce::var heard; // what the AI service hears in a vocal (void: AI is off)

    static std::unique_ptr<engine::DeviceFactory> makeDevices (tracktion::Engine& engine)
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerBundledDevices (*devices, engine, {});
        return devices;
    }

    explicit Fixture (bool withHarness = true)
    {
        ai::AgentInterface::Host h;
        h.projectFolder = [this] { return openFolder; };
        h.projects = [this] { return listed; };
        h.unsavedChanges = [this] { return unsaved; };
        h.newProjectsFolder = [this] { return folder.root.getChildFile ("Projects"); };
        h.projectFolders = [this] { return roots; };
        h.saveProject = [this] (const juce::File& to)
        {
            savedTo = to;
            if (to != juce::File())
                openFolder = to;
            unsaved = false;
            return juce::String();
        };
        h.openProject = [this] (const juce::File& project)
        {
            session.loadProject ({});
            openFolder = project;
            return juce::String();
        };
        h.newProject = [this]
        {
            session.loadProject ({});
            openFolder = juce::File();
            return juce::String();
        };
        h.listenToVocal = [this] (const juce::File& audio, const std::function<void (const juce::String&)>& done)
        {
            if (! heard.isObject())
                return done ("listening to a vocal needs Spinus's AI turned on (Options > AI)");
            session.getTuner().storeAnalysis (audio, heard);
            done ({});
        };
        h.sampleFolders = [] { return juce::Array<juce::File>(); };
        h.context = [] { return object ({ { "project", juce::var (new juce::DynamicObject()) } }); };
        if (withHarness)
        {
            h.listHarnessTools = [] (const auto& reply)
            {
                juce::Array<juce::var> tools;
                tools.add (object ({ { "name", "fake_tempo" }, { "description", "Sets the tempo to 140" } }));
                reply (tools, {});
            };
            h.callHarnessTool =
                [this] (const juce::String& tool, const juce::var&, const juce::var& context, const auto& reply)
            {
                lastContext = context;
                juce::Array<juce::var> steps;
                steps.add (
                    object ({ { "command", "project.setTempo" }, { "parameters", object ({ { "bpm", 140 } }) } }));
                reply (object ({ { "text", tool + ": the tempo is 140" },
                                 { "isError", false },
                                 { "proposal", object ({ { "summary", "Tempo 140" }, { "steps", steps } }) } }),
                       {});
            };
            h.musicToolsOn = [this] { return music; };
        }
        agents = std::make_unique<ai::AgentInterface> (session, folder.root, std::move (h));
        agents->setPort (40000 + juce::Random::getSystemRandom().nextInt (20000));
    }

    struct Answer
    {
        int status = 0;
        juce::String session;
        juce::var json;
        juce::String body, contentType;
    };

    /** One request through handle(), as the server makes it, with the message loop run until it is answered. */
    Answer request (const juce::String& method, const juce::var& body, const juce::String& sessionId = {},
                    const juce::String& auth = "", const juce::String& origin = "",
                    const juce::String& accept = "application/json, text/event-stream")
    {
        juce::StringPairArray headers;
        headers.set ("authorization", auth.isNotEmpty() ? auth : "Bearer " + agents->getToken());
        headers.set ("accept", accept);
        if (sessionId.isNotEmpty())
            headers.set ("mcp-session-id", sessionId);
        if (origin.isNotEmpty())
            headers.set ("origin", origin);
        std::optional<ai::AgentInterface::HttpAnswer> got;
        agents->handle (method, headers, juce::JSON::toString (body),
                        [&] (const ai::AgentInterface::HttpAnswer& a) { got = a; });
        for (int i = 0; i < 4000 && ! got; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
        REQUIRE (got.has_value());
        const auto answer = got.value_or (ai::AgentInterface::HttpAnswer {});
        return { answer.status, answer.headers["Mcp-Session-Id"], juce::JSON::parse (answer.body), answer.body,
                 answer.headers["Content-Type"] };
    }

    /** Turns the interface on at a port it can listen on: a random one may be taken by another test, or reserved
        (Windows keeps blocks of ports for itself), so others are tried. */
    void listen()
    {
        agents->setEnabled (true);
        for (int i = 0; i < 20 && ! agents->isListening(); ++i)
            agents->setPort (40000 + juce::Random::getSystemRandom().nextInt (20000));
    }

    static juce::var rpc (const juce::String& method, const juce::var& params = juce::var (new juce::DynamicObject()))
    {
        return object ({ { "jsonrpc", "2.0" }, { "id", 1 }, { "method", method }, { "params", params } });
    }

    juce::String connect (const juce::String& name = "Test Agent")
    {
        const auto answer =
            request ("POST", rpc ("initialize", object ({ { "clientInfo", object ({ { "name", name } }) } })));
        REQUIRE (answer.status == 200);
        REQUIRE (answer.session.isNotEmpty());
        return answer.session;
    }

    juce::var call (const juce::String& sessionId, const juce::String& tool, const juce::var& arguments)
    {
        const auto answer =
            request ("POST", rpc ("tools/call", object ({ { "name", tool }, { "arguments", arguments } })), sessionId);
        REQUIRE (answer.status == 200);
        return answer.json["result"];
    }
};
} // namespace

TEST_CASE ("The live interface for agents is off until turned on, and needs its token", "[ai][agents]")
{
    Fixture f;
    CHECK_FALSE (f.agents->isEnabled());
    CHECK (f.request ("POST", Fixture::rpc ("initialize")).status == 403);

    f.listen();
    REQUIRE (f.agents->isListening());
    CHECK (f.agents->getToken().length() == 32);
    CHECK (f.request ("POST", Fixture::rpc ("initialize"), {}, "Bearer wrong").status == 401);
    // Web pages can't reach it (a browser sends its page's origin).
    CHECK (f.request ("POST", Fixture::rpc ("initialize"), {}, {}, "https://example.com").status == 403);
    // Everything but initialize needs the session it started.
    CHECK (f.request ("POST", Fixture::rpc ("tools/list")).status == 400);
    CHECK (f.request ("GET", juce::var()).status == 405);

    // The token is kept for this user only, and a new one locks the old one out.
    const auto settings = f.folder.root.getChildFile ("agent-interface.json");
    CHECK (juce::JSON::parse (settings.loadFileAsString())["token"] == f.agents->getToken());
    const auto old = f.agents->getToken();
    f.agents->newToken();
    CHECK (f.request ("POST", Fixture::rpc ("initialize"), {}, "Bearer " + old).status == 401);
    CHECK (f.agents->setupFor ("claude").contains ("--header \"Authorization: Bearer " + f.agents->getToken()));
    CHECK (f.agents->setupFor ("cursor").contains ("\"url\": \"http://127.0.0.1:"));
}

TEST_CASE ("An agent connects, changes the song at once, and its changes are undone together", "[ai][agents]")
{
    Fixture f;
    f.agents->setEnabled (true);
    const auto before = f.session.getProject().settings.tempoBpm;
    const auto sessionId = f.connect();
    CHECK (f.agents->isAgentConnected());
    CHECK (f.agents->getAgentName() == "Test Agent");

    // Spinus's own tools and the harness's.
    const auto listed = f.request ("POST", Fixture::rpc ("tools/list"), sessionId).json["result"]["tools"];
    juce::StringArray names;
    for (auto& t : *listed.getArray())
        names.add (t["name"].toString());
    for (auto* name : { "song_summary", "run_commands", "command_help", "list_devices", "list_presets", "render",
                        "undo_last", "learn_instrument", "fake_tempo" })
        CHECK (names.contains (name));
    CHECK ((bool)f.call (sessionId, "list_presets", object ({ { "device", 999 } }))["isError"]); // no such device
    CHECK ((bool)f.call (sessionId, "learn_instrument", object ({ { "device", 999 } }))["isError"]);

    // A command, done at once as one undo step.
    juce::Array<juce::var> steps;
    steps.add (object ({ { "command", "project.setTempo" }, { "parameters", object ({ { "bpm", 150 } }) } }));
    auto result = f.call (sessionId, "run_commands", object ({ { "summary", "Faster" }, { "steps", steps } }));
    INFO (juce::JSON::toString (result));
    CHECK_FALSE ((bool)result["isError"]);
    CHECK (f.session.getProject().settings.tempoBpm == Catch::Approx (150.0));
    CHECK (f.session.getHistory().getUndoDescriptions().strings.getLast() == "Agent: Faster");

    // A harness tool: its edit is done too, and it got the project.
    result = f.call (sessionId, "fake_tempo", juce::var (new juce::DynamicObject()));
    CHECK_FALSE ((bool)result["isError"]);
    CHECK (result["content"][0]["text"].toString().contains ("Done, as one undo step"));
    CHECK (f.lastContext.hasProperty ("project"));
    CHECK (f.session.getProject().settings.tempoBpm == Catch::Approx (140.0));
    CHECK (f.agents->getAgentChanges() == 2);

    // The agent can undo its own last change.
    result = f.call (sessionId, "undo_last", juce::var (new juce::DynamicObject()));
    CHECK (f.session.getProject().settings.tempoBpm == Catch::Approx (150.0));

    // The user undoes all of the agent's changes, and disconnects it: its session is over.
    f.agents->undoAgentChanges();
    CHECK (f.session.getProject().settings.tempoBpm == Catch::Approx (before));
    f.agents->disconnect();
    CHECK_FALSE (f.agents->isAgentConnected());
    CHECK (f.request ("POST", Fixture::rpc ("tools/list"), sessionId).status == 404);
}

TEST_CASE ("An agent's run_commands that changes many things is one undo step", "[ai][agents]")
{
    Fixture f;
    f.agents->setEnabled (true);
    const auto sessionId = f.connect();
    const auto before = f.session.getProject();
    const auto steps = f.session.getHistory().getUndoDescriptions().size();

    juce::Array<juce::var> commands;
    commands.add (object ({ { "command", "project.setTempo" }, { "parameters", object ({ { "bpm", 96 } }) } }));
    commands.add (object ({ { "command", "pattern.add" }, { "parameters", object ({ { "name", "Verse" } }) } }));
    commands.add (object ({ { "command", "insert.add" }, { "parameters", object ({ { "name", "Keys" } }) } }));
    commands.add (object ({ { "command", "lane.add" }, { "parameters", object ({ { "name", "Keys" } }) } }));
    commands.add (
        object ({ { "command", "marker.add" }, { "parameters", object ({ { "at", 3840 }, { "name", "Drop" } }) } }));
    const auto result =
        f.call (sessionId, "run_commands", object ({ { "summary", "Set up a verse" }, { "steps", commands } }));
    INFO (juce::JSON::toString (result));
    REQUIRE_FALSE ((bool)result["isError"]);
    CHECK (f.session.getProject().patterns.size() == before.patterns.size() + 1);
    CHECK (f.session.getProject().playlist.markers.size() == 1);
    CHECK (f.session.getHistory().getUndoDescriptions().size() == steps + 1);

    REQUIRE (f.session.getHistory().undo());
    auto undone = f.session.getProject();
    undone.nextId = before.nextId; // IDs are never given back
    CHECK (undone == before);
}

TEST_CASE ("An agent's steps can't reach files outside the project and the sample folders", "[ai][agents]")
{
    Fixture f;
    f.agents->setEnabled (true);
    const auto sessionId = f.connect();
    juce::Array<juce::var> steps;
    steps.add (
        object ({ { "command", "clip.addAudio" },
                  { "parameters",
                    object ({ { "lane", 1 }, { "file", "/etc/passwd" }, { "start", 0 }, { "length", 960 } }) } }));
    const auto result = f.call (sessionId, "run_commands", object ({ { "steps", steps } }));
    CHECK ((bool)result["isError"]);
    CHECK (result["content"][0]["text"].toString().contains ("outside the project"));
    CHECK (f.agents->getAgentChanges() == 0);

    // Nor by climbing out of the project from a relative path.
    steps.getReference (0).getProperty ("parameters", {}).getDynamicObject()->setProperty ("file", "../../secret.wav");
    const auto climbed = f.call (sessionId, "run_commands", object ({ { "steps", steps } }));
    CHECK ((bool)climbed["isError"]);
    CHECK (climbed["content"][0]["text"].toString().contains ("climbs out"));
}

TEST_CASE ("Without AI, the music tools say so and Spinus's own still work", "[ai][agents]")
{
    Fixture f (false);
    f.agents->setEnabled (true);
    const auto sessionId = f.connect();
    const auto result = f.call (sessionId, "write_part", object ({ { "part", "Bass" } }));
    CHECK ((bool)result["isError"]);
    CHECK (result["content"][0]["text"].toString().contains ("need AI turned on"));
    const auto summary = f.call (sessionId, "song_summary", juce::var (new juce::DynamicObject()));
    CHECK (juce::JSON::parse (summary["content"][0]["text"].toString()).hasProperty ("channels"));
}

TEST_CASE ("Each tool says whether it only reads, and ready-made requests start common jobs", "[ai][agents]")
{
    Fixture f;
    f.agents->setEnabled (true);
    const auto started =
        f.request ("POST", Fixture::rpc ("initialize", object ({ { "clientInfo", object ({ { "name", "T" } }) } })));
    const auto& result = started.json["result"];
    CHECK (result["capabilities"].hasProperty ("prompts"));
    // The conventions come before the first call.
    CHECK (result["instructions"].toString().contains ("960 a quarter note"));
    CHECK (result["instructions"].toString().contains ("C5 = 60"));

    // Spinus's own tools: an agent's client can run the ones that only read without asking.
    const auto listed = f.request ("POST", Fixture::rpc ("tools/list"), started.session).json["result"]["tools"];
    std::map<juce::String, juce::var> hints;
    for (auto& t : *listed.getArray())
        hints[t["name"].toString()] = t["annotations"];
    CHECK ((bool)hints["song_summary"]["readOnlyHint"]);
    CHECK ((bool)hints["render"]["readOnlyHint"]);
    CHECK_FALSE ((bool)hints["run_commands"]["readOnlyHint"]);
    CHECK ((bool)hints["run_commands"]["destructiveHint"]);

    // The ready-made requests, with their arguments filled in.
    const auto prompts = f.request ("POST", Fixture::rpc ("prompts/list"), started.session).json["result"]["prompts"];
    REQUIRE (prompts.isArray());
    juce::StringArray names;
    for (auto& p : *prompts.getArray())
        names.add (p["name"].toString());
    CHECK (names == juce::StringArray ({ "start_song", "make_beat", "add_section", "check_mix", "shape_a_sound" }));
    const auto got =
        f.request (
             "POST",
             Fixture::rpc ("prompts/get", object ({ { "name", "add_section" },
                                                    { "arguments", object ({ { "section", "an 8-bar bridge" } }) } })),
             started.session)
            .json["result"];
    const auto said = got["messages"][0]["content"]["text"].toString();
    CHECK (said.startsWith ("Add an 8-bar bridge to the song open in Spinus, as you think best."));
    CHECK (got["messages"][0]["role"] == "user");
    CHECK (f.request ("POST", Fixture::rpc ("prompts/get", object ({ { "name", "nope" } })), started.session)
               .json.hasProperty ("error"));
}

TEST_CASE ("An agent hears what the user changed, and can't change the song over it unknowingly", "[ai][agents]")
{
    Fixture f;
    f.agents->setEnabled (true);
    const auto sessionId = f.connect();
    const auto none = juce::var (new juce::DynamicObject());
    CHECK (f.call (sessionId, "what_changed", none)["content"][0]["text"].toString().startsWith ("Nothing"));

    juce::Array<juce::var> steps;
    steps.add (object ({ { "command", "project.setTempo" }, { "parameters", object ({ { "bpm", 150 } }) } }));
    const auto faster = object ({ { "summary", "Faster" }, { "steps", steps } });
    CHECK_FALSE ((bool)f.call (sessionId, "run_commands", faster)["isError"]);
    // Its own change isn't news to it.
    CHECK (f.call (sessionId, "what_changed", none)["content"][0]["text"].toString().startsWith ("Nothing"));

    // The user undoes it and changes the key.
    f.session.getHistory().undo();
    const auto setKey = [&f] (int tonic)
    {
        juce::String error;
        auto key = commands::CommandRegistry::get().create (
            "project.setKey", object ({ { "tonic", tonic }, { "scale", "minor" } }), error);
        REQUIRE (key != nullptr);
        REQUIRE (f.session.getHistory().perform (std::move (key)).wasOk());
    };
    setKey (9);
    const auto userStep = f.session.getHistory().getUndoDescriptions().strings.getLast();

    // The agent's next change is refused once, with what the user did; called again, it is done.
    auto result = f.call (sessionId, "run_commands", faster);
    const auto said = result["content"][0]["text"].toString();
    INFO (said);
    CHECK ((bool)result["isError"]);
    CHECK (said.contains ("The user made 1 change: \"" + userStep + "\""));
    CHECK (said.contains ("The user undid 1 step: \"yours: Faster\""));
    CHECK (f.session.getProject().settings.tempoBpm != Catch::Approx (150.0));
    CHECK_FALSE ((bool)f.call (sessionId, "run_commands", faster)["isError"]);
    CHECK (f.session.getProject().settings.tempoBpm == Catch::Approx (150.0));

    // The harness's tools that change the song too (by their annotations); looking at the song counts as seeing.
    f.request ("POST", Fixture::rpc ("tools/list"), sessionId);
    f.session.getHistory().undo();
    CHECK (f.call (sessionId, "what_changed", none)["content"][0]["text"].toString().contains ("undid 1 step"));
    setKey (4);
    CHECK ((bool)f.call (sessionId, "fake_tempo", none)["isError"]);
    CHECK_FALSE ((bool)f.call (sessionId, "fake_tempo", none)["isError"]);
    f.session.getHistory().undo();
    f.call (sessionId, "song_summary", none);
    CHECK_FALSE ((bool)f.call (sessionId, "fake_tempo", none)["isError"]);
}

TEST_CASE ("An agent plays a section for the user to hear, looped, and stops", "[ai][agents]")
{
    Fixture f;
    f.agents->setEnabled (true);
    const auto sessionId = f.connect();
    juce::Array<juce::var> steps;
    steps.add (
        object ({ { "command", "marker.add" }, { "parameters", object ({ { "at", 0 }, { "name", "Intro" } }) } }));
    steps.add (object (
        { { "command", "marker.add" }, { "parameters", object ({ { "at", 8 * 3840 }, { "name", "Chorus" } }) } }));
    steps.add (object (
        { { "command", "marker.add" }, { "parameters", object ({ { "at", 16 * 3840 }, { "name", "Outro" } }) } }));
    REQUIRE_FALSE ((bool)f.call (sessionId, "run_commands", object ({ { "steps", steps } }))["isError"]);
    const auto undoSteps = f.session.getHistory().getUndoDescriptions().size();

    auto result = f.call (sessionId, "play", object ({ { "bars", "chorus" } }));
    INFO (juce::JSON::toString (result));
    CHECK_FALSE ((bool)result["isError"]);
    CHECK (f.session.getMode() == Session::Mode::song);
    const auto loopOf = [&f]
    {
        const auto loop = f.session.getLoopRegion (Session::Mode::song);
        return loop.has_value() ? std::pair (loop->start, loop->end) : std::pair<model::Ticks, model::Ticks> (-1, -1);
    };
    CHECK (loopOf() == std::pair<model::Ticks, model::Ticks> (8 * 3840, 16 * 3840));
    CHECK (f.call (sessionId, "play", object ({ { "bars", "9-12" } }))["isError"] == juce::var (false));
    CHECK (loopOf() == std::pair<model::Ticks, model::Ticks> (8 * 3840, 12 * 3840));
    CHECK ((bool)f.call (sessionId, "play", object ({ { "bars", "the bridge" } }))["isError"]);
    CHECK_FALSE ((bool)f.call (sessionId, "stop", juce::var (new juce::DynamicObject()))["isError"]);
    CHECK_FALSE (f.session.isPlaying());
    // Playing isn't a change to the song.
    CHECK (f.session.getHistory().getUndoDescriptions().size() == undoSteps);
}

TEST_CASE ("The live interface answers MCP over HTTP on 127.0.0.1", "[ai][agents]")
{
    Fixture f;
    f.listen();
    REQUIRE (f.agents->isListening());
    const auto port = f.agents->getPort();
    const auto token = f.agents->getToken();

    // A client on its own thread (the server answers on the message thread, which this one keeps running).
    juce::String reply;
    std::atomic<bool> done { false };
    std::thread client (
        [&]
        {
            juce::StreamingSocket socket;
            if (socket.connect ("127.0.0.1", port, 5000))
            {
                const juce::String body = R"({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})";
                const auto request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + token +
                                     "\r\nContent-Type: application/json\r\nContent-Length: " +
                                     juce::String ((int)body.getNumBytesAsUTF8()) + "\r\n\r\n" + body;
                socket.write (request.toRawUTF8(), (int)request.getNumBytesAsUTF8());
                char buffer[4096];
                while (socket.waitUntilReady (true, 5000) == 1)
                {
                    const auto n = socket.read (buffer, (int)sizeof (buffer), false);
                    if (n <= 0)
                        break;
                    reply += juce::String::fromUTF8 (buffer, n);
                }
            }
            done = true;
        });
    for (int i = 0; i < 2000 && ! done; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
    client.join();
    INFO (reply);
    CHECK (reply.startsWith ("HTTP/1.1 200 OK"));
    CHECK (reply.contains ("Mcp-Session-Id: "));
    CHECK (reply.contains ("\"serverInfo\""));
    CHECK (f.agents->isAgentConnected());
}

TEST_CASE ("Options > AI > Coding agents shows how to set an agent up, and who is connected", "[ui][agents]")
{
    Fixture f;
    ui::AgentInterfacePage page (*f.agents);
    page.setSize (640, 600);
    CHECK (page.getStatusText().startsWith ("Off"));
    CHECK (page.getSetupText ("claude").isEmpty());

    page.getOnOff().setToggleState (true, juce::sendNotificationSync);
    page.refresh();
    CHECK (f.agents->isEnabled());
    CHECK (page.getStatusText().contains ("No agent connected"));
    CHECK (page.getSetupText ("claude").contains (f.agents->getToken()));
    CHECK (page.getSetupText ("codex").contains ("[mcp_servers.spinus]"));

    f.connect();
    page.refresh();
    CHECK (page.getStatusText().startsWith ("Test Agent is connected"));
    page.getOnOff().setToggleState (false, juce::sendNotificationSync);
    CHECK_FALSE (f.agents->isAgentConnected());
}

namespace
{
juce::var none() { return juce::var (new juce::DynamicObject()); }

juce::var stepsOf (std::initializer_list<juce::var> steps)
{
    juce::Array<juce::var> list;
    for (auto& s : steps)
        list.add (s);
    return object ({ { "steps", list } });
}

juce::var command (const juce::String& name, const juce::var& parameters, const juce::String& as = {})
{
    auto step = object ({ { "command", name }, { "parameters", parameters } });
    if (as.isNotEmpty())
        step.getDynamicObject()->setProperty ("as", as);
    return step;
}

juce::var tempo (int bpm) { return stepsOf ({ command ("project.setTempo", object ({ { "bpm", bpm } })) }); }

juce::String said (const juce::var& result) { return result["content"][0]["text"].toString(); }
} // namespace

TEST_CASE ("Agents each keep their own session, and the undo point stays with the first", "[ai][agents]")
{
    Fixture f;
    f.agents->setEnabled (true);
    const auto before = f.session.getProject().settings.tempoBpm;
    const auto first = f.connect ("claude-code");
    REQUIRE_FALSE ((bool)f.call (first, "run_commands", tempo (150))["isError"]);

    // A second agent (or the first one starting again) doesn't end the first one's session.
    const auto second = f.connect ("cursor");
    CHECK (second != first);
    CHECK (f.agents->getAgentName() == "claude-code and cursor");
    CHECK (f.agents->getAgentCount() == 2);
    CHECK (f.request ("POST", Fixture::rpc ("tools/list"), first).status == 200);

    // Each hears of the other's changes, as another agent's.
    REQUIRE_FALSE ((bool)f.call (second, "run_commands", tempo (160))["isError"]);
    const auto refused = f.call (first, "run_commands", tempo (170));
    CHECK ((bool)refused["isError"]);
    CHECK (said (refused).contains ("Another agent made 1 change"));
    CHECK_FALSE ((bool)f.call (first, "run_commands", tempo (170))["isError"]);
    CHECK (f.agents->getAgentChanges() == 3);
    // undo_last undoes only the agent's own last step.
    CHECK ((bool)f.call (second, "undo_last", none())["isError"]);

    // DELETE ends one session; the other goes on.
    CHECK (f.request ("DELETE", juce::var(), second).status == 200);
    const auto gone = f.request ("POST", Fixture::rpc ("tools/list"), second);
    CHECK (gone.status == 404);
    CHECK (gone.json["error"]["message"].toString().contains ("ended"));
    CHECK (f.agents->getAgentName() == "claude-code");

    // A new session while one is open keeps the undo point: Undo the agent's changes goes back to the start.
    const auto again = f.connect ("claude-code");
    REQUIRE_FALSE ((bool)f.call (again, "run_commands", tempo (180))["isError"]);
    f.agents->undoAgentChanges();
    CHECK (f.session.getProject().settings.tempoBpm == Catch::Approx (before));

    // Disconnect ends them all, and says so.
    f.agents->disconnect();
    const auto after = f.request ("POST", Fixture::rpc ("tools/list"), again);
    CHECK (after.status == 404);
    CHECK (after.json["error"]["message"].toString().contains ("Disconnect"));
}

TEST_CASE ("An agent's undo_last isn't news to it", "[ai][agents]")
{
    Fixture f;
    f.agents->setEnabled (true);
    const auto id = f.connect();
    REQUIRE_FALSE ((bool)f.call (id, "run_commands", tempo (150))["isError"]);
    REQUIRE_FALSE ((bool)f.call (id, "run_commands", tempo (160))["isError"]);
    REQUIRE_FALSE ((bool)f.call (id, "undo_last", none())["isError"]);
    CHECK (f.session.getProject().settings.tempoBpm == Catch::Approx (150.0));
    CHECK (said (f.call (id, "what_changed", none())).startsWith ("Nothing"));
    const auto next = f.call (id, "run_commands", tempo (170));
    INFO (said (next));
    CHECK_FALSE ((bool)next["isError"]);
}

TEST_CASE ("Another project opened while an agent works: it is told, and the undo point starts again", "[ai][agents]")
{
    Fixture f;
    f.agents->setEnabled (true);
    const auto id = f.connect();
    REQUIRE_FALSE ((bool)f.call (id, "run_commands", tempo (150))["isError"]);

    f.session.loadProject ({}); // File > New, or another project opened
    for (int i = 0; i < 10; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
    CHECK (f.agents->isAgentConnected());
    CHECK (f.agents->getAgentChanges() == 0);
    CHECK (said (f.call (id, "what_changed", none())).contains ("opened another project"));
    CHECK (said (f.call (id, "what_changed", none())).startsWith ("Nothing"));

    // A change before it looked is refused once with that news.
    f.session.loadProject ({});
    const auto refused = f.call (id, "run_commands", tempo (130));
    CHECK ((bool)refused["isError"]);
    CHECK (said (refused).contains ("opened another project"));
    REQUIRE_FALSE ((bool)f.call (id, "run_commands", tempo (130))["isError"]);
    CHECK (f.agents->getAgentChanges() == 1);
    // Undo the agent's changes stops at the new project's start.
    f.agents->undoAgentChanges();
    CHECK (f.session.getHistory().getUndoDescriptions().isEmpty());
    CHECK ((bool)f.call (id, "undo_last", none())["isError"]);
}

TEST_CASE ("An agent saves the project, starts a new one, and opens one the Browser lists, never over unsaved work",
           "[ai][agents]")
{
    Fixture f;
    const auto other = f.folder.root.getChildFile ("Mine/Other");
    f.listed = { f.openFolder, other };
    f.agents->setEnabled (true);
    const auto id = f.connect();

    const auto answer = juce::JSON::parse (said (f.call (id, "list_projects", none())));
    CHECK (answer["folders"].size() == 2);
    CHECK (answer["folders"][1].toString() == f.roots[1].getFullPathName());
    const auto& list = answer["projects"];
    REQUIRE (list.size() == 2);
    CHECK (list[0]["name"].toString() == "Song");
    CHECK ((bool)list[0]["open"]);
    CHECK_FALSE ((bool)list[1]["open"]);
    CHECK (juce::JSON::parse (said (f.call (
               id, "list_projects", object ({ { "find", "oth" } }))))["projects"][0]["name"] == juce::var ("Other"));

    // Unsaved changes: neither opening nor a new project throws them away.
    f.unsaved = true;
    for (const auto& [tool, arguments] :
         { std::pair { "open_project", object ({ { "project", "Other" } }) }, std::pair { "new_project", none() } })
    {
        const auto refused = f.call (id, tool, arguments);
        CHECK ((bool)refused["isError"]);
        CHECK (said (refused).contains ("save_project first"));
    }
    CHECK (f.openFolder.getFileName() == "Song");

    // Saved where it is, then the other project opens; the agent's next change isn't refused as news.
    CHECK (said (f.call (id, "save_project", none())).startsWith ("Saved"));
    CHECK (f.savedTo == juce::File());
    CHECK ((bool)f.call (id, "save_project", object ({ { "folder", "Album" } }))["isError"]); // it has one
    CHECK ((bool)f.call (id, "open_project", object ({ { "project", "Missing" } }))["isError"]);
    CHECK (said (f.call (id, "open_project", object ({ { "project", "other" } }))).startsWith ("Opened Other"));
    CHECK (f.openFolder == other);
    CHECK_FALSE ((bool)f.call (id, "run_commands", tempo (150))["isError"]);

    // A new project needs a name to be saved, one that can be a folder's and isn't taken.
    f.unsaved = false;
    CHECK (said (f.call (id, "new_project", none())).startsWith ("Started a new"));
    CHECK (f.openFolder == juce::File());
    CHECK ((bool)f.call (id, "save_project", none())["isError"]);
    CHECK ((bool)f.call (id, "save_project", object ({ { "name", "a/b" } }))["isError"]);
    REQUIRE (f.folder.root.getChildFile ("Projects/Taken").createDirectory());
    CHECK ((bool)f.call (id, "save_project", object ({ { "name", "Taken" } }))["isError"]);
    CHECK_FALSE ((bool)f.call (id, "save_project", object ({ { "name", "New Song" } }))["isError"]);
    CHECK (f.savedTo == f.folder.root.getChildFile ("Projects/New Song"));

    // Or in a folder of projects the Browser has, or a new folder in one (an album), never outside them.
    const auto saveIn = [&] (const juce::String& where)
    {
        f.openFolder = juce::File();
        return f.call (id, "save_project", object ({ { "name", "Cue 1" }, { "folder", where } }));
    };
    CHECK_FALSE ((bool)saveIn ("Ashen Vigil/Act 1")["isError"]);
    CHECK (f.savedTo == f.folder.root.getChildFile ("Projects/Ashen Vigil/Act 1/Cue 1"));
    CHECK_FALSE ((bool)saveIn (f.roots[1].getFullPathName())["isError"]);
    CHECK (f.savedTo == f.folder.root.getChildFile ("Mine/Cue 1"));
    CHECK_FALSE ((bool)saveIn (f.roots[1].getChildFile ("Album").getFullPathName())["isError"]);
    CHECK (f.savedTo == f.folder.root.getChildFile ("Mine/Album/Cue 1"));
    f.savedTo = juce::File ("/nowhere");
    REQUIRE (f.folder.root.getChildFile ("Mine/Other/Other.spn").create());
    for (const auto& where :
         { f.folder.root.getChildFile ("Elsewhere").getFullPathName(), juce::String ("../Elsewhere"),
           f.roots[0].getFullPathName() + "/../Elsewhere", juce::String ("a/b/c/d/e"), juce::String (".hidden"),
           f.folder.root.getChildFile ("Mine/Other/In").getFullPathName() })
    {
        INFO (where);
        CHECK ((bool)saveIn (where)["isError"]);
    }
    CHECK (f.savedTo == juce::File ("/nowhere"));

    // The client is told opening and starting replace the open project.
    const auto tools = f.request ("POST", Fixture::rpc ("tools/list"), id).json["result"]["tools"];
    std::map<juce::String, juce::var> hints;
    for (auto& t : *tools.getArray())
        hints[t["name"].toString()] = t["annotations"];
    CHECK ((bool)hints["list_projects"]["readOnlyHint"]);
    CHECK ((bool)hints["open_project"]["destructiveHint"]);
    CHECK ((bool)hints["new_project"]["destructiveHint"]);
    CHECK_FALSE ((bool)hints["save_project"]["destructiveHint"]);
}

TEST_CASE ("play finds sections past the last clip, before the first marker, and of the same name", "[ai][agents]")
{
    Fixture f;
    f.agents->setEnabled (true);
    const auto id = f.connect();
    constexpr int bar = 3840;
    // Marked before any part is written, as the start_song request does.
    REQUIRE_FALSE ((bool)f.call (
        id, "run_commands",
        stepsOf ({ command ("marker.add", object ({ { "at", 4 * bar }, { "name", "Chorus" } })),
                   command ("marker.add", object ({ { "at", 8 * bar }, { "name", "Verse" } })),
                   command ("marker.add", object ({ { "at", 12 * bar }, { "name", "Chorus" } })) }))["isError"]);
    const auto loopOf = [&f]
    {
        const auto loop = f.session.getLoopRegion (Session::Mode::song);
        return loop.has_value() ? std::pair (loop->start, loop->end) : std::pair<model::Ticks, model::Ticks> (-1, -1);
    };
    CHECK_FALSE ((bool)f.call (id, "play", object ({ { "bars", "Start" } }))["isError"]);
    CHECK (loopOf() == std::pair<model::Ticks, model::Ticks> (0, 4 * bar));
    CHECK_FALSE ((bool)f.call (id, "play", object ({ { "bars", "chorus" } }))["isError"]);
    CHECK (loopOf() == std::pair<model::Ticks, model::Ticks> (4 * bar, 8 * bar));
    // The last section, with nothing in it yet: a bar.
    const auto second = f.call (id, "play", object ({ { "bars", "Chorus 2" } }));
    INFO (said (second));
    CHECK_FALSE ((bool)second["isError"]);
    CHECK (loopOf() == std::pair<model::Ticks, model::Ticks> (12 * bar, 13 * bar));
    const auto wrong = f.call (id, "play", object ({ { "bars", "Bridge" } }));
    CHECK ((bool)wrong["isError"]);
    CHECK (said (wrong).contains ("Chorus 2"));
    f.call (id, "stop", none());
}

TEST_CASE ("render measures each section, the part before the first marker too, in short numbers",
           "[ai][agents][audio]")
{
    Fixture f;
    f.agents->setEnabled (true);
    const auto id = f.connect();
    const auto made = f.call (
        id, "run_commands",
        stepsOf (
            { command (
                  "channel.add",
                  object ({ { "name", "Bass" },
                            { "instrument", object ({ { "kind", "stock" }, { "stockType", "spinus.3x-osc" } }) } }),
                  "bass"),
              command ("pattern.add", object ({ { "name", "Loop" }, { "length", 3840 } }), "loop"),
              command (
                  "pattern.setNotes",
                  object ({ { "pattern", "$loop.pattern" },
                            { "channel", "$bass.channel" },
                            { "notes",
                              juce::Array<juce::var> { object (
                                  { { "start", 0 }, { "length", 3600 }, { "pitch", 45 }, { "velocity", 0.8 } }) } } })),
              command ("lane.add", object ({ { "name", "Music" } }), "lane"),
              command ("clip.addPattern",
                       object ({ { "lane", "$lane.lane" }, { "pattern", "$loop.pattern" }, { "start", 0 } })),
              command ("clip.addPattern",
                       object ({ { "lane", "$lane.lane" }, { "pattern", "$loop.pattern" }, { "start", 3840 } })),
              command ("marker.add", object ({ { "at", 3840 }, { "name", "Verse" } })) }));
    INFO (said (made));
    REQUIRE_FALSE ((bool)made["isError"]);
    const auto rendered = f.call (id, "render", none());
    INFO (said (rendered));
    REQUIRE_FALSE ((bool)rendered["isError"]);
    const auto numbers = juce::JSON::parse (said (rendered));
    REQUIRE (numbers["sections"].size() == 2);
    CHECK (numbers["sections"][0]["name"] == "Start");
    CHECK (numbers["sections"][1]["name"] == "Verse");
    CHECK_FALSE (said (rendered).contains ("9999"));
    CHECK_FALSE (said (rendered).contains ("0000"));
}

TEST_CASE ("list_presets on Spinus's own devices says why there are none", "[ai][agents]")
{
    Fixture f;
    f.agents->setEnabled (true);
    const auto id = f.connect();
    REQUIRE_FALSE ((bool)f.call (
        id, "run_commands",
        stepsOf ({ command (
            "channel.add",
            object ({ { "name", "Drums" },
                      { "instrument",
                        object ({ { "kind", "stock" }, { "stockType", "spinus.drum-machine" } }) } })) }))["isError"]);
    const auto& instrument = f.session.getProject().channels.back().instrument;
    REQUIRE (instrument.has_value());
    const auto device = (int)instrument.value().id.value;
    const auto listed = f.call (id, "list_presets", object ({ { "device", device } }));
    INFO (said (listed));
    CHECK_FALSE ((bool)listed["isError"]);
    CHECK (said (listed).contains ("device.setParameters"));
    CHECK ((bool)f.call (id, "list_presets", object ({ { "device", 999 } }))["isError"]);
}

TEST_CASE ("run_commands takes \"as\" labels, and says how to name earlier steps", "[ai][agents]")
{
    Fixture f;
    f.agents->setEnabled (true);
    const auto id = f.connect();
    const auto labelled = f.call (
        id, "run_commands",
        stepsOf (
            { command (
                  "channel.add",
                  object ({ { "name", "Bass" },
                            { "instrument", object ({ { "kind", "stock" }, { "stockType", "spinus.3x-osc" } }) } }),
                  "bass"),
              command ("device.loadPreset", object ({ { "device", "$bass.device" }, { "preset", "Reese Bass" } })) }));
    INFO (said (labelled));
    CHECK_FALSE ((bool)labelled["isError"]);
    const auto unknown =
        f.call (id, "run_commands",
                stepsOf ({ command ("device.loadPreset",
                                    object ({ { "device", "$nope.device" }, { "preset", "Reese Bass" } })) }));
    CHECK ((bool)unknown["isError"]);
    CHECK (said (unknown).contains ("\"as\" label"));
    // Steps sent as text are read; text that isn't steps says what steps are.
    CHECK_FALSE ((bool)f.call (
        id, "run_commands",
        object ({ { "steps", R"([{"command": "project.setTempo", "parameters": {"bpm": 99}}])" } }))["isError"]);
    const auto text = f.call (id, "run_commands", object ({ { "steps", "make it faster" } }));
    CHECK (said (text).contains ("list of steps"));
    // Arguments that aren't an object.
    const auto bad = f.request (
        "POST", Fixture::rpc ("tools/call", object ({ { "name", "song_summary" }, { "arguments", 3 } })), id);
    CHECK ((bool)bad.json["result"]["isError"]);
}

TEST_CASE ("The live interface checks origins exactly, negotiates the version, and reads chunked bodies",
           "[ai][agents]")
{
    using ai::AgentInterface;
    CHECK (AgentInterface::isLocalOrigin (""));
    CHECK (AgentInterface::isLocalOrigin ("http://localhost"));
    CHECK (AgentInterface::isLocalOrigin ("http://127.0.0.1:6274"));
    CHECK (AgentInterface::isLocalOrigin ("http://[::1]:3000"));
    CHECK_FALSE (AgentInterface::isLocalOrigin ("http://localhost.evil.example"));
    CHECK_FALSE (AgentInterface::isLocalOrigin ("http://127.0.0.1.evil.example"));
    CHECK_FALSE (AgentInterface::isLocalOrigin ("http://localhost:80@evil.example"));
    CHECK_FALSE (AgentInterface::isLocalOrigin ("http://localhost:x"));
    CHECK_FALSE (AgentInterface::isLocalOrigin ("null"));

    Fixture f;
    f.agents->setEnabled (true);
    CHECK (f.request ("POST", Fixture::rpc ("initialize"), {}, {}, "http://localhost.evil.example").status == 403);
    CHECK (f.request ("POST", Fixture::rpc ("initialize"), {}, {}, "http://localhost:6274").status == 200);

    const auto versionFor = [&f] (const juce::String& asked)
    {
        return f.request ("POST", Fixture::rpc ("initialize", object ({ { "protocolVersion", asked } })))
            .json["result"]["protocolVersion"]
            .toString();
    };
    CHECK (versionFor ("2025-06-18") == "2025-06-18");
    CHECK (versionFor ("1999-01-01") == AgentInterface::supportedProtocolVersions()[0]);
    CHECK (versionFor ("") == AgentInterface::supportedProtocolVersions()[0]);

    // A body in chunks, put together only when the last chunk has come.
    const juce::String chunked = "4\r\n{\"a\"\r\n5;x=1\r\n: 12}\r\n0\r\n\r\n";
    juce::MemoryBlock body;
    bool malformed = false;
    CHECK (AgentInterface::decodeChunked (juce::MemoryBlock (chunked.toRawUTF8(), chunked.getNumBytesAsUTF8()), body,
                                          malformed));
    CHECK (body.toString() == "{\"a\": 12}");
    const auto part = chunked.substring (0, 12);
    CHECK_FALSE (AgentInterface::decodeChunked (juce::MemoryBlock (part.toRawUTF8(), part.getNumBytesAsUTF8()), body,
                                                malformed));
    CHECK_FALSE (malformed);
    const juce::String bad = "zz\r\nabc\r\n";
    CHECK_FALSE (
        AgentInterface::decodeChunked (juce::MemoryBlock (bad.toRawUTF8(), bad.getNumBytesAsUTF8()), body, malformed));
    CHECK (malformed);

    // The token: 32 hex digits, a new one each time, in a file only this user can read.
    const auto token = f.agents->getToken();
    CHECK (token.containsOnly ("0123456789abcdef"));
    f.agents->newToken();
    CHECK (f.agents->getToken() != token);
#if ! JUCE_WINDOWS
    struct stat info {};
    REQUIRE (::stat (f.folder.root.getChildFile ("agent-interface.json").getFullPathName().toRawUTF8(), &info) == 0);
    CHECK (((unsigned)info.st_mode & 0777u) == 0600u);
#endif
}

TEST_CASE ("With AI off the agent is told only of the tools it has, and told when the music tools come", "[ai][agents]")
{
    Fixture f;
    f.music = false;
    f.agents->setEnabled (true);
    const auto started =
        f.request ("POST", Fixture::rpc ("initialize", object ({ { "clientInfo", object ({ { "name", "T" } }) } })));
    const auto instructions = started.json["result"]["instructions"].toString();
    CHECK (instructions.contains ("off because AI is off"));
    CHECK (started.json["result"]["capabilities"]["tools"]["listChanged"] == juce::var (true));
    const auto id = started.session;

    const auto toolNames = [&f, &id]
    {
        juce::StringArray names;
        const auto listed = f.request ("POST", Fixture::rpc ("tools/list"), id).json["result"]["tools"];
        if (auto* tools = listed.getArray())
            for (auto& t : *tools)
                names.add (t["name"].toString());
        return names;
    };
    CHECK_FALSE (toolNames().contains ("fake_tempo"));
    const auto prompt =
        f.request ("POST",
                   Fixture::rpc ("prompts/get", object ({ { "name", "start_song" },
                                                          { "arguments", object ({ { "description", "reggae" } }) } })),
                   id)
            .json["result"]["messages"][0]["content"]["text"]
            .toString();
    CHECK (prompt.contains ("AI is off"));
    const auto misspelled = f.call (id, "song_sumary", none());
    CHECK ((bool)misspelled["isError"]);
    CHECK (said (misspelled).contains ("what_changed"));

    // AI turned on: the next answer tells the client its tool list changed, then answers.
    f.music = true;
    const auto told = f.request ("POST", Fixture::rpc ("ping"), id);
    CHECK (told.contentType == "text/event-stream");
    CHECK (told.body.contains ("notifications/tools/list_changed"));
    CHECK (told.body.contains ("\"result\""));
    CHECK (f.request ("POST", Fixture::rpc ("ping"), id).contentType != "text/event-stream"); // told once
    CHECK (toolNames().contains ("fake_tempo"));
}

TEST_CASE ("The live interface answers a POST without a length with 411, and reads a chunked one", "[ai][agents]")
{
    Fixture f;
    f.listen();
    REQUIRE (f.agents->isListening());
    const auto port = f.agents->getPort();
    const auto token = f.agents->getToken();
    const auto send = [&] (const juce::String& extraHeaders, const juce::String& body)
    {
        juce::String reply;
        std::atomic<bool> done { false };
        std::thread client (
            [&]
            {
                juce::StreamingSocket socket;
                if (socket.connect ("127.0.0.1", port, 5000))
                {
                    const auto request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + token +
                                         "\r\nContent-Type: application/json\r\n" + extraHeaders + "\r\n" + body;
                    socket.write (request.toRawUTF8(), (int)request.getNumBytesAsUTF8());
                    char buffer[4096];
                    while (socket.waitUntilReady (true, 5000) == 1)
                    {
                        const auto n = socket.read (buffer, (int)sizeof (buffer), false);
                        if (n <= 0)
                            break;
                        reply += juce::String::fromUTF8 (buffer, n);
                    }
                }
                done = true;
            });
        for (int i = 0; i < 2000 && ! done; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
        client.join();
        return reply;
    };
    const juce::String body = R"({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})";
    const auto noLength = send ("Connection: close\r\n", "");
    INFO (noLength);
    CHECK (noLength.startsWith ("HTTP/1.1 411"));
    const auto inChunks =
        send ("Transfer-Encoding: chunked\r\n",
              juce::String::toHexString ((int)body.getNumBytesAsUTF8()) + "\r\n" + body + "\r\n0\r\n\r\n");
    INFO (inChunks);
    CHECK (inChunks.startsWith ("HTTP/1.1 200 OK"));
    CHECK (inChunks.contains ("\"serverInfo\""));
}

namespace
{
/** One HTTP request to the live interface, as an agent's MCP client sends it; the status, the session header and
    the body. Any thread. */
struct HttpReply
{
    int status = 0;
    juce::String session;
    juce::var json;
};

HttpReply post (int port, const juce::String& token, const juce::String& sessionId, const juce::var& body)
{
    HttpReply r;
    juce::StreamingSocket socket;
    if (! socket.connect ("127.0.0.1", port, 5000))
        return r;
    const auto text = juce::JSON::toString (body, true);
    auto request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + token +
                   "\r\nAccept: application/json, text/event-stream\r\nConnection: close\r\n" +
                   (sessionId.isNotEmpty() ? "Mcp-Session-Id: " + sessionId + "\r\n" : juce::String()) +
                   "Content-Type: application/json\r\nContent-Length: " + juce::String ((int)text.getNumBytesAsUTF8()) +
                   "\r\n\r\n" + text;
    socket.write (request.toRawUTF8(), (int)request.getNumBytesAsUTF8());
    juce::MemoryBlock got;
    char buffer[8192];
    while (socket.waitUntilReady (true, 20000) == 1)
    {
        const auto n = socket.read (buffer, (int)sizeof (buffer), false);
        if (n <= 0)
            break;
        got.append (buffer, (size_t)n);
        // Done once the body is as long as the header says.
        const auto all = got.toString();
        const auto headerEnd = all.indexOf ("\r\n\r\n");
        if (headerEnd > 0)
        {
            const auto headers = all.substring (0, headerEnd);
            const auto length = headers.fromFirstOccurrenceOf ("Content-Length: ", false, true)
                                    .upToFirstOccurrenceOf ("\r\n", false, false)
                                    .getIntValue();
            if (length > 0 && (int)got.getSize() >= headerEnd + 4 + length)
                break;
        }
    }
    const auto all = got.toString();
    r.status = all.fromFirstOccurrenceOf (" ", false, false).getIntValue();
    r.session =
        all.fromFirstOccurrenceOf ("Mcp-Session-Id: ", false, true).upToFirstOccurrenceOf ("\r\n", false, false).trim();
    auto payload = all.fromFirstOccurrenceOf ("\r\n\r\n", false, false);
    if (payload.startsWith ("event:") || payload.startsWith ("data:"))
        payload = payload.fromFirstOccurrenceOf ("data:", false, false).upToFirstOccurrenceOf ("\n\n", false, false);
    r.json = juce::JSON::parse (payload);
    return r;
}
} // namespace

TEST_CASE ("Two agents connected at once over HTTP: each one's changes land, none is lost", "[ai][agents]")
{
    Fixture f;
    f.listen();
    REQUIRE (f.agents->isListening());
    const auto port = f.agents->getPort();
    const auto token = f.agents->getToken();
    constexpr int changesEach = 5;

    // Two agents, each on its own thread, add channels as fast as they can. A change made after the other
    // agent's is refused once (it is told what the other did), then made when sent again.
    struct Agent
    {
        juce::String name;
        int made = 0, refused = 0, failed = 0;
        juce::StringArray problems;
    };
    std::array<Agent, 2> agents { Agent { "claude-code" }, Agent { "cursor" } };
    std::atomic<int> finished { 0 };
    std::vector<std::thread> threads;
    threads.reserve (agents.size());
    for (auto& agent : agents)
        threads.emplace_back (
            [&, a = &agent]
            {
                const auto hello =
                    post (port, token, {},
                          Fixture::rpc ("initialize", object ({ { "clientInfo", object ({ { "name", a->name } }) } })));
                if (hello.status != 200 || hello.session.isEmpty())
                {
                    a->problems.add ("initialize: " + juce::String (hello.status));
                    ++finished;
                    return;
                }
                for (int i = 1; i <= changesEach && a->failed < 20; ++i)
                {
                    const auto name = a->name + " " + juce::String (i);
                    const auto reply = post (
                        port, token, hello.session,
                        Fixture::rpc (
                            "tools/call",
                            object ({ { "name", "run_commands" },
                                      { "arguments",
                                        stepsOf ({ command ("channel.add", object ({ { "name", name } })) }) } })));
                    const auto result = reply.json["result"];
                    if (reply.status != 200 || ! result.isObject())
                    {
                        a->problems.add (name + ": HTTP " + juce::String (reply.status));
                        ++a->failed;
                        --i;
                    }
                    else if ((bool)result["isError"])
                    {
                        // Refused: said why, plainly; sent again.
                        if (! said (result).contains ("Another agent"))
                            a->problems.add (name + ": " + said (result));
                        ++a->refused;
                        --i;
                    }
                    else
                        ++a->made;
                }
                ++finished;
            });
    for (int i = 0; i < 12000 && finished < 2; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
    for (auto& t : threads)
        t.join();

    for (auto& agent : agents)
    {
        INFO (agent.name << ": " << agent.problems.joinIntoString ("; "));
        CHECK (agent.made == changesEach);
        CHECK (agent.problems.isEmpty());
    }
    // Every change is in the song, once.
    juce::StringArray names;
    for (auto& channel : f.session.getProject().channels)
        names.add (channel.name);
    for (auto& agent : agents)
        for (int i = 1; i <= changesEach; ++i)
            CHECK (names.indexOf (agent.name + " " + juce::String (i)) >= 0);
    CHECK (names.size() == 2 * changesEach);
    CHECK (f.agents->getAgentCount() == 2);
    CHECK (f.agents->getAgentChanges() == 2 * changesEach);
}

TEST_CASE ("The music tools are told what a learned instrument's keys do", "[agents][instruments]")
{
    Fixture f;
    juce::String error;
    auto add = commands::CommandRegistry::get().create (
        "channel.add",
        juce::JSON::parse (R"({"name": "Guitar", "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}})"),
        error);
    REQUIRE (add != nullptr);
    REQUIRE (f.session.getHistory().perform (std::move (add)).wasOk());
    const auto index = f.folder.root.getChildFile ("sounds");
    InstrumentProfiles::Mode mode;
    mode.setKeys (40, 91, { InstrumentProfiles::Job::plays, {}, InstrumentProfiles::Source::user });
    mode.setKeys (12, 12, { InstrumentProfiles::Job::switches, "palm mute", InstrumentProfiles::Source::user });
    InstrumentProfiles profiles (index);
    profiles.set ("stock-spinus.3x-osc", InstrumentProfiles::Profile { { mode }, {} });
    REQUIRE (profiles.save());

    ai::ContextOptions options;
    options.deviceChoices = false;
    options.profilesFolder = index;
    const auto context = ai::projectContext (f.session, options);
    const auto& instrument = f.session.getProject().channels.back().instrument;
    REQUIRE (instrument.has_value());
    const auto device = juce::String ((int)instrument.value().id.value);
    CHECK (context["instrumentKeys"][juce::Identifier (device)]["modes"][0]["switches"][0]["what"].toString() ==
           "palm mute");
    // And 3x Osc in the list of what can be added carries them too.
    bool listed = false;
    for (auto& d : *context["devices"].getArray())
        if (d["device"]["stockType"].toString() == "spinus.3x-osc")
            listed = d["keys"]["modes"].size() == 1;
    CHECK (listed);
}

TEST_CASE ("The music tools are told what a learned effect suits and what its settings do", "[agents][effects]")
{
    Fixture f;
    juce::String error;
    auto add = commands::CommandRegistry::get().create (
        "insert.addEffect",
        juce::JSON::parse (R"({"insert": )" + juce::String ((int)model::masterInsertId.value) +
                           R"(, "device": {"kind": "stock", "stockType": "spinus.filter"}})"),
        error);
    REQUIRE (add != nullptr);
    REQUIRE (f.session.getHistory().perform (std::move (add)).wasOk());
    const auto index = f.folder.root.getChildFile ("sounds");
    EffectProfiles::Profile profile;
    profile.goodFor = "taming harsh highs";
    profile.settings.push_back ({ "cutoff", "Cutoff", "where the highs start to fall", "2 kHz", "12 kHz", true });
    EffectProfiles profiles (index);
    profiles.set ("stock-spinus.filter", profile);
    REQUIRE (profiles.save());

    ai::ContextOptions options;
    options.deviceChoices = false;
    options.profilesFolder = index;
    const auto context = ai::projectContext (f.session, options);
    const auto device = juce::String ((int)f.session.getProject().mixer.master.effects.front().device.id.value);
    CHECK (context["effectGoodFor"][juce::Identifier (device)].toString() == "taming harsh highs");
    juce::var cutoff;
    for (auto& p : *context["deviceParameters"][juce::Identifier (device)].getArray())
        if (p["id"].toString() == "cutoff")
            cutoff = p;
    REQUIRE (cutoff.isObject());
    CHECK (cutoff["what"].toString() == "where the highs start to fall");
    CHECK (cutoff["safeRange"].toString() == "2 kHz to 12 kHz");
    CHECK ((bool)cutoff["matters"]);
    CHECK (cutoff["range"].toString().contains (" to "));
    // And Filter in the list of what can be added says what it suits.
    bool listed = false;
    for (auto& d : *context["devices"].getArray())
        if (d["device"]["stockType"].toString() == "spinus.filter")
            listed = d["goodFor"].toString() == "taming harsh highs";
    CHECK (listed);
}

TEST_CASE ("Agents read analyzed songs: what was measured, the exact notes only when the user allows it",
           "[ai][agents][analysis]")
{
    Fixture f;
    f.agents->setEnabled (true);
    const auto sessionId = f.connect();

    // Nothing analyzed yet.
    auto result = f.call (sessionId, "list_analyzed_songs", object ({}));
    CHECK_FALSE ((bool)result["isError"]);
    CHECK (result["content"][0]["text"].toString().startsWith ("No songs are analyzed yet"));

    // One song in the library, as the Analyzer and the Track creator keep it.
    const auto aiFolder = f.folder.root.getChildFile ("ai");
    auto library = AnalysisLibrary::inAiFolder (aiFolder);
    const juce::String digest = "0123456789abcdef0123456789abcdef";
    juce::Array<juce::var> beats, downbeats, notes, hits;
    for (int i = 0; i < 16; ++i)
        beats.add (i * 0.5);
    for (int i = 0; i < 4; ++i)
        downbeats.add (i * 2.0);
    for (int i = 0; i < 10; ++i)
        notes.add (
            object ({ { "start", i * 1.0 }, { "end", i * 1.0 + 0.5 }, { "pitch", 40 + i }, { "velocity", 90 } }));
    hits.add (object ({ { "time", 0.0 }, { "kind", "kick" }, { "velocity", 100 } }));
    REQUIRE (library.set (
        digest, "Reference Song.flac", "rhythm",
        object ({ { "tempo", 120.0 }, { "beatsPerBar", 4 }, { "beats", beats }, { "downbeats", downbeats } })));
    REQUIRE (library.set (digest, "Reference Song.flac", "key", object ({ { "name", "A minor" } })));
    REQUIRE (library.setIn (digest, "Reference Song.flac", "notes", "htdemucs/bass", notes));
    REQUIRE (library.setIn (digest, "Reference Song.flac", "drums", "htdemucs", hits));

    result = f.call (sessionId, "list_analyzed_songs", object ({}));
    auto text = result["content"][0]["text"].toString();
    INFO (text);
    CHECK (text.contains ("Reference Song.flac"));
    CHECK (text.contains ("notes: htdemucs/bass"));
    CHECK_FALSE (text.contains (f.folder.root.getFullPathName())); // names only, no paths

    // What was measured, by a part of its name; no notes by default.
    result = f.call (sessionId, "song_analysis", object ({ { "song", "reference" } }));
    REQUIRE_FALSE ((bool)result["isError"]);
    auto found = juce::JSON::parse (result["content"][0]["text"].toString());
    CHECK (found["name"] == "Reference Song.flac");
    CHECK ((double)found["rhythm"]["tempo"] == Catch::Approx (120.0));
    CHECK ((int)found["rhythm"]["bars"] == 4);
    CHECK (found["key"]["name"] == "A minor");
    CHECK_FALSE ((bool)found["fullData"]);
    CHECK (found["exactNotes"].toString().contains ("Provide full data through MCP"));
    CHECK (! found.hasProperty ("notes"));

    result = f.call (sessionId, "song_analysis", object ({ { "song", "Reference Song" }, { "part", "bass" } }));
    CHECK ((bool)result["isError"]);
    CHECK (result["content"][0]["text"].toString().contains ("Provide full data through MCP"));
    result = f.call (sessionId, "song_analysis", object ({ { "song", "Another" } }));
    CHECK ((bool)result["isError"]);

    // Allowed: one part's notes, a stretch of time, and the drum hits.
    agents::setFullDataAllowed (aiFolder, true);
    result = f.call (sessionId, "song_analysis",
                     object ({ { "song", "Reference Song" }, { "part", "bass" }, { "from", 2.0 }, { "to", 5.0 } }));
    REQUIRE_FALSE ((bool)result["isError"]);
    found = juce::JSON::parse (result["content"][0]["text"].toString());
    CHECK (found["part"] == "htdemucs/bass");
    REQUIRE (found["notes"].getArray() != nullptr);
    CHECK (found["notes"].getArray()->size() == 3); // starting at 2, 3 and 4 s
    CHECK ((int)found["notes"][0]["pitch"] == 42);

    result =
        f.call (sessionId, "song_analysis", object ({ { "song", digest.substring (0, 20) }, { "part", "drums" } }));
    REQUIRE_FALSE ((bool)result["isError"]);
    found = juce::JSON::parse (result["content"][0]["text"].toString());
    CHECK (found["hits"][0]["kind"] == "kick");

    result = f.call (sessionId, "song_analysis", object ({ { "song", "Reference Song" }, { "part", "vocals" } }));
    CHECK ((bool)result["isError"]);
    CHECK (result["content"][0]["text"].toString().contains ("htdemucs/bass"));

    // The setting keeps the Analyzer's other settings.
    agents::setFullDataAllowed (aiFolder, false);
    CHECK_FALSE (agents::fullDataAllowed (aiFolder));
}

TEST_CASE ("Agents tune a vocal: listen to it, read its notes and a melody's, match it to the melody",
           "[ai][agents][tune]")
{
    Fixture f;
    const auto take = f.folder.root.getChildFile ("vocal.wav");
    {
        std::vector<float> silence (96000, 0.0f);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (take);
        auto writer = wav.createWriterFor (
            stream,
            juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (16));
        REQUIRE (writer != nullptr);
        const float* channels[] = { silence.data() };
        writer->writeFromFloatArrays (channels, 1, (int)silence.size());
    }

    model::Project p;
    model::Channel melodyChannel;
    melodyChannel.id = p.allocateId<model::ChannelId>();
    melodyChannel.name = "Melody";
    p.channels = { melodyChannel };
    model::Lane lane { p.allocateId<model::LaneId>(), "Vocal" };
    p.playlist.lanes = { lane };
    model::Pattern melody;
    melody.id = p.allocateId<model::PatternId>();
    melody.notes[melodyChannel.id] = { { 0, 1920, 57 }, { 1920, 1440, 60 } }; // at 120 BPM: 0..1 s, 1..1.75 s
    p.patterns = { melody };
    model::AudioClip audio;
    audio.file = take.getFullPathName();
    audio.onTempoChange = "time";
    const auto clip = p.allocateId<model::ClipId>();
    p.playlist.clips.push_back ({ clip, lane.id, 0, 7680, false, audio });
    p.playlist.clips.push_back (
        { p.allocateId<model::ClipId>(), lane.id, 0, 7680, false, model::PatternClip { melody.id, 0 } });
    f.session.getHistory().reset (p);

    f.listen();
    const auto id = f.connect();
    auto said = [] (const juce::var& result) { return result["content"][0]["text"].toString(); };
    const auto clipArgs = object ({ { "clip", (int)clip.value } });
    CHECK (said (f.call (id, "vocal_notes", clipArgs)).contains ("listen_to_vocal"));
    auto result = f.call (id, "listen_to_vocal", clipArgs); // AI off
    CHECK ((bool)result["isError"]);
    CHECK (said (result).contains ("AI"));

    f.heard = juce::JSON::parse (R"({"duration": 2.0, "track": {"times": [0, 0.005], "hz": [220, 220]},
                                     "notes": [{"start": 0.0, "end": 1.0, "pitch": 57.3},
                                               {"start": 1.0, "end": 1.9, "pitch": 59.6}]})");
    CHECK (said (f.call (id, "listen_to_vocal", clipArgs)).startsWith ("Listened"));

    auto notes = juce::JSON::parse (said (f.call (
        id, "vocal_notes", object ({ { "clip", (int)clip.value }, { "channel", (int)melodyChannel.id.value } }))));
    REQUIRE (notes["notes"].size() == 2);
    CHECK ((int)notes["notes"][1]["index"] == 1);
    CHECK ((double)notes["notes"][1]["sounds"] == Catch::Approx (59.6));
    REQUIRE (notes["melody"].size() == 2);
    CHECK ((double)notes["melody"][1]["end"] == Catch::Approx (1.75));
    CHECK (! (bool)notes["tuned"]);

    result = f.call (id, "match_vocal_to_melody",
                     object ({ { "clip", (int)clip.value }, { "channel", (int)melodyChannel.id.value } }));
    INFO (said (result));
    CHECK (! (bool)result["isError"]);
    CHECK (said (result).contains ("2 notes on the melody's pitch, 2 of them on its timing"));
    const auto& tuned = std::get<model::AudioClip> (f.session.getProject().findClip (clip)->content).tune;
    REQUIRE (tuned.size() == 2);
    CHECK (tuned[0].pitch + tuned[0].shift == Catch::Approx (57.0f));
    CHECK (tuned[1].pitch + tuned[1].shift == Catch::Approx (60.0f));
    CHECK (tuned[1].outputEnd() == Catch::Approx (1.75));
    CHECK (f.session.getHistory().getUndoDescriptions().joinIntoString ("|").contains ("Match vocal to melody"));

    // A channel that plays nothing there.
    result = f.call (id, "match_vocal_to_melody", object ({ { "clip", (int)clip.value }, { "channel", 999 } }));
    CHECK ((bool)result["isError"]);
}

TEST_CASE ("Agents read what song_summary only names: a pattern's notes, a clip's settings, a device's settings",
           "[ai][agents]")
{
    Fixture f;
    model::Project p;
    model::Channel lead;
    lead.id = p.allocateId<model::ChannelId>();
    lead.name = "Lead";
    lead.pan = -0.5f;
    lead.muted = true;
    p.channels = { lead };
    model::Insert vocals;
    vocals.id = p.allocateId<model::InsertId>();
    vocals.name = "Vocals";
    vocals.notesFrom = lead.id;
    model::Device delay;
    delay.id = p.allocateId<model::DeviceId>();
    delay.stockType = "spinus.delay";
    vocals.effects.push_back ({ delay });
    p.mixer.inserts = { vocals };
    model::Pattern riff;
    riff.id = p.allocateId<model::PatternId>();
    riff.name = "Riff";
    riff.swing = 0.25f;
    riff.notes[lead.id] = { { 0, 480, 64 }, { 480, 480, 67 } };
    riff.automation.push_back (
        { { model::ParameterTarget::Kind::channelPan, lead.id }, { { 0, 0.5f }, { 960, 1.0f } } });
    p.patterns = { riff };
    model::Lane lane { p.allocateId<model::LaneId>(), "Lane" };
    lane.solo = true;
    p.playlist.lanes = { lane };
    model::AudioClip audio;
    audio.file = "take.wav";
    audio.gainDb = -3.0f;
    audio.fadeIn = 240;
    const auto audioClip = p.allocateId<model::ClipId>();
    p.playlist.clips.push_back ({ audioClip, lane.id, 0, 3840, false, audio });
    model::AutomationClip sweep;
    sweep.target = { model::ParameterTarget::Kind::channelVolume, lead.id };
    sweep.points = { { 0, 0.2f }, { 1920, 0.8f, 0.5f } };
    const auto sweepClip = p.allocateId<model::ClipId>();
    p.playlist.clips.push_back ({ sweepClip, lane.id, 3840, 1920, false, sweep });
    const model::LauncherTrack track { p.allocateId<model::LauncherTrackId>(), "Loops" };
    const model::Scene scene { p.allocateId<model::SceneId>(), "Intro" };
    p.launcher.tracks = { track };
    p.launcher.scenes = { scene };
    const auto launched = p.allocateId<model::ClipId>();
    p.launcher.clips.push_back ({ launched, track.id, scene.id, 3840, false, model::PatternClip { riff.id, 0 } });
    f.session.getHistory().reset (p);

    f.agents->setEnabled (true);
    const auto id = f.connect();
    const auto read = [&] (const juce::String& tool, const juce::var& arguments)
    {
        const auto result = f.call (id, tool, arguments);
        INFO (said (result));
        CHECK_FALSE ((bool)result["isError"]);
        return juce::JSON::parse (said (result));
    };

    // What song_summary now shows of the mixer, the lanes, the patterns and the launcher.
    const auto summary = read ("song_summary", {});
    CHECK ((double)summary["channels"][0]["pan"] == Catch::Approx (-0.5));
    CHECK ((bool)summary["channels"][0]["muted"]);
    CHECK ((int)summary["inserts"][1]["notesFrom"] == (int)lead.id.value);
    CHECK ((bool)summary["lanes"][0]["solo"]);
    CHECK ((double)summary["patterns"][0]["swing"] == Catch::Approx (0.25));
    CHECK (summary["launcher"]["scenes"][0]["name"] == "Intro");
    CHECK ((int)summary["launcher"]["clips"][0]["pattern"] == (int)riff.id.value);
    CHECK_FALSE ((bool)summary["launcher"]["clips"][0]["loop"]);

    const auto pattern = read ("read_pattern", object ({ { "pattern", (int)riff.id.value } }));
    REQUIRE (pattern["channels"].size() == 1);
    CHECK (pattern["channels"][0]["name"] == "Lead");
    REQUIRE (pattern["channels"][0]["notes"].size() == 2);
    CHECK ((int)pattern["channels"][0]["notes"][1]["pitch"] == 67);
    CHECK ((int)pattern["channels"][0]["notes"][1]["start"] == 480);
    CHECK (pattern["automation"][0]["target"]["kind"] == "channelPan");
    CHECK ((double)pattern["automation"][0]["points"][1]["value"] == Catch::Approx (1.0));
    CHECK (
        read ("read_pattern", object ({ { "pattern", (int)riff.id.value }, { "channel", 999 } }))["channels"].size() ==
        0);
    CHECK ((bool)f.call (id, "read_pattern", object ({ { "pattern", 999 } }))["isError"]);

    const auto clip = read ("read_clip", object ({ { "clip", (int)audioClip.value } }));
    CHECK (clip["type"] == "audio");
    CHECK ((double)clip["gainDb"] == Catch::Approx (-3.0));
    CHECK ((int)clip["fadeIn"] == 240);
    const auto points = read ("read_clip", object ({ { "clip", (int)sweepClip.value } }));
    CHECK (points["target"]["kind"] == "channelVolume");
    CHECK ((double)points["points"][1]["curve"] == Catch::Approx (0.5));
    const auto inLauncher = read ("read_clip", object ({ { "clip", (int)launched.value } }));
    CHECK ((bool)inLauncher["launcher"]);
    CHECK ((int)inLauncher["scene"] == (int)scene.id.value);
    CHECK_FALSE (inLauncher.hasProperty ("lane"));
    CHECK ((bool)f.call (id, "read_clip", object ({ { "clip", 999 } }))["isError"]);

    const auto settings = read ("device_settings", object ({ { "device", (int)delay.id.value } }));
    CHECK (settings["settings"].size() > 3);
    const auto feedback =
        read ("device_settings", object ({ { "device", (int)delay.id.value }, { "find", "feed" } }))["settings"];
    REQUIRE (feedback.size() == 1);
    CHECK (feedback[0]["id"] == "feedback");
    CHECK (feedback[0]["value"] == "35%");
    CHECK (feedback[0]["range"].toString().endsWith ("95%"));
    CHECK ((bool)f.call (id, "device_settings", object ({ { "device", 999 } }))["isError"]);
}

TEST_CASE ("Agents load a Drum Machine kit and samples, only from the project and the sample folders", "[ai][agents]")
{
    Fixture f;
    f.agents->setEnabled (true);
    const auto id = f.connect();
    const auto hit = f.openFolder.getChildFile ("hit.wav");
    {
        REQUIRE (f.openFolder.createDirectory());
        std::vector<float> click (4800, 0.0f);
        click[0] = 1.0f;
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (hit);
        auto writer = wav.createWriterFor (
            stream,
            juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (16));
        REQUIRE (writer != nullptr);
        const float* channels[] = { click.data() };
        writer->writeFromFloatArrays (channels, 1, (int)click.size());
    }
    const auto add = [&] (const juce::String& type)
    {
        REQUIRE_FALSE ((bool)f.call (
            id, "run_commands",
            stepsOf ({ command (
                "channel.add",
                object ({ { "name", type },
                          { "instrument", object ({ { "kind", "stock" }, { "stockType", type } }) } })) }))["isError"]);
        f.session.getEdit().dispatchPendingUpdatesSynchronously();
        const auto device = f.session.getProject().channels.back().instrument.value().id;
        auto* plugin = dynamic_cast<tracktion::ExternalPlugin*> (f.session.getAdapter().getDevicePlugin (device));
        REQUIRE (plugin != nullptr);
        return std::pair { (int)device.value, plugin->getAudioPluginInstance() };
    };
    const auto [drumsId, drumsInstance] = add ("spinus.drum-machine");
    auto* drums = dynamic_cast<dsp::DrumMachineProcessor*> (drumsInstance);
    REQUIRE (drums != nullptr);

    // list_presets names the kits; load_sounds loads one.
    CHECK (said (f.call (id, "list_presets", object ({ { "device", drumsId } }))).contains ("Spinus Synth Kit"));
    auto result = f.call (id, "load_sounds", object ({ { "device", drumsId }, { "kit", "Spinus Synth Kit" } }));
    INFO (said (result));
    CHECK_FALSE ((bool)result["isError"]);
    CHECK (drums->getKitDisplayName() == "Spinus Synth Kit");

    // A file in the project's folder, by its path there, onto pad 2.
    result =
        f.call (id, "load_sounds",
                object ({ { "device", drumsId }, { "files", juce::Array<juce::var> { "hit.wav" } }, { "pad", 2 } }));
    INFO (said (result));
    CHECK_FALSE ((bool)result["isError"]);
    CHECK (drums->getPads()[1].name.containsIgnoreCase ("hit"));

    const auto [samplerId, samplerInstance] = add ("spinus.sampler");
    auto* sampler = dynamic_cast<dsp::SamplerProcessor*> (samplerInstance);
    REQUIRE (sampler != nullptr);
    result =
        f.call (id, "load_sounds",
                object ({ { "device", samplerId }, { "files", juce::Array<juce::var> { hit.getFullPathName() } } }));
    INFO (said (result));
    CHECK_FALSE ((bool)result["isError"]);
    CHECK (sampler->getZones().size() == 1);

    // Refused: a file outside, both or neither of kit and files, a kit on a Sampler, no such kit.
    CHECK ((bool)f.call (
        id, "load_sounds",
        object ({ { "device", samplerId }, { "files", juce::Array<juce::var> { "/etc/hostname" } } }))["isError"]);
    CHECK ((bool)f.call (id, "load_sounds", object ({ { "device", samplerId } }))["isError"]);
    CHECK ((bool)f.call (id, "load_sounds", object ({ { "device", samplerId }, { "kit", "Magpie" } }))["isError"]);
    result = f.call (id, "load_sounds", object ({ { "device", drumsId }, { "kit", "No Such Kit" } }));
    CHECK ((bool)result["isError"]);
    CHECK (said (result).contains ("Spinus Synth Kit"));
}
