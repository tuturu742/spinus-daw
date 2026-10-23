#include "TextLocale.h"

#include <clocale>
#include <cstdlib>
#include <cstring>

namespace spinus::session
{

bool ensureUtf8ForChildProcesses()
{
#if defined(__linux__)
    // The language setting from the environment (LC_ALL, LC_CTYPE, LANG), as child processes will see it.
    const auto* found = std::setlocale (LC_CTYPE, "");
    const bool usable = found != nullptr && std::strcmp (found, "C") != 0 && std::strcmp (found, "POSIX") != 0;
    std::setlocale (LC_CTYPE, "C");
    if (usable || std::setlocale (LC_CTYPE, "C.UTF-8") == nullptr)
    {
        std::setlocale (LC_CTYPE, "C");
        return false;
    }
    std::setlocale (LC_CTYPE, "C");
    setenv ("LC_ALL", "C.UTF-8", 1); // NOLINT(concurrency-mt-unsafe): called at startup, before other threads
    return true;
#else
    return false;
#endif
}

} // namespace spinus::session
