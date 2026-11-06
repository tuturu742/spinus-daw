#include "AgentInterface.h"

#include "ProjectContext.h"
#include "Proposal.h"
#include "commands/CommandRegistry.h"
#include "core/Log.h"
#include "dsp/loudness/LoudnessEngine.h"
#include "dsp/drums/StarterPack.h"
#include "model/ModelJson.h"
#include "model/StockDevices.h"
#include "session/AgentCommandLine.h"
#include "session/Session.h"

#include <SpinusVersion.h>

#include <juce_audio_formats/juce_audio_formats.h>

#if ! JUCE_WINDOWS
#include <sys/stat.h>
#endif

namespace spinus::ai
{
namespace
{
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> properties)
{
    auto* o = new juce::DynamicObject();
    for (auto& [name, value] : properties)
        o->setProperty (name, value);
    return juce::var (o);
}

juce::var schema (std::initializer_list<std::pair<const char*, juce::var>> properties,
                  const juce::StringArray& required = {})
{
    juce::Array<juce::var> names;
    for (auto& r : required)
        names.add (r);
    return object ({ { "type", "object" }, { "properties", object (properties) }, { "required", names } });
}

/** What a tool does to the project, for the agent's client (MCP's tool annotations): it may run one that only
    reads without asking, and ask before one that replaces or removes what is there. */
enum class Does
{
    reads,
    adds,
    replaces,
    plays // playback for the user: the song stays as it is
};

juce::var tool (const juce::String& name, const juce::String& description, const juce::var& inputSchema, Does does)
{
    auto hints = object ({ { "readOnlyHint", does == Does::reads }, { "openWorldHint", false } });
    if (does != Does::reads)
        hints.getDynamicObject()->setProperty ("destructiveHint", does == Does::replaces);
    if (does == Does::plays)
        hints.getDynamicObject()->setProperty ("idempotentHint", true);
    return object (
        { { "name", name }, { "description", description }, { "inputSchema", inputSchema }, { "annotations", hints } });
}

juce::var text (const juce::String& s, bool isError = false)
{
    juce::Array<juce::var> content;
    content.add (object ({ { "type", "text" }, { "text", s } }));
    return object ({ { "content", content }, { "isError", isError } });
}

juce::var rpcResult (const juce::var& id, const juce::var& result)
{
    return object ({ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } });
}

juce::var rpcError (const juce::var& id, int code, const juce::String& message)
{
    return object (
        { { "jsonrpc", "2.0" }, { "id", id }, { "error", object ({ { "code", code }, { "message", message } }) } });
}

juce::String newToken()
{
    juce::Random random;
    juce::String t;
    for (int i = 0; i < 32; ++i)
        t << juce::String::toHexString (random.nextInt (16));
    return t;
}

const juce::String protocolVersion = "2025-06-18";

/** Ready-made requests the agent's client offers (MCP prompts): each starts the agent on a common job in
    Spinus, with the steps that work well. */
struct Prompt
{
    const char* name;
    const char* title;
    const char* description;
    std::vector<std::pair<const char*, const char*>> arguments; // name, description; the first is required
    const char* text;                                           // {name} is replaced by the argument's value
};

const std::vector<Prompt>& prompts()
{
    static const std::vector<Prompt> all {
        { "start_song",
          "Start a song",
          "Build a song in the project open in Spinus from a description.",
          { { "description", "The song: genre, mood, tempo, instruments, length" } },
          "Make a song in the project open in Spinus: {description}.\n"
          "1. Call song_overview to see what is there already.\n"
          "2. Set the tempo and key with run_commands (project.setTempo, project.setKey) if the description "
          "gives them.\n"
          "3. Mark the sections (intro, verse, chorus...) with markers (marker.add), in 4- or 8-bar phrases.\n"
          "4. Add the parts with add_part (drums, bass, chords, a lead...), then write each with write_part, "
          "using loop_bars for parts that repeat.\n"
          "5. Call check and fix what matters, then render and look at each section's loudness.\n"
          "6. Tell me what you made, section by section." },
        { "make_beat",
          "Make a beat",
          "Write a drum part for the song open in Spinus.",
          { { "style", "The groove: genre, feel, tempo" }, { "bars", "Where it plays: '1-8' or a section's name" } },
          "Write a drum beat in the song open in Spinus: {style}, in bars {bars}.\n"
          "1. Call song_overview. Use the drum part if there is one; otherwise add one with add_part (the Drum "
          "Machine).\n"
          "2. Write the beat with write_part, using the drums' names for pitch and loop_bars for the loop, with a "
          "fill in the last bar of each section.\n"
          "3. Call check, then tell me what you wrote." },
        { "add_section",
          "Add a section",
          "Add a section (a bridge, a break, a solo...) to the song open in Spinus.",
          { { "section", "What to add, such as 'an 8-bar bridge'" },
            { "where", "Where, such as 'after the second chorus'" } },
          "Add {section} to the song open in Spinus, {where}.\n"
          "1. Call song_overview to find the sections and their bars.\n"
          "2. Make room with insert_bars (it moves everything after it and adds a marker).\n"
          "3. Fill it: copy_bars from a section to vary, or write_part for new parts, and automate a build into "
          "what follows.\n"
          "4. Call check and render, play the new section for me, and tell me what it does." },
        { "check_mix",
          "Check and fix the mix",
          "Measure the mix of the song open in Spinus and fix what is wrong, in small steps.",
          { { "goal", "Optional: a target, such as 'louder', 'clearer bass' or '-10 LUFS'" } },
          "Check the mix of the song open in Spinus. Goal: {goal}.\n"
          "1. Render with stems and call check: note the mix's loudness and true peak, each section's loudness and "
          "each part's.\n"
          "2. Fix one problem at a time (levels, a muddy low end, masking, ducking the bass under the kick), each "
          "as its own change so I can undo it.\n"
          "3. Render again and compare.\n"
          "4. Tell me each change with the numbers before and after." },
        { "shape_a_sound",
          "Shape a sound",
          "Change how one part of the song open in Spinus sounds.",
          { { "part", "The part, by its name" }, { "how", "How it should sound, such as 'pluckier and darker'" } },
          "Make the part \"{part}\" in the song open in Spinus sound {how}.\n"
          "1. Call sound_settings for the part.\n"
          "2. Use shape_sound for the words it knows, or set_sound for a preset or settings by name.\n"
          "3. Add an effect with add_effect if the instrument alone can't do it.\n"
          "4. Tell me which settings changed." },
    };
    return all;
}

