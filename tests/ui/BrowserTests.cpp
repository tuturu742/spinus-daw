#include <catch2/catch_test_macros.hpp>

#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "ui/Browser.h"
#include "ui/ChannelRack.h"
#include "ui/MainView.h"
#include "ui/Mixer.h"

using namespace spinus;
using namespace spinus::ui;

namespace
{
juce::PluginDescription describe (const juce::String& format, const juce::String& name, const juce::String& file,
                                  bool instrument, const juce::String& maker = "Maker")
{
    juce::PluginDescription d;
    d.pluginFormatName = format;
    d.name = name;
    d.manufacturerName = maker;
    d.fileOrIdentifier = file;
    d.isInstrument = instrument;
    d.uniqueId = name.hashCode();
    return d;
}

/** A session whose device list uses a catalog in a temporary folder, filled by hand: a real CLAP effect
    (the Sidechain Echo test plugin), plugins that only exist on paper, and a problem plugin. */
struct Fixture
{
    Fixture()
        : catalog (temp.getChildFile ("settings"), juce::File (SPINUS_PLUGIN_SCANNER).getParentDirectory()),
          session (host.getEngine(), makeDevices (host.getEngine()))
    {
        plugins::ClapPluginFormat clap;
        juce::OwnedArray<juce::PluginDescription> found;
        clap.findAllTypesForFile (
            found, juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("sidechain-echo.clap").getFullPathName());
        REQUIRE (found.size() == 1);
        echo = *found[0];

        auto& known = catalog.getKnownPlugins();
        known.addType (echo);
        known.addType (describe ("VST3", "Fake Synth", "/usr/lib/vst3/Fake Synth.vst3", true, "Fake Audio"));
        known.addType (describe ("VST3", "Serum", "/home/me/.vst3/yabridge/Serum.vst3", true, "Xfer"));
        known.addType (describe ("VST", "Old Delay", "/usr/lib/vst/OldDelay.so", false));
        // A bundled plugin the scan found in Spinus's own folder: listed once, under Spinus.
        known.addType (describe ("CLAP", "Surge XT",
                                 catalog.getBundledFolder().getChildFile ("Surge XT.clap").getFullPathName(), true,
                                 "Surge Synth Team"));
        // Spinus's own devices installed as plugins for other programs: not listed again.
        known.addType (describe ("VST3", "Spinus Sampler", "/usr/lib/vst3/Spinus Sampler.vst3", true, "Spinus"));
        catalog.getProblems().add ({ "VST3", "/usr/lib/vst3/Broken.vst3", "crashed while scanning", {} });

        catalog.addKnownTypesTo (host.getEngine().getPluginManager().knownPluginList);
        session.getDeviceList().setCatalog (&catalog);
    }

    ~Fixture() { temp.deleteRecursively(); }

    static std::unique_ptr<engine::DeviceFactory> makeDevices (tracktion::Engine& engine)
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
#if defined(SPINUS_PLUGINS_DIR)
        engine::registerBundledDevices (*devices, engine, { juce::File (SPINUS_PLUGINS_DIR) });
#endif
        return devices;
    }

    const plugins::DeviceList::Entry* entry (const juce::String& name)
    {
        for (auto& e : session.getDeviceList().getEntries())
            if (e.name == name)
                return &e;
        return nullptr;
    }

    juce::File temp { juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-browser-test") + "-" + juce::Uuid().toString()) };
    EngineHost host { { .openAudioDevice = false } };
    plugins::PluginCatalog catalog;
    Session session;
    juce::PluginDescription echo;
};

juce::StringArray texts (const Browser& browser, Browser::RowKind kind)
{
    juce::StringArray result;
    for (auto& r : browser.getRows())
        if (r.kind == kind)
            result.add (r.text + (r.badge.isNotEmpty() ? " [" + r.badge + "]" : juce::String()));
    return result;
}
} // namespace

