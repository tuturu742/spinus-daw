#include "PluginCrashGuard.h"

#include "session/ProgramFolder.h"

#include <csignal>
#include <cstring>

#if JUCE_LINUX || JUCE_BSD
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <ucontext.h>
#include <unistd.h>
#elif JUCE_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

namespace spinus::engine
{

#if JUCE_LINUX || JUCE_BSD || JUCE_WINDOWS
namespace
{
constexpr int maxEntries = 512;

struct Entry
{
    char path[1024] {};
    char line[1536] {}; // what goes into the record: {"key": ..., "name": ...}
    bool own = true;
};

Entry entries[maxEntries];
std::atomic<int> entryCount { 0 };

bool startsWith (const char* text, const char* prefix)
{
#if JUCE_WINDOWS
    // Windows ignores the case of paths.
    return prefix[0] != 0 && _strnicmp (text, prefix, std::strlen (prefix)) == 0;
#else
    return prefix[0] != 0 && std::strncmp (text, prefix, std::strlen (prefix)) == 0;
#endif
}

/** The noted plugin whose library `module` (a path) is. */
const Entry* findEntry (const char* module, bool ownOnly)
{
    if (module == nullptr || module[0] == 0)
        return nullptr;
    const int count = entryCount.load (std::memory_order_acquire);
    for (int i = 0; i < count; ++i)
        if ((entries[i].own || ! ownOnly) && startsWith (module, entries[i].path))
            return &entries[i];
    return nullptr;
}
} // namespace

void PluginCrashGuard::add (const juce::String& libraryPath, const juce::String& key, const juce::String& name,
                            bool own)
{
    const int index = entryCount.load();
    if (index >= maxEntries || libraryPath.isEmpty())
        return;
    for (int i = 0; i < index; ++i)
        if (libraryPath == entries[i].path)
            return;
    auto* o = new juce::DynamicObject();
    o->setProperty ("key", key);
    o->setProperty ("name", name);
    libraryPath.copyToUTF8 (entries[index].path, sizeof (entries[index].path));
    entries[index].own = own;
    juce::JSON::toString (juce::var (o), true).copyToUTF8 (entries[index].line, sizeof (entries[index].line));
    entryCount.store (index + 1, std::memory_order_release);
}
#endif

#if JUCE_LINUX || JUCE_BSD
namespace
{
char recordPath[1024] {};
constexpr int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
struct sigaction previous[std::size (signals)];

const Entry* findEntry (const void* address, bool ownOnly)
{
    Dl_info info {};
    if (address == nullptr || dladdr (address, &info) == 0 || info.dli_fname == nullptr)
        return nullptr;
    return findEntry (info.dli_fname, ownOnly);
}

void handler (int signal, siginfo_t* info, void* context)
{
    // Where it crashed, then the frames that called it: a plugin's own library first, then any it loaded.
    const void* at = nullptr;
#if defined(__x86_64__)
    if (context != nullptr)
        // NOLINTNEXTLINE(performance-no-int-to-ptr): the instruction pointer, as an address
        at = (const void*)static_cast<ucontext_t*> (context)->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    if (context != nullptr)
        // NOLINTNEXTLINE(performance-no-int-to-ptr): the instruction pointer, as an address
        at = (const void*)static_cast<ucontext_t*> (context)->uc_mcontext.pc;
#endif
    void* frames[64];
    const int n = backtrace (frames, 64);
    const Entry* found = nullptr;
    for (const bool ownOnly : { true, false })
    {
        found = findEntry (at, ownOnly);
        for (int i = 0; i < n && found == nullptr; ++i)
            found = findEntry (frames[i], ownOnly);
        if (found != nullptr)
            break;
    }
    if (found != nullptr && recordPath[0] != 0)
    {
        const int fd = open (recordPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0)
        {
            [[maybe_unused]] const auto written = write (fd, found->line, std::strlen (found->line));
            close (fd);
        }
    }

    // Carry on as without this handler: the previous one, or the default (a core dump, an exit).
    for (size_t i = 0; i < std::size (signals); ++i)
        if (signals[i] == signal)
        {
            sigaction (signal, &previous[i], nullptr);
            // NOLINTNEXTLINE(bugprone-signed-bitwise): sigaction's flags are an int
            if ((previous[i].sa_flags & SA_SIGINFO) != 0 && previous[i].sa_sigaction != nullptr)
                return previous[i].sa_sigaction (signal, info, context);
            if (previous[i].sa_handler != SIG_DFL && previous[i].sa_handler != SIG_IGN &&
                previous[i].sa_handler != nullptr)
                return previous[i].sa_handler (signal);
        }
    raise (signal);
}

int collectModule (dl_phdr_info* info, size_t, void* data)
{
    if (info->dlpi_name != nullptr && info->dlpi_name[0] != 0)
        static_cast<std::set<std::string>*> (data)->insert (info->dlpi_name);
    return 0;
}
} // namespace

void PluginCrashGuard::install (const juce::File& record)
{
    static bool installed = false;
    record.getFullPathName().copyToUTF8 (recordPath, sizeof (recordPath));
    if (installed)
        return;
    installed = true;

    void* prime[1];
    backtrace (prime, 1); // the first call loads what it needs; not safe inside a handler
    struct sigaction action {};
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset (&action.sa_mask);
    for (size_t i = 0; i < std::size (signals); ++i)
        sigaction (signals[i], &action, &previous[i]);
}

PluginCrashGuard::ModuleWatch::ModuleWatch() { dl_iterate_phdr (collectModule, &before); }

void PluginCrashGuard::ModuleWatch::addNewModules (const juce::String& key, const juce::String& name,
                                                   const juce::String& pluginFile) const
{
    std::set<std::string> after;
    dl_iterate_phdr (collectModule, &after);
    for (auto& module : after)
        if (! before.contains (module))
        {
            const juce::String path (module);
            // In the Flatpak the system's plugins load from /run/host/usr, kept in projects as /usr.
            add (path, key, name,
                 juce::File::isAbsolutePath (pluginFile) &&
                     toPortablePluginPath (path).startsWith (toPortablePluginPath (pluginFile)));
        }
}

#elif JUCE_WINDOWS
namespace
{
wchar_t recordPath[1024] {};
LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
void (*previousAbort) (int) = nullptr;

/** The path (UTF-8) of the module `address` is in; false if none. Safe enough after a crash: no heap. */
bool moduleOf (const void* address, char* path, int size)
{
    HMODULE module = nullptr;
    if (address == nullptr ||
        ! GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR> (address), &module))
        return false;
    wchar_t wide[1024];
    const auto length = GetModuleFileNameW (module, wide, 1024);
    return length > 0 && WideCharToMultiByte (CP_UTF8, 0, wide, (int)length + 1, path, size, nullptr, nullptr) > 0;
}

const Entry* findEntry (const void* address, bool ownOnly)
{
    char path[2048];
    return moduleOf (address, path, sizeof (path)) ? findEntry (path, ownOnly) : nullptr;
}

/** Where it crashed, then the frames that led there (the walk starts in this handler and goes back through
    Windows's exception dispatch into the crashed code): a plugin's own library first, then any it loaded. */
void noteCrash (const void* at)
{
    void* frames[62];
    const auto n = CaptureStackBackTrace (0, 62, frames, nullptr);
    const Entry* found = nullptr;
    for (const bool ownOnly : { true, false })
    {
        found = findEntry (at, ownOnly);
        for (int i = 0; i < (int)n && found == nullptr; ++i)
            found = findEntry (frames[i], ownOnly);
        if (found != nullptr)
            break;
    }
    if (found == nullptr || recordPath[0] == 0)
        return;
    const auto file =
        CreateFileW (recordPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile (file, found->line, (DWORD)std::strlen (found->line), &written, nullptr);
    CloseHandle (file);
}

LONG WINAPI onCrash (EXCEPTION_POINTERS* exception)
{
    noteCrash (exception != nullptr && exception->ExceptionRecord != nullptr
                   ? exception->ExceptionRecord->ExceptionAddress
                   : nullptr);
    return previousFilter != nullptr ? previousFilter (exception) : EXCEPTION_CONTINUE_SEARCH;
}

void onAbort (int signal)
{
    noteCrash (nullptr);
    std::signal (signal, previousAbort != nullptr && previousAbort != SIG_ERR ? previousAbort : SIG_DFL);
    std::raise (signal);
}

void collectModules (std::set<std::string>& into)
{
    HMODULE modules[1024];
    DWORD bytes = 0;
    if (! EnumProcessModules (GetCurrentProcess(), modules, sizeof (modules), &bytes))
        return;
    for (DWORD i = 0; i < bytes / sizeof (HMODULE) && i < 1024; ++i)
    {
        wchar_t path[1024];
        if (const auto length = GetModuleFileNameW (modules[i], path, 1024); length > 0)
            into.insert (juce::String (path, (size_t)length).toStdString());
    }
}
} // namespace

void PluginCrashGuard::install (const juce::File& record)
{
    static bool installed = false;
    record.getFullPathName().copyToUTF16 (reinterpret_cast<juce::CharPointer_UTF16::CharType*> (recordPath),
                                          sizeof (recordPath));
    if (installed)
        return;
    installed = true;
    previousFilter = SetUnhandledExceptionFilter (onCrash);
    previousAbort = std::signal (SIGABRT, onAbort);
}

PluginCrashGuard::ModuleWatch::ModuleWatch() { collectModules (before); }

void PluginCrashGuard::ModuleWatch::addNewModules (const juce::String& key, const juce::String& name,
                                                   const juce::String& pluginFile) const
{
    std::set<std::string> after;
    collectModules (after);
    for (auto& module : after)
        if (! before.contains (module))
        {
            const juce::String path (juce::CharPointer_UTF8 (module.c_str()));
            add (path, key, name, juce::File::isAbsolutePath (pluginFile) && path.startsWithIgnoreCase (pluginFile));
        }
}

#else
void PluginCrashGuard::install (const juce::File&) {}
void PluginCrashGuard::add (const juce::String&, const juce::String&, const juce::String&, bool) {}
PluginCrashGuard::ModuleWatch::ModuleWatch() {}
void PluginCrashGuard::ModuleWatch::addNewModules (const juce::String&, const juce::String&, const juce::String&) const
{
}
#endif

std::optional<PluginCrashGuard::Crash> PluginCrashGuard::takeRecorded (const juce::File& record)
{
    if (! record.existsAsFile())
        return std::nullopt;
    const auto json = juce::JSON::parse (record);
    record.deleteFile();
    if (json["key"].toString().isEmpty())
        return std::nullopt;
    return Crash { json["key"].toString(), json["name"].toString() };
}

} // namespace spinus::engine
