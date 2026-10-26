#include <catch2/catch_test_macros.hpp>

#include "ai/AiService.h"
#include "engine/EngineHost.h"
#include "ui/AiSettingsDialog.h"
#include "ui/MainView.h"

#include <vector>

using namespace spinus;
using namespace spinus::ui;

/* Options > AI > Show experimental features: the Analyzer and the Track creator are experimental, and are
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

/** The experimental panels and the commands that open them. */
const std::vector<std::pair<juce::String, juce::CommandID>> experimentalPanels {
    { "analyzer", MainView::showAnalyzer },
    { "trackCreator", MainView::showTrackCreator },
};

/** A layout (as layouts.xml keeps it) with a panel open in front of the Playlist. */
juce::ValueTree layoutWith (MainView& view, const juce::String& id = "analyzer")
{
    auto layout = view.getCurrentLayout();
    for (auto panel : layout)
        if (panel["id"] == id || panel["id"] == "playlist")
            panel.setProperty ("visible", true, nullptr);
    // Back to front: the panel last.
    auto front = layout.getChildWithProperty ("id", id);
    layout.removeChild (front, nullptr);
    layout.appendChild (front, nullptr);
    return layout;
}

juce::ValueTree layoutWithAnalyzer (MainView& view) { return layoutWith (view, "analyzer"); }

/** Every way to open an experimental panel: false if none worked. */
bool canOpen (MainView& view, const juce::String& id, juce::CommandID command)
{
    auto& workspace = view.getWorkspace();
    workspace.hidePanel (id);
    invoke (view, command);
    workspace.showPanel (id);
    workspace.detachPanel (id);
    view.getWindowLayouts().save ("With it", layoutWith (view, id));
    view.useLayout ("With it");
    view.getWindowLayouts().remove ("With it");
    const bool opened = workspace.isPanelVisible (id);
    workspace.hidePanel (id);
    return opened;
}

bool canOpenAnalyzer (MainView& view) { return canOpen (view, "analyzer", MainView::showAnalyzer); }

/** No experimental panel can be opened, nor is in the View menu or the keyboard shortcuts. */
bool noneAvailable (MainView& view)
{
    for (auto& [id, command] : experimentalPanels)
        if (viewMenuHas (view, command) || view.isCommandAvailable (command) || invoke (view, command) ||
            canOpen (view, id, command))
            return false;
    return true;
}

/** Each experimental panel is in the View menu, the keyboard shortcuts, and opens every way. */
bool allAvailable (MainView& view)
{
    for (auto& [id, command] : experimentalPanels)
        if (! viewMenuHas (view, command) || ! view.isCommandAvailable (command) || ! canOpen (view, id, command))
            return false;
    return true;
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
    CHECK (noneAvailable (ui.view)); // the Track creator neither
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

    // Off by default: no Analyzer, no Track creator.
    CHECK (! ui.view.areExperimentalFeaturesShown());
    CHECK (! viewMenuHas (ui.view, MainView::showAnalyzer));
    CHECK (! ui.view.isCommandAvailable (MainView::showAnalyzer));
    CHECK (! canOpenAnalyzer (ui.view));
    CHECK (noneAvailable (ui.view));

    // Turned on in Options > AI (AI itself still off): it is there at once.
    AiSettingsDialog settings (service);
    CHECK (settings.getExperimentalNote().contains ("Analyzer"));
    CHECK (settings.getExperimentalNote().contains ("Track creator"));
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
    CHECK (allAvailable (ui.view));
    REQUIRE (invoke (ui.view, MainView::showTrackCreator));
    CHECK (workspace.getPanel ("trackCreator")->getPanelTitle() == "Track creator (experimental)");
    juce::ApplicationCommandInfo creatorInfo (MainView::showTrackCreator);
    ui.view.getCommandInfo (MainView::showTrackCreator, creatorInfo);
    CHECK (creatorInfo.shortName == "Track creator (experimental)");
    workspace.hidePanel ("trackCreator");

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
    CHECK (noneAvailable (ui.view));

    // The same for an open Track creator, in a layout of its own.
    settings.setShowExperimental (true);
    ui.view.getWindowLayouts().save ("References", layoutWith (ui.view, "trackCreator"));
    REQUIRE (ui.view.useLayout ("References"));
    CHECK (workspace.isPanelVisible ("trackCreator"));
    settings.setShowExperimental (false);
    CHECK (! workspace.isPanelVisible ("trackCreator"));
    REQUIRE (ui.view.useLayout ("References"));
    CHECK (! workspace.isPanelVisible ("trackCreator"));
    CHECK (noneAvailable (ui.view));

    // A detached Analyzer (or Track creator) closes too.
    settings.setShowExperimental (true);
    workspace.showPanel ("analyzer");
    workspace.detachPanel ("analyzer");
    CHECK (workspace.isPanelDetached ("analyzer"));
    settings.setShowExperimental (false);
    CHECK (! workspace.isPanelVisible ("analyzer"));
    CHECK (! workspace.isPanelDetached ("analyzer"));
    settings.setShowExperimental (true);
    workspace.showPanel ("trackCreator");
    workspace.detachPanel ("trackCreator");
    CHECK (workspace.isPanelDetached ("trackCreator"));
    settings.setShowExperimental (false);
    CHECK (! workspace.isPanelVisible ("trackCreator"));
    CHECK (! workspace.isPanelDetached ("trackCreator"));
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
    CHECK (allAvailable (ui.view));
}