TEST_CASE ("The device list has Spinus's devices and the installed plugins, with formats", "[ui][browser]")
{
    Fixture f;
    const auto& entries = f.session.getDeviceList().getEntries();

    // Spinus's own and bundled devices come first, as stock devices; bundled ones credit their projects.
    REQUIRE (entries.size() == model::getSpinusDevices().size() + model::getBundledDevices().size() + 4);
    CHECK (entries[0].origin == plugins::DeviceList::Origin::spinus);
    CHECK (entries[0].name == "Sampler");
    const auto firstBundled = model::getSpinusDevices().size();
    CHECK (entries[1].name == "Drum Machine");
    CHECK (entries[firstBundled].origin == plugins::DeviceList::Origin::bundled);
    CHECK (entries[firstBundled].maker == "Surge XT");
    CHECK (f.entry ("Hall Reverb")->maker == "Dragonfly Reverb");
    CHECK (f.entry ("Hall Reverb")->detail.contains ("not made by Spinus"));
    CHECK (entries[0].device.kind == model::Device::Kind::stock);
    CHECK (entries[firstBundled].device.stockType == model::getBundledDevices()[0].stockType);

    // The scanned copy of a bundled plugin is not listed again.
    int surge = 0;
    for (auto& e : entries)
        surge += e.name == "Surge XT" ? 1 : 0;
    CHECK (surge == 1);

    REQUIRE (f.entry ("Serum") != nullptr);
    CHECK (f.entry ("Serum")->badge == "WIN");
    CHECK (f.entry ("Old Delay")->badge == "VST2");
    CHECK (f.entry ("Fake Synth")->badge == "VST3");
    CHECK (f.entry ("Fake Synth")->device.kind == model::Device::Kind::plugin);
    CHECK (f.entry ("Fake Synth")->device.plugin.fileOrIdentifier == "/usr/lib/vst3/Fake Synth.vst3");

    // Drag and drop carries an entry as a var.
    plugins::DeviceList::Entry back;
    REQUIRE (plugins::DeviceList::fromVar (plugins::DeviceList::toVar (*f.entry ("Fake Synth")), back));
    CHECK (back.device == f.entry ("Fake Synth")->device);
    CHECK (back.instrument);
    CHECK (! plugins::DeviceList::fromVar (juce::var ("something else"), back));
}

TEST_CASE ("The browser groups, folds and searches", "[ui][browser]")
{
    Fixture f;
    Browser browser (f.session);
    browser.setSize (220, 600);

    CHECK (texts (browser, Browser::RowKind::section) ==
           juce::StringArray { "Projects", "Samples", "Instruments", "Effects", "Problem plugins  (1)" });
    // Spinus's own devices between installed and bundled: four instruments; the Filter, the Ducker, the Stereo
    // Utility, Spinus Tune and the Loudness Meter.
    CHECK (texts (browser, Browser::RowKind::group) ==
           juce::StringArray { "Installed plugins  (2)", "Spinus  (4)", "Free open source  (3)",
                               "Installed plugins  (2)", "Spinus  (9)", "Free open source  (12)" });
    CHECK (texts (browser, Browser::RowKind::device).contains ("Serum [WIN]"));
    CHECK (texts (browser, Browser::RowKind::problem).isEmpty()); // problems start folded

    browser.toggleOpen ("problems");
    CHECK (texts (browser, Browser::RowKind::problem) == juce::StringArray { "Broken.vst3 [VST3]" });

    browser.toggleOpen ("instruments");
    CHECK (! texts (browser, Browser::RowKind::device).contains ("Serum [WIN]"));
    browser.toggleOpen ("instruments");

    // Search matches names, makers and formats, and opens folded sections.
    browser.toggleOpen ("effects");
    browser.setSearchText ("reverb"); // names, and the bundled project "Dragonfly Reverb"
    CHECK (texts (browser, Browser::RowKind::device) ==
           juce::StringArray { "Hall Reverb", "Room Reverb", "Plate Reverb", "Early Reflections" });
    browser.setSearchText ("lsp");
    CHECK (texts (browser, Browser::RowKind::device).size() == 6);
    browser.setSearchText ("xfer");
    CHECK (texts (browser, Browser::RowKind::device) == juce::StringArray { "Serum [WIN]" });
    browser.setSearchText ("vst2");
    CHECK (texts (browser, Browser::RowKind::device) == juce::StringArray { "Old Delay [VST2]" });
}

