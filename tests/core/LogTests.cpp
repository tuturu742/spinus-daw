#include <catch2/catch_test_macros.hpp>

#include "core/Log.h"
#include "session/SystemInfo.h"

#include <cstdlib>
#include <thread>
#include <vector>

using spinus::core::LogRedactor;
using spinus::core::Logs;

namespace
{
// Keys are made from pieces, so no text that looks like a real key is in the repository.
juce::String fakeKey (const char* prefix, int length = 40)
{
    return juce::String (prefix) + juce::String::repeatedString ("Ab9_", length / 4);
}

juce::String anthropicKey()
{
    return fakeKey ("sk-"
                    "ant-"
                    "api03-");
}
juce::String openRouterKey()
{
    return fakeKey ("sk-"
                    "or-"
                    "v1-");
}

/** A fresh folder for a test's logs, removed afterwards. */
struct ScratchFolder
{
    ScratchFolder()
        : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("spinus-log-tests-" + juce::String::toHexString (juce::Random().nextInt64())))
    {
        folder.createDirectory();
    }
    ~ScratchFolder()
    {
        Logs::stop();
        folder.deleteRecursively();
    }
    ScratchFolder (const ScratchFolder&) = delete;
    ScratchFolder& operator= (const ScratchFolder&) = delete;

    juce::File folder;
};

Logs::Options optionsFor (const juce::File& folder, juce::Time start = juce::Time::getCurrentTime())
{
    Logs::Options o;
    o.folder = folder;
    o.startTime = start;
    o.header = { "Spinus DAW test header" };
    return o;
}

/** A run's log as an older run would have left it. */
juce::File writeOldRun (const juce::File& folder, const juce::String& name, bool clean)
{
    auto f = folder.getChildFile ("spinus-" + name + ".log");
    f.replaceWithText ("header\n" +
                       juce::String (clean ? juce::String ("2026-01-01 info  quit: ") + Logs::cleanShutdownLine
                                           : juce::String ("2026-01-01 info  audio: playing")) +
                       "\n");
    return f;
}
} // namespace

//==============================================================================
TEST_CASE ("Log redaction: the home folder becomes ~", "[core][log]")
{
    const LogRedactor onLinux ({ "/home/alice" });
    CHECK (onLinux.apply ("Opened /home/alice/Music/Song/Song.spn") == "Opened ~/Music/Song/Song.spn");
    CHECK (onLinux.apply ("in /home/alice") == "in ~");
    CHECK (onLinux.apply ("Saved in /home/alice.") == "Saved in ~.");
    CHECK (onLinux.apply ("\"/home/alice/a\", '/home/alice/b'") == "\"~/a\", '~/b'");
    CHECK (onLinux.apply ("file:///home/alice/x.wav") == "file://~/x.wav");
    // Not the same folder: another user's, or a longer name, loses only the name.
    CHECK (onLinux.apply ("/home/alice2/x") == "/home/<user>/x");
    CHECK (onLinux.apply ("/home/bob/.wine") == "/home/<user>/.wine");
    CHECK (onLinux.apply ("/mnt/home/alice/x") == "/mnt/home/<user>/x");
    // Names in other scripts are names too.
    CHECK (LogRedactor ({ juce::String::fromUTF8 ("/home/\xd0\xb4\xd0\xb5\xd0\xbd") })
               .apply (juce::String::fromUTF8 ("/home/\xd0\xb4\xd0\xb5\xd0\xbd/Music")) == "~/Music");
    CHECK (LogRedactor().apply (juce::String::fromUTF8 ("/home/\xd0\xb4\xd0\xb5\xd0\xbd/Music")) ==
           "/home/<user>/Music");
    // A root-like home would mask every path: ignored.
    CHECK (LogRedactor ({ "/" }).apply ("/usr/lib/spinus") == "/usr/lib/spinus");
    // The text that is not a path stays.
    CHECK (onLinux.apply ("Audio: JACK, 48.0 kHz, 256 samples") == "Audio: JACK, 48.0 kHz, 256 samples");
}