juce::var promptList()
{
    juce::Array<juce::var> list;
    for (auto& p : prompts())
    {
        juce::Array<juce::var> arguments;
        for (size_t i = 0; i < p.arguments.size(); ++i)
            arguments.add (object ({ { "name", p.arguments[i].first },
                                     { "description", p.arguments[i].second },
                                     { "required", i == 0 && juce::String (p.name) != "check_mix" } }));
        list.add (object ({ { "name", p.name },
                            { "title", p.title },
                            { "description", p.description },
                            { "arguments", arguments } }));
    }
    return list;
}

/** The prompt's message with the arguments filled in (void for an unknown prompt). */
juce::var promptMessages (const juce::String& name, const juce::var& arguments)
{
    for (auto& p : prompts())
    {
        if (name != p.name)
            continue;
        juce::String said (p.text);
        for (auto& [argument, _] : p.arguments)
        {
            auto value = arguments[argument].toString().trim();
            if (value.isEmpty())
                value = juce::String (argument) == "goal" ? "a clean, balanced mix" : "as you think best";
            said = said.replace ("{" + juce::String (argument) + "}", value);
        }
        juce::Array<juce::var> messages;
        messages.add (
            object ({ { "role", "user" }, { "content", object ({ { "type", "text" }, { "text", said } }) } }));
        return messages;
    }
    return {};
}

const juce::String instructions =
    "You are working on the song open in Spinus, a digital audio workstation (FL Studio-like: Channel Rack, Piano "
    "Roll, Playlist of clips, Mixer). Start with song_overview (or song_summary). Edit with the music tools "
    "(write_part, add_part, insert_bars, automate, add_effect, send, duck, set_sound, shape_sound) or with "
    "run_commands (Spinus commands; command_help gives their parameters). Each change applies at once as one undo "
    "step the user can undo. The user may edit the song while you work: what_changed tells you what they did, and a "
    "change of yours is refused once when they changed the song since you last looked. play lets the user hear a "
    "section (you don't hear it). search_manual answers how a device, command or feature works; reference_style "
    "gives the style of the user's reference songs (from the Track creator). Check your work with check, and render to "
    "measure the loudness of the mix, each "
    "section and each part. The user hears the song in Spinus: tell them what you changed.\n\n"
    "Conventions. The music tools: bars count from 1 ('9-16', or a section's name from the Playlist's markers); "
    "notes are 'step pitch length [velocity]' with steps of a sixteenth from the first bar's start (16 a bar in "
    "4/4); pitches are MIDI numbers or names with C5 = 60 (FL Studio's octaves); velocity is 1-127. Spinus's "
    "commands: times are in ticks, 960 a quarter note, from the song's start at 0; channels, patterns, inserts "
    "and devices are named by the ids song_summary shows. Settings and automation take text in the units the "
    "device shows ('-6 dB', '220 ms', '1.2 kHz', '30% L', a choice by name); a plain number in "
    "device.setParameters' values is 0 to 1 over the setting's whole range. A part's volume is its fader in dB.";

/** A rendered file's loudness from `from` to `to` seconds (integrated LUFS; -100 when silent). */
double loudnessOf (juce::AudioFormatReader& reader, double from, double to)
{
    dsp::LoudnessEngine meter;
    meter.prepare (reader.sampleRate);
    const int block = juce::jmax (1, (int)std::round (reader.sampleRate / 10.0));
    juce::AudioBuffer<float> buffer ((int)juce::jmin (2u, reader.numChannels), block);
    const auto start = (juce::int64)(from * reader.sampleRate);
    const auto end = juce::jmin (reader.lengthInSamples, (juce::int64)(to * reader.sampleRate));
    for (auto at = start; at < end; at += block)
    {
        const auto n = (int)juce::jmin ((juce::int64)block, end - at);
        buffer.setSize (buffer.getNumChannels(), n, false, false, true);
        reader.read (&buffer, 0, n, at, true, true);
        meter.process (buffer);
    }
    const auto v = meter.getIntegrated();
    return std::isfinite (v) ? std::round (v * 10.0) / 10.0 : -100.0;
}
} // namespace

//==============================================================================
AgentInterface::AgentInterface (Session& s, const juce::File& settingsFolder, Host h)
    : juce::Thread ("Spinus agent interface"), session (s),
      settingsFile (settingsFolder.getChildFile ("agent-interface.json")), host (std::move (h))
{
    load();
    if (enabled)
        start();
}

AgentInterface::~AgentInterface()
{
    *alive = false;
    stop();
}

void AgentInterface::load()
{
    const auto json = juce::JSON::parse (settingsFile.loadFileAsString());
    enabled = (bool)json["enabled"];
    port = juce::jlimit (1024, 65535, json.hasProperty ("port") ? (int)json["port"] : defaultPort);
    token = json["token"].toString();
}

