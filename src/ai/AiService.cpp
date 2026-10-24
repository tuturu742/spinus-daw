#include "AiService.h"

#include "core/Log.h"
#include "session/ProgramFolder.h"

#if JUCE_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace spinus::ai
{

namespace
{
constexpr int connectTimeoutMs = 20000;

/** This process's id: the service stops by itself once it has ended (a crash leaves no one to tell it). */
int ownProcessId()
{
#if JUCE_WINDOWS
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : items)
        o->setProperty (k, v);
    return juce::var (o);
}

//==============================================================================
// The connection: TCP on this computer only (127.0.0.1), the same on Linux and Windows.
constexpr std::intptr_t noSocket = -1;

#if JUCE_WINDOWS
bool startNetworking()
{
    static const bool started = []
    {
        WSADATA data {};
        return WSAStartup (MAKEWORD (2, 2), &data) == 0;
    }();
    return started;
}
std::intptr_t openSocket() { return (std::intptr_t)::socket (AF_INET, SOCK_STREAM, IPPROTO_TCP); }
void closeSocket (std::intptr_t s)
{
    ::shutdown ((SOCKET)s, SD_BOTH);
    ::closesocket ((SOCKET)s);
}
int connectTo (std::intptr_t s, const sockaddr_in& a)
{
    return ::connect ((SOCKET)s, reinterpret_cast<const sockaddr*> (&a), sizeof (a));
}
int sendSome (std::intptr_t s, const char* data, size_t size) { return ::send ((SOCKET)s, data, (int)size, 0); }
int receiveSome (std::intptr_t s, char* data, size_t size) { return ::recv ((SOCKET)s, data, (int)size, 0); }
bool waitForData (std::intptr_t s, int milliseconds)
{
    WSAPOLLFD p { (SOCKET)s, POLLRDNORM, 0 };
    return WSAPoll (&p, 1, milliseconds) > 0;
}
#else
bool startNetworking() { return true; }
std::intptr_t openSocket() { return ::socket (AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); }
void closeSocket (std::intptr_t s)
{
    ::shutdown ((int)s, SHUT_RDWR);
    ::close ((int)s);
}
int connectTo (std::intptr_t s, const sockaddr_in& a)
{
    return ::connect ((int)s, reinterpret_cast<const sockaddr*> (&a), sizeof (a));
}
int sendSome (std::intptr_t s, const char* data, size_t size) { return (int)::send ((int)s, data, size, MSG_NOSIGNAL); }
int receiveSome (std::intptr_t s, char* data, size_t size) { return (int)::recv ((int)s, data, size, 0); }
bool waitForData (std::intptr_t s, int milliseconds)
{
    pollfd p { (int)s, POLLIN, 0 };
    return ::poll (&p, 1, milliseconds) > 0;
}
#endif

/** A file only this user can read (Linux: mode 600; on Windows the user's settings folder already is). */
bool writePrivateFile (const juce::File& file, const juce::String& text)
{
    file.deleteFile();
    if (! file.create())
        return false;
#if ! JUCE_WINDOWS
    if (::chmod (file.getFullPathName().toRawUTF8(), 0600) != 0)
        return false;
#endif
    // Written in place (replaceWithText would write a new file, without that mode).
    juce::FileOutputStream out (file);
    return out.openedOk() && out.writeText (text, false, false, nullptr) && (out.flush(), out.getStatus().wasOk());
}
} // namespace

juce::File AiService::getDefaultModelsFolder()
{
    return getUserDataRoot().getChildFile ("spinus").getChildFile ("models");
}

AiService::AiService (const juce::File& settingsFolder, const juce::File& models)
    : juce::Thread ("Spinus AI service"), folder (settingsFolder.getChildFile ("ai")), modelsFolder (models),
      installation (findInstallation())
{
    const auto settings = juce::JSON::parse (folder.getChildFile ("settings.json"));
    enabled = (bool)settings["enabled"];
    experimental = (bool)settings["experimental"];
    state = enabled ? State::stopped : State::off;
}

AiService::~AiService()
{
    *alive = false;
    stop();
}

