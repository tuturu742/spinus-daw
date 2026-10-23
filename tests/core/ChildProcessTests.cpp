#include <catch2/catch_test_macros.hpp>

#include "core/ChildProcess.h"

#include <atomic>
#include <thread>
#include <vector>

#if ! JUCE_WINDOWS
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using spinus::core::ChildProcess;

namespace
{
juce::StringArray shell (const juce::String& script) { return { "sh", "-c", script }; }
} // namespace

#if ! JUCE_WINDOWS
TEST_CASE ("A helper program's output: standard output, error, both or neither", "[core][process]")
{
    const auto script = shell ("echo out; echo err >&2");
    auto run = [&] (int flags)
    {
        ChildProcess p;
        REQUIRE (p.start (script, flags));
        const auto text = p.readAllProcessOutput();
        CHECK (p.waitForProcessToFinish (5000));
        CHECK (p.getExitCode() == 0);
        return text;
    };
    CHECK (run (ChildProcess::wantStdOut) == "out\n");
    CHECK (run (ChildProcess::wantStdErr) == "err\n");
    CHECK (run (ChildProcess::wantStdOut | ChildProcess::wantStdErr) == "out\nerr\n");
    CHECK (run (0).isEmpty());

    // The default is both; a command line splits into arguments.
    ChildProcess p;
    REQUIRE (p.start ("echo hello  world"));
    CHECK (p.readAllProcessOutput() == "hello world\n");
}

TEST_CASE ("A helper program's exit code", "[core][process]")
{
    for (int i = 0; i < 50; ++i)
    {
        // Read everything, then ask at once: its own code, not 0 because it had not quite exited yet.
        ChildProcess p;
        REQUIRE (p.start (shell ("echo done; exit 7")));
        CHECK (p.readAllProcessOutput() == "done\n");
        CHECK (p.getExitCode() == 7);
        CHECK (! p.isRunning());
        CHECK (p.getExitCode() == 7); // and it stays so
    }

    ChildProcess quiet;
    REQUIRE (quiet.start (shell ("exit 3"), 0));
    CHECK (quiet.waitForProcessToFinish (5000));
    CHECK (quiet.getExitCode() == 3);

    // Ended by a signal: 128 + the signal, as in a shell.
    ChildProcess crashed;
    REQUIRE (crashed.start (shell ("kill -SEGV $$"), 0));
    CHECK (crashed.waitForProcessToFinish (5000));
    CHECK (crashed.getExitCode() == 128 + SIGSEGV);

    // While it runs: 0, as JUCE.
    ChildProcess running;
    REQUIRE (running.start (shell ("sleep 0.3"), 0));
    CHECK (running.isRunning());
    CHECK (running.getExitCode() == 0);
    CHECK (running.waitForProcessToFinish (5000));
}

TEST_CASE ("A helper program that cannot run does not start", "[core][process]")
{
    ChildProcess p;
    CHECK (! p.start (juce::StringArray {}));
    CHECK (! p.start (juce::StringArray { "spinus-no-such-program-anywhere" }));
    CHECK (! p.start (juce::StringArray { "/nonexistent/folder/program" }));
    CHECK (! p.isRunning());
    CHECK (p.readAllProcessOutput().isEmpty());
    CHECK (p.kill());
    CHECK (p.getExitCode() == 0);
}

TEST_CASE ("A helper program is stopped, waited for with a time limit, and found in PATH", "[core][process]")
{
    ChildProcess p;
    REQUIRE (p.start (juce::StringArray { "sleep", "30" }, 0));
    CHECK (p.isRunning());

    const auto started = juce::Time::getMillisecondCounter();
    CHECK (! p.waitForProcessToFinish (150));
    const auto waited = juce::Time::getMillisecondCounter() - started;
    CHECK (waited >= 150u);
    CHECK (waited < 5000u);

    CHECK (p.kill());
    CHECK (p.waitForProcessToFinish (5000));
    CHECK (! p.isRunning());
    CHECK (p.getExitCode() == 128 + SIGKILL);
    CHECK (p.kill()); // already ended: nothing is signalled

    // Several threads wait for the same program: all see it end, with its code.
    ChildProcess shared;
    REQUIRE (shared.start (shell ("sleep 0.2; exit 5"), 0));
    std::atomic<int> sawEnd { 0 }, sawCode { 0 };
    std::vector<std::thread> waiters;
    waiters.reserve (4);
    for (int i = 0; i < 4; ++i)
        waiters.emplace_back (
            [&]
            {
                if (shared.waitForProcessToFinish (10000))
                    ++sawEnd;
                if (shared.getExitCode() == 5)
                    ++sawCode;
            });
    for (auto& t : waiters)
        t.join();
    CHECK (sawEnd == 4);
    CHECK (sawCode == 4);
}

