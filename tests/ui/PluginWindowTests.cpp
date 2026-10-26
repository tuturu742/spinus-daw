#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "model/ModelJson.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "ui/ChannelRack.h"
#include "ui/Mixer.h"
#include "ui/PluginWindow.h"

using namespace spinus;
using namespace spinus::ui;
namespace te = tracktion;

namespace
{
juce::File bundledPlugins()
{
#if defined(SPINUS_PLUGINS_DIR)
    return juce::File (SPINUS_PLUGINS_DIR);
#else
    return {};
#endif
}

struct Fixture
{
    Fixture() : session (host.getEngine(), makeDevices (host.getEngine())), mixer (session)
    {
        PluginWindow::install (session);
    }

    ~Fixture()
    {
        PluginWindow::uninstall (session);
        for (auto* plugin : te::getAllPlugins (session.getEdit(), false))
            plugin->hideWindowForShutdown();
    }

    static std::unique_ptr<engine::DeviceFactory> makeDevices (te::Engine& engine)
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerBundledDevices (*devices, engine, { bundledPlugins() });
        return devices;
    }

    model::InsertId addInsertWith (std::initializer_list<const char*> stockTypes)
    {
        REQUIRE (mixer.addInsert());
        const auto id = session.getProject().mixer.inserts.back().id;
        for (auto* type : stockTypes)
            REQUIRE (mixer.addEffect (id, type));
        return id;
    }

    const model::Insert& insert (model::InsertId id) const { return *session.getProject().findInsert (id); }

    /** Adds the "Sidechain Echo" test plugin (no editor of its own) to the insert. */
    model::DeviceId addEcho (model::InsertId id)
    {
        if (echo == nullptr)
        {
            plugins::ClapPluginFormat format;
            juce::OwnedArray<juce::PluginDescription> found;
            format.findAllTypesForFile (
                found, juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("sidechain-echo.clap").getFullPathName());
            REQUIRE (found.size() == 1);
            echo = std::make_unique<juce::PluginDescription> (*found[0]);
            host.getEngine().getPluginManager().knownPluginList.addType (*echo);
        }

        model::Device device;
        device.kind = model::Device::Kind::plugin;
        device.plugin = { "CLAP", echo->name, echo->manufacturerName, echo->fileOrIdentifier, echo->uniqueId };
        auto parameters = juce::JSON::parse ("{\"insert\": " + juce::String ((int)id.value) + "}");
        parameters.getDynamicObject()->setProperty ("device", model::deviceToJson (device));

        juce::String error;
        auto command = commands::CommandRegistry::get().create ("insert.addEffect", parameters, error);
        REQUIRE (command != nullptr);
        auto* raw = command.get();
        REQUIRE (session.getHistory().perform (std::move (command)).wasOk());
        return model::DeviceId { (uint32_t)(int)raw->getResult()["device"] };
    }

#if defined(SPINUS_TEST_LV2_UI_BUNDLE)
    /** Adds one of the LV2 test plugins (tests/plugins/Lv2UiTestPlugin.c) to the insert. */
    model::DeviceId addLv2 (model::InsertId id, const juce::String& kind)
    {
        // The engine's own LV2 format loads the bundle, as a scan does.
        juce::AudioPluginFormat* format = nullptr;
        for (auto* candidate : host.getEngine().getPluginManager().pluginFormatManager.getFormats())
            if (candidate->getName() == "LV2")
                format = candidate;
        REQUIRE (format != nullptr);
        juce::OwnedArray<juce::PluginDescription> found;
        format->findAllTypesForFile (found, SPINUS_TEST_LV2_UI_BUNDLE);
        const juce::PluginDescription* lv2 = nullptr;
        for (auto* d : found)
            if (d->fileOrIdentifier == "urn:spinus:test:lv2-ui#" + kind)
                lv2 = d;
        REQUIRE (lv2 != nullptr);
        host.getEngine().getPluginManager().knownPluginList.addType (*lv2);

        model::Device device;
        device.kind = model::Device::Kind::plugin;
        device.plugin = { "LV2", lv2->name, lv2->manufacturerName, lv2->fileOrIdentifier, lv2->uniqueId };
        auto parameters = juce::JSON::parse ("{\"insert\": " + juce::String ((int)id.value) + "}");
        parameters.getDynamicObject()->setProperty ("device", model::deviceToJson (device));

        juce::String error;
        auto command = commands::CommandRegistry::get().create ("insert.addEffect", parameters, error);
        REQUIRE (command != nullptr);
        auto* raw = command.get();
        REQUIRE (session.getHistory().perform (std::move (command)).wasOk());
        return model::DeviceId { (uint32_t)(int)raw->getResult()["device"] };
    }
#endif

