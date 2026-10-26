#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "ai/AiService.h"
#include "ui/MainView.h"
#include "ui/Playlist.h"
#include "ui/AudioSettings.h"
#include "ui/ColourMenu.h"
#include "ui/PianoRoll.h"
#include "ui/Shortcuts.h"
#include "ui/TempoControl.h"
#include "ui/WindowLayouts.h"

using namespace spinus;
using namespace spinus::ui;

namespace
{
std::unique_ptr<commands::Command> make (const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
    INFO (error);
    REQUIRE (command != nullptr);
    return command;
}

bool invoke (MainView& view, int command) { return view.getCommandManager().invokeDirectly (command, false); }

/** A main view on a headless engine. */
struct Ui
{
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    MainView view { session };
    commands::CommandHistory& history = session.getHistory();
};
} // namespace

TEST_CASE ("Themes define every colour and load from JSON", "[ui]")
{
    const auto dark = Theme::dark(), light = Theme::light();
    for (auto& role : Theme::roles())
    {
        INFO (role);
        CHECK (dark.get (role) != light.get (role));
    }

    juce::StringArray errors;
    const auto custom = Theme::fromJson (
        juce::JSON::parse (
            R"({"name": "Mine", "colours": {"accent": "#ff00ff00", "panel": "red", "sparkle": "#ff000000"}})"),
        errors);
    CHECK (custom.getName() == "Mine");
    CHECK (custom.get ("accent") == juce::Colour (0xff00ff00));
    CHECK (custom.get ("panel") == dark.get ("panel")); // invalid value: falls back to dark
    CHECK (errors.size() == 2);
    CHECK (errors.joinIntoString ("\n").contains ("'panel' must be a colour"));
    CHECK (errors.joinIntoString ("\n").contains ("unknown colour role 'sparkle'"));

    errors.clear();
    const auto roundTrip = Theme::fromJson (light.toJson(), errors);
    CHECK (errors.isEmpty());
    for (auto& role : Theme::roles())
        CHECK (roundTrip.get (role) == light.get (role));
}

TEST_CASE ("FL shortcuts open and close the panels", "[ui]")
{
    Ui ui;
    auto& view = ui.view;
    auto& history = ui.history;
    juce::ignoreUnused (history);
    auto& workspace = view.getWorkspace();

    CHECK (workspace.getPanelIds() == juce::StringArray { "channelRack", "playlist", "pianoRoll", "mixer", "history",
                                                          "routing", "analyzer", "trackCreator", "automixer",
                                                          "launcher", "automation", "audioTracks" });

    auto keyFor = [&] (int command)
    { return view.getCommandManager().getKeyMappings()->getKeyPressesAssignedToCommand (command)[0]; };
    CHECK (keyFor (MainView::showPlaylist) == juce::KeyPress (juce::KeyPress::F5Key));
    CHECK (keyFor (MainView::showChannelRack) == juce::KeyPress (juce::KeyPress::F6Key));
    CHECK (keyFor (MainView::showPianoRoll) == juce::KeyPress (juce::KeyPress::F7Key));
    CHECK (keyFor (MainView::showMixer) == juce::KeyPress (juce::KeyPress::F9Key));
    CHECK (keyFor (MainView::toggleBrowser) == juce::KeyPress (juce::KeyPress::F8Key));
    CHECK (keyFor (MainView::showAudioTracks) ==
           juce::KeyPress (juce::KeyPress::F6Key, juce::ModifierKeys::shiftModifier, 0));
    CHECK (keyFor (MainView::undo) == juce::KeyPress ('z', juce::ModifierKeys::ctrlModifier, 0));
    CHECK (keyFor (MainView::showHistory) == juce::KeyPress ('h', juce::ModifierKeys::ctrlModifier, 0));

    CHECK (! workspace.isPanelVisible ("mixer"));
    REQUIRE (invoke (view, MainView::showMixer));
    CHECK (workspace.isPanelVisible ("mixer"));
    CHECK (workspace.getFrontPanelId() == "mixer");

    // Pressing the shortcut of the front panel closes it (FL behaviour); otherwise it comes to front.
    REQUIRE (invoke (view, MainView::showPlaylist));
    CHECK (workspace.getFrontPanelId() == "playlist");
    REQUIRE (invoke (view, MainView::showPlaylist));
    CHECK (! workspace.isPanelVisible ("playlist"));
    CHECK (workspace.isPanelVisible ("mixer"));

    CHECK (view.isBrowserVisible());
    REQUIRE (invoke (view, MainView::toggleBrowser));
    CHECK (! view.isBrowserVisible());
    REQUIRE (invoke (view, MainView::toggleAssistant));
    CHECK (view.isAssistantVisible());
}

TEST_CASE ("P B C E pick the mouse tool of the Piano Roll or Playlist in front", "[ui]")
{
    Ui ui;
    auto& view = ui.view;
    auto& workspace = view.getWorkspace();
    auto keyFor = [&] (int command)
    { return view.getCommandManager().getKeyMappings()->getKeyPressesAssignedToCommand (command)[0]; };
    CHECK (keyFor (MainView::toolDraw) == juce::KeyPress ('p'));
    CHECK (keyFor (MainView::toolPaint) == juce::KeyPress ('b'));
    CHECK (keyFor (MainView::toolSlice) == juce::KeyPress ('c'));
    CHECK (keyFor (MainView::toolSelect) == juce::KeyPress ('e'));

    auto* roll = dynamic_cast<PianoRoll*> (workspace.getPanel ("pianoRoll"));
    auto* playlist = dynamic_cast<Playlist*> (workspace.getPanel ("playlist"));
    REQUIRE (roll != nullptr);
    REQUIRE (playlist != nullptr);
    workspace.showPanel ("playlist");
    workspace.showPanel ("pianoRoll");
    REQUIRE (invoke (view, MainView::toolPaint));
    CHECK (roll->getTool() == PianoRoll::Tool::paint);
    CHECK (playlist->getTool() == Playlist::Tool::draw);
    workspace.showPanel ("playlist"); // to the front
    REQUIRE (invoke (view, MainView::toolSlice));
    CHECK (playlist->getTool() == Playlist::Tool::slice);
    CHECK (roll->getTool() == PianoRoll::Tool::paint);
    REQUIRE (invoke (view, MainView::toolSelect));
    CHECK (playlist->getTool() == Playlist::Tool::select);
    REQUIRE (invoke (view, MainView::toolDraw));
    CHECK (playlist->getTool() == Playlist::Tool::draw);
}

TEST_CASE ("Undo and redo from the UI go through the command history", "[ui]")
{
    Ui ui;
    auto& view = ui.view;
    auto& history = ui.history;
    juce::ignoreUnused (history);

    juce::ApplicationCommandInfo info (MainView::undo);
    view.getCommandInfo (MainView::undo, info);
    CHECK (((unsigned)info.flags & (unsigned)juce::ApplicationCommandInfo::isDisabled) != 0);

    REQUIRE (history.perform (make ("project.setTempo", R"({"bpm": 98})")).wasOk());
    view.getCommandInfo (MainView::undo, info);
    CHECK (((unsigned)info.flags & (unsigned)juce::ApplicationCommandInfo::isDisabled) == 0);
    CHECK (info.description == "Undo: Set tempo to 98.00 BPM");

    REQUIRE (invoke (view, MainView::undo));
    CHECK (juce::approximatelyEqual (history.getProject().settings.tempoBpm, 120.0));
    CHECK (view.getHintBar().getText() == "Undone: Set tempo to 98.00 BPM");

    REQUIRE (invoke (view, MainView::redo));
    CHECK (juce::approximatelyEqual (history.getProject().settings.tempoBpm, 98.0));
}

TEST_CASE ("Panels detach into their own window and dock back", "[ui]")
{
    Ui ui;
    auto& view = ui.view;
    auto& history = ui.history;
    juce::ignoreUnused (history);
    auto& workspace = view.getWorkspace();
    auto* panel = workspace.getPanel ("pianoRoll");
    REQUIRE (panel != nullptr);

    workspace.showPanel ("pianoRoll");
    workspace.detachPanel ("pianoRoll");
    CHECK (workspace.isPanelDetached ("pianoRoll"));
    CHECK (workspace.isPanelVisible ("pianoRoll"));
    CHECK (dynamic_cast<juce::DocumentWindow*> (panel->getTopLevelComponent()) != nullptr);
    CHECK (panel->isShowing());

    workspace.attachPanel ("pianoRoll");
    CHECK (! workspace.isPanelDetached ("pianoRoll"));
    CHECK (panel->getTopLevelComponent() == &view);
    CHECK (workspace.getFrontPanelId() == "pianoRoll");

    // Closing a detached panel docks it back and hides it.
    workspace.detachPanel ("pianoRoll");
    workspace.hidePanel ("pianoRoll");
    CHECK (! workspace.isPanelDetached ("pianoRoll"));
    CHECK (! workspace.isPanelVisible ("pianoRoll"));
}

TEST_CASE ("The hint bar shows the nearest help text", "[ui]")
{
    juce::Component outer, middle, inner;
    outer.addChildComponent (middle);
    middle.addChildComponent (inner);
    outer.setHelpText ("outer hint");
    CHECK (HintBar::hintFor (&inner) == "outer hint");
    middle.setHelpText ("middle hint");
    CHECK (HintBar::hintFor (&inner) == "middle hint");
    CHECK (HintBar::hintFor (nullptr).isEmpty());
}

TEST_CASE ("Transport commands and the tempo control", "[ui][session]")
{
    Ui ui;
    auto& session = ui.session;

    auto keyFor = [&] (int command)
    { return ui.view.getCommandManager().getKeyMappings()->getKeyPressesAssignedToCommand (command)[0]; };
    CHECK (keyFor (MainView::playStop) == juce::KeyPress (juce::KeyPress::spaceKey));
    CHECK (keyFor (MainView::togglePlayMode) == juce::KeyPress ('l'));
    CHECK (keyFor (MainView::showAudioSettings) == juce::KeyPress (juce::KeyPress::F10Key));

    REQUIRE (invoke (ui.view, MainView::playStop));
    CHECK (session.isPlaying());
    REQUIRE (invoke (ui.view, MainView::playStop));
    CHECK (! session.isPlaying());

    REQUIRE (invoke (ui.view, MainView::togglePlayMode));
    CHECK (session.getMode() == Session::Mode::song);
    REQUIRE (invoke (ui.view, MainView::toggleMetronome));
    CHECK (session.isMetronomeOn());

    // A tempo edit is one undoable command.
    TempoControl tempo;
    tempo.onCommit = [&] (double bpm)
    {
        juce::String error;
        ui.history.perform (commands::CommandRegistry::get().create (
            "project.setTempo", juce::JSON::parse ("{\"bpm\": " + juce::String (bpm) + "}"), error));
    };
    tempo.nudge (1.0);
    CHECK (juce::approximatelyEqual (ui.history.getProject().settings.tempoBpm, 121.0));
    CHECK (ui.history.getUndoDescriptions().size() == 1);
}

TEST_CASE ("R arms recording, which switches to pattern mode", "[ui][recording]")
{
    Ui ui;
    ui.session.setMode (Session::Mode::song);
    REQUIRE (invoke (ui.view, MainView::toggleRecord));
    CHECK (ui.session.isRecordArmed());
    CHECK (ui.session.getMode() == Session::Mode::pattern);

    juce::ApplicationCommandInfo info (0);
    ui.view.getCommandInfo (MainView::toggleRecord, info);
    CHECK (info.defaultKeypresses.contains (juce::KeyPress ('r', 0, 0)));
    CHECK (((unsigned)info.flags & (unsigned)juce::ApplicationCommandInfo::isTicked) != 0);

    REQUIRE (invoke (ui.view, MainView::toggleRecord));
    CHECK (! ui.session.isRecordArmed());

    CHECK (ui.session.getRecorder().isQuantizing());
    REQUIRE (invoke (ui.view, MainView::toggleRecordQuantize));
    CHECK (! ui.session.getRecorder().isQuantizing());
}

TEST_CASE ("Record idea and Generate show only while AI is on", "[ui][ai]")
{
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile (juce::String ("spinus-ai-buttons") + "-" + juce::Uuid().toString());
    {
        ai::AiService service (folder); // outlives the main view, as in the app
        Ui ui;
        auto* roll = dynamic_cast<PianoRoll*> (ui.view.getWorkspace().getPanel ("pianoRoll"));
        REQUIRE (roll != nullptr);
        ui.view.setAiService (&service);
        CHECK (! roll->hasAiButtons()); // AI is off until turned on
        CHECK (ui.view.getTopBarHelpText ("Assistant").contains ("AI is off"));
        service.setEnabled (true);
        CHECK (roll->hasAiButtons());
        CHECK (! ui.view.getTopBarHelpText ("Assistant").contains ("AI is off"));
        CHECK (ui.view.getTopBarHelpText (juce::CharPointer_UTF8 ("\xe2\x99\xa9")).startsWith ("Metronome"));
        service.setEnabled (false);
        CHECK (! roll->hasAiButtons());
    }
    folder.deleteRecursively();
}

TEST_CASE ("The assistant is as wide as dragged, within limits", "[ui]")
{
    Ui ui;
    ui.view.setSize (1600, 900);
    CHECK (ui.view.getAssistantWidth() == 300);
    REQUIRE (invoke (ui.view, MainView::toggleAssistant));
    ui.view.setAssistantWidth (500);
    CHECK (ui.view.getAssistantWidth() == 500);
    ui.view.setAssistantWidth (100);
    CHECK (ui.view.getAssistantWidth() == 220);
    ui.view.setAssistantWidth (5000);
    CHECK (ui.view.getAssistantWidth() == 800); // half the window
}

TEST_CASE ("Every top bar button fits a 1280-pixel window", "[ui]")
{
    Ui ui;
    ui.view.setSize (1280, 800);
    for (const auto* text : { "Playlist", "Rack", "Piano Roll", "Mixer", "Browser", "Assistant" })
    {
        INFO (text);
        CHECK (ui.view.getTopBarHelpText (text).isNotEmpty());
    }
    CHECK (! ui.view.getTopBarButtonsOverlap());
}

TEST_CASE ("The colour menu ticks the current colour", "[ui]")
{
    auto ticked = [] (const juce::PopupMenu& menu)
    {
        juce::StringArray names;
        for (juce::PopupMenu::MenuItemIterator i (menu, true); i.next();)
            if (i.getItem().isTicked)
                names.add (i.getItem().text);
        return names;
    };
    juce::PopupMenu menu;
    ColourMenu::addTo (menu, 0xff5c8dd6, [] (juce::uint32) {});
    CHECK (ticked (menu).joinIntoString (",").contains ("Blue"));
}

TEST_CASE ("The pattern list renames, colours and deletes the current pattern", "[ui]")
{
    Ui ui;
    auto add = make ("pattern.add", R"({"length": 3840})");
    auto* raw = add.get();
    REQUIRE (ui.history.perform (std::move (add)).wasOk());
    const model::PatternId pattern { (uint32_t)(int)raw->getResult()["pattern"] };
    ui.session.setCurrentPattern (pattern);
    REQUIRE (ui.history.getProject().findPattern (pattern) != nullptr);
    REQUIRE (ui.history
                 .perform (make ("pattern.set",
                                 "{\"pattern\": " + juce::String (pattern.value) + ", \"colour\": \"#ffd65c5c\"}"))
                 .wasOk());

    // The actions below the toolbar's pattern list.
    juce::PopupMenu menu;
    ui.view.addPatternActions (menu);
    juce::StringArray texts, ticked;
    for (juce::PopupMenu::MenuItemIterator i (menu, true); i.next();)
    {
        texts.add (i.getItem().text);
        if (i.getItem().isTicked)
            ticked.add (i.getItem().text);
    }
    const auto name = ui.history.getProject().findPattern (pattern)->name;
    INFO (texts.joinIntoString (" | "));
    CHECK (texts.contains ("New pattern"));
    CHECK (texts.contains ("Clone " + name));
    CHECK (texts.contains ("Rename " + name + "..."));
    CHECK (texts.contains ("Delete " + name));
    CHECK (ticked.joinIntoString (",").contains ("Red"));
}

TEST_CASE ("Audio settings without a sound card say so and change nothing", "[ui]")
{
    juce::AudioDeviceManager devices; // not opened
    AudioSettings settings (devices);
    CHECK (settings.getStatusText() == "No audio device open");
    CHECK (settings.getSpeakerChoices().isEmpty());
    CHECK (settings.getInputChoices().isEmpty());
    CHECK (! settings.chooseSpeakers (-1));
}

TEST_CASE ("Keyboard shortcuts can be changed, and are kept for the next start", "[ui]")
{
    const auto file = juce::File::createTempFile (".xml");
    {
        Ui ui;
        ui.view.setShortcutsFile (file);
        auto* keys = ui.view.getCommandManager().getKeyMappings();
        keys->removeKeyPress (MainView::showPlaylist, 0); // F5
        keys->addKeyPress (MainView::showPlaylist, juce::KeyPress (juce::KeyPress::F12Key));
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100); // saved on change, asynchronously
    }
    REQUIRE (file.existsAsFile());
    CHECK (! file.loadFileAsString().contains ("showMixer")); // only what differs from Spinus's keys

    Ui again;
    again.view.setShortcutsFile (file);
    const auto keys =
        again.view.getCommandManager().getKeyMappings()->getKeyPressesAssignedToCommand (MainView::showPlaylist);
    CHECK (keys.contains (juce::KeyPress (juce::KeyPress::F12Key)));
    CHECK (! keys.contains (juce::KeyPress (juce::KeyPress::F5Key)));
    // The others keep Spinus's keys.
    CHECK (again.view.getCommandManager().getKeyMappings()->getKeyPressesAssignedToCommand (MainView::showMixer)[0] ==
           juce::KeyPress (juce::KeyPress::F9Key));
    file.deleteFile();
}

