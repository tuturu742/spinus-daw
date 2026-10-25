#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "ui/ChannelRack.h"
#include "model/Chords.h"
#include "model/NoteTools.h"
#include "ui/PianoRoll.h"

using namespace spinus;
using namespace spinus::ui;

namespace
{
juce::TextButton* buttonOf (juce::Component& bar, const juce::String& text)
{
    for (auto* child : bar.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*> (child);
            button != nullptr && button->getButtonText() == text)
            return button;
    return nullptr;
}

struct Roll
{
    Roll() { roll.setSize (760, 420); }

    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    ChannelRack rack { session };
    PianoRoll roll { session };
};
} // namespace

TEST_CASE ("The piano roll needs a channel", "[ui][pianoroll]")
{
    Roll r;
    CHECK (! r.roll.addNote (60, 0, 240, 0.8f));
    CHECK (r.session.getProject().patterns.empty()); // nothing was created
}

TEST_CASE ("Notes are added, moved, resized, re-velocitied and deleted, one undo step each", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Lead"));
    CHECK (r.session.getSelectedChannel() == r.session.getProject().channels[0].id);

    REQUIRE (r.roll.addNote (64, 960, 480, 0.8f)); // also creates a one-bar pattern
    REQUIRE (r.roll.addNote (60, 0, 240, 0.5f));
    REQUIRE (r.roll.getNotes().size() == 2);
    CHECK (r.roll.getNotes()[0].pitch == 60); // sorted by start
    CHECK (r.roll.getNotes()[1].start == 960);

    REQUIRE (r.roll.moveNote (0, 240, 7));
    CHECK (r.roll.getNotes()[0].start == 240);
    CHECK (r.roll.getNotes()[0].pitch == 67);

    REQUIRE (r.roll.resizeNote (1, 720));
    CHECK (r.roll.getNotes()[1].length == 720);

    REQUIRE (r.roll.setVelocity (1, 0.25f));
    CHECK (juce::approximatelyEqual (r.roll.getNotes()[1].velocity, 0.25f));

    REQUIRE (r.roll.deleteNote (0));
    CHECK (r.roll.getNotes().size() == 1);

    const auto history = r.session.getHistory().getUndoDescriptions();
    CHECK (history.joinIntoString (" | ").endsWith (
        "Add note | Add note | Move note | Resize note | Change velocity | Delete note"));

    // The engine plays what the piano roll shows.
    const auto description = r.session.getAdapter().describe();
    CHECK (description.contains ("pattern preview channel"));
    CHECK (description.contains ("64@1.000"));

    for (int i = 0; i < 5; ++i)
        r.session.getHistory().undo();
    r.roll.handleUpdateNowIfNeeded();
    CHECK (r.roll.getNotes().size() == 1);
}

TEST_CASE ("Grid coordinates and snapping", "[ui][pianoroll]")
{
    Roll r;
    for (model::Ticks t : { (model::Ticks)0, (model::Ticks)240, (model::Ticks)1000, (model::Ticks)3840 })
        CHECK (std::abs (r.roll.xToTick (r.roll.tickToX (t)) - t) <= 10);
    for (int pitch : { 0, 36, 60, 127 })
        CHECK (r.roll.yToPitch (r.roll.pitchToY (pitch) + 1.0f) == pitch);

    CHECK (r.roll.snapTicks (100) == 0);
    CHECK (r.roll.snapTicks (130) == 240);
    CHECK (r.roll.snapTicks (1000) == 960);
}

TEST_CASE ("Snap follows the pattern's steps, triplets too, or a chosen value", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Lead"));
    REQUIRE (r.roll.addNote (60, 0, 240, 0.8f)); // makes the pattern
    CHECK (r.roll.getSnapChoice() == PianoRoll::Snap::step);
    CHECK (r.roll.getSnap() == 240);

    // Triplet steps in the Channel Rack: the Piano Roll's grid is in sixteenth-note triplets too.
    REQUIRE (r.rack.setTripletSteps (true));
    r.session.dispatchPendingMessages();
    r.roll.handleUpdateNowIfNeeded();
    CHECK (r.roll.getSnap() == 160);
    CHECK (r.roll.snapTicks (250) == 320);

    for (auto [choice, ticks] :
         { std::pair { PianoRoll::Snap::bar, 3840 }, std::pair { PianoRoll::Snap::beat, 960 },
           std::pair { PianoRoll::Snap::eighth, 480 }, std::pair { PianoRoll::Snap::sixteenth, 240 },
           std::pair { PianoRoll::Snap::thirtySecond, 120 }, std::pair { PianoRoll::Snap::eighthTriplet, 320 },
           std::pair { PianoRoll::Snap::sixteenthTriplet, 160 }, std::pair { PianoRoll::Snap::none, 1 } })
    {
        r.roll.setSnapChoice (choice);
        CHECK (r.roll.getSnap() == ticks);
    }
    CHECK (r.roll.snapTicks (1001) == 1001); // none
}

TEST_CASE ("The selected channel follows channels being added and removed", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "A"));
    REQUIRE (r.rack.addChannel ({}, "B"));
    const auto& channels = r.session.getProject().channels;
    const auto a = channels[0].id, b = channels[1].id;
    CHECK (r.session.getSelectedChannel() == b);

    r.session.setSelectedChannel (a);
    CHECK (r.session.getSelectedChannel() == a);

    REQUIRE (r.rack.removeChannel (a));
    CHECK (r.session.getSelectedChannel() == b);
}