void AgentInterface::save() const
{
    settingsFile.getParentDirectory().createDirectory();
    settingsFile.replaceWithText (
        juce::JSON::toString (object ({ { "enabled", enabled }, { "port", port }, { "token", token } })));
    // Only this user may read the token.
#if ! JUCE_WINDOWS
    ::chmod (settingsFile.getFullPathName().toRawUTF8(), 0600);
#endif
}

void AgentInterface::changed()
{
    if (onChange)
        onChange();
}

void AgentInterface::setEnabled (bool on)
{
    if (on == enabled)
        return;
    enabled = on;
    if (enabled && token.isEmpty())
        token = ai::newToken();
    save();
    if (enabled)
        start();
    else
    {
        stop();
        disconnect();
    }
    changed();
}

void AgentInterface::setPort (int p)
{
    p = juce::jlimit (1024, 65535, p);
    if (p == port)
        return;
    port = p;
    save();
    if (enabled)
    {
        stop();
        start();
    }
    changed();
}

void AgentInterface::newToken()
{
    token = ai::newToken();
    save();
    disconnect();
    changed();
}

juce::String AgentInterface::setupFor (const juce::String& agent) const
{
    const auto url = "http://127.0.0.1:" + juce::String (port) + "/mcp";
    if (agent == "codex")
        return "[mcp_servers.spinus]\nurl = \"" + url + "\"\nhttp_headers = { Authorization = \"Bearer " + token +
               "\" }";
    if (agent == "cursor")
        return "{\"mcpServers\": {\"spinus\": {\"url\": \"" + url + "\", \"headers\": {\"Authorization\": \"Bearer " +
               token + "\"}}}}";
    return "claude mcp add --transport http spinus " + url + " --header \"Authorization: Bearer " + token + "\"";
}

void AgentInterface::disconnect()
{
    const bool was = connected;
    sessionId = {};
    connected = false;
    agentName = {};
    changes = 0;
    if (was)
    {
        core::Logs::info ("agents", "The agent was disconnected");
        changed();
    }
}

void AgentInterface::undoAgentChanges()
{
    auto& history = session.getHistory();
    int undone = 0;
    while (history.getUndoDescriptions().size() > undoMark && history.undo())
        ++undone;
    changes = 0;
    core::Logs::info ("agents", "Undid the agent's changes (" + juce::String (undone) + " undo steps)");
    changed();
}

//==============================================================================
// The server: 127.0.0.1 only, one request at a time, each answered on the message thread.

void AgentInterface::start()
{
    std::scoped_lock lock (socketLock);
    listener = std::make_unique<juce::StreamingSocket>();
    if (! listener->createListener (port, "127.0.0.1"))
    {
        listener = nullptr;
        listening = false;
        problem = "Port " + juce::String (port) + " is in use; choose another.";
        core::Logs::warning ("agents", problem);
        return;
    }
    problem = {};
    listening = true;
    core::Logs::info ("agents", "The live interface for coding agents is on, at 127.0.0.1:" + juce::String (port));
    startThread();
}

void AgentInterface::stop()
{
    signalThreadShouldExit();
    {
        std::scoped_lock lock (socketLock);
        if (listener != nullptr)
            listener->close();
    }
    stopThread (5000);
    std::scoped_lock lock (socketLock);
    listener = nullptr;
    listening = false;
}

void AgentInterface::run()
{
    while (! threadShouldExit())
    {
        juce::StreamingSocket* next = nullptr;
        {
            std::scoped_lock lock (socketLock);
            if (listener == nullptr)
                return;
        }
        next = listener->waitForNextConnection();
        if (next == nullptr)
            continue;
        std::unique_ptr<juce::StreamingSocket> client (next);
        serve (*client);
    }
}

