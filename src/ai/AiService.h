#pragma once

#include "core/ChildProcess.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <map>
#include <mutex>

namespace spinus::ai
{

/** Spinus's side of the AI service: AI is off until the user turns it on; then the
    `spinus-ai` Python service is started on first use and spoken to with JSON-RPC over a TCP connection
    on this computer only (127.0.0.1, with a token).

    Everything Spinus needs keeps working when the service is missing, fails to start or crashes
    (AGENTS rule 4): calls then answer with a plain error, and the next call starts it again.

    Message thread only, except the reader thread inside. Replies and notifications arrive on the
    message thread. */
class AiService final : private juce::Thread
{
public:
    /** `settingsFolder`: Spinus's settings; the service keeps its connections, routing and usage in
        settingsFolder/ai, and Spinus remembers there whether AI is on. */
    explicit AiService (const juce::File& settingsFolder, const juce::File& modelsFolder = getDefaultModelsFolder());
    ~AiService() override;

    /** Off until the user turns it on. Turning it off stops the service. */
    bool isEnabled() const noexcept { return enabled; }
    void setEnabled (bool);

    /** Options > AI > Show experimental features: off until the user turns it on. While
        it is off, experimental features (the Analyzer) are not shown anywhere. Independent of isEnabled(),
        and kept with it in settings.json. */
    bool showsExperimentalFeatures() const noexcept { return experimental; }
    void setShowExperimentalFeatures (bool);

    /** Where the service is: the Python package, the Python to run it and the manual it searches. Empty
        python when it is not installed. */
    struct Installation
    {
        juce::File python, package, docs, schema;
    };
    static Installation findInstallation();

    /** Where downloaded models go: ~/.local/share/spinus/models, or under $XDG_DATA_HOME. */
    static juce::File getDefaultModelsFolder();
    /** Where the service keeps its settings and caches (the folder it was made with). */
    juce::File getFolder() const { return folder; }
    void setInstallation (Installation i) { installation = std::move (i); }

    enum class State
    {
        off,      // AI not turned on
        stopped,  // on, but not started yet (or stopped after a crash: the next call starts it)
        starting, // process started, socket not connected yet
        running,
        failed // could not start: see getProblem()
    };
    State getState() const noexcept { return state; }
    juce::String getProblem() const { return problem; }

    using Reply = std::function<void (const juce::var& result, const juce::String& error)>;

    /** Calls an ai.v1 method; the reply comes later on the message thread (error non-empty on failure). */
    void call (const juce::String& method, const juce::var& params, Reply);

    using Listener = std::function<void (const juce::String& method, const juce::var& params)>;
    int addListener (Listener);
    void removeListener (int);

    /** Called on the message thread when getState(), isEnabled() or showsExperimentalFeatures() changes. */
    int addStateListener (std::function<void()>);
    void removeStateListener (int);

    /** Stops the service and waits for it (on quit, and when AI is turned off). */
    void stop();

private:
    void start();
    void setState (State, const juce::String& why = {});
    void saveSettings();
    void notifyStateListeners();
    void run() override;
    bool connectToService();
    bool writeLine (const juce::String&);
    static bool sendLine (std::intptr_t socket, const juce::String&);
    void received (const juce::String& line);
    void failPending (const juce::String& error);
    juce::String readLogTail() const;

    juce::File folder, modelsFolder;
    Installation installation;
    bool enabled = false, experimental = false;
    State state = State::off;
    juce::String problem;

    std::unique_ptr<core::ChildProcess> process;
    juce::File portFile;
    juce::String token;                           // the service's password for this run
    std::atomic<std::intptr_t> connection { -1 }; // the socket (an int on Linux, a SOCKET on Windows)
    std::mutex writeLock, pendingLock;
    std::vector<juce::String> queued; // lines waiting for the connection
    std::map<int, Reply> pending;
    int nextId = 1;
    std::map<int, Listener> listeners;
    std::map<int, std::function<void()>> stateListeners;
    int nextListener = 1;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};

} // namespace spinus::ai
