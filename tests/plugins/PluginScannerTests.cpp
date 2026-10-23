#include <catch2/catch_test_macros.hpp>

#include "engine/EngineHost.h"
#include "plugins/PluginCatalog.h"
#include "plugins/PluginScanner.h"
#include "plugins/clap/ClapPluginFormat.h"

using namespace spinus::plugins;

namespace
{

PluginScanner scanner (int timeoutMs = 30000)
{
    return PluginScanner ({ juce::File (SPINUS_PLUGIN_SCANNER), timeoutMs });
}

juce::File brokenDir() { return juce::File (SPINUS_BROKEN_PLUGINS_DIR); }

juce::File realClapPlugin()
{
#if defined(SPINUS_PLUGINS_DIR)
    return juce::File (SPINUS_PLUGINS_DIR).getChildFile ("DragonflyRoomReverb.clap");
#else
    return {};
#endif
}

struct TempFolder
{
    TempFolder()
        : root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile (juce::String ("spinus-scan") + "-" + juce::Uuid().toString()))
    {
        root.createDirectory();
    }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root;
};

} // namespace

TEST_CASE ("A plugin is scanned in a separate process", "[plugins][scanner]")
{
    if (! realClapPlugin().existsAsFile())
        SKIP ("bundled plugins not built");

    juce::OwnedArray<juce::PluginDescription> found;
    const auto result = scanner().scanFile ("CLAP", realClapPlugin().getFullPathName(), found);
    INFO (result.getErrorMessage());
    REQUIRE (result.wasOk());
    REQUIRE (found.size() == 1);
    CHECK (found[0]->name == "Dragonfly Room Reverb");
    CHECK (found[0]->pluginFormatName == "CLAP");
}

TEST_CASE ("A plugin that crashes or hangs while loading is reported, not fatal", "[plugins][scanner]")
{
    juce::OwnedArray<juce::PluginDescription> found;

    const auto crashed = scanner().scanFile ("CLAP", brokenDir().getChildFile ("crash.clap").getFullPathName(), found);
    CHECK (crashed.getErrorMessage() == "crashed while loading");

    const auto start = juce::Time::getMillisecondCounter();
    const auto hung = scanner (2000).scanFile ("CLAP", brokenDir().getChildFile ("hang.clap").getFullPathName(), found);
    CHECK (hung.getErrorMessage() == "did not finish loading within 2 s");
    CHECK (juce::Time::getMillisecondCounter() - start < 10000);
    CHECK (found.isEmpty());

    CHECK (PluginScanner ({ juce::File ("/nonexistent/scanner"), 1000 })
               .scanFile ("CLAP", "x.clap", found)
               .getErrorMessage()
               .contains ("missing"));
}

TEST_CASE ("A full scan skips unchanged files and known problems; retry rescans", "[plugins][scanner]")
{
    TempFolder temp;
    REQUIRE (brokenDir().getChildFile ("crash.clap").copyFileTo (temp.root.getChildFile ("crash.clap")));
    REQUIRE (brokenDir().getChildFile ("hang.clap").copyFileTo (temp.root.getChildFile ("hang.clap")));
    const bool haveReal = realClapPlugin().existsAsFile();
    if (haveReal)
        REQUIRE (realClapPlugin().copyFileTo (temp.root.getChildFile ("room.clap")));

    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<ClapPluginFormat>());
    juce::KnownPluginList known;
    ProblemPlugins problems;
    const std::map<juce::String, juce::FileSearchPath> paths { { "CLAP",
                                                                 juce::FileSearchPath (temp.root.getFullPathName()) } };

    std::vector<int> progress;
    auto first = scanner (2000).scanAll (formats, known, problems, paths,
                                         [&] (const PluginScanner::Progress& p) { progress.push_back (p.done); });
    CHECK (first.scanned == (haveReal ? 3 : 2));
    CHECK (first.newProblems == 2);
    CHECK (first.pluginsFound == (haveReal ? 1 : 0));
    CHECK (known.getNumTypes() == (haveReal ? 1 : 0));
    CHECK (problems.getAll().size() == 2);
    CHECK (problems.contains ("CLAP", temp.root.getChildFile ("hang.clap").getFullPathName()));
    CHECK (progress.back() == first.scanned);

    // Nothing changed: nothing is scanned again.
    auto second = scanner (2000).scanAll (formats, known, problems, paths);
    CHECK (second.scanned == 0);
    CHECK (second.skippedProblems == 2);
    CHECK (second.skippedUnchanged == (haveReal ? 1 : 0));

    // "Retry" on a problem plugin: remove it from the list and scan again (it still crashes).
    const auto crashFile = temp.root.getChildFile ("crash.clap").getFullPathName();
    problems.remove ("CLAP", crashFile);
    auto third = scanner (2000).scanAll (formats, known, problems, paths);
    CHECK (third.scanned == 1);
    CHECK (problems.contains ("CLAP", crashFile));

    // Cancelling stops before scanning anything.
    problems.remove ("CLAP", crashFile);
    std::atomic<bool> cancel { true };
    CHECK (scanner (2000).scanAll (formats, known, problems, paths, {}, &cancel).scanned == 0);
}

