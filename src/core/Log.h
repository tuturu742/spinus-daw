#pragma once

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

namespace spinus::core
{

/** Masks what a log must not hold, in every line before it is written:
    - the home folder becomes ~ (also C:\Users\<name> on Windows, written with / or \, or JSON's \\), and any
      other user's folder (/home/<name>, C:\Users\<name>, a Wine prefix's drive_c/users/<name>) loses the
      name: /home/<user>;
    - API keys and tokens: sk-ant-..., sk-or-..., other sk-... keys, "Bearer <token>", and the value after
      api_key, x-api-key, authorization, token, secret or password (as `name: value`, `name=value` or JSON);
    - any text added with addSecret() (the AI service's token, a key Spinus passed on);
    - email addresses.
    The same rules as the AI service's (ai/spinus_ai/logs.py). Thread-safe once made. */
class LogRedactor
{
public:
    /** `homeFolders`: the full paths that become ~ (the home folder, and where its link points). */
    explicit LogRedactor (juce::StringArray homeFolders = {});

    /** This computer's home folder. */
    static LogRedactor forThisComputer();

    void addSecret (const juce::String&);
    juce::String apply (const juce::String&) const;

private:
    std::vector<std::string> homes;   // each with / and \ and \\ spellings
    std::vector<std::string> secrets; // 8 characters or longer
};

//==============================================================================
/** Spinus's session logs: one file per run of Spinus, so a problem a tester had can be told from the file
    they attach (Help > Open log folder).

    - The folder is the settings folder's logs/ (getLogFolder() in session/ProgramFolder.h). Each run writes
      spinus-<start time>.log (spinus-2026-10-24_14-03-12.log); the AI service writes its own beside it
      (spinus-2026-10-24_14-03-12-ai.log). The files of the newest `keepRuns` runs are kept; older ones are
      deleted when a run starts.
    - The file starts with a header (version, package, system, CPU...), then a line per event:
      `2026-10-24 14:03:12.345 WARN  audio: ...`. Lines from the helper programs (the plugin scanner and the
      plugin host, which append to the same file) name the program: `WARN  [plugin host 4242] ...`.
    - Every line goes through LogRedactor first.
    - Each line is written to the file at once with one system call (no buffer in Spinus), so the lines
      before a crash are on disk. A run that ends normally ends its log with cleanShutdownLine; a log
      without it (and whose Spinus no longer runs) is a run that ended unexpectedly, which the next start
      notes.
    - A file stops growing at `maxBytes` (a note says so); the shutdown line is still written.

    Any thread except the audio thread: writing takes a lock and does file I/O. Real-time code hands what it
    wants logged to another thread (an atomic counter read by a timer, a lock-free FIFO). Until start() (or
    joinParentLog() in a helper program) nothing is written: tests and the command-line tools write no logs. */
class Logs
{
public:
    enum class Level
    {
        info,
        warning,
        error
    };

    struct Options
    {
        juce::File folder;
        int keepRuns = 10;
        juce::int64 maxBytes = 4 * 1024 * 1024;
        juce::Time startTime = juce::Time::getCurrentTime();
        juce::StringArray header; // lines describing this run (session/SystemInfo)
    };

    /** Starts this run's log: deletes the older runs' files, notes whether the previous run ended
        unexpectedly, writes the header, and tells helper programs started from now on where the log is
        (the SPINUS_LOG_FILE environment variable). False if the file could not be made (then nothing is
        logged). Once per process; call stop() before starting again (tests). */
    static bool start (const Options&);

    /** Writes the clean shutdown line and closes the file. */
    static void stop();

    /** In a helper program Spinus started (the plugin scanner, the plugin host): its lines go to the log of
        that Spinus, labelled `label` and the process id. Nothing happens when Spinus is not logging. */
    static void joinParentLog (const juce::String& label);

    static bool isActive();

    static void write (Level, const juce::String& area, const juce::String& text);
    static void info (const juce::String& area, const juce::String& text) { write (Level::info, area, text); }
    static void warning (const juce::String& area, const juce::String& text) { write (Level::warning, area, text); }
    static void error (const juce::String& area, const juce::String& text) { write (Level::error, area, text); }

    /** Masks `secret` in every later line (and in this run's redaction generally). Empty and short texts
        (under 8 characters) are ignored. */
    static void addSecret (const juce::String& secret);

    /** Text as it would be written (tests). */
    static juce::String redact (const juce::String&);

    static juce::File getFolder();
    static juce::File getCurrentFile();
    /** Where the AI service writes its log for this run (beside the current file); none while not logging. */
    static juce::File getAiServiceFile();
    /** The previous run's log if it ended without a clean shutdown (a crash, a kill), as found by start(). */
    static juce::File getUnexpectedlyEndedRun();

    /** The log files of the newest `runs` runs in `folder`, newest run first: each run's own log, then its
        AI service's log if there is one. The current run counts. For Help > Report a problem. */
    static juce::Array<juce::File> recentFiles (int runs, const juce::File& folder = getFolder());

    /** True while the Spinus that writes `log` runs (it holds a lock on its log until it ends, however it
        ends). */
    static bool isStillRunning (const juce::File& log);

    /** True for a log file of a run (spinus-<time>.log or spinus-<time>-ai.log). */
    static bool isLogFileName (const juce::String& fileName);

    /** For a crash handler: writes one line without allocating, locking or formatting (async-signal-safe).
        `text` should hold no personal data: it is not redacted. */
    static void writeFromCrashHandler (const char* text) noexcept;

    /** Logs what a C++ exception nobody caught says before the program ends (std::terminate). */
    static void installTerminateHandler();

    static constexpr const char* environmentVariable = "SPINUS_LOG_FILE";
    static constexpr const char* cleanShutdownLine = "Spinus closed normally.";
};

} // namespace spinus::core
