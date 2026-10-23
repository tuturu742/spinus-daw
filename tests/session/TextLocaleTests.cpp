#include <catch2/catch_test_macros.hpp>

#include "session/TextLocale.h"

#include <juce_core/juce_core.h>

#include <cstdlib>

using namespace spinus;

#if JUCE_LINUX
TEST_CASE ("A language setting that isn't installed falls back to UTF-8 for the programs Spinus starts",
           "[session][locale]")
{
    const auto* before = std::getenv ("LC_ALL");
    const juce::String saved = before != nullptr ? before : "";
    const auto restore = [&]
    {
        if (before != nullptr)
            setenv ("LC_ALL", saved.toRawUTF8(), 1);
        else
            unsetenv ("LC_ALL");
    };

    // Not installed anywhere: the file chooser would print "???" for Cyrillic names.
    setenv ("LC_ALL", "xx_XX.UTF-8", 1);
    CHECK (session::ensureUtf8ForChildProcesses());
    CHECK (juce::String (std::getenv ("LC_ALL")) == "C.UTF-8");

    // Usable now: nothing changes.
    CHECK (! session::ensureUtf8ForChildProcesses());
    CHECK (juce::String (std::getenv ("LC_ALL")) == "C.UTF-8");

    // Plain ASCII "C" also gets UTF-8.
    setenv ("LC_ALL", "C", 1);
    CHECK (session::ensureUtf8ForChildProcesses());

    // Spinus's own locale stays "C" (number formatting).
    CHECK (juce::String (std::setlocale (LC_ALL, nullptr)) == "C");
    restore();
}
#endif