TEST_CASE ("The problem list is saved and loaded", "[plugins][scanner]")
{
    ProblemPlugins problems;
    problems.add ({ "VST3", "/a/b.vst3", "crashed while loading", juce::Time (2026, 9, 12, 10, 30) });
    problems.add ({ "CLAP", "/c.clap", "did not finish loading within 60 s", juce::Time (2026, 9, 12, 10, 31) });

    ProblemPlugins loaded;
    loaded.loadFromXml (*problems.toXml());
    REQUIRE (loaded.getAll().size() == 2);
    CHECK (loaded.getAll()[1].reason == "did not finish loading within 60 s");
    CHECK (loaded.contains ("VST3", "/a/b.vst3"));
    CHECK (loaded.getAll()[0].when == problems.getAll()[0].when);
}

TEST_CASE ("A plugin file that does not load is a problem with the loader's reason", "[plugins][scanner]")
{
    // Not a library at all: the loader says why, and the scan lists it under Problem plugins with that
    // reason (in the Flatpak: a system plugin built against newer libraries, or missing one).
    TempFolder temp;
    const auto file = temp.root.getChildFile ("Broken.clap");
    REQUIRE (file.replaceWithText ("not a library"));

    CHECK (whyPluginDoesNotLoad ("CLAP", file.getFullPathName()).startsWith ("does not load: "));
    CHECK (whyPluginDoesNotLoad ("LV2", "urn:example:plugin").isEmpty());

    juce::OwnedArray<juce::PluginDescription> found;
    const auto result = scanner().scanFile ("CLAP", file.getFullPathName(), found);
    CHECK (result.failed());
    CHECK (result.getErrorMessage().startsWith ("does not load: "));
    CHECK (found.isEmpty());
}

TEST_CASE ("Plugins whose files are gone leave the plugin and problem lists", "[plugins][scanner]")
{
    TempFolder temp;
    const auto kept = temp.root.getChildFile ("Kept.clap"), gone = temp.root.getChildFile ("Gone.clap");
    REQUIRE (kept.replaceWithText ("x"));

    juce::KnownPluginList known;
    for (const auto* f : { &kept, &gone })
    {
        juce::PluginDescription d;
        d.pluginFormatName = "CLAP";
        d.name = f->getFileNameWithoutExtension();
        d.fileOrIdentifier = f->getFullPathName();
        known.addType (d);
    }
    juce::PluginDescription lv2;
    lv2.pluginFormatName = "LV2";
    lv2.name = "An LV2 plugin";
    lv2.fileOrIdentifier = "urn:example:lv2"; // not a file: stays
    known.addType (lv2);

    ProblemPlugins problems;
    problems.add ({ "CLAP", gone.getFullPathName() + "x", "crashed while loading", juce::Time::getCurrentTime() });
    problems.add ({ "CLAP", kept.getFullPathName(), "crashed while loading", juce::Time::getCurrentTime() });

    CHECK (forgetVanishedPlugins (known, &problems) == 1);
    CHECK (known.getNumTypes() == 2);
    CHECK (problems.getAll().size() == 1);
    CHECK (problems.contains ("CLAP", kept.getFullPathName()));
}

#if defined(SPINUS_OWN_LV2)
TEST_CASE ("An LV2 plugin installed while Spinus runs loads after a scan", "[plugins][lv2]")
{
    spinus::EngineHost host { { .openAudioDevice = false } };
    auto& manager = host.getEngine().getPluginManager();

    // "Installed" now: in a folder on LV2_PATH, described by a scan (here: another format instance).
    const juce::File bundle (SPINUS_OWN_LV2);
    setenv ("LV2_PATH", bundle.getParentDirectory().getFullPathName().toRawUTF8(), 1);
    const juce::ScopeGuard unset { [] { unsetenv ("LV2_PATH"); } };
    juce::LV2PluginFormat scanner;
    juce::OwnedArray<juce::PluginDescription> found;
    scanner.findAllTypesForFile (found, bundle.getFullPathName());
    REQUIRE (found.size() == 1);

    auto create = [&]
    {
        juce::String error;
        return manager.pluginFormatManager.createPluginInstance (*found[0], 48000.0, 512, error);
    };
    CHECK (create() == nullptr); // the engine's LV2 host has not seen it yet
    PluginCatalog::refreshFormats (manager.pluginFormatManager);
    CHECK (create() != nullptr);
}
#endif