TEST_CASE ("Window layouts: Spinus's three, saved ones, and the last one at the next start", "[ui][layouts]")
{
    const auto file = juce::File::createTempFile (".xml");
    {
        Ui ui;
        ui.view.setSize (1280, 800);
        ui.view.setLayoutsFile (file);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        auto& workspace = ui.view.getWorkspace();

        // Mix: the Mixer in front of the Playlist, everything else closed, no Browser.
        REQUIRE (invoke (ui.view, MainView::layout3));
        CHECK (workspace.isPanelVisible ("mixer"));
        CHECK (workspace.isPanelVisible ("playlist"));
        CHECK (! workspace.isPanelVisible ("channelRack"));
        CHECK (! workspace.isPanelVisible ("pianoRoll"));
        CHECK (workspace.getFrontPanelId() == "mixer");
        CHECK (! ui.view.isBrowserVisible());
        CHECK (workspace.getPanel ("mixer")->getParentComponent()->getBounds().getY() ==
               juce::roundToInt (0.4f * (float)workspace.getHeight()));

        // Compose: Channel Rack, Piano Roll and Playlist.
        REQUIRE (ui.view.useLayout ("Compose"));
        CHECK (workspace.isPanelVisible ("pianoRoll"));
        CHECK (workspace.isPanelVisible ("channelRack"));
        CHECK (! workspace.isPanelVisible ("mixer"));
        CHECK (ui.view.isBrowserVisible());

        // A layout of one's own: the windows as they are, under a name.
        workspace.hidePanel ("playlist");
        workspace.showPanel ("history");
        auto& layouts = ui.view.getWindowLayouts();
        layouts.save ("Sketch", ui.view.getCurrentLayout());
        CHECK (layouts.getNames() == juce::StringArray { "Compose", "Arrange", "Mix", "Sketch" });
        REQUIRE (ui.view.useLayout ("Mix"));
        REQUIRE (ui.view.useLayout ("Sketch"));
        CHECK (workspace.isPanelVisible ("history"));
        CHECK (! workspace.isPanelVisible ("playlist"));
        CHECK (workspace.getFrontPanelId() == "history");
        CHECK (! ui.view.useLayout ("Nothing"));

        // Changing a built-in one, and back to Spinus's.
        layouts.save ("Mix", ui.view.getCurrentLayout());
        CHECK (! layouts.get ("Mix").getChildWithProperty ("id", "mixer")["visible"]);
        layouts.remove ("Mix");
        CHECK ((bool)layouts.get ("Mix").getChildWithProperty ("id", "mixer")["visible"]);
        layouts.remove ("Sketch");
        CHECK (layouts.getNames().size() == 3);

        ui.view.useLayout ("Arrange"); // what Spinus has when it closes
    }

    Ui again;
    again.view.setSize (1280, 800);
    again.view.setLayoutsFile (file);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    auto& workspace = again.view.getWorkspace();
    CHECK (workspace.isPanelVisible ("playlist"));
    CHECK (workspace.isPanelVisible ("channelRack"));
    CHECK (! workspace.isPanelVisible ("pianoRoll"));
    CHECK (workspace.getFrontPanelId() == "playlist");
    file.deleteFile();
}

