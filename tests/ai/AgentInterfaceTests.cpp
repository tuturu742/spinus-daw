#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ai/AgentInterface.h"
#include "engine/BundledDevices.h"
#include "engine/DeviceFactory.h"
#include "engine/EngineHost.h"
#include "session/Session.h"
#include "ui/AgentInterfacePage.h"

#include <thread>

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

    static std::unique_ptr<engine::DeviceFactory> makeDevices (tracktion::Engine& engine)
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerBundledDevices (*devices, engine, {});
        return devices;
    }

    explicit Fixture (bool withHarness = true)
    {
        ai::AgentInterface::Host h;
        h.projectFolder = [this] { return folder.root.getChildFile ("Song"); };
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
        }
        agents = std::make_unique<ai::AgentInterface> (session, folder.root, std::move (h));
        agents->setPort (40000 + juce::Random::getSystemRandom().nextInt (20000));
    }

    struct Answer
    {
        int status = 0;
        juce::String session;
        juce::var json;
    };

    /** One request through handle(), as the server makes it, with the message loop run until it is answered. */
    Answer request (const juce::String& method, const juce::var& body, const juce::String& sessionId = {},
                    const juce::String& auth = "", const juce::String& origin = "")
    {
        juce::StringPairArray headers;
        headers.set ("authorization", auth.isNotEmpty() ? auth : "Bearer " + agents->getToken());
        if (sessionId.isNotEmpty())
            headers.set ("mcp-session-id", sessionId);
        if (origin.isNotEmpty())
            headers.set ("origin", origin);
        std::optional<ai::AgentInterface::HttpAnswer> got;
        agents->handle (method, headers, juce::JSON::toString (body),
                        [&] (const ai::AgentInterface::HttpAnswer& a) { got = a; });
        for (int i = 0; i < 2000 && ! got; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
        REQUIRE (got.has_value());
        const auto answer = got.value_or (ai::AgentInterface::HttpAnswer {});
        return { answer.status, answer.headers["Mcp-Session-Id"], juce::JSON::parse (answer.body) };
    }

    static juce::var rpc (const juce::String& method, const juce::var& params = juce::var (new juce::DynamicObject()))
    {
        return object ({ { "jsonrpc", "2.0" }, { "id", 1 }, { "method", method }, { "params", params } });
    }

    juce::String connect()
    {
        const auto answer =
            request ("POST", rpc ("initialize", object ({ { "clientInfo", object ({ { "name", "Test Agent" } }) } })));
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

    f.agents->setEnabled (true);
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
    for (auto* name :
         { "song_summary", "run_commands", "command_help", "list_devices", "render", "undo_last", "fake_tempo" })
        CHECK (names.contains (name));

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

TEST_CASE ("The live interface answers MCP over HTTP on 127.0.0.1", "[ai][agents]")
{
    Fixture f;
    f.agents->setEnabled (true);
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
