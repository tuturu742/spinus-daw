#include <catch2/catch_test_macros.hpp>

#include "core/ChildProcess.h"
#include "engine/PluginCrashGuard.h"
#include "plugins/SandboxList.h"

using spinus::engine::PluginCrashGuard;

namespace
{
/** Runs the helper and waits for it to end (crashed, as it should). */
void runHelper (const juce::File& record, const juce::String& target)
{
    spinus::core::ChildProcess helper;
    REQUIRE (helper.start (juce::StringArray { SPINUS_CRASH_GUARD_HELPER, record.getFullPathName(), target }, 0));
    REQUIRE (helper.waitForProcessToFinish (30000));
    // The helper's own codes (12 to 127) mean it did not crash. Linux: a process ended by a signal reports
    // 128 + the signal, and AddressSanitizer's handler exits with 1. Windows: an access violation ends it
    // with 0xC0000005, abort() with 3.
#if JUCE_WINDOWS
    CHECK ((helper.getExitCode() == 0xC0000005u || helper.getExitCode() == 3));
#else
    const auto code = helper.getExitCode();
    CHECK ((code == 1 || code > 128));
#endif
}
} // namespace

TEST_CASE ("A crash inside a plugin names the plugin for the next start", "[plugins][sandbox][crash]")
{
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("spinus-crash-guard-" + juce::Uuid().toString());
    folder.createDirectory();
    const auto record = folder.getChildFile ("plugin-crash.json");
    const auto plugin = juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("crash-test.clap").getFullPathName();

    runHelper (record, plugin);
    REQUIRE (record.existsAsFile());
    const auto crash = PluginCrashGuard::takeRecorded (record);
    REQUIRE (crash.has_value());
    CHECK (crash.value_or (PluginCrashGuard::Crash {}).key == spinus::plugins::SandboxList::keyOf ("CLAP", plugin));
    CHECK (crash.value_or (PluginCrashGuard::Crash {}).name == "Crash Test");
    CHECK (! record.exists()); // taken: offered once

    // A crash elsewhere names nothing.
    runHelper (record, "--elsewhere");
    CHECK (! record.exists());
    CHECK (! PluginCrashGuard::takeRecorded (record).has_value());
    folder.deleteRecursively();
}