TEST_CASE ("Log redaction: Windows user folders", "[core][log]")
{
    const LogRedactor onWindows ({ "C:\\Users\\Alice" });
    CHECK (onWindows.apply ("C:\\Users\\Alice\\Music\\Song.spn") == "~\\Music\\Song.spn");
    CHECK (onWindows.apply ("c:\\users\\alice\\Music") == "~\\Music"); // Windows ignores case
    CHECK (onWindows.apply ("C:/Users/Alice/AppData/Roaming/Spinus DAW") == "~/AppData/Roaming/Spinus DAW");
    CHECK (onWindows.apply ("{\"path\": \"C:\\\\Users\\\\Alice\\\\Music\"}") == "{\"path\": \"~\\\\Music\"}");
    // Any user's folder: the name goes, also with spaces in it, and in a Wine prefix.
    const LogRedactor none;
    CHECK (none.apply ("D:\\Users\\Jo Ann\\Music\\x.wav") == "D:\\Users\\<user>\\Music\\x.wav");
    CHECK (none.apply ("C:\\Users\\Bob") == "C:\\Users\\<user>");
    CHECK (none.apply ("Wine said: C:\\users\\alice\\AppData failed") ==
           "Wine said: C:\\users\\<user>\\AppData failed");
    CHECK (none.apply ("prefix/drive_c/users/alice/AppData/Roaming") == "prefix/drive_c/users/<user>/AppData/Roaming");
    // Windows's shared folders are nobody's.
    CHECK (none.apply ("C:\\Users\\Public\\Documents") == "C:\\Users\\Public\\Documents");
    // "users" elsewhere is a word.
    CHECK (none.apply ("3 users/day and /srv/users/x") == "3 users/day and /srv/users/x");
}

TEST_CASE ("Log redaction: keys and tokens", "[core][log]")
{
    const LogRedactor r;
    CHECK (r.apply ("key " + anthropicKey() + " refused") == "key sk-ant-*** refused");
    CHECK (r.apply ("(" + openRouterKey() + ")") == "(sk-or-***)");
    CHECK (r.apply ("OpenAI " + fakeKey ("sk-"
                                         "proj-")) == "OpenAI sk-***");
    // "sk-" inside a word, or too short to be a key, stays.
    CHECK (r.apply ("disk-cache-folder-for-the-render") == "disk-cache-folder-for-the-render");
    CHECK (r.apply ("task-1 and sk-12") == "task-1 and sk-12");
    CHECK (r.apply ("Authorization: Bearer abc.def-ghi_123=") == "Authorization: Bearer ***");
    CHECK (r.apply ("header bearer xyz/123+") == "header bearer ***");
    CHECK (r.apply ("x-api-key: abcdef123456") == "x-api-key: ***");
    CHECK (r.apply ("{\"apiKey\": \"abcdef123456\", \"model\": \"m\"}") == "{\"apiKey\": \"***\", \"model\": \"m\"}");
    CHECK (r.apply ("url?api_key=abc123&x=1") == "url?api_key=***&x=1");
    CHECK (r.apply ("access_token=abc password = 'hunter22'") == "access_token=*** password = '***'");
    CHECK (r.apply ("Authorization: Basic dXNlcjpwYXNz") == "Authorization: Basic ***");
    // Words that only look like names of secrets stay.
    CHECK (r.apply ("token file missing; 120 tokens: fine; Secret Service is off") ==
           "token file missing; 120 tokens: fine; Secret Service is off");
    CHECK (r.apply ("key: C minor") == "key: C minor"); // the song's key is no secret
    CHECK (r.apply ("Bearer") == "Bearer");

    auto withSecret = LogRedactor();
    withSecret.addSecret ("0123456789abcdef0123");
    withSecret.addSecret ("short"); // too short to tell from ordinary words: ignored
    CHECK (withSecret.apply ("token file says 0123456789abcdef0123.") == "token file says ***.");
    CHECK (withSecret.apply ("a short line") == "a short line");
}

