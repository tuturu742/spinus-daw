#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace spinus
{
class Session;
}

namespace spinus::ai
{

/** The live interface for outside coding agents (DESIGN.md §12.12): an MCP server in the running Spinus, so an
    agent such as Claude Code, Codex or Cursor can read the open project, change it and listen to it by numbers.

    Off until the user turns it on (Options > AI > Coding agents). It listens on 127.0.0.1 only and every request
    needs the token shown there ("Authorization: Bearer <token>"). MCP over HTTP (JSON-RPC, one JSON answer per
    request).

    Owner decision 2026-10-29 (AGENTS rule 3's one exception): an agent's changes apply at once, without a
    proposal to approve, because an outside agent works in many small steps; turning the interface on is the
    consent. Each tool call that changes the project is one undo step ("Agent: ..."). While an agent is connected
    Spinus shows it, with Disconnect and Undo the agent's changes (back to the project as it was when it
    connected).

    Its tools: Spinus's own (the song's summary, commands, devices, a render measured by numbers, undo) always,
    and the project harness's (song_overview, write_part, insert_bars, shape_sound, check...) through the local AI
    service when AI is on; neither needs an internet connection. Message thread for everything public; the
    server's own thread hands each request to the message thread and waits. */
class AgentInterface final : private juce::Thread
{
public:
    /** An answer from the AI service: its result, or an error. */
    using Reply = std::function<void (const juce::var& result, const juce::String& error)>;
    /** A tool's answer (MCP's {content, isError}). */
    using ToolReply = std::function<void (const juce::var&)>;

    /** What the interface needs from the rest of Spinus (so tests can stand in for the AI service). */
    struct Host
    {
        std::function<juce::File()> projectFolder;              // the open project's folder (empty when untitled)
        std::function<juce::Array<juce::File>()> sampleFolders; // the user's sample folders (Browser > Samples)
        std::function<juce::var()> context;                     // what the AI service is told (ProjectContext)
        /** The project harness through the AI service. `reply` gets the tools' list ([{name, description,
            inputSchema}]) or an error. */
        std::function<void (const Reply& reply)> listHarnessTools;
        /** Runs one harness tool on `context`; `reply` gets {text, isError, proposal} or an error. */
        std::function<void (const juce::String& tool, const juce::var& arguments, const juce::var& context,
                            const Reply& reply)>
            callHarnessTool;
    };

    /** `settingsFolder` keeps agent-interface.json (on or off, the port, the token). */
    AgentInterface (Session&, const juce::File& settingsFolder, Host);
    ~AgentInterface() override;

    bool isEnabled() const noexcept { return enabled; }
    /** Turning it on starts the server (and makes a token the first time); off stops it and disconnects. */
    void setEnabled (bool);
    int getPort() const noexcept { return port; }
    void setPort (int);
    juce::String getToken() const { return token; }
    /** A new token: agents set up with the old one can't connect any more. */
    void newToken();
    /** Listening, or why not (a port in use). */
    bool isListening() const noexcept { return listening; }
    juce::String getProblem() const { return problem; }

    /** How to set an agent up: "claude", "codex" or "cursor". */
    juce::String setupFor (const juce::String& agent) const;

    bool isAgentConnected() const noexcept { return connected; }
    juce::String getAgentName() const { return agentName; }
    /** How many changes the connected agent made. */
    int getAgentChanges() const noexcept { return changes; }
    /** Ends the agent's session; it has to connect again (and a new undo point starts then). */
    void disconnect();
    /** Undoes back to the project as it was when the agent connected (your own changes since then too). */
    void undoAgentChanges();

    /** Called on the message thread when the state above changes. */
    std::function<void()> onChange;

    /** One MCP request as the server gets it, for tests: the HTTP method, the headers (lower-case names), the
        body; the answer's status, headers and body. Message thread: the work it starts may finish later, so it
        returns through `reply`. */
    struct HttpAnswer
    {
        int status = 200;
        juce::StringPairArray headers;
        juce::String body;
    };
    void handle (const juce::String& method, const juce::StringPairArray& headers, const juce::String& body,
                 const std::function<void (const HttpAnswer&)>& reply);

    static constexpr int defaultPort = 47219;

private:
    void run() override;
    void serve (juce::StreamingSocket&);
    void start();
    void stop();
    void load();
    void save() const;
    void changed();

    void rpc (const juce::var& request, const ToolReply& reply, juce::String& newSession);
    void callTool (const juce::String& name, const juce::var& arguments, const ToolReply& reply);
    juce::var nativeTools() const;
    /** Performs steps as one undo step; an error text (empty when done). */
    juce::String apply (const juce::var& steps, const juce::String& summary);
    juce::String checkPaths (const juce::var& value) const;
    void render (bool stems, const ToolReply& reply);

    Session& session;
    juce::File settingsFile;
    Host host;
    bool enabled = false;
    int port = defaultPort;
    juce::String token;
    std::atomic<bool> listening { false };
    juce::String problem;

    juce::String sessionId, agentName;
    bool connected = false;
    int changes = 0;
    int undoMark = 0;     // undo steps there were when the agent connected
    juce::var lastRender; // the last render's numbers, for the harness's check
    juce::var harnessTools;

    std::mutex socketLock;
    std::unique_ptr<juce::StreamingSocket> listener;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};

} // namespace spinus::ai