void AgentInterface::serve (juce::StreamingSocket& client)
{
    // The request: its line, headers (names lower-cased) and body.
    juce::MemoryBlock data;
    char buffer[8192];
    int headerEnd = -1;
    while (headerEnd < 0 && data.getSize() < 65536 && ! threadShouldExit())
    {
        if (client.waitUntilReady (true, 10000) != 1)
            return;
        const auto n = client.read (buffer, (int)sizeof (buffer), false);
        if (n <= 0)
            return;
        data.append (buffer, (size_t)n);
        headerEnd = data.toString().indexOf ("\r\n\r\n");
    }
    if (headerEnd < 0)
        return;
    const auto all = data.toString();
    auto lines = juce::StringArray::fromLines (all.substring (0, headerEnd));
    const auto method = lines[0].upToFirstOccurrenceOf (" ", false, false);
    const auto path = lines[0].fromFirstOccurrenceOf (" ", false, false).upToFirstOccurrenceOf (" ", false, false);
    juce::StringPairArray headers;
    for (int i = 1; i < lines.size(); ++i)
        headers.set (lines[i].upToFirstOccurrenceOf (":", false, false).trim().toLowerCase(),
                     lines[i].fromFirstOccurrenceOf (":", false, false).trim());
    const auto length = headers["content-length"].getIntValue();
    if (length < 0 || length > 16 * 1024 * 1024)
        return;
    const auto bodyStart = (size_t)headerEnd + 4;
    juce::MemoryBlock body (data.begin() + juce::jmin (bodyStart, data.getSize()),
                            data.getSize() > bodyStart ? data.getSize() - bodyStart : 0);
    while ((int)body.getSize() < length && ! threadShouldExit())
    {
        if (client.waitUntilReady (true, 10000) != 1)
            return;
        const auto n =
            client.read (buffer, (int)juce::jmin ((size_t)sizeof (buffer), (size_t)length - body.getSize()), false);
        if (n <= 0)
            return;
        body.append (buffer, (size_t)n);
    }

    HttpAnswer answer;
    if (path.upToFirstOccurrenceOf ("?", false, false) != "/mcp")
    {
        answer.status = 404;
        answer.body = "Not found: the MCP endpoint is /mcp";
    }
    else
    {
        // Answered on the message thread (where the project lives); this thread waits.
        struct Pending
        {
            juce::WaitableEvent done;
            HttpAnswer answer;
        };
        auto pending = std::make_shared<Pending>();
        juce::MessageManager::callAsync (
            [this, pending, method, headers, text = body.toString(), stillAlive = alive]
            {
                if (! *stillAlive)
                {
                    pending->done.signal();
                    return;
                }
                handle (method, headers, text,
                        [pending] (const HttpAnswer& a)
                        {
                            pending->answer = a;
                            pending->done.signal();
                        });
            });
        // A render or a slow plugin may take minutes; give up after 15.
        for (int waited = 0; ! pending->done.wait (100); waited += 100)
            if (threadShouldExit() || waited > 15 * 60 * 1000)
                return;
        answer = pending->answer;
    }

    const juce::String reason = answer.status == 200   ? "OK"
                                : answer.status == 202 ? "Accepted"
                                : answer.status == 400 ? "Bad Request"
                                : answer.status == 401 ? "Unauthorized"
                                : answer.status == 403 ? "Forbidden"
                                : answer.status == 404 ? "Not Found"
                                : answer.status == 405 ? "Method Not Allowed"
                                                       : "Error";
    const auto bodyText = answer.body.toStdString();
    juce::String response = "HTTP/1.1 " + juce::String (answer.status) + " " + reason + "\r\n";
    if (! answer.headers.containsKey ("Content-Type"))
        answer.headers.set ("Content-Type", "application/json");
    for (auto& key : answer.headers.getAllKeys())
        response << key << ": " << answer.headers[key] << "\r\n";
    response << "Content-Length: " << (int)bodyText.size() << "\r\nConnection: close\r\n\r\n";
    const auto head = response.toStdString();
    client.write (head.data(), (int)head.size());
    if (! bodyText.empty())
        client.write (bodyText.data(), (int)bodyText.size());
}

//==============================================================================
void AgentInterface::handle (const juce::String& method, const juce::StringPairArray& headers, const juce::String& body,
                             const std::function<void (const HttpAnswer&)>& reply)
{
    HttpAnswer answer;
    const auto fail = [&] (int status, const juce::String& why)
    {
        answer.status = status;
        answer.body = juce::JSON::toString (rpcError ({}, -32000, why));
        reply (answer);
    };
    if (! enabled)
        return fail (403, "Spinus's live interface for coding agents is off");
    // A web page can't reach it: only requests from this computer's own programs, never a browser's.
    if (const auto& origin = headers["origin"];
        origin.isNotEmpty() && ! origin.startsWith ("http://127.0.0.1") && ! origin.startsWith ("http://localhost"))
        return fail (403, "requests from web pages are refused");
    if (headers["authorization"] != "Bearer " + token || token.isEmpty())
        return fail (401, "the token is missing or wrong (Options > AI > Coding agents shows it)");

    const auto& given = headers["mcp-session-id"];
    if (method == "DELETE")
    {
        if (given.isNotEmpty() && given == sessionId)
            disconnect();
        answer.body = {};
        return reply (answer);
    }
    if (method != "POST")
    {
        answer.status = 405;
        answer.headers.set ("Allow", "POST, DELETE");
        answer.body = {};
        return reply (answer);
    }

    const auto request = juce::JSON::parse (body);
    if (! request.isObject())
    {
        answer.status = 400;
        answer.body = juce::JSON::toString (rpcError ({}, -32700, "not JSON"));
        return reply (answer);
    }
    const auto rpcMethod = request["method"].toString();
    if (rpcMethod != "initialize")
    {
        if (given.isEmpty())
            return fail (400, "no Mcp-Session-Id: initialize first");
        if (given != sessionId)
            return fail (404, "this session ended (disconnected in Spinus): initialize again");
    }
    if (! request.hasProperty ("id")) // a notification: nothing to answer
    {
        answer.status = 202;
        answer.body = {};
        return reply (answer);
    }

    juce::String newSession;
    if (rpcMethod == "initialize") // answered at once, with the new session's ID
    {
        juce::var result;
        rpc (request, [&result] (const juce::var& r) { result = r; }, newSession);
        answer.body = juce::JSON::toString (result, true);
        answer.headers.set ("Mcp-Session-Id", newSession);
        return reply (answer);
    }
    rpc (
        request,
        [reply, answer] (const juce::var& result) mutable
        {
            answer.body = juce::JSON::toString (result, true);
            reply (answer);
        },
        newSession);
}