TEST_CASE ("Ghost notes: the other channels' notes show faintly and clicks pass through them", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Drums"));
    const auto drums = r.session.getSelectedChannel();
    REQUIRE (r.roll.addNote (36, 0, 240, 0.9f));
    REQUIRE (r.roll.addNote (38, 960, 240, 0.9f));
    REQUIRE (r.rack.addChannel ({}, "Bass"));
    const auto bass = r.session.getSelectedChannel();
    REQUIRE (r.rack.addChannel ({}, "Pad"));
    REQUIRE (r.roll.addNote (60, 0, 3840, 0.5f));

    // Editing the Bass: the Drums and the Pad are ghosts; the Bass has none of its own yet.
    r.session.setSelectedChannel (bass);
    r.session.dispatchPendingMessages();
    r.roll.handleUpdateNowIfNeeded();
    CHECK (r.roll.getNotes().empty());
    REQUIRE (r.roll.getGhosts().size() == 2);
    CHECK (r.roll.getGhosts()[0].channel == drums);
    CHECK (r.roll.getGhosts()[0].notes.size() == 2);
    CHECK (r.roll.getGhosts()[1].name == "Pad");

    // Clicking on a ghost note writes a Bass note there; the drums are untouched.
    auto* grid = r.roll.getChildComponent (0);
    REQUIRE (grid != nullptr);
    const juce::Point<float> onKick { r.roll.tickToX (120), r.roll.pitchToY (36) + 3.0f };
    juce::MouseEvent click (juce::Desktop::getInstance().getMainMouseSource(), onKick, {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            grid, grid, juce::Time::getCurrentTime(), onKick, juce::Time::getCurrentTime(), 1, false);
    grid->mouseDown (click);
    grid->mouseUp (click);
    r.roll.handleUpdateNowIfNeeded();
    REQUIRE (r.roll.getNotes().size() == 1);
    CHECK (r.roll.getNotes()[0].pitch == 36);
    CHECK (r.roll.getGhosts()[0].notes.size() == 2);

    // Choosing whose ghosts show; the selected channel never is one.
    r.roll.setGhostChannelShown (drums, false);
    REQUIRE (r.roll.getGhosts().size() == 1);
    CHECK (r.roll.getGhosts()[0].name == "Pad");
    r.roll.setGhostsShown (false);
    CHECK (r.roll.getGhosts().empty());
    r.roll.setGhostsShown (true);
    r.roll.setGhostChannelShown (drums, true);
    r.session.setSelectedChannel (drums);
    r.session.dispatchPendingMessages();
    r.roll.handleUpdateNowIfNeeded();
    REQUIRE (r.roll.getGhosts().size() == 2);
    CHECK (r.roll.getGhosts()[0].channel == bass);
}

TEST_CASE ("A suggestion shows over the edited channel's notes with Accept and Discard", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Bass"));
    const auto bass = r.session.getSelectedChannel();
    REQUIRE (r.roll.addNote (36, 0, 960, 0.8f));
    const auto pattern = r.session.getCurrentPattern();
    auto& suggestions = r.session.getSuggestions();
    auto& bar = r.roll.getSuggestionBar();
    CHECK (! bar.isVisible());
    CHECK (r.roll.getSuggestedNotes() == nullptr);

    auto line = [&] (int pitch)
    {
        return Suggestions::Suggestion {
            "Walking bass",
            pattern,
            { { bass, { model::Note { 0, 480, pitch, 0.8f }, model::Note { 480, 480, pitch + 2, 0.8f } } } }
        };
    };
    suggestions.show ({ line (36), line (40) });
    suggestions.dispatchPendingMessages();
    r.roll.handleUpdateNowIfNeeded();
    CHECK (bar.isVisible());
    REQUIRE (r.roll.getSuggestedNotes() != nullptr);
    CHECK (r.roll.getSuggestedNotes()->size() == 2);
    CHECK (r.roll.getNotes().size() == 1); // the real notes are still the old ones

    // The bar's buttons: the next variation, then Accept.
    auto* buttons = dynamic_cast<juce::TextButton*> (bar.getChildComponent (1));
    REQUIRE (buttons != nullptr);
    CHECK (buttons->getButtonText() == ">");
    buttons->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    r.roll.handleUpdateNowIfNeeded();
    CHECK ((*r.roll.getSuggestedNotes())[0].pitch == 40);

    auto* accept = buttonOf (bar, "Accept");
    REQUIRE (accept != nullptr);
    CHECK (accept->getButtonText() == "Accept");
    accept->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    r.roll.handleUpdateNowIfNeeded();
    CHECK (! bar.isVisible());
    REQUIRE (r.roll.getNotes().size() == 2);
    CHECK (r.roll.getNotes()[0].pitch == 40);
    CHECK (
        r.session.getHistory().getUndoDescriptions().joinIntoString ("|").endsWith ("Accept suggestion: Walking bass"));

    // Discard leaves the notes.
    suggestions.show ({ line (30) });
    suggestions.discard();
    suggestions.dispatchPendingMessages();
    r.roll.handleUpdateNowIfNeeded();
    CHECK (! bar.isVisible());
    CHECK (r.roll.getNotes()[0].pitch == 40);
}

TEST_CASE ("Listen plays a suggestion instead of the pattern's notes, without changing the project", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Bass"));
    const auto bass = r.session.getSelectedChannel();
    REQUIRE (r.roll.addNote (36, 0, 960, 0.8f));
    const auto pattern = r.session.getCurrentPattern();
    const auto before = r.session.getProject();
    auto& suggestions = r.session.getSuggestions();
    auto& bar = r.roll.getSuggestionBar();
    auto plays = [&] (const char* note) { return r.session.getAdapter().describe().contains (note); };
    auto settle = [&]
    {
        suggestions.dispatchPendingMessages();
        r.roll.handleUpdateNowIfNeeded();
    };

    auto line = [&] (int pitch)
    { return Suggestions::Suggestion { "Bass", pattern, { { bass, { model::Note { 480, 480, pitch, 0.8f } } } } }; };
    suggestions.show ({ line (43), line (45) });
    settle();
    CHECK (plays ("36@0.000"));
    CHECK (! plays ("43@"));

    auto* listen = buttonOf (bar, "Listen");
    REQUIRE (listen != nullptr);
    CHECK (listen->isVisible());
    listen->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    settle();
    CHECK (suggestions.isListening());
    CHECK (listen->getToggleState());
    CHECK (r.session.isPlaying()); // listening while stopped plays the pattern
    CHECK (plays ("43@"));
    CHECK (! plays ("36@0.000"));
    CHECK (r.session.getProject() == before); // only heard
    CHECK (! r.session.getHistory().getUndoDescriptions().joinIntoString ("|").contains ("Accept"));

    suggestions.showVariation (1); // the next variation is heard at once
    CHECK (plays ("45@"));

    listen->triggerClick(); // off again: the pattern's own notes
    juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    CHECK (! suggestions.isListening());
    CHECK (plays ("36@0.000"));

    // Accept while listening: the notes are now the pattern's own, and listening is over.
    suggestions.setListening (true);
    REQUIRE (suggestions.accept().wasOk());
    settle();
    CHECK (! suggestions.isListening());
    CHECK (plays ("45@"));
    r.session.getHistory().undo();
    CHECK (plays ("36@0.000"));

    // Discard while listening: back to the pattern's notes.
    suggestions.show ({ line (50) });
    suggestions.setListening (true);
    CHECK (plays ("50@"));
    suggestions.discard();
    CHECK (! suggestions.isListening());
    CHECK (! plays ("50@"));
    r.session.stop();
}

TEST_CASE ("Scale highlighting follows the song's key, or another key, or none", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Lead"));
    CHECK (r.roll.isFollowingSongKey());
    CHECK (! r.roll.getHighlightedKey()); // the song has no key yet
    CHECK (r.roll.getScaleButtonText() == "Scale: off");

    juce::String error;
    auto setKey = commands::CommandRegistry::get().create (
        "project.setKey", juce::JSON::parse (R"({"tonic": 9, "scale": "minor"})"), error);
    REQUIRE (setKey != nullptr);
    REQUIRE (r.session.getHistory().perform (std::move (setKey)).wasOk());
    CHECK (r.roll.getHighlightedKey() == std::optional<model::Key> (model::Key { 9, "minor" }));
    CHECK (r.roll.getScaleButtonText() == "Scale: A minor");

    r.roll.highlightKey (model::Key { 2, "dorian" });
    CHECK (! r.roll.isFollowingSongKey());
    CHECK (r.roll.getScaleButtonText() == "Scale: D dorian");
    CHECK (r.session.getProject().settings.key == model::Key { 9, "minor" });   // a view setting only
    CHECK (r.roll.createComponentSnapshot (r.roll.getLocalBounds()).isValid()); // draws

    r.roll.highlightKey (std::nullopt);
    CHECK (! r.roll.getHighlightedKey());
    CHECK (r.roll.getScaleButtonText() == "Scale: off");
    r.roll.highlightSongKey();
    CHECK (r.roll.getScaleButtonText() == "Scale: A minor");
}

namespace
{
/** Sends a mouse gesture to the Piano Roll's grid: down at `from`, drags through `to`, up there. Positions
    in (ticks, pitch), at the middle of the row. */
struct Mouse
{
    explicit Mouse (PianoRoll& r, int child = 0) : roll (r), grid (r.getChildComponent (child)) {}

    juce::Point<float> at (model::Ticks tick, int pitch) const
    {
        return { roll.tickToX (tick), roll.pitchToY (pitch) + (float)roll.getKeyHeight() * 0.5f };
    }

    juce::MouseEvent event (juce::Point<float> p, juce::ModifierKeys mods, juce::Point<float> down, bool dragged,
                            int clicks = 1) const
    {
        return { juce::Desktop::getInstance().getMainMouseSource(),
                 p,
                 mods,
                 juce::MouseInputSource::defaultPressure,
                 juce::MouseInputSource::defaultOrientation,
                 juce::MouseInputSource::defaultRotation,
                 juce::MouseInputSource::defaultTiltX,
                 juce::MouseInputSource::defaultTiltY,
                 grid,
                 grid,
                 juce::Time::getCurrentTime(),
                 down,
                 juce::Time::getCurrentTime(),
                 clicks,
                 dragged };
    }

    /** Two clicks: the second one's mouseDown comes as a double-click. */
    void doubleClick (juce::Point<float> p)
    {
        click (p);
        const auto button = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
        grid->mouseDown (event (p, button, p, false, 2));
        grid->mouseUp (event (p, {}, p, false, 2));
        roll.handleUpdateNowIfNeeded();
    }

    void drag (juce::Point<float> from, std::initializer_list<juce::Point<float>> through, juce::ModifierKeys mods = {})
    {
        const auto button = mods.isPopupMenu() ? mods : mods.withFlags (juce::ModifierKeys::leftButtonModifier);
        grid->mouseDown (event (from, button, from, false));
        auto last = from;
        for (auto p : through)
        {
            grid->mouseDrag (event (p, button, from, true));
            last = p;
        }
        grid->mouseUp (event (last, mods, from, last != from));
        roll.handleUpdateNowIfNeeded();
    }

    void click (juce::Point<float> p, juce::ModifierKeys mods = {}) { drag (p, {}, mods); }

    PianoRoll& roll;
    juce::Component* grid;
};

std::vector<int> pitchesOf (const std::vector<model::Note>& notes)
{
    std::vector<int> pitches (notes.size());
    std::transform (notes.begin(), notes.end(), pitches.begin(), [] (auto& n) { return n.pitch; });
    return pitches;
}
} // namespace

TEST_CASE ("Several notes: select, move, resize and delete together, one undo step each", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Keys"));
    for (auto [pitch, start] : { std::pair { 60, 0 }, { 64, 480 }, { 67, 960 }, { 72, 1920 } })
        REQUIRE (r.roll.addNote (pitch, start, 240, 0.8f));
    r.roll.scrollToPitch (66);
    const auto steps = r.session.getHistory().getUndoDescriptions().size();

    // A box around the first three.
    r.roll.selectArea (0, 1000, 0, 127, false);
    CHECK (r.roll.getSelection() == std::set<int> { 0, 1, 2 });
    REQUIRE (r.roll.moveSelection (240, 2));
    CHECK (pitchesOf (r.roll.getNotes()) == std::vector<int> { 62, 66, 69, 72 });
    CHECK (r.roll.getNotes()[0].start == 240);
    CHECK (r.roll.getSelection() == std::set<int> { 0, 1, 2 }); // still selected after the edit
    REQUIRE (r.roll.resizeSelection (240));
    CHECK (r.roll.getNotes()[1].length == 480);
    CHECK (r.roll.getNotes()[3].length == 240); // not selected
    REQUIRE (r.roll.deleteSelection());
    CHECK (pitchesOf (r.roll.getNotes()) == std::vector<int> { 72 });
    CHECK (r.roll.getSelection().empty());
    CHECK (r.session.getHistory().getUndoDescriptions().size() == steps + 3);
    CHECK (r.session.getHistory().getUndoDescriptions().strings.getLast() == "Delete notes");

    // Undo brings them back, and the selection with them.
    r.session.getHistory().undo();
    r.roll.handleUpdateNowIfNeeded();
    CHECK (r.roll.getNotes().size() == 4);

    r.roll.selectAll();
    CHECK (r.roll.getSelection().size() == 4);
    r.roll.clearSelection();
    CHECK (r.roll.getSelection().empty());
}

TEST_CASE ("Slice cuts notes in two; paint lays notes one after another", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Keys"));
    REQUIRE (r.roll.addNote (60, 0, 960, 0.8f));
    REQUIRE (r.roll.addNote (64, 0, 960, 0.8f));
    REQUIRE (r.roll.addNote (67, 1920, 240, 0.8f));

    // Only the notes across the line (and between its pitches) are cut.
    REQUIRE (r.roll.slice (480, 60, 62));
    REQUIRE (r.roll.getNotes().size() == 4);
    CHECK (r.roll.getNotes()[0].pitch == 60);
    CHECK (r.roll.getNotes()[0].length == 480);
    CHECK (r.roll.getNotes()[2].start == 480); // sorted by start: 60, 64, then the second half of 60
    CHECK (r.roll.getNotes()[2].pitch == 60);
    CHECK (r.roll.getNotes()[2].length == 480);
    CHECK (! r.roll.slice (480, 60, 62)); // nothing left to cut there
    CHECK (r.session.getHistory().getUndoDescriptions().strings.getLast() == "Slice notes");

    // Paint: from beat 3 to beat 4, notes of the last length (240), each at its pitch.
    REQUIRE (r.roll.paint (2880, 3600, { 70, 72 }));
    std::vector<model::Note> painted;
    for (auto& n : r.roll.getNotes())
        if (n.start >= 2880)
            painted.push_back (n);
    REQUIRE (painted.size() == 4); // 2880, 3120, 3360, 3600
    CHECK (pitchesOf (painted) == std::vector<int> { 70, 72, 72, 72 });
    CHECK (r.session.getHistory().getUndoDescriptions().strings.getLast() == "Paint notes");
}

TEST_CASE ("Mouse: Ctrl+drag selects, dragging a selected note moves them all, right-drag deletes", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Keys"));
    for (auto [pitch, start] : { std::pair { 60, 0 }, { 62, 480 }, { 64, 960 } })
        REQUIRE (r.roll.addNote (pitch, start, 240, 0.8f));
    r.roll.scrollToPitch (62);
    Mouse mouse (r.roll);
    const auto ctrl = juce::ModifierKeys (juce::ModifierKeys::ctrlModifier);

    // Ctrl+drag a box over the first two.
    mouse.drag (mouse.at (0, 63), { mouse.at (600, 59) }, ctrl);
    CHECK (r.roll.getSelection() == std::set<int> { 0, 1 });

    // Shift+click adds the third, and takes it out again.
    mouse.click (mouse.at (1000, 64), juce::ModifierKeys::shiftModifier);
    CHECK (r.roll.getSelection().size() == 3);
    mouse.click (mouse.at (1000, 64), juce::ModifierKeys::shiftModifier);
    CHECK (r.roll.getSelection() == std::set<int> { 0, 1 });

    // Drag a selected note a beat later and up two: both move, one undo step.
    const auto steps = r.session.getHistory().getUndoDescriptions().size();
    mouse.drag (mouse.at (60, 60), { mouse.at (300, 61), mouse.at (540, 62) });
    CHECK (r.session.getHistory().getUndoDescriptions().size() == steps + 1);
    CHECK (r.session.getHistory().getUndoDescriptions().strings.getLast() == "Move notes");
    CHECK (pitchesOf (r.roll.getNotes()) == std::vector<int> { 62, 64, 64 });
    CHECK (r.roll.getNotes()[0].start == 480);
    CHECK (r.roll.getNotes()[1].start == 960);

    // Drag the end of a selected note: both get longer.
    const auto& first = r.roll.getNotes()[0];
    mouse.drag ({ r.roll.tickToX (first.start + first.length) - 3.0f, mouse.at (0, 62).y }, { mouse.at (960, 62) });
    CHECK (r.session.getHistory().getUndoDescriptions().strings.getLast() == "Resize notes");
    CHECK (r.roll.getNotes()[0].length == 480);
    CHECK (r.roll.getNotes()[1].length == 480);

    // Right-drag across the row of 64: every note it touches goes, in one step.
    mouse.drag (mouse.at (970, 64), { mouse.at (1100, 64), mouse.at (1300, 64) },
                juce::ModifierKeys::rightButtonModifier);
    CHECK (pitchesOf (r.roll.getNotes()) == std::vector<int> { 62 });
    CHECK (r.session.getHistory().getUndoDescriptions().strings.getLast() == "Delete notes");
}

TEST_CASE ("Mouse: the Paint, Slice and Select tools", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Keys"));
    REQUIRE (r.roll.addNote (60, 0, 960, 0.8f)); // the last length is now 960
    REQUIRE (r.roll.resizeNote (0, 960));
    REQUIRE (r.roll.addNote (62, 0, 240, 0.8f)); // and now 240
    r.roll.scrollToPitch (62);
    Mouse mouse (r.roll);

    r.roll.setTool (PianoRoll::Tool::slice);
    mouse.drag (mouse.at (480, 60), { mouse.at (480, 59) });
    CHECK (r.roll.getNotes().size() == 3);
    CHECK (r.session.getHistory().getUndoDescriptions().strings.getLast() == "Slice notes");

    r.roll.setTool (PianoRoll::Tool::paint);
    mouse.drag (mouse.at (1920, 65), { mouse.at (2200, 65), mouse.at (2650, 67) });
    std::vector<model::Note> painted;
    for (auto& n : r.roll.getNotes())
        if (n.start >= 1920)
            painted.push_back (n);
    CHECK (painted.size() == 4); // 1920, 2160, 2400, 2640
    CHECK (painted.back().pitch == 67);
    CHECK (r.session.getHistory().getUndoDescriptions().strings.getLast() == "Paint notes");

    r.roll.setTool (PianoRoll::Tool::select);
    mouse.drag (mouse.at (1800, 68), { mouse.at (2700, 64) });
    CHECK (r.roll.getSelection().size() == 4); // the painted notes
}

TEST_CASE ("Notes past the pattern's end make it longer, to the end of their bar, in the same step; removing "
           "them makes it shorter",
           "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Bass"));
    REQUIRE (r.roll.addNote (36, 0, 240, 0.8f)); // makes a one-bar pattern
    const auto* pattern = r.session.getProject().findPattern (r.session.getCurrentPattern());
    REQUIRE (pattern != nullptr);
    CHECK (pattern->length == 3840);

    REQUIRE (r.roll.addNote (38, 4800, 240, 0.8f)); // in the second bar
    CHECK (r.session.getProject().findPattern (r.session.getCurrentPattern())->length == 7680);
    REQUIRE (r.roll.addNote (40, 7500, 600, 0.8f)); // ends in the third bar
    CHECK (r.session.getProject().findPattern (r.session.getCurrentPattern())->length == 11520);

    // One undo step takes back the note and the length.
    REQUIRE (r.session.getHistory().undo());
    r.roll.handleUpdateNowIfNeeded();
    CHECK (r.session.getProject().findPattern (r.session.getCurrentPattern())->length == 7680);
    CHECK (r.roll.getNotes().size() == 2);

    // Removing the note in the second bar makes it one bar again.
    REQUIRE (r.roll.deleteNote (1));
    CHECK (r.session.getProject().findPattern (r.session.getCurrentPattern())->length == 3840);

    // A 3/4 pattern is three beats long, on the Playlist too.
    juce::String error;
    auto threeFour = commands::CommandRegistry::get().create (
        "pattern.setTimeSignature",
        juce::JSON::parse ("{\"pattern\": " + juce::String ((int)r.session.getCurrentPattern().value) +
                           ", \"numerator\": 3, \"denominator\": 4}"),
        error);
    REQUIRE (threeFour != nullptr);
    REQUIRE (r.session.getHistory().perform (std::move (threeFour)).wasOk());
    CHECK (r.session.getProject().findPattern (r.session.getCurrentPattern())->length == 2880);
    REQUIRE (r.session.getHistory().undo());
    CHECK (r.session.getProject().findPattern (r.session.getCurrentPattern())->length == 3840);
}