    /** The open top-level window with this title, if any. */
    static juce::TopLevelWindow* findWindow (const juce::String& title)
    {
        for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
            if (auto* w = juce::TopLevelWindow::getTopLevelWindow (i); w->getName() == title && w->isVisible())
                return w;
        return nullptr;
    }

    EngineHost host { { .openAudioDevice = false } };
    Session session;
    Mixer mixer;
    std::unique_ptr<juce::PluginDescription> echo;
};
} // namespace

TEST_CASE ("A window opens with a title, and closes when its effect is removed", "[ui][pluginwindow]")
{
    Fixture f;
    const auto bus = f.addInsertWith ({});
    REQUIRE (f.mixer.renameInsert (bus, "Reverb"));
    const auto echo = f.addEcho (bus);

    CHECK (! f.session.getDevicesShown().contains (echo));
    REQUIRE (f.mixer.showEffectWindow (echo));
    CHECK (f.session.isDeviceWindowShowing (echo));
    CHECK (f.session.getDevicesShown().contains (echo)); // its settings may now change: asked about on closing
    auto* window = Fixture::findWindow ("Sidechain Echo - 1 Reverb");
    REQUIRE (window != nullptr);
    CHECK (window->getWidth() > 50); // the generic editor: the test plugin has no window of its own

    // Removing the effect closes its window first (no window may outlive its plugin).
    REQUIRE (f.mixer.removeEffect (echo));
    CHECK (Fixture::findWindow ("Sidechain Echo - 1 Reverb") == nullptr);

    // Undo brings the effect back, with its window closed; it opens again.
    REQUIRE (f.session.getHistory().undo());
    CHECK (! f.session.isDeviceWindowShowing (echo));
    REQUIRE (f.mixer.showEffectWindow (echo));
    f.session.closeDeviceWindow (echo);
    CHECK (! f.session.isDeviceWindowShowing (echo));
}

TEST_CASE ("An open window stays open when its effect is reordered", "[ui][pluginwindow]")
{
    Fixture f;
    const auto a = f.addInsertWith ({});
    const auto b = f.addInsertWith ({});
    f.addEcho (a);
    const auto second = f.addEcho (a);

    REQUIRE (f.mixer.showEffectWindow (second));
    REQUIRE (f.mixer.moveEffect (second, a, 0));
    CHECK (f.session.isDeviceWindowShowing (second));

    // Moving to another insert recreates the plugin (with its settings); its window closes with it.
    REQUIRE (f.mixer.moveEffect (second, b, 0));
    CHECK (! f.session.isDeviceWindowShowing (second));
}

TEST_CASE ("Bundled plugins open their own windows, from the Mixer and the Channel Rack", "[ui][pluginwindow]")
{
#if defined(__SANITIZE_ADDRESS__)
    // Third-party plugin GUIs load graphics libraries that LeakSanitizer reports as leaks it cannot
    // attribute. The window code itself is covered under ASan by the tests above.
    SKIP ("real plugin GUIs are not run under AddressSanitizer");
#endif
    if (! bundledPlugins().getChildFile ("DragonflyHallReverb.clap").existsAsFile())
        SKIP ("bundled plugins not built");
    Fixture f;
    const auto bus = f.addInsertWith ({ "bundled.dragonfly.hall" });
    REQUIRE (f.mixer.renameInsert (bus, "Reverb"));
    REQUIRE (f.mixer.showEffectWindow (f.insert (bus).effects[0].device.id));
    CHECK (Fixture::findWindow ("Hall Reverb - 1 Reverb") != nullptr);

    ChannelRack rack (f.session);
    REQUIRE (rack.addChannel ({}, "Empty"));
    CHECK (! rack.showInstrumentWindow (f.session.getProject().channels[0].id)); // no instrument

    if (! bundledPlugins().getChildFile ("Surge XT.clap").existsAsFile())
        SKIP ("Surge XT not built");
    REQUIRE (rack.addChannel ("bundled.surge-xt", "Lead"));
    REQUIRE (rack.showInstrumentWindow (f.session.getProject().channels[1].id));
    CHECK (Fixture::findWindow ("Surge XT - Lead") != nullptr);
}