void AgentInterface::rpc (const juce::var& request, const ToolReply& reply, juce::String& newSession)
{
    const auto& id = request["id"];
    const auto method = request["method"].toString();
    const auto& params = request["params"];

    if (method == "initialize")
    {
        sessionId = juce::Uuid().toString();
        newSession = sessionId;
        agentName = params["clientInfo"]["name"].toString();
        if (agentName.isEmpty())
            agentName = "An agent";
        connected = true;
        changes = 0;
        undoMark = session.getHistory().getUndoDescriptions().size();
        harnessTools = juce::var();
        markSeen();
        core::Logs::info ("agents", agentName + " connected");
        changed();
        const auto asked = params["protocolVersion"].toString();
        reply (rpcResult (
            id, object ({ { "protocolVersion", asked.isNotEmpty() ? asked : protocolVersion },
                          { "capabilities", object ({ { "tools", object ({ { "listChanged", false } }) },
                                                      { "prompts", object ({ { "listChanged", false } }) } }) },
                          { "serverInfo", object ({ { "name", "spinus" }, { "version", versionString } }) },
                          { "instructions", instructions } })));
        return;
    }
    if (method == "ping")
        return reply (rpcResult (id, juce::var (new juce::DynamicObject())));
    if (method == "tools/list")
    {
        auto tools = nativeTools();
        if (! host.listHarnessTools)
            return reply (rpcResult (id, object ({ { "tools", tools } })));
        auto stillAlive = alive;
        host.listHarnessTools (
            [this, reply, id, tools, stillAlive] (const juce::var& listed, const juce::String& error) mutable
            {
                if (! *stillAlive)
                    return;
                if (error.isEmpty() && listed.isArray())
                {
                    harnessTools = listed;
                    for (auto& t : *listed.getArray())
                        tools.getArray()->add (t);
                }
                reply (rpcResult (id, object ({ { "tools", tools } })));
            });
        return;
    }
    if (method == "prompts/list")
        return reply (rpcResult (id, object ({ { "prompts", promptList() } })));
    if (method == "prompts/get")
    {
        const auto messages = promptMessages (params["name"].toString(), params["arguments"]);
        if (messages.isVoid())
            return reply (rpcError (id, -32602, "unknown prompt " + params["name"].toString()));
        return reply (rpcResult (id, object ({ { "messages", messages } })));
    }
    if (method == "tools/call")
    {
        callTool (params["name"].toString(), params["arguments"],
                  [reply, id] (const juce::var& result) { reply (rpcResult (id, result)); });
        return;
    }
    reply (rpcError (id, -32601, "unknown method " + method));
}

juce::var AgentInterface::nativeTools() const
{
    juce::Array<juce::var> tools;
    tools.add (tool ("song_summary",
                     "The open project as Spinus sees it: tempo, key, length, channels (instruments, kits), mixer "
                     "inserts (effects, sends, sidechains), patterns, Playlist lanes, clips and markers, and what is "
                     "missing on this computer.",
                     schema ({}), Does::reads));
    tools.add (tool (
        "run_commands",
        "Change the project with Spinus commands, performed in order as one undo step. Each step "
        "{command, parameters}; a parameter can name an earlier step's result as \"$<step>.<field>\" "
        "(\"$1.channel\"). Automation points may give 'text' (\"-12 dB\") instead of 'value'. Use "
        "command_help for a command's parameters.",
        schema ({ { "summary", object ({ { "type", "string" } }) }, { "steps", object ({ { "type", "array" } }) } },
                { "steps" }),
        Does::replaces));
    tools.add (tool ("command_help", "A Spinus command's parameters (JSON Schema), or the list of commands.",
                     schema ({ { "command", object ({ { "type", "string" } }) } }), Does::reads));
    tools.add (tool (
        "list_devices",
        "Instruments and effects that can be added (Spinus's own, the free plugins, installed plugins), "
        "by words in their names, each with the device object channel.add and insert.addEffect take.",
        schema ({ { "query", object ({ { "type", "string" } }) }, { "type", object ({ { "type", "string" } }) } }),
        Does::reads));
    tools.add (tool ("render",
                     "Render the song offline and measure it: the mix's loudness (LUFS) and true peak, each "
                     "section's loudness (between the Playlist's markers), and with stems each mixer insert's "
                     "loudness and how long it sounds. Takes as long as the song does to render.",
                     schema ({ { "stems", object ({ { "type", "boolean" } }) } }), Does::reads));
    tools.add (tool (
        "list_presets",
        "The presets device.loadPreset can load on a device of the song (an instrument or effect): "
        "3x Osc's, a plugin's own (its programs, the presets it lists, Surge XT's patches) and the "
        "sounds the user saved for it. 'find' keeps those whose name or category has all its words.",
        schema ({ { "device", object ({ { "type", "integer" } }) }, { "find", object ({ { "type", "string" } }) } },
                { "device" }),
        Does::reads));
    tools.add (tool ("undo_last", "Undo your last change (only if it is still the newest undo step).", schema ({}),
                     Does::replaces));
    tools.add (tool ("what_changed",
                     "What the user changed in Spinus since you last looked (connected, song_summary, song_overview "
                     "or this): their undo steps, and the steps they undid (yours too). A change of yours is refused "
                     "once when the user changed the song since your last look, with what they changed.",
                     schema ({}), Does::reads));
    tools.add (tool (
        "play",
        "Play the song for the user to hear (they listen in Spinus; you don't hear it): 'bars' as '9' or "
        "'9-16' (from 1) or a section's name from the Playlist's markers, looped unless loop is false. "
        "Without bars, plays from where the playhead is. It changes nothing in the song.",
        schema ({ { "bars", object ({ { "type", "string" } }) }, { "loop", object ({ { "type", "boolean" } }) } }),
        Does::plays));
    tools.add (tool ("stop", "Stop playback (the playhead goes back to where it started).", schema ({}), Does::plays));
    return tools;
}

