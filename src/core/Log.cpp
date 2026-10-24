#include "Log.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <mutex>
#include <string_view>

#if JUCE_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdlib>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace spinus::core
{

//==============================================================================
// Redaction. The text is handled as UTF-8 bytes: every rule looks for ASCII characters, which never occur
// inside another character's bytes.
namespace
{
bool isAsciiAlnum (char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool isAsciiLetter (char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

char lowerAscii (char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

bool equalsIgnoreCaseAt (const std::string& s, size_t pos, std::string_view word)
{
    if (pos + word.size() > s.size())
        return false;
    for (size_t i = 0; i < word.size(); ++i)
        if (lowerAscii (s[pos + i]) != lowerAscii (word[i]))
            return false;
    return true;
}

size_t findFrom (const std::string& s, std::string_view what, size_t from, bool ignoreCase)
{
    if (! ignoreCase)
        return s.find (what, from);
    for (size_t i = from; i + what.size() <= s.size(); ++i)
        if (equalsIgnoreCaseAt (s, i, what))
            return i;
    return std::string::npos;
}

bool isSeparator (char c) { return c == '/' || c == '\\'; }

/** Where a path's folder name (or a word) ends: a separator, a quote, white space, punctuation that closes. */
bool endsName (char c)
{
    return isSeparator (c) || std::string_view (" \t\r\n\"'<>:;,)]}|*?").find (c) != std::string_view::npos;
}

/** After a home folder's path: the path ends, or goes on below it (not "/home/alice2"). */
bool isPathBoundary (const std::string& s, size_t pos)
{
    if (pos >= s.size())
        return true;
    const auto c = s[pos];
    if (c == '.') // the end of a sentence, not a longer name
        return pos + 1 >= s.size() || s[pos + 1] == ' ' || s[pos + 1] == '\n';
    if ((unsigned char)c >= 0x80)
        return false;
    return ! isAsciiAlnum (c) && c != '_' && c != '-';
}

bool isTokenChar (char c) { return isAsciiAlnum (c) || c == '_' || c == '-'; }
bool isBearerChar (char c) { return isTokenChar (c) || std::string_view ("._~+/=").find (c) != std::string_view::npos; }

constexpr std::string_view masked = "***";
constexpr std::string_view someUser = "<user>";

/** A key that starts with `prefix` (sk-ant-): the rest is masked, if at least `minimum` key characters follow. */
void maskPrefixedKeys (std::string& s, std::string_view prefix, size_t minimum)
{
    for (size_t at = s.find (prefix); at != std::string::npos; at = s.find (prefix, at + 1))
    {
        if (at > 0 && isAsciiAlnum (s[at - 1]))
            continue; // "task-..." or "disk-..."
        const auto start = at + prefix.size();
        auto end = start;
        while (end < s.size() && isTokenChar (s[end]))
            ++end;
        if (end - start >= minimum)
            s.replace (start, end - start, masked);
    }
}

/** "Bearer <token>" (any case): the token. */
void maskBearer (std::string& s)
{
    for (size_t at = findFrom (s, "bearer", 0, true); at != std::string::npos;
         at = findFrom (s, "bearer", at + 1, true))
    {
        if (at > 0 && isAsciiAlnum (s[at - 1]))
            continue;
        auto start = at + 6;
        if (start >= s.size() || (s[start] != ' ' && s[start] != '\t'))
            continue;
        while (start < s.size() && (s[start] == ' ' || s[start] == '\t'))
            ++start;
        auto end = start;
        while (end < s.size() && isBearerChar (s[end]))
            ++end;
        if (end > start && std::string_view (s).substr (start, end - start) != masked)
            s.replace (start, end - start, masked);
    }
}

/** `api_key: value`, `token=value`, `"password": "value"`: the value. */
void maskNamedValues (std::string& s)
{
    static constexpr std::string_view names[] = { "api_key",      "api-key",       "apikey", "authorization",
                                                  "access_token", "refresh_token", "token",  "client_secret",
                                                  "secret",       "password",      "passwd" };
    for (const auto name : names)
        for (size_t at = findFrom (s, name, 0, true); at != std::string::npos; at = findFrom (s, name, at + 1, true))
        {
            if (at > 0 && isAsciiAlnum (s[at - 1]))
                continue; // "mytoken"; "_token" and "-key" count (access_token, x-api-key)
            auto i = at + name.size();
            if (i < s.size() && (s[i] == '"' || s[i] == '\''))
                ++i;
            while (i < s.size() && s[i] == ' ')
                ++i;
            if (i >= s.size() || (s[i] != ':' && s[i] != '='))
                continue; // "token file", "tokens: 120"
            ++i;
            while (i < s.size() && s[i] == ' ')
                ++i;
            if (i < s.size() && (s[i] == '"' || s[i] == '\''))
                ++i;
            // "Authorization: Bearer x" or "Basic x": the credentials after the scheme.
            for (const auto scheme : { std::string_view ("bearer "), std::string_view ("basic ") })
                if (equalsIgnoreCaseAt (s, i, scheme))
                {
                    i += scheme.size();
                    while (i < s.size() && s[i] == ' ')
                        ++i;
                }
            auto end = i;
            while (end < s.size() && std::string_view (" \t\r\n\"',;&})").find (s[end]) == std::string_view::npos)
                ++end;
            if (end > i && std::string_view (s).substr (i, end - i) != masked)
                s.replace (i, end - i, masked);
        }
}

/** /home/<name>/...: the name, unless it is <user> already. */
void maskHomeFolders (std::string& s)
{
    for (size_t at = s.find ("/home/"); at != std::string::npos; at = s.find ("/home/", at + 1))
    {
        const auto start = at + 6;
        auto end = start;
        while (end < s.size() && ! endsName (s[end]))
            ++end;
        if (end > start && std::string_view (s).substr (start, end - start) != someUser)
            s.replace (start, end - start, someUser);
    }
}

/** C:\Users\<name> (any drive, / or \ or \\, any case) and a Wine prefix's drive_c/users/<name>: the name.
    Windows's shared folders (Public, Default, All Users) are not anyone's. */
void maskWindowsUserFolders (std::string& s)
{
    for (size_t at = findFrom (s, "users", 0, true); at != std::string::npos; at = findFrom (s, "users", at + 1, true))
    {
        // Before: separators, then a drive ("C:") or drive_c.
        auto before = at;
        while (before > 0 && isSeparator (s[before - 1]))
            --before;
        if (before == at)
            continue;
        const bool drive = before >= 2 && s[before - 1] == ':' && isAsciiLetter (s[before - 2]);
        const bool wine = before >= 7 && equalsIgnoreCaseAt (s, before - 7, "drive_c");
        if (! drive && ! wine)
            continue;
        // After: separators, then the name.
        auto start = at + 5;
        const auto separators = start;
        while (start < s.size() && isSeparator (s[start]))
            ++start;
        if (start == separators)
            continue;
        // A name may have spaces ("C:\Users\Jo Ann\Music"), as far as the next separator; without one the name
        // ends at a space.
        auto end = start;
        while (end < s.size() && (! endsName (s[end]) || s[end] == ' '))
            ++end;
        if (end >= s.size() || ! isSeparator (s[end]))
            if (const auto space = s.find (' ', start); space < end)
                end = space;
        const auto name = std::string_view (s).substr (start, end - start);
        if (name.empty() || name == someUser)
            continue;
        if (name == "Public" || name == "Default" || name == "All Users" || name == "public")
            continue;
        s.replace (start, end - start, someUser);
    }
}

void maskEmails (std::string& s)
{
    const auto local = [] (char c)
    { return isAsciiAlnum (c) || std::string_view ("._%+-").find (c) != std::string_view::npos; };
    const auto domain = [] (char c) { return isAsciiAlnum (c) || c == '.' || c == '-'; };
    for (size_t at = s.find ('@'); at != std::string::npos; at = s.find ('@', at + 1))
    {
        auto start = at;
        while (start > 0 && local (s[start - 1]))
            --start;
        auto end = at + 1;
        while (end < s.size() && domain (s[end]))
            ++end;
        while (end > at + 1 && (s[end - 1] == '.' || s[end - 1] == '-'))
            --end; // the end of a sentence
        if (start == at || end <= at + 1)
            continue;
        const auto host = std::string_view (s).substr (at + 1, end - at - 1);
        const auto dot = host.rfind ('.');
        if (dot == std::string_view::npos || dot == 0 || host.size() - dot - 1 < 2)
            continue;
        bool letters = true;
        for (auto c : host.substr (dot + 1))
            letters = letters && isAsciiLetter (c);
        if (! letters)
            continue; // "plugin@1.2.30"
        s.replace (start, end - start, "<email>");
        at = start;
    }
}

bool hasDriveLetter (const std::string& path) { return path.size() >= 2 && path[1] == ':' && isAsciiLetter (path[0]); }

void replaceHome (std::string& s, const std::string& home)
{
    const bool ignoreCase = hasDriveLetter (home); // Windows ignores the case of paths
    for (size_t at = findFrom (s, home, 0, ignoreCase); at != std::string::npos;
         at = findFrom (s, home, at + 1, ignoreCase))
    {
        if (at > 0 && (isAsciiAlnum (s[at - 1]) || std::string_view ("_-.").find (s[at - 1]) != std::string_view::npos))
            continue; // inside a longer path ("/mnt/home/alice")
        if (! isPathBoundary (s, at + home.size()))
            continue;
        s.replace (at, home.size(), "~");
    }
}
} // namespace

LogRedactor::LogRedactor (const juce::StringArray& homeFolders)
{
    for (auto home : homeFolders)
    {
        home = home.trimCharactersAtEnd ("/\\");
        if (home.length() < 3) // "/" or "C:" would mask every path
            continue;
        const auto forward = home.replaceCharacter ('\\', '/'), backward = home.replaceCharacter ('/', '\\');
        for (const auto& spelling : { home, forward, backward, backward.replace ("\\", "\\\\") })
            if (const auto text = spelling.toStdString(); std::find (homes.begin(), homes.end(), text) == homes.end())
                homes.push_back (text);
    }
    // Longest first: the JSON spelling contains no shorter one, but a home's link target may contain the home.
    std::sort (homes.begin(), homes.end(), [] (const auto& a, const auto& b) { return a.size() > b.size(); });
}

LogRedactor LogRedactor::forThisComputer()
{
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    juce::StringArray homes { home.getFullPathName() };
    if (home.isSymbolicLink())
        homes.add (home.getLinkedTarget().getFullPathName());
    return LogRedactor (homes);
}

void LogRedactor::addSecret (const juce::String& secret)
{
    const auto text = secret.trim().toStdString();
    if (text.size() >= 8 && std::find (secrets.begin(), secrets.end(), text) == secrets.end())
        secrets.push_back (text);
}

juce::String LogRedactor::apply (const juce::String& text) const
{
    auto s = text.toStdString();
    for (const auto& secret : secrets)
        for (auto at = s.find (secret); at != std::string::npos; at = s.find (secret, at + masked.size()))
            s.replace (at, secret.size(), masked);
    maskPrefixedKeys (s, "sk-ant-", 1);
    maskPrefixedKeys (s, "sk-or-", 1);
    maskPrefixedKeys (s, "sk-", 20); // OpenAI and others (sk-proj-...)
    maskBearer (s);
    maskNamedValues (s);
    for (const auto& home : homes)
        replaceHome (s, home);
    maskHomeFolders (s);
    maskWindowsUserFolders (s);
    maskEmails (s);
    return juce::String::fromUTF8 (s.data(), (int)s.size());
}

//==============================================================================
// The file: appended to with one system call per line, so several processes can write it (each line lands
// whole), and nothing waits in a buffer when Spinus crashes.
namespace
{
#if JUCE_WINDOWS
using Native = HANDLE;
const Native noFile = INVALID_HANDLE_VALUE;

Native openForAppending (const juce::File& f, bool create)
{
    return CreateFileW (f.getFullPathName().toWideCharPointer(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void closeFile (Native f)
{
    if (f != noFile)
        CloseHandle (f);
}

bool writeAll (Native f, const char* data, size_t size)
{
    while (size > 0)
    {
        DWORD written = 0;
        if (! WriteFile (f, data, (DWORD)juce::jmin (size, (size_t)1 << 20), &written, nullptr) || written == 0)
            return false;
        data += written;
        size -= written;
    }
    return true;
}

juce::int64 sizeOf (Native f)
{
    LARGE_INTEGER size {};
    return GetFileSizeEx (f, &size) ? (juce::int64)size.QuadPart : 0;
}

// The lock that says "this run is still going": a byte far past the end of the file, which no read or write
// touches. Windows releases it when the process ends, however it ends.
OVERLAPPED lockRegion()
{
    OVERLAPPED o {};
    o.Offset = 0;
    o.OffsetHigh = 0x40000000;
    return o;
}

Native takeRunLock (const juce::File& f)
{
    auto h = CreateFileW (f.getFullPathName().toWideCharPointer(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == noFile)
        return noFile;
    auto o = lockRegion();
    if (! LockFileEx (h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &o))
    {
        CloseHandle (h);
        return noFile;
    }
    return h;
}

void releaseRunLock (Native h) { closeFile (h); }

bool isRunStillGoing (const juce::File& f)
{
    const auto h = takeRunLock (f);
    if (h == noFile)
        return f.existsAsFile();
    auto o = lockRegion();
    UnlockFileEx (h, 0, 1, 0, &o);
    CloseHandle (h);
    return false;
}

void setEnvironment (const char* name, const juce::String& value)
{
    _wputenv_s (juce::String (name).toWideCharPointer(), value.toWideCharPointer());
}

juce::String getEnvironment (const char* name) { return juce::SystemStats::getEnvironmentVariable (name, {}); }

int processId() { return (int)GetCurrentProcessId(); }
#else
using Native = int;
const Native noFile = -1;

Native openForAppending (const juce::File& f, bool create)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2)
    return ::open (f.getFullPathName().toRawUTF8(), O_WRONLY | O_APPEND | O_CLOEXEC | (create ? O_CREAT : 0), 0600);
}

void closeFile (Native f)
{
    if (f != noFile)
        ::close (f);
}

bool writeAll (Native f, const char* data, size_t size)
{
    while (size > 0)
    {
        const auto n = ::write (f, data, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n;
        size -= (size_t)n;
    }
    return true;
}

juce::int64 sizeOf (Native f)
{
    struct stat st {};
    return ::fstat (f, &st) == 0 ? (juce::int64)st.st_size : 0;
}

// The lock that says "this run is still going" (flock: released when the process ends, however it ends).
Native takeRunLock (const juce::File& f)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2)
    const int fd = ::open (f.getFullPathName().toRawUTF8(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return noFile;
    if (::flock (fd, LOCK_EX | LOCK_NB) != 0)
    {
        ::close (fd);
        return noFile;
    }
    return fd;
}

void releaseRunLock (Native fd) { closeFile (fd); }

bool isRunStillGoing (const juce::File& f)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2)
    const int fd = ::open (f.getFullPathName().toRawUTF8(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool locked = ::flock (fd, LOCK_SH | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    ::close (fd);
    return locked;
}

void setEnvironment (const char* name, const juce::String& value)
{
    ::setenv (name, value.toRawUTF8(), 1); // NOLINT(concurrency-mt-unsafe): at start, before helpers start
}

juce::String getEnvironment (const char* name) { return juce::SystemStats::getEnvironmentVariable (name, {}); }

int processId() { return (int)::getpid(); }
#endif

struct State
{
    std::mutex lock;
    bool active = false, child = false, full = false;
    Native file = noFile, runLock = noFile;
    juce::File folder, current, aiFile, unexpected;
    juce::int64 maxBytes = juce::int64 { 4194304 };
    int dropped = 0;
    juce::String label; // a helper program's "[plugin host 4242] "
    LogRedactor redactor = LogRedactor::forThisComputer();
};

State& state()
{
    static auto* s = new State(); // never destroyed: lines may still come while statics are torn down
    return *s;
}

std::atomic<std::intptr_t> crashFile { (std::intptr_t)-1 };

constexpr int maxLineLength = 4000;

const char* levelName (Logs::Level level)
{
    switch (level)
    {
    case Logs::Level::warning:
        return "WARN ";
    case Logs::Level::error:
        return "ERROR";
    case Logs::Level::info:
        break;
    }
    return "info ";
}

juce::String timestamp()
{
    const auto now = juce::Time::getCurrentTime();
    return now.formatted ("%Y-%m-%d %H:%M:%S") + "." + juce::String (now.getMilliseconds()).paddedLeft ('0', 3);
}

/** The run's name in a log's file name: spinus-2026-10-24_14-03-12 (or ...-2 when two started in the same
    second), without .log and -ai; empty if it is no log of a run. */
juce::String runOf (const juce::String& fileName)
{
    if (! fileName.startsWith ("spinus-") || ! fileName.endsWith (".log"))
        return {};
    auto stem = fileName.dropLastCharacters (4);
    if (stem.endsWith ("-ai"))
        stem = stem.dropLastCharacters (3);
    // spinus-YYYY-MM-DD_HH-MM-SS
    constexpr const char* pattern = "spinus-0000-00-00_00-00-00";
    const int length = (int)std::strlen (pattern);
    if (stem.length() < length)
        return {};
    for (int i = 0; i < length; ++i)
        if (pattern[i] == '0' ? ! juce::CharacterFunctions::isDigit (stem[i]) : stem[i] != (juce::juce_wchar)pattern[i])
            return {};
    // A second run in the same second: -2, -3...
    if (const auto rest = stem.substring (length);
        rest.isNotEmpty() && (rest.length() < 2 || rest[0] != '-' || ! rest.substring (1).containsOnly ("0123456789")))
        return {};
    return stem;
}

/** The runs in `folder`, newest first. */
juce::StringArray runsIn (const juce::File& folder)
{
    juce::StringArray runs;
    for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, "spinus-*.log"))
        if (const auto run = runOf (f.getFileName()); run.isNotEmpty())
            runs.addIfNotAlreadyThere (run);
    runs.sort (false);
    std::reverse (runs.begin(), runs.end());
    return runs;
}

bool endedCleanly (const juce::File& log)
{
    juce::FileInputStream in (log);
    if (! in.openedOk())
        return true;
    const auto tail = juce::jmin ((juce::int64)16384, in.getTotalLength());
    in.setPosition (in.getTotalLength() - tail);
    return in.readEntireStreamAsString().contains (Logs::cleanShutdownLine) ||
           in.getTotalLength() == 0; // nothing at all: a run too short to say anything
}

/** Writes one line (with the lock held). */
void writeLine (State& s, const juce::String& text, bool always)
{
    if (s.file == noFile)
        return;
    const auto line = s.redactor.apply (text) + "\n";
    const auto* utf8 = line.toRawUTF8();
    const auto size = std::strlen (utf8);
    if (! always && sizeOf (s.file) + (juce::int64)size > s.maxBytes)
    {
        if (! s.full && ! s.child)
        {
            const auto note = timestamp() + " WARN  The log reached its size limit (" +
                              juce::String (s.maxBytes / (juce::int64 { 1024 } * 1024)) +
                              " MB): later lines are left out, apart from the last one.\n";
            writeAll (s.file, note.toRawUTF8(), std::strlen (note.toRawUTF8()));
        }
        s.full = true;
        ++s.dropped;
        return;
    }
    writeAll (s.file, utf8, size);
}

juce::String formatLine (const State& s, Logs::Level level, const juce::String& area, const juce::String& text)
{
    auto body = text.trimEnd().replace ("\r\n", "\n").replace ("\r", "\n");
    if (body.length() > maxLineLength)
        body = body.substring (0, maxLineLength) + " ... (cut)";
    body = body.replace ("\n", "\n    ");
    return timestamp() + " " + levelName (level) + " " + s.label + (area.isNotEmpty() ? area + ": " : juce::String()) +
           body;
}

[[noreturn]] void onTerminate()
{
    juce::String what = "std::terminate was called without an exception";
    if (const auto e = std::current_exception())
    {
        what = "an exception that is not a std::exception";
        try
        {
            std::rethrow_exception (e);
        }
        catch (const std::exception& x)
        {
            what = juce::String ("a C++ exception: ") + x.what();
        }
        catch (...) // NOLINT(bugprone-empty-catch): nothing more to tell
        {
        }
    }
    Logs::error ("crash", "Ending because nothing caught " + what);
    std::abort();
}
} // namespace

//==============================================================================
bool Logs::start (const Options& options)
{
    auto& s = state();
    const std::scoped_lock lock (s.lock);
    if (s.active || ! options.folder.createDirectory())
        return false;

    // The previous run (the newest log): did it end cleanly? A Spinus that still runs (two at once) holds
    // its log's lock.
    juce::File unexpected;
    if (const auto runs = runsIn (options.folder); ! runs.isEmpty())
        if (const auto previous = options.folder.getChildFile (runs[0] + ".log");
            previous.existsAsFile() && ! endedCleanly (previous) && ! isRunStillGoing (previous))
            unexpected = previous;

    // This run's file: named by its start time; a second Spinus started in the same second adds a number.
    const auto name = "spinus-" + options.startTime.formatted ("%Y-%m-%d_%H-%M-%S");
    auto file = options.folder.getChildFile (name + ".log");
    for (int i = 2; file.exists() && i < 100; ++i)
        file = options.folder.getChildFile (name + "-" + juce::String (i) + ".log");
    s.file = openForAppending (file, true);
    if (s.file == noFile)
        return false;
    s.runLock = takeRunLock (file);

    // Keep the newest runs (this one among them); a run that still goes is never deleted.
    const auto runs = runsIn (options.folder);
    for (int i = juce::jmax (1, options.keepRuns); i < runs.size(); ++i)
        for (const auto& suffix : { ".log", "-ai.log" })
            if (const auto old = options.folder.getChildFile (runs[i] + suffix);
                old != file && old.existsAsFile() && ! isRunStillGoing (old))
                old.deleteFile();

    s.active = true;
    s.child = s.full = false;
    s.dropped = 0;
    s.label = {};
    s.folder = options.folder;
    s.current = file;
    s.aiFile = file.getSiblingFile (file.getFileNameWithoutExtension() + "-ai.log");
    s.unexpected = unexpected;
    s.maxBytes = juce::jmax ((juce::int64)4096, options.maxBytes);
    crashFile = (std::intptr_t)s.file;

    // Helper programs started from now on append to this file (joinParentLog).
    setEnvironment (environmentVariable, file.getFullPathName());

    for (const auto& line : options.header)
        writeLine (s, line, true);
    writeLine (s, "Log folder: " + options.folder.getFullPathName(), true);
    writeLine (s, juce::String::repeatedString ("-", 72), true);
    if (unexpected != juce::File())
        writeLine (
            s,
            formatLine (s, Level::warning, "start", "The previous run ended unexpectedly: " + unexpected.getFileName()),
            false);
    return true;
}

void Logs::stop()
{
    auto& s = state();
    const std::scoped_lock lock (s.lock);
    if (! s.active)
        return;
    if (! s.child)
    {
        auto line = juce::String (cleanShutdownLine);
        if (s.dropped > 0)
            line << " (" << s.dropped << " lines were left out: the log was full.)";
        writeLine (s, formatLine (s, Level::info, "quit", line), true);
        setEnvironment (environmentVariable, {});
    }
    crashFile = (std::intptr_t)-1;
    closeFile (s.file);
    releaseRunLock (s.runLock);
    s.file = s.runLock = noFile;
    s.active = false;
    s.current = s.aiFile = juce::File();
}

void Logs::joinParentLog (const juce::String& label)
{
    const auto path = getEnvironment (environmentVariable);
    if (! juce::File::isAbsolutePath (path))
        return;
    auto& s = state();
    const std::scoped_lock lock (s.lock);
    if (s.active)
        return;
    const juce::File file (path);
    if (! file.existsAsFile())
        return;
    s.file = openForAppending (file, false);
    if (s.file == noFile)
        return;
    s.active = s.child = true;
    s.label = "[" + label + " " + juce::String (processId()) + "] ";
    s.folder = file.getParentDirectory();
    s.current = file;
    crashFile = (std::intptr_t)s.file;
}

bool Logs::isActive()
{
    auto& s = state();
    const std::scoped_lock lock (s.lock);
    return s.active;
}

void Logs::write (Level level, const juce::String& area, const juce::String& text)
{
    auto& s = state();
    const std::scoped_lock lock (s.lock);
    if (s.active)
        writeLine (s, formatLine (s, level, area, text), false);
}

void Logs::addSecret (const juce::String& secret)
{
    auto& s = state();
    const std::scoped_lock lock (s.lock);
    s.redactor.addSecret (secret);
}

juce::String Logs::redact (const juce::String& text)
{
    auto& s = state();
    const std::scoped_lock lock (s.lock);
    return s.redactor.apply (text);
}

juce::File Logs::getFolder()
{
    auto& s = state();
    const std::scoped_lock lock (s.lock);
    return s.folder;
}

juce::File Logs::getCurrentFile()
{
    auto& s = state();
    const std::scoped_lock lock (s.lock);
    return s.current;
}

juce::File Logs::getAiServiceFile()
{
    auto& s = state();
    const std::scoped_lock lock (s.lock);
    return s.active && ! s.child ? s.aiFile : juce::File();
}

juce::File Logs::getUnexpectedlyEndedRun()
{
    auto& s = state();
    const std::scoped_lock lock (s.lock);
    return s.unexpected;
}

juce::Array<juce::File> Logs::recentFiles (int runs, const juce::File& folder)
{
    juce::Array<juce::File> files;
    if (folder == juce::File())
        return files;
    const auto all = runsIn (folder);
    for (int i = 0; i < juce::jmin (runs, all.size()); ++i)
        for (const auto& suffix : { ".log", "-ai.log" })
            if (const auto f = folder.getChildFile (all[i] + suffix); f.existsAsFile())
                files.add (f);
    return files;
}

bool Logs::isStillRunning (const juce::File& log) { return isRunStillGoing (log); }

bool Logs::isLogFileName (const juce::String& fileName) { return runOf (fileName).isNotEmpty(); }

void Logs::writeFromCrashHandler (const char* text) noexcept
{
    const auto f = crashFile.load();
    if (f == (std::intptr_t)-1 || text == nullptr)
        return;
    const auto file = (Native)f; // NOLINT(performance-no-int-to-ptr): the handle, kept as a number
    constexpr const char prefix[] = "CRASH ";
    writeAll (file, prefix, sizeof (prefix) - 1);
    writeAll (file, text, std::strlen (text));
    writeAll (file, "\n", 1);
}

void Logs::installTerminateHandler() { std::set_terminate (onTerminate); }

} // namespace spinus::core
