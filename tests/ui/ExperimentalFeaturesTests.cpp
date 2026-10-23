#include <catch2/catch_test_macros.hpp>

#include "ai/AiService.h"
#include "engine/EngineHost.h"
#include "ui/AiSettingsDialog.h"
#include "ui/MainView.h"

using namespace spinus;
using namespace spinus::ui;

/* Options > AI > Show experimental features: the Analyzer is experimental, and is
   nowhere in the main window while that is off. */

namespace
{
struct TempFolder
{
    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-experimental-tests-" + juce::Uuid().toString());
};

struct Ui
{
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    MainView view { session };
};

bool invoke (MainView& view, int command) { return view.getCommandManager().invokeDirectly (command, false); }

bool viewMenuHas (MainView& view, juce::CommandID command)
{
    auto menu = view.getMenuForIndex (2, "View");
    for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next();)
        if (it.getItem().itemID == command)
            return true;
    return false;
}

/** A layout (as layouts.xml keeps it) with the Analyzer open in front of the Playlist. */
juce::ValueTree layoutWithAnalyzer (MainView& view)
{
    auto layout = view.getCurrentLayout();
    for (auto panel : layout)
        if (panel["id"] == "analyzer" || panel["id"] == "playlist")
            panel.setProperty ("visible", true, nullptr);
    // Back to front: the Analyzer last.
    auto analyzer = layout.getChildWithProperty ("id", "analyzer");
    layout.removeChild (analyzer, nullptr);
    layout.appendChild (analyzer, nullptr);
    return layout;
}

/** Every way to open the Analyzer: false if none worked. */
bool canOpenAnalyzer (MainView& view)
{
    auto& workspace = view.getWorkspace();
    workspace.hidePanel ("analyzer");
    invoke (view, MainView::showAnalyzer);
    workspace.showPanel ("analyzer");
    workspace.detachPanel ("analyzer");
    view.getWindowLayouts().save ("With the Analyzer", layoutWithAnalyzer (view));
    view.useLayout ("With the Analyzer");
    view.getWindowLayouts().remove ("With the Analyzer");
    const bool opened = workspace.isPanelVisible ("analyzer");
    workspace.hidePanel ("analyzer");
    return opened;
}
} // namespace

TEST_CASE ("Show experimental features is off by default and remembered", "[ui][ai][experimental]")
{
    TempFolder t;
    {
        ai::AiService service (t.root);
        CHECK (! service.showsExperimentalFeatures());
        int changes = 0;
        const auto listener = service.addStateListener ([&] { ++changes; });
        service.setShowExperimentalFeatures (true);
        CHECK (service.showsExperimentalFeatures());
        CHECK (changes == 1);
        CHECK (! service.isEnabled()); // independent of AI being on
        service.removeStateListener (listener);
    }
    {
        ai::AiService service (t.root);
        CHECK (service.showsExperimentalFeatures());
        service.setEnabled (true); // turning AI on or off keeps it
        service.setEnabled (false);
    }
    ai::AiService service (t.root);
    CHECK (service.showsExperimentalFeatures());
    service.setShowExperimentalFeatures (false);
    CHECK (! ai::AiService (t.root).showsExperimentalFeatures());
}

TEST_CASE ("Without the AI service the Analyzer is nowhere", "[ui][experimental]")
{
    Ui ui;
    ui.view.setSize (1280, 800);
    CHECK (! ui.view.areExperimentalFeaturesShown());
    CHECK (! viewMenuHas (ui.view, MainView::showAnalyzer));
    CHECK (viewMenuHas (ui.view, MainView::showAutomixer));
    CHECK (! ui.view.isCommandAvailable (MainView::showAnalyzer)); // not in Options > Keyboard shortcuts
    CHECK (! invoke (ui.view, MainView::showAnalyzer));
    CHECK (! canOpenAnalyzer (ui.view));
    // The rest of the window works as before.
    CHECK (invoke (ui.view, MainView::showMixer));
    CHECK (ui.view.getWorkspace().isPanelVisible ("mixer"));
}

TEST_CASE ("Show experimental features: off, on, off again", "[ui][ai][experimental]")
{
    TempFolder t;
    ai::AiService service (t.root);
    Ui ui;
    ui.view.setSize (1280, 800);
    ui.view.setAiService (&service);
    auto& workspace = ui.view.getWorkspace();

    // Off by default: no Analyzer.
    CHECK (! ui.view.areExperimentalFeaturesShown());
    CHECK (! viewMenuHas (ui.view, MainView::showAnalyzer));
    CHECK (! ui.view.isCommandAvailable (MainView::showAnalyzer));
    CHECK (! canOpenAnalyzer (ui.view));

    // Turned on in Options > AI (AI itself still off): it is there at once.
    AiSettingsDialog settings (service);
    CHECK (settings.getExperimentalNote().contains ("Analyzer"));
    settings.setShowExperimental (true);
    CHECK (ui.view.areExperimentalFeaturesShown());
    CHECK (viewMenuHas (ui.view, MainView::showAnalyzer));
    CHECK (ui.view.isCommandAvailable (MainView::showAnalyzer));
    REQUIRE (invoke (ui.view, MainView::showAnalyzer));
    CHECK (workspace.isPanelVisible ("analyzer"));
    CHECK (workspace.getPanel ("analyzer")->getPanelTitle() == "Analyzer (experimental)");
    juce::ApplicationCommandInfo info (MainView::showAnalyzer);
    ui.view.getCommandInfo (MainView::showAnalyzer, info);
    CHECK (info.shortName.contains ("experimental"));
    CHECK (canOpenAnalyzer (ui.view));

    // A layout saved now, with the Analyzer open (as layouts.xml keeps the last one).
    const auto saved = layoutWithAnalyzer (ui.view);
    ui.view.getWindowLayouts().save ("Study", saved);
    REQUIRE (ui.view.useLayout ("Study"));
    CHECK (workspace.isPanelVisible ("analyzer"));

    // Turned off while the Analyzer is open: it closes, and nothing brings it back.
    settings.setShowExperimental (false);
    CHECK (! ui.view.areExperimentalFeaturesShown());
    CHECK (! workspace.isPanelVisible ("analyzer"));
    CHECK (! viewMenuHas (ui.view, MainView::showAnalyzer));
    CHECK (! invoke (ui.view, MainView::showAnalyzer));
    REQUIRE (ui.view.useLayout ("Study"));
    CHECK (! workspace.isPanelVisible ("analyzer"));
    CHECK (workspace.isPanelVisible ("playlist")); // the rest of the layout still applies
    CHECK (! canOpenAnalyzer (ui.view));

    // A detached Analyzer closes too.
    settings.setShowExperimental (true);
    workspace.showPanel ("analyzer");
    workspace.detachPanel ("analyzer");
    CHECK (workspace.isPanelDetached ("analyzer"));
    settings.setShowExperimental (false);
    CHECK (! workspace.isPanelVisible ("analyzer"));
    CHECK (! workspace.isPanelDetached ("analyzer"));
}

TEST_CASE ("Show experimental features, remembered: the Analyzer is there at the next start", "[ui][ai][experimental]")
{
    TempFolder t;
    ai::AiService (t.root).setShowExperimentalFeatures (true);

    ai::AiService service (t.root);
    Ui ui;
    ui.view.setSize (1280, 800);
    CHECK (! ui.view.areExperimentalFeaturesShown()); // until the main window has the service
    ui.view.setAiService (&service);
    CHECK (ui.view.areExperimentalFeaturesShown());
    CHECK (viewMenuHas (ui.view, MainView::showAnalyzer));
    CHECK (canOpenAnalyzer (ui.view));
}
