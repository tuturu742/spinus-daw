#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "plugins/PluginCatalog.h"
#include "plugins/PluginScanner.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "session/ProjectDocument.h"
#include "session/Session.h"

using namespace spinus;

// A plugin's settings changed in its own window (a preset picked there; no Spinus control touched) are saved
// with the project; rescanning plugins with a project open changes nothing in it.

namespace
{
/** A song with the Crash Test effect (a CLAP plugin, inside Spinus) on an insert, at gain 50 % (set by a
    command, as Spinus's knob or the assistant does, or left at its own 100 %). */
struct Fixture
{
    explicit Fixture (bool gainSetBySpinus = true)
    {
        root.createDirectory();
        plugins::ClapPluginFormat clap;
        juce::OwnedArray<juce::PluginDescription> found;
        clap.findAllTypesForFile (found, file);
        REQUIRE (found.size() == 1);
        host.getEngine().getPluginManager().knownPluginList.addType (*found[0]);
        session = std::make_unique<Session> (host.getEngine());
        document = std::make_unique<ProjectDocument> (*session, root.getChildFile ("settings"));

        const auto insert = run ("insert.add", "{}")["insert"].toString();
        device = model::DeviceId { (uint32_t)(int)run (
            "insert.addEffect",
            R"({"insert": )" + insert +
                R"(, "device": {"kind": "plugin", "plugin": {"format": "CLAP", "name": "Crash Test", "manufacturer": "Spinus tests", "fileOrIdentifier": )" +
                juce::JSON::toString (file) + R"(}}})")["device"] };
        if (gainSetBySpinus)
            run ("device.setParameters", R"({"device": )" + juce::String (device.value) + R"(, "values": {"0": 0.5}})");
        REQUIRE (document->saveAs (root.getChildFile ("Song")).wasOk());
    }

    ~Fixture()
    {
        document = nullptr;
        session = nullptr;
        root.deleteRecursively();
    }

    juce::var run (const juce::String& name, const juce::String& params)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
        INFO (name << ": " << error);
        REQUIRE (command != nullptr);
        auto* raw = command.get();
        REQUIRE (session->getHistory().perform (std::move (command)).wasOk());
        return raw->getResult();
    }

    juce::AudioPluginInstance* instance() const
    {
        auto* external = dynamic_cast<tracktion::ExternalPlugin*> (session->getAdapter().getDevicePlugin (device));
        return external != nullptr ? external->getAudioPluginInstance() : nullptr;
    }

    float gain() const
    {
        auto* i = instance();
        return i != nullptr ? i->getParameters()[0]->getValue() : -1.0f;
    }

    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-plugin-state-" + juce::Uuid().toString());
    const juce::String file = juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("crash-test.clap").getFullPathName();
    EngineHost host { { .openAudioDevice = false } };
    std::unique_ptr<Session> session;
    std::unique_ptr<ProjectDocument> document;
    model::DeviceId device;
};
} // namespace

TEST_CASE ("A plugin's own setting change in its window is saved with the project", "[session][document][plugins]")
{
    // Gain never set in Spinus, and set in Spinus before (the project holds that value too): either way the
    // change made in the plugin's own window is what is saved, and what plays after opening it again.
    for (const bool setBySpinus : { false, true })
    {
        INFO ("Gain set by Spinus before: " << (setBySpinus ? "yes" : "no"));
        Fixture f (setBySpinus);
        REQUIRE (f.instance() != nullptr);
        CHECK (std::abs (f.gain() - (setBySpinus ? 0.5f : 1.0f)) < 0.01f);
        CHECK (! f.document->hasUnsavedChanges());
        const auto steps = f.session->getHistory().getUndoDescriptions().size();

        // Its window is opened, and a preset is picked there: the plugin changes its own settings, without a
        // gesture on a knob, and not through Spinus.
        f.session->noteDeviceWindowShown (f.device);
        f.instance()->getParameters()[0]->setValueNotifyingHost (0.25f);
        CHECK (f.session->getHistory().getUndoDescriptions().size() == steps); // not a Spinus edit
        CHECK (f.document->hasUnsavedChanges());                               // but asked about on closing

        REQUIRE (f.document->save().wasOk());
        CHECK (! f.document->hasUnsavedChanges());
        f.document->newProject();
        REQUIRE (f.document->open (f.root.getChildFile ("Song")).wasOk());
        REQUIRE (f.instance() != nullptr);
        CHECK (std::abs (f.gain() - 0.25f) < 0.01f);

        // A command setting it again still works.
        f.run ("device.setParameters",
               R"({"device": )" + juce::String (f.device.value) + R"(, "values": {"0": 0.75}})");
        CHECK (std::abs (f.gain() - 0.75f) < 0.01f);
    }
}

TEST_CASE ("Rescanning plugins with a project open changes nothing in the project", "[session][plugins][scanner]")
{
    Fixture f;
    auto* before = f.instance();
    REQUIRE (before != nullptr);
    const auto project = f.session->getProject();
    const auto states = f.session->getAdapter().captureDeviceStates();
    const auto steps = f.session->getHistory().getUndoDescriptions();

    // A scan of a folder with the plugin and others (one that crashes the scanner), then what the Browser does
    // when a scan ends: the engine's list and the device list follow the scan.
    const auto folder = f.root.getChildFile ("plugins");
    folder.createDirectory();
    for (auto name : { "crash-test.clap", "test-synth.clap", "crash.clap" })
        REQUIRE (juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile (name).copyFileTo (folder.getChildFile (name)));
    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<plugins::ClapPluginFormat>());
    juce::KnownPluginList known;
    plugins::ProblemPlugins problems;
    const auto summary =
        plugins::PluginScanner ({ juce::File (SPINUS_PLUGIN_SCANNER), 5000 })
            .scanAll (formats, known, problems, { { "CLAP", juce::FileSearchPath (folder.getFullPathName()) } });
    CHECK (summary.pluginsFound >= 2);

    auto& manager = f.host.getEngine().getPluginManager();
    plugins::PluginCatalog::refreshFormats (manager.pluginFormatManager);
    plugins::forgetVanishedPlugins (manager.knownPluginList);
    for (auto& type : known.getTypes())
        manager.knownPluginList.addType (type);
    f.session->getDeviceList().refresh();
    f.session->retryMissingDevices();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

    // The same project, the same plugin instance with the same settings, nothing to undo or save.
    CHECK (f.session->getProject() == project);
    CHECK (f.instance() == before);
    CHECK (f.session->getAdapter().captureDeviceStates() == states);
    CHECK (f.session->getHistory().getUndoDescriptions() == steps);
    CHECK (! f.document->hasUnsavedChanges());
    CHECK (f.session->getAdapter().getMissingDevices().empty());
}