TEST_CASE ("Right-dragging the Piano Roll's timeline loops part of the pattern; right-click clears it",
           "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Keys"));
    REQUIRE (r.roll.addNote (60, 0, 240, 0.8f));
    Mouse ruler (r.roll, 2);
    const auto y = 10.0f;
    ruler.drag ({ r.roll.tickToX (965), y }, { juce::Point<float> (r.roll.tickToX (2000), y) },
                juce::ModifierKeys::rightButtonModifier);
    const auto region = r.session.getLoopRegion (Session::Mode::pattern);
    REQUIRE (region);
    const auto looped = region.value_or (Session::LoopRegion {});
    CHECK (looped.start == 960); // on the grid
    CHECK (looped.end == 1920);
    ruler.click ({ r.roll.tickToX (500), y }, juce::ModifierKeys::rightButtonModifier);
    CHECK (! r.session.getLoopRegion (Session::Mode::pattern));
}

TEST_CASE ("A pattern's own time signature sets its bars in the Piano Roll", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Lead"));
    REQUIRE (r.roll.addNote (60, 0, 240, 0.8f)); // a one-bar 4/4 pattern
    CHECK (r.roll.getBarLength() == 3840);

    juce::String error;
    auto command = commands::CommandRegistry::get().create (
        "pattern.setTimeSignature",
        juce::JSON::parse ("{\"pattern\": " + juce::String (r.session.getCurrentPattern().value) +
                           ", \"numerator\": 6, \"denominator\": 8}"),
        error);
    REQUIRE (command != nullptr);
    REQUIRE (r.session.getHistory().perform (std::move (command)).wasOk());
    CHECK (r.roll.getBarLength() == 2880);
    CHECK (r.session.getProject().settings.timeSignature == model::TimeSignature { 4, 4 }); // the song's stays

    // A note past the end grows the pattern by its own bars.
    REQUIRE (r.roll.addNote (62, 3900, 240, 0.8f));
    CHECK (r.session.getProject().findPattern (r.session.getCurrentPattern())->length == 2 * 2880);
}