TEST_CASE ("Log redaction: email addresses", "[core][log]")
{
    const LogRedactor r;
    CHECK (r.apply ("Signed in as jane.doe+spinus@example.co.uk.") == "Signed in as <email>.");
    CHECK (r.apply ("<a.b@c.io>") == "<<email>>");
    CHECK (r.apply ("plugin@1.2.30 and user@localhost and @mention") ==
           "plugin@1.2.30 and user@localhost and @mention");
}

TEST_CASE ("Log redaction: everything in one line", "[core][log]")
{
    const LogRedactor r ({ "/home/alice" });
    CHECK (r.apply ("alice@example.com saved /home/alice/x with " + anthropicKey()) ==
           "<email> saved ~/x with sk-ant-***");
}

//==============================================================================
TEST_CASE ("A run's log: header, lines, and a clean end", "[core][log]")
{
    const ScratchFolder scratch;
    REQUIRE (Logs::start (optionsFor (scratch.folder)));
    CHECK (Logs::isActive());
    const auto file = Logs::getCurrentFile();
    CHECK (file.getParentDirectory() == scratch.folder);
    CHECK (Logs::isLogFileName (file.getFileName()));
    CHECK (Logs::getAiServiceFile().getFileName() == file.getFileNameWithoutExtension() + "-ai.log");
    CHECK (juce::SystemStats::getEnvironmentVariable (Logs::environmentVariable, {}) == file.getFullPathName());

    Logs::info ("audio", "Opened the device");
    Logs::warning ("project", "Could not save:\nthe disk is full");
    Logs::error ("ai", "Refused " + anthropicKey());
    Logs::addSecret ("the-service-token-0001");
    Logs::info ("ai", "token the-service-token-0001 sent");

    // On disk at once, before the run ends (a crash loses nothing written).
    const auto during = file.loadFileAsString();
    CHECK (during.startsWith ("Spinus DAW test header\n"));
    CHECK (during.contains (" info  audio: Opened the device\n"));
    CHECK (during.contains (" WARN  project: Could not save:\n    the disk is full\n"));
    CHECK (during.contains (" ERROR ai: Refused sk-ant-***\n"));
    CHECK (during.contains ("token *** sent"));
    CHECK (! during.contains ("api03"));
    CHECK (! during.contains (Logs::cleanShutdownLine));

    Logs::stop();
    CHECK (! Logs::isActive());
    CHECK (file.loadFileAsString().trimEnd().endsWith (Logs::cleanShutdownLine));
    CHECK (juce::SystemStats::getEnvironmentVariable (Logs::environmentVariable, {}).isEmpty());
    Logs::info ("audio", "after the end"); // nowhere: logging is off
    CHECK (! file.loadFileAsString().contains ("after the end"));
}