AiService::Installation AiService::findInstallation()
{
    Installation i;
    const auto exeFolder = getProgramFolder();
#if JUCE_WINDOWS
    // Windows has no system Python to rely on: the installer brings one (python/python.exe).
    const juce::String systemPython, ownPython = "python/python.exe", venvPython = "ai/.venv/Scripts/python.exe";
#else
    const juce::String systemPython = "/usr/bin/python3", ownPython = "python/bin/python3",
                       venvPython = "ai/.venv/bin/python";
#endif
    const auto fileOrNone = [] (const juce::String& path)
    { return juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File(); };

    // 1. Given explicitly (tests, developers).
    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("SPINUS_AI_DIR", {}); dir.isNotEmpty())
    {
        i.package = juce::File (dir).getChildFile ("spinus_ai");
        i.python = fileOrNone (juce::SystemStats::getEnvironmentVariable ("SPINUS_AI_PYTHON", systemPython));
    }
    // 2. Installed next to Spinus: its own folder, with its own Python when it brings one (AppImage,
    //    Flatpak, Windows), else the system's with python-httpx and python-keyring (the Arch package).
    else if (const auto installed = exeFolder.getChildFile ("ai"); installed.getChildFile ("spinus_ai").isDirectory())
    {
        i.package = installed.getChildFile ("spinus_ai");
        const auto own = installed.getChildFile (ownPython);
        i.python = own.existsAsFile() ? own : fileOrNone (systemPython);
        i.docs = installed.getChildFile ("docs/user");
        i.schema = installed.getChildFile ("commands.schema.json");
    }
#if defined(SPINUS_SOURCE_DIR)
    // 3. A development build: the source tree, with the uv environment made by `uv sync` in ai/.
    else if (const juce::File source (SPINUS_SOURCE_DIR); source.getChildFile ("ai/spinus_ai").isDirectory())
    {
        i.package = source.getChildFile ("ai/spinus_ai");
        i.python = source.getChildFile (venvPython);
    }
#endif

    const auto root = i.package.getParentDirectory().getParentDirectory();
    if (i.docs == juce::File())
        i.docs = root.getChildFile ("docs/user");
    if (i.schema == juce::File())
        i.schema = root.getChildFile ("docs/dev/commands.schema.json");
    if (! i.python.existsAsFile() || ! i.package.isDirectory())
        i.python = juce::File();
    return i;
}

void AiService::setEnabled (bool shouldBeOn)
{
    if (shouldBeOn == enabled)
        return;
    enabled = shouldBeOn;
    core::Logs::info ("ai", enabled ? "AI turned on" : "AI turned off");
    saveSettings();
    if (! enabled)
        stop();
    setState (enabled ? State::stopped : State::off);
}

void AiService::setShowExperimentalFeatures (bool shouldShow)
{
    if (shouldShow == experimental)
        return;
    experimental = shouldShow;
    saveSettings();
    notifyStateListeners();
}

void AiService::saveSettings()
{
    folder.createDirectory();
    folder.getChildFile ("settings.json")
        .replaceWithText (juce::JSON::toString (object ({ { "enabled", enabled }, { "experimental", experimental } })));
}

void AiService::setState (State s, const juce::String& why)
{
    problem = s == State::failed ? why : juce::String();
    if (s == state)
        return;
    if (s == State::running)
        core::Logs::info ("ai", "AI service running");
    else if (s == State::failed)
        core::Logs::warning ("ai", "The AI service is not available: " + why);
    state = s;
    notifyStateListeners();
}

void AiService::notifyStateListeners()
{
    auto copy = stateListeners;
    for (auto& [id, listener] : copy)
        listener();
}

int AiService::addStateListener (std::function<void()> l)
{
    stateListeners[nextListener] = std::move (l);
    return nextListener++;
}

void AiService::removeStateListener (int id) { stateListeners.erase (id); }

//==============================================================================
void AiService::start()
{
    if (! enabled || state == State::starting || state == State::running)
        return;
    if (installation.python == juce::File())
        return setState (State::failed, "The AI service is not installed. Reinstall Spinus (it needs the "
                                        "python-httpx and python-keyring packages).");
    core::Logs::info ("ai", "Starting the AI service (" + installation.python.getFullPathName() + ")");

    stop();
    folder.createDirectory();
    const auto log = folder.getChildFile ("service.log");
    log.replaceWithText ({});

    // The service listens on a port of its choosing on 127.0.0.1 and writes it to `port`; it answers only a
    // connection that first sends the token Spinus gave it in a file only this user can read.
    portFile = folder.getChildFile ("service.port");
    portFile.deleteFile();
    token = juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()) +
            juce::String::toHexString (juce::Random::getSystemRandom().nextInt64());
    core::Logs::addSecret (token); // the service's password: never in a log
    const auto tokenFile = folder.getChildFile ("service.token");
    if (! writePrivateFile (tokenFile, token))
        return setState (State::failed,
                         "The AI service could not be started: " + folder.getFullPathName() + " cannot be written.");

    // run_spinus_ai.py sits beside the package, so Python finds the package from it; the service's errors go
    // to service.log, so a failed start can say why.
    process = std::make_unique<core::ChildProcess>();
    juce::StringArray command { installation.python.getFullPathName(),
                                installation.package.getSiblingFile ("run_spinus_ai.py").getFullPathName(),
                                "--log",
                                log.getFullPathName(),
                                "--port-file",
                                portFile.getFullPathName(),
                                "--token-file",
                                tokenFile.getFullPathName(),
                                "--config",
                                folder.getFullPathName(),
                                "--docs",
                                installation.docs.getFullPathName(),
                                "--schema",
                                installation.schema.getFullPathName(),
                                "--models",
                                modelsFolder.getFullPathName(),
                                "--parent-pid",
                                juce::String (ownProcessId()) };
    // The service's own log for this run, beside Spinus's (the same redaction: ai/spinus_ai/logs.py).
    if (const auto aiLog = core::Logs::getAiServiceFile(); aiLog != juce::File())
        command.addArray ({ "--session-log", aiLog.getFullPathName() });
    if (! process->start (command, 0))
    {
        process = nullptr;
        return setState (State::failed, "The AI service could not be started.");
    }
    setState (State::starting);
    startThread();
}