TEST_CASE ("Copy, cut, paste and duplicate notes, also into another channel", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Lead"));
    const auto lead = r.session.getSelectedChannel();
    REQUIRE (r.roll.addNote (60, 0, 480, 0.8f));
    REQUIRE (r.roll.addNote (64, 480, 480, 0.8f));
    REQUIRE (r.roll.addNote (67, 1920, 480, 0.8f));
    REQUIRE (r.rack.addChannel ({}, "Pad"));
    const auto pad = r.session.getSelectedChannel();
    auto show = [&] (model::ChannelId channel)
    {
        r.session.setSelectedChannel (channel);
        r.session.dispatchPendingMessages();
        r.roll.handleUpdateNowIfNeeded();
    };

    // The lead's first two notes, into the pad at the same times; the pasted notes are selected.
    show (lead);
    r.roll.selectArea (0, 900, 0, 127, false);
    REQUIRE (r.roll.copySelection());
    CHECK (PianoRoll::getCopiedNotes().size() == 2);
    show (pad);
    REQUIRE (r.roll.paste());
    REQUIRE (r.roll.getNotes().size() == 2);
    CHECK (r.roll.getNotes()[0].start == 0);
    CHECK (r.roll.getNotes()[1].start == 480);
    CHECK (r.roll.getSelection().size() == 2);
    CHECK (r.session.getHistory().getUndoDescriptions().strings.getLast() == "Paste notes");

    // Pasting again lands on the very same notes: right after them instead (the two take one beat).
    REQUIRE (r.roll.paste());
    REQUIRE (r.roll.getNotes().size() == 4);
    CHECK (r.roll.getNotes()[2].start == 960);
    CHECK (r.roll.getNotes()[3].start == 1440);

    // Duplicate: the selection (the last two) again right after itself.
    REQUIRE (r.roll.duplicateSelection());
    REQUIRE (r.roll.getNotes().size() == 6);
    CHECK (r.roll.getNotes()[4].start == 1920);

    // Cut: copied and gone, one undo step.
    r.roll.selectArea (0, 900, 0, 127, false);
    REQUIRE (r.roll.cutSelection());
    CHECK (r.roll.getNotes().size() == 4);
    r.session.getHistory().undo();
    r.roll.handleUpdateNowIfNeeded();
    CHECK (r.roll.getNotes().size() == 6);

    // Nothing selected: copy takes them all.
    show (lead);
    r.roll.clearSelection();
    REQUIRE (r.roll.copySelection());
    CHECK (PianoRoll::getCopiedNotes().size() == 3);
}

