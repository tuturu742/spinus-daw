#include <catch2/catch_test_macros.hpp>

#include "plugins/SurgePresets.h"

using namespace spinus;

namespace
{
struct TempFolder
{
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-surge-presets") + "-" + juce::Uuid().toString());
};
} // namespace

TEST_CASE ("Surge XT's presets are listed by source, category and name", "[plugins][surge]")
{
    TempFolder t;
    const auto data = t.root.getChildFile ("plugins/SurgeXTData");
    for (auto path : { "patches_factory/Leads/Saw Lead.fxp", "patches_factory/Basses/Bass 2.fxp",
                       "patches_factory/Basses/Bass 10.fxp", "patches_3rdparty/Someone/Pads/Air.fxp",
                       "patches_factory/Basses/readme.txt" })
        REQUIRE (data.getChildFile (path).create().wasOk());
    const auto user = t.root.getChildFile ("user");
    REQUIRE (user.getChildFile ("Mine.fxp").create().wasOk());

    // The data folder is found beside the plugin file, or above it.
    CHECK (plugins::SurgePresets::findDataFolder (t.root.getChildFile ("plugins/Surge XT.clap")) == data);
    CHECK (plugins::SurgePresets::findDataFolder (data.getChildFile ("x/y/Surge XT.clap")) == data);

    const auto presets = plugins::SurgePresets::scan (data, user);
    juce::StringArray names;
    for (auto& p : presets)
        names.add (plugins::SurgePresets::describe (p.source) + " | " + p.category + " | " + p.name);
    // Each patch once: no Spinus list of its own in front.
    CHECK (names == juce::StringArray { "Factory | Basses | Bass 2", "Factory | Basses | Bass 10",
                                        "Factory | Leads | Saw Lead", "Third party | Someone/Pads | Air",
                                        "Yours |  | Mine" });
}
