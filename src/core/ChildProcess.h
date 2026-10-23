#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <mutex>

namespace spinus::core
{

/** Starts a helper program (ffmpeg, Wine, the plugin scanner, the AI service...) and reads what it prints.
    Spinus uses this instead of juce::ChildProcess: on Linux, JUCE 9's version closes the file
    descriptor of its output pipe twice once the output has been read (fclose, then close on the same
    number). When another thread opens a file in between and gets that number, the second close closes the
    other thread's file: an audio device, a plugin's file, the X11 connection, a sandbox socket. It made
    Spinus crash or misbehave at random.

    The same interface as juce::ChildProcess, and on Linux the same behaviour, apart from:
    - each file descriptor is closed exactly once;
    - the output pipe is close-on-exec, and the program gets only standard input, output and error from
      Spinus (no other open files), so a program started at the same time from another thread does not
      keep the pipe open and readAllProcessOutput() returns when this program is done;
    - start() returns false when the program cannot be run at all (not found, not executable), where JUCE
      starts a process that exits with 255 at once;
    - getExitCode() of a program ended by a signal is 128 + the signal number (as in a shell), not 0; right
      after readAllProcessOutput() it waits up to a second for the program to exit, so the code is the
      program's own rather than 0 because it had not quite exited yet. While the program runs it is 0;
    - after the program has ended, kill() does nothing (JUCE would signal whatever process has the number
      now), and isRunning() and getExitCode() keep reporting the same code;
    - the destructor collects a program that has already ended, so it does not stay as a zombie.
    Like JUCE: the program inherits Spinus's standard input and environment and is in Spinus's process
    group; the streams not asked for go to /dev/null; kill() sends SIGKILL to the program only; the
    destructor does not stop a program that is still running. On Windows this is juce::ChildProcess.

    The status functions (isRunning, waitForProcessToFinish, getExitCode, kill) may be called from several
    threads at once; reading the output is for one thread at a time. Never on the audio thread. */
class ChildProcess
{
public:
    enum StreamFlags
    {
        wantStdOut = 1,
        wantStdErr = 2
    };

    ChildProcess();
    ~ChildProcess();

    /** Splits the command line into arguments as JUCE does (StringArray::fromTokens, quotes kept). */
    bool start (const juce::String& command, int streamFlags = wantStdOut | wantStdErr);

    /** The first argument is the program: a path, or a name looked up in PATH. Empty arguments are left
        out. Starting again forgets the previous program (without stopping it). */
    bool start (const juce::StringArray& arguments, int streamFlags = wantStdOut | wantStdErr);

    bool isRunning() const;

    /** Blocks until some output is there; returns how many bytes were read, 0 once the output has ended. */
    int readProcessOutput (void* destBuffer, int numBytesToRead);

    /** Reads until the program closes its output (normally: until it ends). */
    juce::String readAllProcessOutput();

    /** True once the program has ended; false if it still runs after timeoutMs (negative: wait forever). */
    bool waitForProcessToFinish (int timeoutMs) const;

    juce::uint32 getExitCode() const;

    /** Ends the program at once (SIGKILL). True if it was signalled or had already ended. */
    bool kill();

private:
#if JUCE_WINDOWS
    juce::ChildProcess process;
#else
    void forget();
    bool collect() const; // true once the program has ended; call with `lock` held

    mutable std::mutex lock;
    int pid = 0; // 0: none started
    mutable bool ended = true;
    mutable int exitCode = 0;
    int outputFd = -1;                       // our end of the output pipe; -1 when there is none (any more)
    std::atomic<bool> outputEnded { false }; // the pipe gave end-of-file
#endif

    JUCE_DECLARE_NON_COPYABLE (ChildProcess)
};

/** For Spinus's helper programs (the plugin scanner and the plugin host), called first in main(): a crash
    ends the program at once. On Windows a crashed program otherwise stays for seconds while Windows Error
    Reporting writes its report (or shows a box), and Spinus, waiting on its pipe, would only notice when
    it times out. Error boxes for a library that cannot be loaded are not shown either. On Linux a crash
    ends a program at once already: nothing to do. */
void endAtOnceOnCrash();

} // namespace spinus::core