TEST_CASE ("Piano Roll keys: Shift+arrows, Ctrl+Up/Down an octave, the note tools", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Lead"));
    REQUIRE (r.roll.addNote (60, 0, 480, 0.8f));
    REQUIRE (r.roll.addNote (64, 480, 480, 0.8f));
    auto* grid = r.roll.getChildComponent (0);
    REQUIRE (grid != nullptr);
    using KP = juce::KeyPress;
    constexpr auto ctrl = juce::ModifierKeys::ctrlModifier, shift = juce::ModifierKeys::shiftModifier;
    const auto pitches = [&r]
    {
        std::vector<int> p;
        for (auto& n : r.roll.getNotes())
            p.push_back (n.pitch);
        return p;
    };

    // Nothing selected: the keys move every note, one undo step each.
    const auto before = r.session.getHistory().getUndoDescriptions().size();
    CHECK (grid->keyPressed (KP (KP::upKey, ctrl, 0)));
    CHECK (pitches() == std::vector<int> { 72, 76 });
    CHECK (grid->keyPressed (KP (KP::downKey, shift, 0)));
    CHECK (pitches() == std::vector<int> { 71, 75 });
    CHECK (r.session.getHistory().getUndoDescriptions().size() == before + 2);

    // A selection: only it moves. Shift+Right moves it a snap step.
    r.roll.setSelection ({ 1 });
    CHECK (grid->keyPressed (KP (KP::downKey, ctrl, 0)));
    CHECK (pitches() == std::vector<int> { 71, 63 });
    const auto was = r.roll.getNotes()[1].start;
    CHECK (grid->keyPressed (KP (KP::rightKey, shift, 0)));
    CHECK (r.roll.getNotes()[1].start > was);
    CHECK (r.roll.getNotes()[0].start == 0);

    // Nothing moves past the keyboard's ends or before the start.
    r.roll.setSelection ({ 0 });
    CHECK (grid->keyPressed (KP (KP::leftKey, shift, 0)));
    CHECK (r.roll.getNotes()[0].start == 0);
    CHECK (! r.roll.transposeSelection (60));
    CHECK (pitches()[0] == 71);

    // Ctrl+D selects nothing; Ctrl+L is legato on all the notes.
    CHECK (grid->keyPressed (KP ('d', ctrl, 0)));
    CHECK (r.roll.getSelection().empty());
    CHECK (grid->keyPressed (PianoRoll::getNoteToolKey (PianoRoll::NoteTool::legato)));
    CHECK (r.session.getHistory().getUndoDescriptions().strings.getLast() == "Legato");
    CHECK (r.roll.getNotes()[0].start + r.roll.getNotes()[0].length == r.roll.getNotes()[1].start);
}