#if defined(SPINUS_TEST_LV2_UI_BUNDLE)
TEST_CASE ("An LV2 plugin whose own window can't open gets Spinus's sliders", "[ui][pluginwindow][lv2-gui]")
{
    if (juce::SystemStats::getEnvironmentVariable ("DISPLAY", {}).isEmpty())
        SKIP ("no X display");

    Fixture f;
    const auto bus = f.addInsertWith ({});
    REQUIRE (f.mixer.renameInsert (bus, "Bus"));
    const auto fails = f.addLv2 (bus, "fails");
    const auto opens = f.addLv2 (bus, "null-resize"); // as DrumGizmo, which crashed Spinus here

    const auto content = [] (const juce::String& title) -> juce::Component*
    {
        auto* window = dynamic_cast<juce::ResizableWindow*> (Fixture::findWindow (title));
        return window != nullptr ? window->getContentComponent() : nullptr;
    };
    const auto isGeneric = [] (juce::Component* c)
    { return dynamic_cast<juce::GenericAudioProcessorEditor*> (c->getChildComponent (0)) != nullptr; };

    for (int attempt = 0; attempt < 2; ++attempt) // and again after closing
    {
        INFO (f.mixer.getLastError());
        REQUIRE (f.mixer.showEffectWindow (fails));
        auto* sliders = content ("Spinus LV2 UI test (fails) - 1 Bus");
        REQUIRE (sliders != nullptr);
        CHECK (isGeneric (sliders));
        f.session.closeDeviceWindow (fails);

        REQUIRE (f.mixer.showEffectWindow (opens));
        auto* own = content ("Spinus LV2 UI test (null-resize) - 1 Bus");
        REQUIRE (own != nullptr);
        CHECK (! isGeneric (own));
        CHECK (own->getWidth() == 320); // the plugin's own window
        f.session.closeDeviceWindow (opens);
    }
}
#endif

TEST_CASE ("A missing plugin has no window, and the mixer says why", "[ui][pluginwindow]")
{
    Fixture f;
    model::Device device;
    device.kind = model::Device::Kind::plugin;
    device.plugin = { "VST3", "Not Installed", "Nobody", "/nowhere/missing.vst3", 1 };
    auto parameters = juce::JSON::parse (R"({"insert": 1})");
    parameters.getDynamicObject()->setProperty ("device", model::deviceToJson (device));

    juce::String error;
    REQUIRE (f.session.getHistory()
                 .perform (commands::CommandRegistry::get().create ("insert.addEffect", parameters, error))
                 .wasOk());
    const auto missing = f.session.getProject().mixer.master.effects[0].device.id;

    CHECK (! f.mixer.showEffectWindow (missing));
    CHECK (f.mixer.getLastError().contains ("cannot be opened"));
}

TEST_CASE ("A plugin's own window opens again after it was closed", "[ui][pluginwindow]")
{
#if defined(__SANITIZE_ADDRESS__)
    SKIP ("real plugin GUIs are not run under AddressSanitizer");
#endif
    if (! bundledPlugins().getChildFile ("Surge XT.clap").existsAsFile())
        SKIP ("Surge XT not built");

    Fixture f;
    ChannelRack rack (f.session);
    REQUIRE (rack.addChannel ("bundled.surge-xt", "Lead"));
    const auto& channel = f.session.getProject().channels[0];
    REQUIRE (channel.instrument.has_value());
    const auto device = channel.instrument.value_or (model::Device {}).id;
    auto* plugin = dynamic_cast<te::ExternalPlugin*> (f.session.getAdapter().getDevicePlugin (device));
    REQUIRE (plugin != nullptr);
    REQUIRE (plugin->getAudioPluginInstance() != nullptr);

    for (int round = 0; round < 2; ++round)
    {
        INFO ("open number " << round + 1);
        REQUIRE (rack.showInstrumentWindow (channel.id));
        // Let the window manager map the window and the plugin attach to it (and focus changes run).
        juce::MessageManager::getInstance()->runDispatchLoopUntil (1000);

        // Surge opens straight on its own editor, like any other plugin: no Spinus panel.
        auto* window = dynamic_cast<PluginWindow*> (Fixture::findWindow ("Surge XT - Lead"));
        REQUIRE (window != nullptr);
        CHECK (window->hasEditor());
        auto* editor = plugin->getAudioPluginInstance()->getActiveEditor();
        REQUIRE (editor != nullptr);
        // Surge's own editor, not the generic one with sliders, embedded in the window.
        CHECK (dynamic_cast<juce::GenericAudioProcessorEditor*> (editor) == nullptr);
        CHECK (plugins::ClapPluginFormat::isEditorAttached (editor));
        // The window holds the editor and nothing else: no bar or button above it.
        CHECK (editor->getParentComponent() == window->getContentComponent());
        CHECK (window->getContentComponent()->getNumChildComponents() == 1);
        CHECK (editor->getY() == 0);

        f.session.closeDeviceWindow (device);
        CHECK (plugin->getAudioPluginInstance()->getActiveEditor() == nullptr);
    }
}
