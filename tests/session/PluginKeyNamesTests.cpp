#include <catch2/catch_test_macros.hpp>

#include "session/PluginKeyNames.h"

using namespace spinus;
using Job = InstrumentProfiles::Job;

TEST_CASE ("Plugin key names: plain note names say nothing", "[instruments][pluginkeys]")
{
    for (const auto* name : { "C3", "C#-1", "Db5", "g9", "F#10", " A0 " })
        CHECK (pluginkeys::isNoteName (name));
    for (const auto* name : { "Kick", "C", "Crash", "Bass Drum 1", "C3 Kick", "A0b", "" })
        CHECK_FALSE (pluginkeys::isNoteName (name));
}

TEST_CASE ("Plugin key names: generic words say a key switches", "[instruments][pluginkeys]")
{
    for (const auto* name : { "KS Legato", "Legato (KS)", "Keyswitch: palm mute", "Articulation 2", "Switch to slide" })
        CHECK (pluginkeys::namesASwitch (name));
    // Words that only contain the letters: not switches.
    for (const auto* name : { "Kick", "Snare", "Kshaker", "Masks", "Palm mute" })
        CHECK_FALSE (pluginkeys::namesASwitch (name));
}

TEST_CASE ("Plugin key names: as a profile's keys, from the plugin", "[instruments][pluginkeys]")
{
    const auto keys = pluginkeys::asKeys ({ { 24, "Palm mute", true }, { 36, "Kick", false }, { 200, "Out", false } });
    CHECK (keys[24].job == Job::switches);
    CHECK (keys[24].what == "Palm mute");
    CHECK (keys[24].source == InstrumentProfiles::Source::plugin);
    CHECK (keys[36].job == Job::plays);
    CHECK (keys[36].what == "Kick");
    CHECK (keys[60].job == Job::unknown);
    CHECK (std::count_if (keys.begin(), keys.end(), [] (const auto& k) { return k.job != Job::unknown; }) == 2);
}