TEST_CASE ("Logs keep the newest runs and notice one that ended unexpectedly", "[core][log]")
{
    const ScratchFolder scratch;
    for (int i = 1; i <= 12; ++i)
    {
        const auto name = "2026-01-" + juce::String (i).paddedLeft ('0', 2) + "_10-00-00";
        writeOldRun (scratch.folder, name, i != 12); // the newest one crashed
        if (i % 2 == 0)
            scratch.folder.getChildFile ("spinus-" + name + "-ai.log").replaceWithText ("ai\n");
    }
    scratch.folder.getChildFile ("notes.txt").replaceWithText ("not a log"); // not Spinus's: stays

    auto options = optionsFor (scratch.folder, juce::Time (2026, 1, 20, 10, 0)); // month 1: February
    options.keepRuns = 10;
    REQUIRE (Logs::start (options));
    CHECK (Logs::getUnexpectedlyEndedRun().getFileName() == "spinus-2026-01-12_10-00-00.log");
    CHECK (Logs::getCurrentFile().getFileName() == "spinus-2026-02-20_10-00-00.log");
    CHECK (Logs::getCurrentFile().loadFileAsString().contains (
        "The previous run ended unexpectedly: spinus-2026-01-12_10-00-00.log"));

    // This run and the 9 before it; the oldest 3 runs are gone with their AI logs.
    const auto recent = Logs::recentFiles (100);
    CHECK (recent.getFirst() == Logs::getCurrentFile());
    juce::StringArray names;
    for (const auto& f : recent)
        names.add (f.getFileName());
    CHECK (names.contains ("spinus-2026-01-04_10-00-00.log"));
    CHECK (names.contains ("spinus-2026-01-04_10-00-00-ai.log"));
    CHECK (! names.contains ("spinus-2026-01-03_10-00-00.log"));
    CHECK (! scratch.folder.getChildFile ("spinus-2026-01-02_10-00-00-ai.log").exists());
    CHECK (scratch.folder.getChildFile ("notes.txt").existsAsFile());
    CHECK (names.size() == 10 + 5); // 10 runs; 5 of them (02-20 has none yet) with an AI log

    // The newest two runs: this one, then the crashed one with its AI log.
    const auto two = Logs::recentFiles (2);
    REQUIRE (two.size() == 3);
    CHECK (two[1].getFileName() == "spinus-2026-01-12_10-00-00.log");
    CHECK (two[2].getFileName() == "spinus-2026-01-12_10-00-00-ai.log");
    Logs::stop();

    // A clean previous run is no news.
    REQUIRE (Logs::start (optionsFor (scratch.folder, juce::Time (2026, 1, 21, 10, 0))));
    CHECK (Logs::getUnexpectedlyEndedRun() == juce::File());
}

TEST_CASE ("Two runs started in the same second get a log each", "[core][log]")
{
    const ScratchFolder scratch;
    const juce::Time when (2026, 9, 24, 14, 3, 12);
    writeOldRun (scratch.folder, "2026-10-24_14-03-12", true);
    REQUIRE (Logs::start (optionsFor (scratch.folder, when)));
    CHECK (Logs::getCurrentFile().getFileName() == "spinus-2026-10-24_14-03-12-2.log");
    CHECK (Logs::isLogFileName ("spinus-2026-10-24_14-03-12-2-ai.log"));
    CHECK (! Logs::isLogFileName ("spinus-2026-10-24.log"));
    CHECK (! Logs::isLogFileName ("spinus-2026-10-24_14-03-12-x.log"));
    CHECK (Logs::recentFiles (1).getFirst() == Logs::getCurrentFile());
}

TEST_CASE ("A run that still goes does not count as ended unexpectedly", "[core][log]")
{
    const ScratchFolder scratch;
    REQUIRE (Logs::start (optionsFor (scratch.folder, juce::Time (2026, 0, 1, 10, 0))));
    const auto first = Logs::getCurrentFile();
    // Its lock tells a second Spinus that it still runs (two Spinus at once): not reported, not deleted.
    CHECK (Logs::isStillRunning (first));
    Logs::stop();
    CHECK (! Logs::isStillRunning (first));

    // A log without its last line whose run has ended: reported.
    first.replaceWithText ("header\n");
    REQUIRE (Logs::start (optionsFor (scratch.folder, juce::Time (2026, 0, 2, 10, 0))));
    CHECK (Logs::getUnexpectedlyEndedRun() == first);
}

TEST_CASE ("A full log stops growing but still ends cleanly", "[core][log]")
{
    const ScratchFolder scratch;
    auto options = optionsFor (scratch.folder);
    options.maxBytes = 8192;
    REQUIRE (Logs::start (options));
    for (int i = 0; i < 500; ++i)
        Logs::info ("test", "line " + juce::String (i) + " " + juce::String::repeatedString ("x", 40));
    const auto file = Logs::getCurrentFile();
    Logs::stop();
    const auto text = file.loadFileAsString();
    CHECK (file.getSize() < 8192 + 512);
    CHECK (text.contains ("The log reached its size limit"));
    CHECK (text.trimEnd().endsWith ("lines were left out: the log was full.)"));
    CHECK (text.contains (Logs::cleanShutdownLine));
}

