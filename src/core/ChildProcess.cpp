#include "ChildProcess.h"

#if ! JUCE_WINDOWS
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ; // NOLINT: POSIX declares it nowhere else
#endif

namespace spinus::core
{

#if JUCE_WINDOWS
// The double close is in JUCE's POSIX code only: on Windows, JUCE's own class.
ChildProcess::ChildProcess() = default;
ChildProcess::~ChildProcess() = default;

bool ChildProcess::start (const juce::String& command, int streamFlags) { return process.start (command, streamFlags); }

bool ChildProcess::start (const juce::StringArray& arguments, int streamFlags)
{
    return process.start (arguments, streamFlags);
}

bool ChildProcess::isRunning() const { return process.isRunning(); }

int ChildProcess::readProcessOutput (void* destBuffer, int numBytesToRead)
{
    return process.readProcessOutput (destBuffer, numBytesToRead);
}

juce::String ChildProcess::readAllProcessOutput() { return process.readAllProcessOutput(); }

bool ChildProcess::waitForProcessToFinish (int timeoutMs) const { return process.waitForProcessToFinish (timeoutMs); }

juce::uint32 ChildProcess::getExitCode() const { return process.getExitCode(); }

bool ChildProcess::kill() { return process.kill(); }

#else

namespace
{
void closeOnce (int& fd)
{
    if (fd >= 0)
        ::close (fd); // not retried on EINTR: on Linux the descriptor is closed even then
    fd = -1;
}

/** posix_spawn's file actions, destroyed on every path out. */
struct FileActions
{
    FileActions() { ok = posix_spawn_file_actions_init (&actions) == 0; }
    ~FileActions()
    {
        if (ok)
            posix_spawn_file_actions_destroy (&actions);
    }
    FileActions (const FileActions&) = delete;
    FileActions& operator= (const FileActions&) = delete;

    posix_spawn_file_actions_t actions {};
    bool ok = false;
};
} // namespace

ChildProcess::ChildProcess() = default;

ChildProcess::~ChildProcess() { forget(); }

void ChildProcess::forget()
{
    closeOnce (outputFd);
    const std::lock_guard<std::mutex> l (lock);
    collect(); // an ended program is collected; a running one is left running, as JUCE does
    pid = 0;
    ended = true;
    exitCode = 0;
    outputEnded = false;
}

bool ChildProcess::start (const juce::String& command, int streamFlags)
{
    return start (juce::StringArray::fromTokens (command, true), streamFlags);
}

bool ChildProcess::start (const juce::StringArray& arguments, int streamFlags)
{
    forget();
    if (arguments.isEmpty())
        return false;

    // Everything the child needs is made here, before it exists: posix_spawn runs no code of ours in it.
    // As JUCE: the program is the first argument without quotes; the argument list has it as given.
    const auto program = arguments[0].unquoted().toStdString();
    std::vector<std::string> texts;
    for (const auto& a : arguments)
        if (a.isNotEmpty())
            texts.push_back (a.toStdString());
    std::vector<char*> argv;
    argv.reserve (texts.size() + 1);
    for (auto& t : texts)
        argv.push_back (t.data());
    argv.push_back (nullptr);

    const auto flags = (unsigned)streamFlags;
    const bool wantOut = (flags & (unsigned)wantStdOut) != 0, wantErr = (flags & (unsigned)wantStdErr) != 0;
    int pipeFds[2] = { -1, -1 };
    if ((wantOut || wantErr) && ::pipe2 (pipeFds, O_CLOEXEC) != 0)
        return false;

    FileActions files;
    bool ok = files.ok;
    // dup2 clears close-on-exec on the copy, so the program gets the write end as its output only.
    auto route = [&] (int stream, bool wanted)
    {
        if (ok)
            ok = (wanted ? posix_spawn_file_actions_adddup2 (&files.actions, pipeFds[1], stream)
                         : posix_spawn_file_actions_addopen (&files.actions, stream, "/dev/null", O_WRONLY, 0)) == 0;
    };
    route (STDOUT_FILENO, wantOut);
    route (STDERR_FILENO, wantErr);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    // The program gets no other file of Spinus's (sound card, X11 connection, sockets, other pipes).
    if (ok)
        ok = posix_spawn_file_actions_addclosefrom_np (&files.actions, STDERR_FILENO + 1) == 0;
#endif

    pid_t child = 0;
    if (ok)
        ok = posix_spawnp (&child, program.c_str(), &files.actions, nullptr, argv.data(), environ) == 0;

    closeOnce (pipeFds[1]); // the child has its copy; ours would keep the pipe from ever ending
    if (! ok)
    {
        closeOnce (pipeFds[0]);
        return false;
    }

    const std::lock_guard<std::mutex> l (lock);
    pid = child;
    ended = false;
    exitCode = 0;
    outputFd = pipeFds[0];
    return true;
}

bool ChildProcess::collect() const
{
    if (ended)
        return true;
    int status = 0;
    pid_t result = 0;
    do
        result = ::waitpid (pid, &status, WNOHANG);
    while (result < 0 && errno == EINTR);
    if (result == 0)
        return false;

    // Collected now, or (ECHILD) by someone else: either way it has ended, and its number is never used
    // again, since another process may get it.
    ended = true;
    if (result == pid)
        exitCode = WIFEXITED (status) ? WEXITSTATUS (status) : WIFSIGNALED (status) ? 128 + WTERMSIG (status) : 0;
    return true;
}

bool ChildProcess::isRunning() const
{
    const std::lock_guard<std::mutex> l (lock);
    return ! collect();
}

int ChildProcess::readProcessOutput (void* destBuffer, int numBytesToRead)
{
    if (outputFd < 0 || destBuffer == nullptr || numBytesToRead <= 0)
        return 0;
    for (;;)
    {
        const auto n = ::read (outputFd, destBuffer, (size_t)numBytesToRead);
        if (n > 0)
            return (int)n;
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            pollfd p { outputFd, POLLIN, 0 };
            ::poll (&p, 1, -1);
            continue;
        }
        // End of file (or a broken pipe): the output has ended. Closed here, once.
        closeOnce (outputFd);
        outputEnded = true;
        return 0;
    }
}

juce::String ChildProcess::readAllProcessOutput()
{
    juce::MemoryOutputStream result;
    char buffer[4096];
    for (;;)
    {
        const auto n = readProcessOutput (buffer, (int)sizeof (buffer));
        if (n <= 0)
            break;
        result.write (buffer, (size_t)n);
    }
    return result.toString();
}

bool ChildProcess::waitForProcessToFinish (int timeoutMs) const
{
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32)timeoutMs;
    do
    {
        if (! isRunning())
            return true;
        juce::Thread::sleep (2);
    } while (timeoutMs < 0 || juce::Time::getMillisecondCounter() < deadline);
    return false;
}

juce::uint32 ChildProcess::getExitCode() const
{
    // A program that has closed its output is about to exit: give it a moment, so reading everything and
    // then asking gives its own exit code.
    if (outputEnded && isRunning())
        waitForProcessToFinish (1000);
    const std::lock_guard<std::mutex> l (lock);
    return collect() ? (juce::uint32)exitCode : 0;
}

bool ChildProcess::kill()
{
    const std::lock_guard<std::mutex> l (lock);
    if (collect())
        return true;
    return ::kill (pid, SIGKILL) == 0;
}

#endif

} // namespace spinus::core