// Restored maximised, the main window opens small and gets the screen's size a moment later: the last layout
// is put back at that size, not squeezed into the small window.
TEST_CASE ("The last layout fits the window's size once it is restored maximised", "[ui][layouts]")
{
    const auto file = juce::File::createTempFile (".xml");
    {
        Ui ui;
        ui.view.setSize (1600, 1000);
        ui.view.setLayoutsFile (file);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        REQUIRE (invoke (ui.view, MainView::layout3)); // Mix: the Mixer over the lower 60 %
        ui.view.getWindowLayouts().setLast (ui.view.getCurrentLayout());
    }

    Ui again;
    again.view.setSize (800, 600); // the window before the desktop maximises it
    again.view.setLayoutsFile (file);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    again.view.setSize (1600, 1000);
    auto& workspace = again.view.getWorkspace();
    const auto mixer = workspace.getPanel ("mixer")->getParentComponent()->getBounds();
    CHECK (mixer.getY() == juce::roundToInt (0.4f * (float)workspace.getHeight()));
    CHECK (mixer.getBottom() >= workspace.getHeight() - 1);
    CHECK (mixer.getWidth() >= workspace.getWidth() - 1);
    file.deleteFile();
}

TEST_CASE ("Saved layouts get Ctrl+Alt+4 to 9; the windows are kept while Spinus runs", "[ui][layouts]")
{
    const auto file = juce::File::createTempFile (".xml");
    Ui ui;
    ui.view.setSize (1280, 800);
    ui.view.setLayoutsFile (file);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    auto& workspace = ui.view.getWorkspace();
    auto& commands = ui.view.getCommandManager();
    const auto ctrlAlt = juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier;

    // No saved layout yet: Ctrl+Alt+4 is there, but does nothing.
    CHECK (commands.getKeyMappings()->findCommandForKeyPress (juce::KeyPress ('4', ctrlAlt, 0)) == MainView::layout4);
    juce::ApplicationCommandInfo info (MainView::layout4);
    ui.view.getCommandInfo (MainView::layout4, info);
    CHECK (((unsigned)info.flags & (unsigned)juce::ApplicationCommandInfo::isDisabled) != 0);

    // The first saved layout is Ctrl+Alt+4, the second Ctrl+Alt+5.
    workspace.hidePanel ("playlist");
    workspace.showPanel ("history");
    ui.view.getWindowLayouts().save ("Sketch", ui.view.getCurrentLayout());
    workspace.showPanel ("mixer");
    ui.view.getWindowLayouts().save ("Mixing desk", ui.view.getCurrentLayout());
    ui.view.getCommandInfo (MainView::layout4, info);
    CHECK (info.shortName == "Sketch");
    CHECK (((unsigned)info.flags & (unsigned)juce::ApplicationCommandInfo::isDisabled) == 0);
    REQUIRE (ui.view.useLayout ("Mix"));
    REQUIRE (invoke (ui.view, MainView::layout4));
    CHECK (workspace.isPanelVisible ("history"));
    CHECK (! workspace.isPanelVisible ("mixer"));
    REQUIRE (invoke (ui.view, MainView::layout5));
    CHECK (workspace.isPanelVisible ("mixer"));
    CHECK (MainView::layoutIndexOf (MainView::layout9) == 8);
    CHECK (MainView::layoutCommand (3) == MainView::layout4);

    // The windows as they are now reach the file within a few seconds, without Spinus closing.
    REQUIRE (ui.view.useLayout ("Mix"));
    juce::MessageManager::getInstance()->runDispatchLoopUntil (3500);
    WindowLayouts reread (file);
    CHECK (reread.getLast().getChildWithProperty ("id", "mixer")["visible"]);
    CHECK (! reread.getLast().getChildWithProperty ("id", "history")["visible"]);
    file.deleteFile();
}