TEST_CASE ("Lines from several threads land whole", "[core][log]")
{
    const ScratchFolder scratch;
    REQUIRE (Logs::start (optionsFor (scratch.folder)));
    std::vector<std::thread> threads;
    threads.reserve (4);
    for (int t = 0; t < 4; ++t)
        threads.emplace_back (
            [t]
            {
                for (int i = 0; i < 100; ++i)
                    Logs::info ("thread" + juce::String (t), "line " + juce::String (i) + " end");
            });
    for (auto& t : threads)
        t.join();
    const auto file = Logs::getCurrentFile();
    Logs::stop();
    int lines = 0;
    for (const auto& line : juce::StringArray::fromLines (file.loadFileAsString()))
        if (line.contains (": line "))
        {
            ++lines;
            CHECK (line.endsWith (" end"));
        }
    CHECK (lines == 400);
}

TEST_CASE ("A helper program writes into the log of the Spinus that started it", "[core][log]")
{
    const ScratchFolder scratch;
    REQUIRE (Logs::start (optionsFor (scratch.folder)));
    const auto file = Logs::getCurrentFile();
    Logs::info ("start", "the main program");
    // What a helper program sees: the variable, after Spinus's own log has closed in this test process.
    Logs::stop();
    const auto previous = juce::SystemStats::getEnvironmentVariable (Logs::environmentVariable, {});
#if JUCE_WINDOWS
    _wputenv_s (L"SPINUS_LOG_FILE", file.getFullPathName().toWideCharPointer());
#else
    setenv (Logs::environmentVariable, file.getFullPathName().toRawUTF8(), 1); // NOLINT(concurrency-mt-unsafe)
#endif
    Logs::joinParentLog ("plugin scanner");
    CHECK (Logs::isActive());
    CHECK (Logs::getAiServiceFile() == juce::File()); // a helper program has none of its own
    Logs::warning ("scan", "could not write the result");
    Logs::stop();
#if JUCE_WINDOWS
    _wputenv_s (L"SPINUS_LOG_FILE", previous.toWideCharPointer());
#else
    setenv (Logs::environmentVariable, previous.toRawUTF8(), 1); // NOLINT(concurrency-mt-unsafe)
#endif
    const auto text = file.loadFileAsString();
    CHECK (text.contains (" WARN  [plugin scanner "));
    CHECK (text.contains ("] scan: could not write the result"));
    // The helper's end is not Spinus's: the clean line is Spinus's own, written once.
    CHECK (text.indexOf (Logs::cleanShutdownLine) == text.lastIndexOf (Logs::cleanShutdownLine));
}

TEST_CASE ("Without start() nothing is logged", "[core][log]")
{
    Logs::stop();
    CHECK (! Logs::isActive());
    CHECK (Logs::getCurrentFile() == juce::File());
    CHECK (Logs::getAiServiceFile() == juce::File());
    Logs::warning ("test", "nowhere");
    Logs::writeFromCrashHandler ("nowhere either");
    CHECK (Logs::recentFiles (5, juce::File()).isEmpty());
}

TEST_CASE ("The log's header names the run without personal details", "[core][log]")
{
    const auto header = spinus::session::describeRunForLog (juce::Time::getCurrentTime()).joinIntoString ("\n");
    CHECK (header.contains ("Spinus DAW"));
    CHECK (header.contains ("Package: "));
    CHECK (header.contains ("System: "));
    CHECK (header.contains ("JUCE"));
    CHECK (header.contains ("Tracktion"));
    CHECK (header.contains ("threads"));
    const auto computer = juce::SystemStats::getComputerName();
    if (computer.length() > 3)
        CHECK (! header.contains (computer));
    const auto user = juce::SystemStats::getLogonName();
    if (user.length() > 3)
        CHECK (! header.containsIgnoreCase ("/" + user));
}