TEST_CASE ("Chords: named, built from the key, and stamped with one click", "[ui][pianoroll]")
{
    using model::nameChord;
    CHECK (nameChord ({ 60, 64, 67 }) == "C");
    CHECK (nameChord ({ 64, 67, 72 }) == "C/E"); // first inversion
    CHECK (nameChord ({ 57, 60, 64, 67 }) == "Am7");
    CHECK (nameChord ({ 62, 65, 69, 72 }) == "Dm7");
    CHECK (nameChord ({ 55, 59, 62, 65 }) == "G7");
    CHECK (nameChord ({ 60, 64 }).isEmpty());     // two notes: no chord
    CHECK (nameChord ({ 60, 61, 62 }).isEmpty()); // not a chord Spinus knows
    CHECK (model::chordInKey (62, model::Key { 0, "major" }, 3) == std::vector<int> { 62, 65, 69 });     // D minor in C
    CHECK (model::chordInKey (69, model::Key { 9, "minor" }, 4) == std::vector<int> { 69, 72, 76, 79 }); // Am7
    CHECK (model::chordInKey (61, model::Key { 0, "major" }, 3).front() != 61); // C# isn't in C: moved into the key

    // Stamping a major chord: one click, three notes, one undo step, named above them.
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Keys"));
    r.roll.scrollToPitch (62);
    r.roll.setChordStamp (10); // Major, the first chord type
    Mouse mouse (r.roll);
    const auto steps = r.session.getHistory().getUndoDescriptions().size();
    mouse.click (mouse.at (0, 60));
    CHECK (pitchesOf (r.roll.getNotes()) == std::vector<int> { 60, 64, 67 });
    const auto descriptions = r.session.getHistory().getUndoDescriptions();
    CHECK (descriptions.strings.getLast() == "Add chord"); // the chord is one step (after the new pattern)
    CHECK (descriptions.size() - steps <= 2);
    r.roll.setChordStamp (1);
    CHECK (r.roll.stampPitches (60) == std::vector<int> { 60 });
}

