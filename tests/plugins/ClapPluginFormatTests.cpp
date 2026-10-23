#include <catch2/catch_test_macros.hpp>

#include "engine/EngineHost.h"
#include "plugins/clap/ClapPluginFormat.h"

using namespace spinus;

TEST_CASE ("The engine offers CLAP alongside the JUCE plugin formats", "[plugins]")
{
    EngineHost host ({ .openAudioDevice = false });

    juce::StringArray names;
    for (auto* format : host.getEngine().getPluginManager().pluginFormatManager.getFormats())
        names.add (format->getName());

    CHECK (names.contains (plugins::ClapPluginFormat::formatName));
    CHECK (names.contains ("VST3"));
}

TEST_CASE ("The CLAP format ignores files that are not CLAP plugins", "[plugins]")
{
    plugins::ClapPluginFormat format;
    juce::TemporaryFile notAPlugin (".clap");
    REQUIRE (notAPlugin.getFile().replaceWithText ("not an ELF file"));

    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (found, notAPlugin.getFile().getFullPathName());
    CHECK (found.isEmpty());

    CHECK_FALSE (format.fileMightContainThisPluginType ("/tmp/plugin.vst3"));
}
