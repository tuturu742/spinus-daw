#include <catch2/catch_test_macros.hpp>

#include "../model/ExampleProject.h"
#include "commands/CommandRegistry.h"
#include "session/Suggestions.h"

using namespace spinus;
using namespace spinus::model;

namespace
{
// IDs in makeExample(): channels kick 7 and synth 9; pattern 11 (the kick on every beat, one synth note).
constexpr ChannelId kick { 7 }, synth { 9 };
constexpr PatternId beat { 11 };

Suggestions::Suggestion bassLine (const juce::String& title, int pitch)
{
    return { title, beat, { { synth, { Note { 0, 480, pitch, 0.8f }, Note { 960, 480, pitch + 7, 0.7f } } } } };
}
} // namespace

TEST_CASE ("Accepting a suggestion writes its notes as one undo step; discarding changes nothing", "[suggestions]")
{
    commands::CommandHistory history (test::makeExample());
    Suggestions suggestions (history);
    CHECK (! suggestions.isShowing());
    CHECK (suggestions.accept().failed());

    suggestions.show ({ bassLine ("Bass line", 33) });
    REQUIRE (suggestions.isShowing());
    CHECK (suggestions.getCurrent()->title == "Bass line");
    CHECK (history.getProject() == test::makeExample()); // only shown

    suggestions.discard();
    CHECK (! suggestions.isShowing());
    CHECK (! history.canUndo());

    suggestions.show ({ bassLine ("Bass line", 33) });
    REQUIRE (suggestions.accept().wasOk());
    CHECK (! suggestions.isShowing());
    const auto& notes = history.getProject().findPattern (beat)->notes;
    REQUIRE (notes.at (synth).size() == 2);
    CHECK (notes.at (synth)[1].pitch == 40);
    CHECK (notes.at (kick).size() == 4); // other channels untouched
    CHECK (history.getUndoDescriptions().size() == 1);
    CHECK (history.getUndoDescriptions()[0] == "Accept suggestion: Bass line");

    history.undo();
    CHECK (history.getProject() == test::makeExample());
}

TEST_CASE ("Suggestions: variations, regenerate, several channels, and a pattern that went away", "[suggestions]")
{
    commands::CommandHistory history (test::makeExample());
    Suggestions suggestions (history);

    int asked = 0;
    suggestions.show ({ bassLine ("One", 33), bassLine ("Two", 36), bassLine ("Three", 38) },
                      { "Test source", [&] { ++asked; } });
    CHECK (suggestions.getNumVariations() == 3);
    suggestions.showVariation (2);
    CHECK (suggestions.getCurrent()->title == "Three");
    suggestions.showVariation (5); // out of range: ignored
    CHECK (suggestions.getVariationIndex() == 2);

    CHECK (suggestions.regenerate());
    CHECK (asked == 1);
    suggestions.setWorking ("Generating...");
    CHECK (suggestions.getWorkingMessage() == "Generating...");
    suggestions.show ({ bassLine ("Four", 31) }); // the source's answer
    CHECK (suggestions.getWorkingMessage().isEmpty());
    CHECK (! suggestions.regenerate()); // this one came without a way to regenerate

    // A suggestion for two channels is one undo step too.
    auto both = bassLine ("Groove", 33);
    both.notes[kick] = { Note { 0, 240, 36, 1.0f } };
    suggestions.show ({ both });
    REQUIRE (suggestions.accept().wasOk());
    CHECK (history.getProject().findPattern (beat)->notes.at (kick).size() == 1);
    CHECK (history.getUndoDescriptions().size() == 1);

    // The pattern was removed meanwhile: accepting says so and keeps the suggestion.
    suggestions.show ({ bassLine ("Late", 33) });
    juce::String error;
    REQUIRE (history
                 .perform (commands::CommandRegistry::get().create ("pattern.remove",
                                                                    juce::JSON::parse (R"({"pattern": 11})"), error))
                 .wasOk());
    const auto result = suggestions.accept();
    CHECK (result.failed());
    CHECK (result.getErrorMessage().contains ("no longer exists"));
    CHECK (suggestions.isShowing());
}

TEST_CASE ("A suggestion longer than its pattern makes the pattern longer when accepted (and while listening)",
           "[suggestions]")
{
    commands::CommandHistory history (test::makeExample());
    Suggestions suggestions (history);
    const auto before = history.getProject().findPattern (beat)->length;
    auto longer = bassLine ("Eight bars", 33);
    longer.notes[synth].push_back (Note { before + 960, 480, 36, 0.8f });
    longer.patternLength = before * 2;
    suggestions.show ({ longer });

    suggestions.setListening (true);
    CHECK (suggestions.withSuggestion (history.getProject()).findPattern (beat)->length == before * 2);
    CHECK (history.getProject().findPattern (beat)->length == before); // only heard

    REQUIRE (suggestions.accept().wasOk());
    CHECK (history.getProject().findPattern (beat)->length == before * 2);
    CHECK (history.getUndoDescriptions().size() == 1);
    history.undo();
    CHECK (history.getProject().findPattern (beat)->length == before);
}