TEST_CASE ("Note tools: quantize, strum, arpeggiate, chop, legato, humanize", "[ui][pianoroll]")
{
    namespace nt = model::noteTools;
    auto note = [] (int pitch, model::Ticks start, model::Ticks length)
    { return model::Note { start, length, pitch, 0.8f, 0.5f, 0.0f, 0.0f, {} }; };
    auto starts = [] (const std::vector<model::Note>& ns)
    {
        std::vector<model::Ticks> r;
        r.reserve (ns.size());
        for (auto& n : ns)
            r.push_back (n.start);
        return r;
    };

    CHECK (starts (nt::quantize ({ note (60, 130, 240), note (62, 470, 240) }, 240)) ==
           std::vector<model::Ticks> { 240, 480 });

    const std::vector<model::Note> chord { note (60, 0, 960), note (64, 0, 960), note (67, 0, 960) };
    const auto up = nt::strum (chord, 40, true);
    CHECK (starts (up) == std::vector<model::Ticks> { 0, 40, 80 });
    CHECK (up[2].pitch == 67);
    CHECK (up[2].start + up[2].length == 960);           // still ends together
    CHECK (nt::strum (chord, 40, false)[0].pitch == 67); // down: the highest first

    const auto arp = nt::arpeggiate (chord, 240);
    REQUIRE (arp.size() == 4);
    CHECK (pitchesOf (arp) == std::vector<int> { 60, 64, 67, 60 });
    CHECK (starts (arp) == std::vector<model::Ticks> { 0, 240, 480, 720 });

    CHECK (nt::chop ({ note (60, 0, 960) }, 240).size() == 4);
    CHECK (nt::chop ({ note (60, 0, 600) }, 240).back().length == 120); // the last piece is what's left

    const auto joined = nt::legato ({ note (60, 0, 100), note (62, 480, 100), note (64, 960, 100) });
    CHECK (joined[0].length == 480);
    CHECK (joined[1].length == 480);
    CHECK (joined[2].length == 100);

    juce::Random random (1);
    for (auto& n : nt::humanizeVelocity (chord, 0.15f, random))
        CHECK ((n.velocity >= 0.65f - 1.0e-4f && n.velocity <= 0.95f + 1.0e-4f));

    // Through the Piano Roll: only the selection changes, one undo step, the result selected.
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Keys"));
    for (auto pitch : { 60, 64, 67 })
        REQUIRE (r.roll.addNote (pitch, 0, 960, 0.8f));
    REQUIRE (r.roll.addNote (72, 1920, 240, 0.8f));
    r.roll.setSelection ({ 0, 1, 2 });
    REQUIRE (r.roll.applyNoteTool (PianoRoll::NoteTool::strumUp));
    CHECK (r.session.getHistory().getUndoDescriptions().strings.getLast() == "Strum up");
    CHECK (r.roll.getSelection().size() == 3);
    CHECK (r.roll.getNotes().back().start == 1920); // the unselected note didn't move
    r.session.getHistory().undo();
    r.roll.handleUpdateNowIfNeeded();
    CHECK (r.roll.getNotes()[1].start == 0);
}