TEST_CASE ("Using a browser entry adds a channel or an effect", "[ui][browser]")
{
    Fixture f;
    MainView view (f.session);
    auto& browser = view.getBrowser();
    auto* mixer = dynamic_cast<Mixer*> (view.getWorkspace().getPanel ("mixer"));
    REQUIRE (mixer != nullptr);

    auto rowOf = [&] (const juce::String& text)
    {
        const auto& rows = browser.getRows();
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].text == text)
                return (int)i;
        FAIL ("no row " << text);
        return -1;
    };

    // An installed instrument: a new channel with that plugin (not installed here, so its slot is
    // "missing", but the channel and its settings are kept).
    browser.useRow (rowOf ("Fake Synth"));
    const auto& project = f.session.getProject();
    REQUIRE (project.channels.size() == 1);
    CHECK (project.channels[0].name == "Fake Synth");
    REQUIRE (project.channels[0].instrument.has_value());
    CHECK (project.channels[0].instrument.value_or (model::Device {}).plugin.name == "Fake Synth");
    CHECK (view.getWorkspace().isPanelVisible ("channelRack"));

    // An installed effect goes into the selected insert: the master by default.
    browser.useRow (rowOf (f.echo.name));
    REQUIRE (project.mixer.master.effects.size() == 1);
    CHECK (project.mixer.master.effects[0].device.plugin.name == f.echo.name);
    CHECK (f.session.getAdapter().getMissingDevices().size() == 1); // the echo loads; Fake Synth does not
    CHECK (view.getWorkspace().isPanelVisible ("mixer"));

    // Headers and messages do nothing.
    browser.useRow (rowOf ("Instruments"));
    CHECK (project.channels.size() == 1);
}

TEST_CASE ("Dropping browser entries on the Channel Rack and the Mixer", "[ui][browser]")
{
    Fixture f;
    ChannelRack rack (f.session);
    Mixer mixer (f.session);
    mixer.setSize (900, 420);
    REQUIRE (mixer.addInsert());
    const auto insert = f.session.getProject().mixer.inserts.back().id;
    mixer.selectInsert (insert);
    mixer.updateNow();

    using Source = juce::DragAndDropTarget::SourceDetails;
    const Source synth (plugins::DeviceList::toVar (*f.entry ("Fake Synth")), nullptr, { 10, 10 });
    const Source delay (plugins::DeviceList::toVar (*f.entry ("Old Delay")), nullptr, { 10, 10 });

    // The rack takes instruments only; the mixer effects only.
    CHECK (rack.isInterestedInDragSource (synth));
    CHECK (! rack.isInterestedInDragSource (delay));
    CHECK (mixer.isInterestedInDragSource (delay));
    CHECK (! mixer.isInterestedInDragSource (synth));
    CHECK (! rack.isInterestedInDragSource (Source ("text", nullptr, {})));

    rack.itemDropped (synth);
    REQUIRE (f.session.getProject().channels.size() == 1);

    // Dropped on a strip area: added to the selected insert's slots.
    mixer.itemDropped (delay);
    REQUIRE (f.session.getProject().findInsert (insert)->effects.size() == 1);

    // Dropped on the first slot of the details panel: goes first.
    const Source compressor (plugins::DeviceList::toVar (*f.entry ("Compressor")), nullptr,
                             { 900 - Mixer::detailsWidth + 40, 28 + 4 });
    mixer.itemDropped (compressor);
    const auto& effects = f.session.getProject().findInsert (insert)->effects;
    REQUIRE (effects.size() == 2);
    CHECK (effects[0].device.stockType == f.entry ("Compressor")->device.stockType);
}