void AgentInterface::callTool (const juce::String& name, const juce::var& arguments, const ToolReply& reply)
{
    if (name == "song_summary")
    {
        markSeen();
        return reply (text (
            juce::JSON::toString (cli::summarize (session, host.projectFolder ? host.projectFolder() : juce::File()))));
    }
    if (name == "what_changed")
    {
        const auto changes = userChanges();
        markSeen();
        return reply (text (changes.isNotEmpty() ? changes : "Nothing: the song is as you last saw it."));
    }
    if (name == "play")
    {
        const auto error =
            play (arguments["bars"].toString(), arguments.hasProperty ("loop") ? (bool)arguments["loop"] : true);
        return reply (error.isEmpty() ? text ("Playing (" + session.getPositionText() + ").") : text (error, true));
    }
    if (name == "stop")
    {
        session.stop();
        return reply (text ("Stopped."));
    }
    // A change after the user changed the song: refused once, with what they changed (now seen).
    if (name != "undo_last" && changesProject (name))
        if (const auto changes = userChanges(); changes.isNotEmpty())
        {
            markSeen();
            return reply (text ("Not done: the user changed the song since you last looked. " + changes +
                                    " Check that your change still fits (song_summary or song_overview), then "
                                    "call again.",
                                true));
        }
    if (name == "command_help")
    {
        const auto wanted = arguments["command"].toString();
        if (const auto* info = commands::CommandRegistry::get().find (wanted))
            return reply (text (juce::JSON::toString (object ({ { "command", info->name },
                                                                { "summary", info->summary },
                                                                { "parameters", info->parameterSchema } }))));
        juce::StringArray names;
        for (auto& c : commands::CommandRegistry::get().getCommands())
            names.add (c.name + ": " + c.summary);
        return reply (text ((wanted.isEmpty() ? "" : "Unknown command \"" + wanted + "\". ") + "Commands:\n" +
                                names.joinIntoString ("\n"),
                            wanted.isNotEmpty()));
    }
    if (name == "list_devices")
    {
        const auto words = juce::StringArray::fromTokens (arguments["query"].toString().toLowerCase(), true);
        const auto kind = arguments["type"].toString();
        juce::Array<juce::var> found;
        for (auto& entry : session.getDeviceList().getEntries())
        {
            if ((kind == "instrument" && ! entry.instrument) || (kind == "effect" && entry.instrument))
                continue;
            const auto hay = (entry.name + " " + entry.maker).toLowerCase();
            bool all = true;
            for (auto& w : words)
                all = all && hay.contains (w);
            if (! all)
                continue;
            auto device = model::deviceToJson (entry.device);
            if (auto* o = device.getDynamicObject())
            {
                o->removeProperty ("id");
                o->removeProperty ("bypassed");
            }
            found.add (object ({ { "name", entry.name },
                                 { "type", juce::String (entry.instrument ? "instrument" : "effect") },
                                 { "available", entry.available },
                                 { "device", device } }));
            if (found.size() >= 40)
                break;
        }
        return reply (text (juce::JSON::toString (found)));
    }
    if (name == "list_presets")
    {
        const auto found = cli::presets (session, (int)arguments["device"], arguments["find"].toString());
        if (found.size() == 0)
            return reply (text ("There is no device " + arguments["device"].toString() + " in the song.", true));
        const auto& entry = found[0];
        if (auto* list = entry["presets"].getArray(); list != nullptr && list->size() > 300)
        {
            const auto all = list->size();
            list->removeRange (300, all - 300);
            entry.getDynamicObject()->setProperty ("more", "the first 300 of " + juce::String (all) +
                                                               ": give 'find' words to narrow them");
        }
        return reply (text (juce::JSON::toString (entry)));
    }
    if (name == "run_commands")
    {
        const auto error = apply (arguments["steps"], arguments["summary"].toString());
        return reply (error.isEmpty() ? text ("Done, as one undo step.") : text ("Not done: " + error, true));
    }
    if (name == "undo_last")
    {
        auto& history = session.getHistory();
        const auto steps = history.getUndoDescriptions();
        if (steps.isEmpty() || ! steps[steps.size() - 1].startsWith ("Agent: ") || steps.size() <= undoMark)
            return reply (text ("The newest undo step is not yours: nothing undone.", true));
        history.undo();
        changes = juce::jmax (0, changes - 1);
        changed();
        return reply (text ("Undone: " + steps[steps.size() - 1].fromFirstOccurrenceOf ("Agent: ", false, false)));
    }
    if (name == "render")
        return render ((bool)arguments["stems"], reply);

    // The project harness's tools, through the local AI service.
    if (! host.callHarnessTool)
        return reply (text ("\"" + name +
                                "\" isn't available: the music tools need AI turned on in Spinus "
                                "(Options > AI). Spinus's own tools work without it.",
                            true));
    auto context = host.context ? host.context() : juce::var();
    if (auto* o = context.getDynamicObject(); o != nullptr && lastRender.isObject())
        o->setProperty ("measured", lastRender);
    auto stillAlive = alive;
    host.callHarnessTool (name, arguments, context,
                          [this, reply, name, stillAlive] (const juce::var& result, const juce::String& error)
                          {
                              if (! *stillAlive)
                                  return;
                              if (error.isNotEmpty())
                                  return reply (text (error, true));
                              auto said = result["text"].toString();
                              if ((bool)result["isError"])
                                  return reply (text (said, true));
                              if (name == "song_overview")
                                  markSeen();
                              const auto& proposal = result["proposal"];
                              if (proposal.isObject())
                              {
                                  const auto failure = apply (proposal["steps"], proposal["summary"].toString());
                                  if (failure.isNotEmpty())
                                      return reply (text (said + "\nBut Spinus could not do it: " + failure, true));
                                  said << "\n(Done, as one undo step.)";
                              }
                              reply (text (said));
                          });
}