TEST_CASE ("Double-clicking another channel's ghost note edits that channel", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Bass"));
    const auto bass = r.session.getSelectedChannel();
    REQUIRE (r.roll.addNote (48, 960, 480, 0.8f));
    REQUIRE (r.rack.addChannel ({}, "Lead"));
    const auto lead = r.session.getSelectedChannel();
    r.session.dispatchPendingMessages();
    r.roll.handleUpdateNowIfNeeded();
    REQUIRE (r.roll.getNotes().empty()); // the lead has none; the bass's note is a ghost
    r.roll.scrollToPitch (48);
    Mouse mouse (r.roll);
    const auto steps = r.session.getHistory().getUndoDescriptions().size();
    mouse.doubleClick (mouse.at (1000, 48));
    r.session.dispatchPendingMessages();
    r.roll.handleUpdateNowIfNeeded();
    CHECK (r.session.getSelectedChannel() == bass);
    CHECK (r.roll.getNotes().size() == 1); // the bass's own note, now edited
    // The note the first click added to the lead is gone again.
    const auto* pattern = r.session.getProject().findPattern (r.session.getCurrentPattern());
    CHECK ((! pattern->notes.contains (lead) || pattern->notes.at (lead).empty()));
    CHECK (r.session.getHistory().getUndoDescriptions().size() <= steps);
}

TEST_CASE ("A note placed or dragged sounds while the mouse is down", "[ui][pianoroll]")
{
    Roll r;
    REQUIRE (r.rack.addChannel ({}, "Keys"));
    r.roll.scrollToPitch (62);
    Mouse mouse (r.roll);
    const auto button = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
    const auto from = mouse.at (0, 60), to = mouse.at (0, 62);

    mouse.grid->mouseDown (mouse.event (from, button, from, false));
    CHECK (r.roll.getPreviewedPitches() == std::vector<int> { 60 });
    mouse.grid->mouseDrag (mouse.event (to, button, from, true));
    CHECK (r.roll.getPreviewedPitches() == std::vector<int> { 62 }); // follows the pitch
    mouse.grid->mouseUp (mouse.event (to, {}, from, true));
    CHECK (r.roll.getPreviewedPitches().empty());
    r.roll.handleUpdateNowIfNeeded();
    REQUIRE (r.roll.getNotes().size() == 1);

    // Picking up a note sounds it too; not while recording (it would be recorded).
    const auto on = mouse.at (0, 62);
    mouse.grid->mouseDown (mouse.event (on, button, on, false));
    CHECK (r.roll.getPreviewedPitches() == std::vector<int> { 62 });
    mouse.grid->mouseUp (mouse.event (on, {}, on, false));
    r.session.setRecordArmed (true);
    mouse.grid->mouseDown (mouse.event (on, button, on, false));
    CHECK (r.roll.getPreviewedPitches().empty());
    mouse.grid->mouseUp (mouse.event (on, {}, on, false));
    r.session.setRecordArmed (false);
}