TEST_CASE ("A helper program's output larger than a pipe holds", "[core][process]")
{
    // 2 MB: far more than a pipe's 64 KB, so the program waits for Spinus to read as it goes.
    ChildProcess p;
    REQUIRE (p.start (shell ("head -c 2097152 /dev/zero | tr '\\0' a; echo end >&2")));
    const auto text = p.readAllProcessOutput();
    CHECK (text.length() == 2097152 + 4);
    CHECK (text.endsWith ("aend\n"));
    CHECK (text.containsOnly ("aend\n"));
    CHECK (p.getExitCode() == 0);

    // Read in small pieces too.
    ChildProcess q;
    REQUIRE (q.start (shell ("seq 1 20000")));
    char buffer[7];
    juce::MemoryOutputStream all;
    for (int n; (n = q.readProcessOutput (buffer, (int)sizeof (buffer))) > 0;)
        all.write (buffer, (size_t)n);
    CHECK (all.toString().endsWith ("\n19999\n20000\n"));
    CHECK (q.readProcessOutput (buffer, (int)sizeof (buffer)) == 0); // stays ended
}

TEST_CASE ("A helper program gets none of Spinus's open files", "[core][process]")
{
    const juce::TemporaryFile temp;
    REQUIRE (temp.getFile().replaceWithText ("x"));
    const int fd = ::open (temp.getFile().getFullPathName().toRawUTF8(), O_RDONLY); // inheritable on purpose
    REQUIRE (fd > 2);
    ChildProcess p;
    REQUIRE (p.start (shell ("if [ -e /proc/self/fd/" + juce::String (fd) + " ]; then echo has; else echo none; fi")));
    CHECK (p.readAllProcessOutput() == "none\n");
    CHECK (::close (fd) == 0);
}

TEST_CASE ("Helper programs started from many threads never close another thread's files", "[core][process]")
{
    // JUCE's ChildProcess closed its pipe twice: between the two, another thread could open a file and get
    // the same number, and the second close closed that file. Here programs start and finish on several
    // threads while others open files and check that what they opened is still theirs until they close it.
    const juce::TemporaryFile temp;
    REQUIRE (temp.getFile().replaceWithText ("spinus"));
    const auto path = temp.getFile().getFullPathName().toStdString();
    struct stat expected {};
    REQUIRE (::stat (path.c_str(), &expected) == 0);

    std::atomic<bool> stop { false };
    std::atomic<int> runs { 0 }, badRuns { 0 }, opens { 0 }, stolen { 0 };
    std::vector<std::thread> threads;
    threads.reserve (8);
    for (int i = 0; i < 4; ++i)
        threads.emplace_back (
            [&]
            {
                while (! stop)
                {
                    ChildProcess p;
                    if (! p.start (shell ("echo hello; echo there >&2")) ||
                        p.readAllProcessOutput() != "hello\nthere\n" || p.getExitCode() != 0)
                        ++badRuns;
                    ++runs;
                }
            });
    for (int i = 0; i < 4; ++i)
        threads.emplace_back (
            [&]
            {
                while (! stop)
                {
                    const int fd = ::open (path.c_str(), O_RDONLY | O_CLOEXEC);
                    if (fd < 0)
                        continue;
                    ++opens;
                    for (int check = 0; check < 20; ++check)
                    {
                        struct stat now {};
                        char c = 0;
                        if (::fstat (fd, &now) != 0 || now.st_ino != expected.st_ino || ::pread (fd, &c, 1, 0) != 1 ||
                            c != 's')
                        {
                            ++stolen;
                            break;
                        }
                    }
                    if (::close (fd) != 0)
                        ++stolen; // someone else closed it already
                }
            });
    juce::Thread::sleep (3000);
    stop = true;
    for (auto& t : threads)
        t.join();

    INFO (runs.load() << " programs, " << opens.load() << " files opened");
    CHECK (runs > 100);
    CHECK (opens > 1000);
    CHECK (badRuns == 0);
    CHECK (stolen == 0);
}
#endif

TEST_CASE ("Spinus code starts programs through core::ChildProcess, not JUCE's", "[core][process]")
{
    // JUCE's ChildProcess closes a file descriptor twice on Linux. JUCE's ChildProcessCoordinator,
    // ChildProcessWorker and ChildProcessManager are other classes and fine. Only core/ChildProcess may name it.
    const auto root = juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory();
    const juce::String banned = juce::String ("juce::") + "ChildProcess";
    juce::StringArray uses;
    for (auto* folder : { "src", "app", "tests" })
        for (const auto& entry : juce::RangedDirectoryIterator (root.getChildFile (folder), true, "*.cpp;*.h"))
        {
            const auto file = entry.getFile();
            if (file.getParentDirectory().getFileName() == "core" &&
                file.getFileNameWithoutExtension() == "ChildProcess")
                continue;
            const auto text = file.loadFileAsString();
            for (int i = text.indexOf (banned); i >= 0; i = text.indexOf (i + 1, banned))
                if (! juce::CharacterFunctions::isLetterOrDigit (text[i + banned.length()]))
                    uses.add (file.getRelativePathFrom (root));
        }
    INFO (uses.joinIntoString (", "));
    CHECK (uses.isEmpty());
}