juce::String AgentInterface::checkPaths (const juce::var& value) const
{
    // Files a step names must be in the project's folder, the user's sample folders or Spinus's starter pack.
    if (auto* o = value.getDynamicObject())
    {
        for (auto& p : o->getProperties())
        {
            const auto key = p.name.toString();
            if (key == "file" || key == "files" || key == "path" || key == "folder" || key == "kit")
            {
                juce::Array<juce::var> named;
                if (p.value.isArray())
                    named = *p.value.getArray();
                else
                    named.add (p.value);
                for (auto& n : named)
                {
                    const auto path = n.toString();
                    // Relative: the project's own audio, never climbing out of it. A path that starts at a root
                    // ("/etc/x" is relative to the drive on Windows) is checked as absolute.
                    const auto rooted = path.startsWithChar ('/') || path.startsWithChar ('\\');
                    if (! juce::File::isAbsolutePath (path) && ! rooted)
                    {
                        if (path.replaceCharacter ('\\', '/').contains ("../") || path.endsWith (".."))
                            return "\"" + path + "\" climbs out of the project";
                        continue;
                    }
                    if (! juce::File::isAbsolutePath (path))
                        return "\"" + path + "\" is outside the project and your sample folders";
                    const juce::File file (path);
                    juce::Array<juce::File> allowed;
                    if (host.projectFolder)
                        allowed.add (host.projectFolder());
                    if (host.sampleFolders)
                        allowed.addArray (host.sampleFolders());
                    allowed.add (dsp::starter::findPack());
                    bool inside = false;
                    for (auto& folder : allowed)
                        inside = inside || (folder != juce::File() && file.isAChildOf (folder));
                    if (! inside)
                        return "\"" + path + "\" is outside the project and your sample folders";
                }
            }
            else if (auto e = checkPaths (p.value); e.isNotEmpty())
                return e;
        }
    }
    else if (auto* a = value.getArray())
        for (auto& item : *a)
            if (auto e = checkPaths (item); e.isNotEmpty())
                return e;
    return {};
}

juce::String AgentInterface::apply (const juce::var& steps, const juce::String& summary)
{
    if (! steps.isArray() || steps.getArray()->isEmpty())
        return "no steps";
    if (auto e = checkPaths (steps); e.isNotEmpty())
        return e;
    // {command, parameters} or {command, params}; actions (loadKit, loadSamples) are --run's, not here.
    juce::Array<juce::var> normalised;
    for (auto& step : *steps.getArray())
    {
        if (step.hasProperty ("action"))
            return "\"action\" steps are for spinus --run; here use commands (device.loadPreset for presets)";
        normalised.add (
            object ({ { "command", step["command"] },
                      { "parameters", step.hasProperty ("parameters") ? step["parameters"] : step["params"] } }));
    }
    juce::String textError;
    const auto read = cli::readStepTexts (session, normalised, textError);
    if (textError.isNotEmpty())
        return textError;
    const auto label = summary.isNotEmpty() ? summary : juce::String ("changes");
    auto proposal = Proposal::prepare (object ({ { "summary", label }, { "steps", read } }), session.getHistory());
    if (! proposal.canApply())
        return proposal.error.isNotEmpty() ? proposal.error : juce::String ("nothing to do");
    proposal.undoName = "Agent: " + label;
    if (const auto done = proposal.apply (session.getHistory()); done.failed())
        return done.getErrorMessage();
    ++changes;
    markSeen();               // the agent knows the song as its own change left it
    lastRender = juce::var(); // the song changed: the last render's numbers no longer hold
    core::Logs::info ("agents", agentName + ": " + label);
    changed();
    return {};
}

void AgentInterface::markSeen()
{
    seenSteps = session.getHistory().getUndoDescriptions();
    seenState = session.getHistory().getStateId();
}

juce::String AgentInterface::userChanges() const
{
    const auto& history = session.getHistory();
    if (history.getStateId() == seenState)
        return {};
    const auto now = history.getUndoDescriptions();
    int same = 0;
    while (same < now.size() && same < seenSteps.size() && now[same] == seenSteps[same])
        ++same;
    const auto list = [] (const juce::StringArray& names)
    {
        juce::StringArray shown;
        for (int i = 0; i < juce::jmin (names.size(), 20); ++i)
            shown.add ("\"" + names[i] + "\"");
        if (names.size() > 20)
            shown.add ("and " + juce::String (names.size() - 20) + " more");
        return shown.joinIntoString ("; ");
    };
    juce::StringArray made, undone;
    for (int i = same; i < now.size(); ++i)
        if (! now[i].startsWith ("Agent: "))
            made.add (now[i]);
    for (int i = seenSteps.size(); --i >= same;)
        undone.add (seenSteps[i].startsWith ("Agent: ")
                        ? "yours: " + seenSteps[i].fromFirstOccurrenceOf ("Agent: ", false, false)
                        : seenSteps[i]);
    juce::String said;
    if (! made.isEmpty())
        said << "The user made " << made.size() << (made.size() == 1 ? " change: " : " changes: ") << list (made)
             << ".";
    if (! undone.isEmpty())
        said << (said.isEmpty() ? "" : " ") << "The user undid " << undone.size()
             << (undone.size() == 1 ? " step: " : " steps: ") << list (undone) << ".";
    return said;
}

bool AgentInterface::changesProject (const juce::String& tool) const
{
    if (tool == "run_commands" || tool == "undo_last")
        return true;
    if (auto* listed = harnessTools.getArray())
        for (auto& t : *listed)
            if (t["name"].toString() == tool)
                return ! (bool)t["annotations"]["readOnlyHint"];
    return false; // Spinus's other tools only read; an unknown tool fails on its own
}