void AiService::stop()
{
    if (connection != noSocket)
        writeLine (juce::JSON::toString (object ({ { "jsonrpc", "2.0" }, { "method", "ai.v1.shutdown" } }), true));
    signalThreadShouldExit();
    if (const auto s = connection.exchange (noSocket); s != noSocket)
        closeSocket (s);
    stopThread (3000);
    if (process != nullptr)
    {
        if (! process->waitForProcessToFinish (3000))
            process->kill();
        process = nullptr;
    }
    portFile.deleteFile();
    folder.getChildFile ("service.token").deleteFile();
    {
        const std::scoped_lock lock (writeLock);
        queued.clear();
    }
    failPending ("The AI service stopped.");
    if (state == State::starting || state == State::running)
    {
        core::Logs::info ("ai", "AI service stopped");
        setState (enabled ? State::stopped : State::off);
    }
}

//==============================================================================
void AiService::call (const juce::String& method, const juce::var& params, Reply reply)
{
    // A key the user typed passes through here once: masked wherever it might appear later.
    if (params.isObject() && params.hasProperty ("key"))
        core::Logs::addSecret (params["key"].toString());
    // Failed calls are noted (the method and the error: never what was asked).
    reply = [method, inner = std::move (reply)] (const juce::var& result, const juce::String& error)
    {
        if (error.isNotEmpty())
            core::Logs::warning ("ai", method + " failed: " + error.substring (0, 300));
        if (inner)
            inner (result, error);
    };
    auto fail = [this, &reply] (const juce::String& error)
    {
        juce::MessageManager::callAsync (
            [reply = std::move (reply), error, a = alive]
            {
                if (*a && reply)
                    reply ({}, error);
            });
    };
    if (! enabled)
        return fail ("AI is off. Turn it on in Options > AI.");
    if (state == State::stopped || state == State::failed)
        start();
    if (state == State::failed)
        return fail (problem);

    int id = 0;
    {
        const std::scoped_lock lock (pendingLock);
        id = nextId++;
        pending[id] = std::move (reply);
    }
    const auto line = juce::JSON::toString (
        object ({ { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", params } }), true);
    {
        const std::scoped_lock lock (writeLock);
        if (connection == noSocket)
        {
            queued.push_back (line);
            return;
        }
    }
    writeLine (line);
}

int AiService::addListener (Listener l)
{
    listeners[nextListener] = std::move (l);
    return nextListener++;
}

void AiService::removeListener (int id) { listeners.erase (id); }

bool AiService::writeLine (const juce::String& line)
{
    const std::scoped_lock lock (writeLock);
    return sendLine (connection.load(), line);
}

bool AiService::sendLine (std::intptr_t s, const juce::String& line)
{
    if (s == noSocket)
        return false;
    const auto data = (line + "\n").toStdString();
    size_t sent = 0;
    while (sent < data.size())
    {
        const auto n = sendSome (s, data.data() + sent, data.size() - sent);
        if (n <= 0)
            return false;
        sent += (size_t)n;
    }
    return true;
}

void AiService::failPending (const juce::String& error)
{
    std::map<int, Reply> failed;
    {
        const std::scoped_lock lock (pendingLock);
        failed.swap (pending);
    }
    for (auto& [id, reply] : failed)
        juce::MessageManager::callAsync (
            [reply, error, a = alive]
            {
                if (*a && reply)
                    reply ({}, error);
            });
}

juce::String AiService::readLogTail() const
{
    juce::StringArray lines;
    lines.addLines (folder.getChildFile ("service.log").loadFileAsString().trim());
    while (lines.size() > 3)
        lines.remove (0);
    return lines.joinIntoString (" ");
}

//==============================================================================
bool AiService::connectToService()
{
    // The service writes its port once it listens.
    const auto port = portFile.loadFileAsString().trim().getIntValue();
    if (port <= 0 || port > 65535 || ! startNetworking())
        return false;
    const auto s = openSocket();
    if (s == noSocket)
        return false;
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons ((uint16_t)port);
    address.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    // The first line is the token: the service answers nobody else.
    if (connectTo (s, address) != 0 || ! sendLine (s, token))
    {
        closeSocket (s);
        return false;
    }
    connection = s;
    return true;
}

void AiService::run()
{
    // Wait for the service to listen.
    const auto started = juce::Time::getMillisecondCounter();
    while (! threadShouldExit() && ! connectToService())
    {
        if ((process != nullptr && ! process->isRunning()) ||
            juce::Time::getMillisecondCounter() - started > (juce::uint32)connectTimeoutMs)
        {
            auto log = readLogTail();
            if (log.isEmpty() && process != nullptr && ! process->isRunning())
                log = "(It ended at once, with exit code " + juce::String ((int)process->getExitCode()) + ".)";
#if JUCE_WINDOWS
            const juce::String missingPackages =
                "The AI service is missing Python packages: reinstall Spinus to bring them back.";
#else
            const juce::String missingPackages = "The AI service needs Python packages that are not installed: run "
                                                 "sudo pacman -S python-httpx python-keyring";
#endif
            const auto why =
                log.contains ("No module named") ? missingPackages : "The AI service could not start. " + log;
            juce::MessageManager::callAsync (
                [this, why, a = alive]
                {
                    if (! *a)
                        return;
                    failPending (why);
                    setState (State::failed, why);
                });
            return;
        }
        wait (100);
    }
    if (threadShouldExit())
        return;

    std::vector<juce::String> waiting;
    {
        const std::scoped_lock lock (writeLock);
        waiting.swap (queued);
    }
    for (auto& line : waiting)
        writeLine (line);
    juce::MessageManager::callAsync (
        [this, a = alive]
        {
            if (*a)
                setState (State::running);
        });

    // Read replies and notifications, one JSON object per line.
    std::string buffer;
    char chunk[8192];
    while (! threadShouldExit())
    {
        const auto s = connection.load();
        if (s == noSocket)
            break;
        if (! waitForData (s, 200))
            continue;
        const auto n = receiveSome (s, chunk, sizeof (chunk));
        if (n <= 0)
            break;
        buffer.append (chunk, (size_t)n);
        for (auto end = buffer.find ('\n'); end != std::string::npos; end = buffer.find ('\n'))
        {
            received (juce::String::fromUTF8 (buffer.data(), (int)end));
            buffer.erase (0, end + 1);
        }
    }

    if (! threadShouldExit())
    {
        // The service went away on its own (a crash): the next call starts it again.
        juce::MessageManager::callAsync (
            [this, a = alive]
            {
                if (! *a)
                    return;
                if (const auto s = connection.exchange (noSocket); s != noSocket)
                    closeSocket (s);
                core::Logs::warning ("ai", "The AI service stopped unexpectedly" +
                                               (process != nullptr && ! process->isRunning()
                                                    ? " (exit code " + juce::String ((int)process->getExitCode()) + ")"
                                                    : juce::String()) +
                                               ". Its last words: " + readLogTail());
                failPending ("The AI service stopped unexpectedly. " + readLogTail());
                setState (enabled ? State::stopped : State::off);
            });
    }
}

void AiService::received (const juce::String& line)
{
    const auto message = juce::JSON::parse (line);
    if (! message.isObject())
        return;

    if (message.hasProperty ("id") && ! message["id"].isVoid())
    {
        Reply reply;
        {
            const std::scoped_lock lock (pendingLock);
            if (auto it = pending.find ((int)message["id"]); it != pending.end())
            {
                reply = std::move (it->second);
                pending.erase (it);
            }
        }
        if (! reply)
            return;
        const auto error = message["error"].isObject() ? message["error"]["message"].toString() : juce::String();
        juce::MessageManager::callAsync (
            [reply, result = message["result"], error, a = alive]
            {
                if (*a)
                    reply (result, error);
            });
        return;
    }

    juce::MessageManager::callAsync (
        [this, method = message["method"].toString(), params = message["params"], a = alive]
        {
            if (! *a)
                return;
            auto copy = listeners;
            for (auto& [id, listener] : copy)
                listener (method, params);
        });
}

} // namespace spinus::ai
