#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace spinus
{
class Session;
}

namespace spinus::engine
{
class ExportJob;
}

namespace spinus::ai
{

/** The live interface for outside coding agents (DESIGN.md §12.12): an MCP server in the running Spinus, so an
    agent such as Claude Code, Codex or Cursor can read the open project, change it and listen to it by numbers.

    Off until the user turns it on (Options > AI > Coding agents). It listens on 127.0.0.1 only and every request
    needs the token shown there ("Authorization: Bearer <token>"). MCP over HTTP (JSON-RPC, one JSON answer per
    request; an answer that carries a notification first is a short event stream).

    Owner decision 2026-10-29 (AGENTS rule 3's one exception): an agent's changes apply at once, without a
    proposal to approve, because an outside agent works in many small steps; turning the interface on is the
    consent. Each tool call that changes the project is one undo step ("Agent: ..."). While an agent is connected
    Spinus shows it, with Disconnect and Undo the agent's changes (back to the project as it was when it
    connected). Several agents may be connected at once, each in its own session.

    A change is refused once when the user (or another agent) changed the song since the agent last looked, with
    what they changed, so an agent doesn't undo the user's work without knowing (what_changed tells it at any
    time). Opening another project or starting a new one counts as such a change, and the undo point starts again.

    Its tools: Spinus's own (the song's summary, commands, devices, a render measured by numbers, undo, what the
    user changed, playback for the user to hear) always,
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
        /** Whether AI is on, so the music tools are offered (the instructions, prompts and tool list follow it).
            Unset: on when listHarnessTools is set. */
        std::function<bool()> musicToolsOn;
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

    bool isAgentConnected() const noexcept { return ! sessions.empty(); }
    /** The connected agents' names ("claude-code", or "claude-code and cursor"). */
    juce::String getAgentName() const;
    /** How many different agents are connected (by name). */
    int getAgentCount() const;
    /** How many changes the connected agents made. */
    int getAgentChanges() const noexcept { return changes; }
    /** Ends every agent's session. An agent's client may start a new one at once (MCP clients do after their
        session ends): only turning the interface off or a new token keeps an agent out. A new undo point starts
        when the next agent connects. */
    void disconnect();
    /** Undoes back to the project as it was when the first of the connected agents connected (your own changes
        since then too). */
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
    /** The MCP versions it speaks, newest first. */
    static juce::StringArray supportedProtocolVersions();
    /** Whether a request's Origin header is allowed: none, or this computer's own address (never a web page's). */
    static bool isLocalOrigin (const juce::String& origin);
    /** A request body sent in chunks (Transfer-Encoding: chunked), put together: false until it is complete (or
        when it is malformed, with `malformed` set). */
    static bool decodeChunked (const juce::MemoryBlock& data, juce::MemoryBlock& body, bool& malformed);

private:
    void run() override;
    void serve (juce::StreamingSocket&);
    void start();
    void stop();
    void load();
    void save() const;
    void changed();

    /** One agent's session: from its initialize to its DELETE, Disconnect, or the interface going off. Several
        agents (two terminals, or Claude Code and Cursor) each have their own. */
    struct AgentSession
    {
        juce::String id, name;
        juce::StringArray seenSteps;        // the undo steps when the agent last looked
        std::vector<uint64_t> seenStates;   // and the state each led to
        uint64_t seenState = 0;             // and the history's state then
        bool projectSwitched = false;       // another project was opened (or a new one started) since it looked
        std::set<uint64_t> own;             // the states its own changes led to
        std::optional<bool> toolsWithMusic; // whether its tool list has the music tools (as it was last told)
        juce::uint32 lastUsed = 0;
    };
    AgentSession* findSession (const juce::String& id);
    void endSession (const juce::String& id, const juce::String& why);
    bool musicToolsOn() const;
    /** Another project was opened or a new one started: every agent is told, and the undo point starts again. */
    void projectReplaced();

    void rpc (const juce::var& request, const juce::String& sessionId, const ToolReply& reply,
              juce::String& newSession);
    void callTool (const juce::String& sessionId, const juce::String& name, const juce::var& arguments,
                   const ToolReply& reply);
    juce::var nativeTools() const;
    /** Performs steps as one undo step of the agent's; an error text (empty when done). */
    juce::String apply (const juce::String& sessionId, const juce::var& steps, const juce::String& summary);
    juce::String checkPaths (const juce::var& value) const;
    void render (bool stems, const ToolReply& reply);
    /** Playback for the user to hear: bars ("9-16", a section's name; empty: from where it is), looped or not. An
        error text (empty when it plays). */
    juce::String play (const juce::String& bars, bool loop);

    /** What the agent has seen of the song: the undo steps when it last looked (connected, read the song or
        asked what changed). */
    void markSeen (AgentSession&);
    /** The changes since then, in words (empty when none): the user's undo steps, the steps they undid, another
        agent's steps, and another project opened. */
    juce::String userChanges (const AgentSession&) const;
    /** Whether a tool changes the project (Spinus's own by name; the harness's by its annotations). */
    bool changesProject (const juce::String& tool) const;

    Session& session;
    juce::File settingsFile;
    Host host;
    bool enabled = false;
    int port = defaultPort;
    juce::String token;
    std::atomic<bool> listening { false };
    juce::String problem;

    std::vector<AgentSession> sessions;
    std::vector<std::pair<juce::String, juce::String>> ended; // recently ended sessions, and why
    juce::uint32 requests = 0;
    int historyListener = 0;
    int changes = 0;
    int undoMark = 0;     // undo steps there were when the first agent connected
    juce::var lastRender; // the last render's numbers, for the harness's check
    std::vector<std::shared_ptr<std::unique_ptr<engine::ExportJob>>> renders; // running renders, ours to free
    juce::var harnessTools;

    std::mutex socketLock;
    std::unique_ptr<juce::StreamingSocket> listener;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};

} // namespace spinus::ai
