#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "ui/HistoryPanel.h"

using namespace spinus;
using namespace spinus::ui;

TEST_CASE ("The History panel lists steps and jumps to any of them", "[ui][history]")
{
    EngineHost host ({ .openAudioDevice = false });
    Session session (host.getEngine());
    HistoryPanel panel (session);

    auto perform = [&] (const char* name, const char* params)
    {
        juce::String error;
        REQUIRE (session.getHistory()
                     .perform (commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error))
                     .wasOk());
    };

    perform ("project.setTempo", R"({"bpm": 100})");
    perform ("insert.add", R"({"name": "Drums"})");
    perform ("project.setTempo", R"({"bpm": 140})");
    panel.updateNow();

    REQUIRE (panel.getNumRows() == 4);
    CHECK (panel.getRowText (0) == "Open project");
    CHECK (panel.getRowText (1) == "Set tempo to 100.00 BPM");
    CHECK (panel.getRowText (2) == "Add mixer insert");
    CHECK (panel.getCurrentRow() == 3);

    // Back to after the first step: two undos; the undone steps stay listed (redoable).
    panel.goTo (1);
    CHECK (panel.getCurrentRow() == 1);
    CHECK (panel.getNumRows() == 4);
    CHECK (juce::approximatelyEqual (session.getProject().settings.tempoBpm, 100.0));
    CHECK (session.getProject().mixer.inserts.empty());
    CHECK (panel.getRowText (3) == "Set tempo to 140.00 BPM");

    // Forward again, then all the way back.
    panel.goTo (3);
    CHECK (juce::approximatelyEqual (session.getProject().settings.tempoBpm, 140.0));
    panel.goTo (0);
    CHECK (! session.getHistory().canUndo());

    // A new step after going back drops the redo steps.
    perform ("insert.add", R"({"name": "Bass"})");
    panel.updateNow();
    CHECK (panel.getNumRows() == 2);
}