juce::String AgentInterface::play (const juce::String& bars, bool loop)
{
    const auto& project = session.getProject();
    const auto barTicks = (model::Ticks)project.settings.timeSignature.barTicks();
    model::Ticks end = 0;
    for (auto& clip : project.playlist.clips)
        end = std::max (end, clip.start + clip.length);

    std::optional<Session::LoopRegion> range;
    const auto wanted = bars.trim();
    if (wanted.isNotEmpty())
    {
        auto markers = project.playlist.markers;
        std::sort (markers.begin(), markers.end(), [] (auto& a, auto& b) { return a.at < b.at; });
        for (size_t i = 0; i < markers.size() && ! range; ++i)
            if (markers[i].name.equalsIgnoreCase (wanted))
                range = Session::LoopRegion { markers[i].at, i + 1 < markers.size() ? markers[i + 1].at : end };
        if (! range && wanted.containsOnly ("0123456789- "))
        {
            const auto from = wanted.upToFirstOccurrenceOf ("-", false, false).trim().getIntValue();
            const auto to =
                wanted.contains ("-") ? wanted.fromFirstOccurrenceOf ("-", false, false).trim().getIntValue() : from;
            if (from >= 1 && to >= from)
                range = Session::LoopRegion { (from - 1) * barTicks, to * barTicks };
        }
        if (! range || range->end <= range->start)
            return "\"" + wanted +
                   "\" isn't bars ('9' or '9-16', from 1) or a section's name from the Playlist's markers";
    }

    session.stop();
    session.setMode (Session::Mode::song);
    if (range)
    {
        if (loop)
            session.setLoopRegion (Session::Mode::song, range);
        else if (auto current = session.getLoopRegion (Session::Mode::song);
                 current && (range->start < current->start || range->start >= current->end))
            session.setLoopRegion (Session::Mode::song, std::nullopt); // it would hold the playhead elsewhere
        const auto seconds = (double)range->start * 60.0 / juce::jmax (1.0, (double)project.settings.tempoBpm) /
                             model::ticksPerQuarterNote;
        session.getEdit().getTransport().setPosition (tracktion::TimePosition::fromSeconds (seconds));
    }
    session.play();
    return {};
}

void AgentInterface::render (bool stems, const ToolReply& reply)
{
    const auto scratch = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("spinus-agent-render-" + juce::Uuid().toString());
    scratch.createDirectory();
    engine::ExportSettings settings;
    settings.format = engine::ExportSettings::Format::wav24;
    settings.stems = stems;
    settings.tailSeconds = 2.0;
    if (host.projectFolder && host.projectFolder() != juce::File())
        settings.audioFolder = host.projectFolder().getChildFile ("audio");

    // Section times, from the markers, in seconds (the tempo is constant).
    const auto& project = session.getProject();
    const auto secondsPerTick = 60.0 / juce::jmax (1.0, (double)project.settings.tempoBpm) / 960.0;
    std::vector<std::pair<juce::String, double>> marks;
    marks.reserve (project.playlist.markers.size());
    for (auto& m : project.playlist.markers)
        marks.emplace_back (m.name, (double)m.at * secondsPerTick);
    std::sort (marks.begin(), marks.end(), [] (auto& a, auto& b) { return a.second < b.second; });

    auto job = std::make_shared<std::unique_ptr<engine::ExportJob>>();
    auto stillAlive = alive;
    *job = session.createExportJob (
        scratch.getChildFile ("Mix.wav"), settings,
        [this, reply, scratch, marks, job, stillAlive] (const engine::ExportJob::Result& result)
        {
            struct Cleanup
            {
                juce::File folder;
                ~Cleanup() { folder.deleteRecursively(); }
            } cleanup { scratch };
            juce::MessageManager::callAsync ([job] { job->reset(); });
            if (! *stillAlive)
                return;
            if (! result.ok || result.files.empty())
                return reply (text ("The render failed: " + result.error, true));
            auto mix = cli::measureFile (result.files[0]);
            juce::Array<juce::var> sections;
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            if (std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (result.files[0])); reader)
            {
                const auto end = (double)reader->lengthInSamples / reader->sampleRate;
                for (size_t i = 0; i < marks.size(); ++i)
                {
                    const auto to = i + 1 < marks.size() ? marks[i + 1].second : end;
                    if (to > marks[i].second)
                        sections.add (object (
                            { { "name", marks[i].first }, { "lufs", loudnessOf (*reader, marks[i].second, to) } }));
                }
            }
            juce::Array<juce::var> stemList, silent;
            for (size_t i = 1; i < result.files.size(); ++i)
            {
                auto measured = cli::measureFile (result.files[i]);
                auto* o = measured.getDynamicObject();
                const auto stemName =
                    result.files[i].getFileNameWithoutExtension().fromFirstOccurrenceOf (" ", false, false);
                o->removeProperty ("file");
                o->setProperty ("name", stemName);
                if ((double)measured["soundingSeconds"] <= 0.0)
                    silent.add (stemName);
                stemList.add (measured);
            }
            if (auto* o = mix.getDynamicObject())
                o->removeProperty ("file");
            lastRender = object ({ { "mix", mix }, { "sections", sections }, { "silentParts", silent } });
            auto out = lastRender.clone();
            if (! stemList.isEmpty())
                out.getDynamicObject()->setProperty ("stems", stemList);
            reply (text (juce::JSON::toString (out)));
        });
    if (*job == nullptr)
    {
        scratch.deleteRecursively();
        return reply (text ("Nothing to render: the song is empty.", true));
    }
    (*job)->start();
}

} // namespace spinus::ai