TEST_CASE ("A plugin installed in several formats is listed once, by its product name", "[ui][browser]")
{
    Fixture f;
    auto vst2 =
        describe ("VST", "TubeSaturatorVintage_64", "/home/me/.vst/TubeSaturatorVintage_64.so", false, "Wave Arts");
    vst2.descriptiveName = "Tube Saturator Vintage";
    f.catalog.getKnownPlugins().addType (vst2);
    f.catalog.getKnownPlugins().addType (
        describe ("VST3", "Tube Saturator Vintage", "/home/me/.vst3/TubeSaturatorVintage_64.vst3", false, "Wave Arts"));
    auto guitar = describe ("VST", "AGML2", "/home/me/.vst/AGML2.so", true, "Ample Sound");
    guitar.descriptiveName = "Ample Guitar M II Lite";
    f.catalog.getKnownPlugins().addType (guitar);
    f.session.getDeviceList().setCatalog (&f.catalog);

    int tubes = 0;
    for (auto& e : f.session.getDeviceList().getEntries())
        tubes += e.name.startsWith ("Tube") ? 1 : 0;
    CHECK (tubes == 1);
    REQUIRE (f.entry ("Tube Saturator Vintage") != nullptr);
    CHECK (f.entry ("Tube Saturator Vintage")->badge == "VST3"); // VST3 rather than VST2
    CHECK (f.entry ("Tube Saturator Vintage")->detail.contains ("also installed as VST2"));
    REQUIRE (f.entry ("Ample Guitar M II Lite") != nullptr);
    CHECK (f.entry ("Ample Guitar M II Lite")->device.plugin.name == "AGML2"); // loaded by its own name
    CHECK (f.entry ("AGML2") == nullptr);

    // The other format chosen instead (the listed one is laggy, say): listed and added from now on, also
    // after a restart.
    const auto prefs = f.temp.getChildFile ("plugin-formats.json");
    f.session.getDeviceList().setPreferencesFile (prefs);
    const auto key = f.entry ("Tube Saturator Vintage")->key;
    CHECK (f.entry ("Tube Saturator Vintage")->formats == juce::StringArray { "VST3", "VST2" });
    CHECK (! f.session.getDeviceList().preferFormat (key, "CLAP")); // not installed as CLAP
    REQUIRE (f.session.getDeviceList().preferFormat (key, "VST2"));
    CHECK (f.entry ("Tube Saturator Vintage")->badge == "VST2");
    CHECK (f.entry ("Tube Saturator Vintage")->device.plugin.format == "VST");
    CHECK (f.entry ("Tube Saturator Vintage")->detail.contains ("also installed as VST3"));
    plugins::DeviceList again;
    again.setPreferencesFile (prefs);
    again.setCatalog (&f.catalog);
    const auto tube = std::find_if (again.getEntries().begin(), again.getEntries().end(),
                                    [] (auto& e) { return e.name == "Tube Saturator Vintage"; });
    REQUIRE (tube != again.getEntries().end());
    CHECK (tube->badge == "VST2");
}

TEST_CASE ("Asked to scan during a scan the Browser scans again afterwards", "[ui][browser]")
{
    Fixture f;
    REQUIRE (f.temp.createDirectory());
    // Plugin folders under the home folder are the test's own (empty) ones.
    const auto home = juce::SystemStats::getEnvironmentVariable ("HOME", {});
    setenv ("HOME", f.temp.getFullPathName().toRawUTF8(), 1); // NOLINT(concurrency-mt-unsafe): before any thread
    plugins::PluginCatalog catalog (f.temp.getChildFile ("settings"), f.temp.getChildFile ("bin"));
    f.session.getDeviceList().setCatalog (&catalog);
    Browser browser (f.session);

    browser.startScan();
    REQUIRE (browser.isScanning());
    CHECK (! browser.isRescanPending());
    // A Windows plugin installed now: its scan request is kept, not dropped.
    browser.startScan();
    CHECK (browser.isRescanPending());

    // Cancelled by the user: no scan again.
    browser.cancelScan();
    browser.finishScanForTests();
    CHECK (! browser.isScanning());
    CHECK (! browser.isRescanPending());

    // Finished normally: the kept request starts the next scan.
    browser.startScan();
    browser.startScan();
    browser.finishScanForTests();
    CHECK (browser.isScanning());
    CHECK (! browser.isRescanPending());
    browser.finishScanForTests();
    CHECK (! browser.isScanning());

    f.session.getDeviceList().setCatalog (nullptr);
    setenv ("HOME", home.toRawUTF8(), 1); // NOLINT(concurrency-mt-unsafe)
}