TEST_CASE ("Two Spinus windows open at once keep each other's saved layouts", "[ui][layouts]")
{
    const auto file = juce::File::createTempFile (".xml");
    WindowLayouts first (file), second (file);
    first.save ("Mine", WindowLayouts::builtIn ("Mix"));
    second.setLast (WindowLayouts::builtIn ("Arrange")); // the second one closes, knowing nothing of "Mine"
    second.save ("Also mine", WindowLayouts::builtIn ("Compose"));
    WindowLayouts reread (file);
    CHECK (reread.getSavedNames() == juce::StringArray { "Mine", "Also mine" });
    CHECK (reread.getLast().isValid());
    file.deleteFile();
}

TEST_CASE ("Shortcuts reach the main window from other windows and from text fields", "[ui][shortcuts]")
{
    Ui ui;
    ui.view.setSize (1280, 800);
    auto& workspace = ui.view.getWorkspace();
    workspace.hidePanel ("mixer");

    // A window of its own (a plugin's, a detached panel): F9 opens the Mixer.
    juce::Component window;
    CHECK (shortcuts::getWindowKeys().keyPressed (juce::KeyPress (juce::KeyPress::F9Key), &window));
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    CHECK (workspace.isPanelVisible ("mixer"));

    // In a text field, Ctrl+Alt+3 is the Mix layout, not a typed "3"; Ctrl+A stays with the field.
    juce::TextEditor field;
    const auto ctrlAlt = juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier;
    REQUIRE (ui.view.useLayout ("Compose"));
    CHECK (shortcuts::getTextFieldKeys().keyPressed (juce::KeyPress ('3', ctrlAlt, '3'), &field));
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    CHECK (workspace.isPanelVisible ("mixer"));
    CHECK (! workspace.isPanelVisible ("pianoRoll"));
    CHECK_FALSE (
        shortcuts::getTextFieldKeys().keyPressed (juce::KeyPress ('a', juce::ModifierKeys::ctrlModifier, 'a'), &field));
    CHECK_FALSE (shortcuts::getTextFieldKeys().keyPressed (juce::KeyPress ('3', 0, '3'), &field)); // typing
}
