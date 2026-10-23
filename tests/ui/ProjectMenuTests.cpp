#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "project/ProjectFiles.h"
#include "ui/About.h"
#include "ui/Browser.h"
#include "ui/MainView.h"
#include "ui/Manual.h"

#include <SpinusVersion.h>

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

/** A main view with a project document in a temporary folder. Questions and file choices are answered
    by the test: the next answers are queued, and each question is recorded. */
struct Ui
{
    Ui()
    {
        view.setMessageBoxHandler (
            [this] (const juce::MessageBoxOptions& options, const std::function<void (int)>& done)
            {
                questions.add (options.getTitle());
                REQUIRE (! answers.empty());
                const auto answer = answers.front();
                answers.erase (answers.begin());
                REQUIRE (answer < options.getNumButtons());
                if (done)
                    done (answer);
            });
        view.setFileChooserHandler (
            [this] (const juce::String& title, const juce::File&, bool, const std::function<void (juce::File)>& done)
            {
                questions.add (title);
                if (chosenFile != juce::File())
                    done (chosenFile);
            });
        view.onQuit = [this] { ++quits; };
    }

    ~Ui() { root.deleteRecursively(); }

    void edit() { REQUIRE (history.perform (make ("pattern.add", R"({"length": 3840})")).wasOk()); }

    juce::File root { juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-menu-test") + "-" + juce::Uuid().toString()) };
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    ProjectDocument document { session, root.getChildFile ("settings") };
    MainView view { session, &document };
    commands::CommandHistory& history = session.getHistory();

    std::vector<int> answers; // button index: 0 is the first button
    juce::StringArray questions;
    juce::File chosenFile;
    int quits = 0;
};

enum SaveQuestion
{
    save = 0,
    dontSave = 1,
    cancel = 2
};
} // namespace

TEST_CASE ("Quitting asks to save unsaved changes", "[ui][document]")
{
    Ui ui;

    // Nothing changed: no question.
    ui.view.quitApp();
    CHECK (ui.questions.isEmpty());
    CHECK (ui.quits == 1);

    ui.edit();
    CHECK (ui.view.getWindowTitle() == "Untitled* - Spinus DAW");

    ui.answers = { cancel };
    REQUIRE (invoke (ui.view, MainView::quit));
    CHECK (ui.questions == juce::StringArray { "Save changes?" });
    CHECK (ui.quits == 1);

    ui.answers = { dontSave };
    ui.view.quitApp();
    CHECK (ui.quits == 2);

    // Save on an untitled project asks where; choosing nothing cancels the quit.
    ui.questions.clear();
    ui.answers = { save };
    ui.view.quitApp();
    CHECK (ui.questions == juce::StringArray { "Save changes?", "Save project as" });
    CHECK (ui.quits == 2);

    ui.answers = { save };
    ui.chosenFile = ui.root.getChildFile ("My Song");
    ui.view.quitApp();
    CHECK (ui.quits == 3);
    CHECK (ui.document.getFolder() == ui.root.getChildFile ("My Song"));
    CHECK (ui.view.getWindowTitle() == "My Song - Spinus DAW");
}

TEST_CASE ("File menu: save, new and open", "[ui][document]")
{
    Ui ui;
    ui.edit();
    ui.chosenFile = ui.root.getChildFile ("Beat");
    REQUIRE (invoke (ui.view, MainView::saveProject)); // untitled: Save asks where
    CHECK (ui.document.getName() == "Beat");
    CHECK (! ui.document.hasUnsavedChanges());
    const auto saved = ui.session.getProject();

    // Ctrl+S saves in place without asking.
    ui.questions.clear();
    ui.edit();
    REQUIRE (invoke (ui.view, MainView::saveProject));
    CHECK (ui.questions.isEmpty());
    CHECK (! ui.document.hasUnsavedEdits());
    const auto savedTwice = ui.session.getProject();

    // New asks about unsaved changes first.
    ui.edit();
    ui.answers = { dontSave };
    REQUIRE (invoke (ui.view, MainView::newProject));
    CHECK (ui.questions == juce::StringArray { "Save changes?" });
    CHECK (ui.document.isUntitled());
    CHECK (ui.session.getProject().patterns.empty());

    // Save As into an existing project folder asks before replacing it.
    ui.edit();
    ui.questions.clear();
    ui.answers = { 1 }; // Cancel
    REQUIRE (invoke (ui.view, MainView::saveProjectAs));
    CHECK (ui.questions == juce::StringArray { "Save project as", "Replace project?" });
    CHECK (ui.document.isUntitled());

    // Open (from the recent list, as the menu does) shows the saved project.
    ui.answers = { dontSave };
    ui.chosenFile = ui.root.getChildFile ("Beat");
    REQUIRE (invoke (ui.view, MainView::openProject));
    CHECK (ui.session.getProject() == savedTwice);
    CHECK (ui.session.getProject() != saved);
    CHECK (ui.view.getWindowTitle() == "Beat - Spinus DAW");
}

TEST_CASE ("A problem opening a project is shown, and the open project stays", "[ui][document]")
{
    Ui ui;
    ui.edit();
    const auto before = ui.session.getProject();

    ui.answers = { 0 }; // OK
    CHECK (! ui.view.openProjectFile (ui.root.getChildFile ("missing")));
    CHECK (ui.questions == juce::StringArray { "Could not open the project" });
    CHECK (ui.session.getProject() == before);
}

TEST_CASE ("Opening a project whose last session crashed offers the autosave", "[ui][document]")
{
    Ui ui;
    const auto folder = ui.root.getChildFile ("Crashed");
    ui.edit();
    REQUIRE (ui.view.saveProjectTo (folder));
    ui.edit();
    const auto unsaved = ui.session.getProject();
    REQUIRE (ui.document.autosave().wasOk());
    ui.document.newProject();
    folder.getChildFile (".session.lock").replaceWithText ("pid 999999999\n");

    ui.answers = { 0 }; // Recover
    REQUIRE (ui.view.openProjectFile (folder));
    CHECK (ui.questions == juce::StringArray { "Recover your work?" });
    CHECK (ui.session.getProject() == unsaved);
    CHECK (ui.view.getWindowTitle() == "Crashed* - Spinus DAW");

    // Discard opens the saved version instead.
    ui.document.newProject();
    folder.getChildFile (".session.lock").replaceWithText ("pid 999999999\n");
    ui.answers = { 1 };
    REQUIRE (ui.view.openProjectFile (folder));
    CHECK (ui.session.getProject() != unsaved);
    CHECK (ui.session.getProject().patterns.size() == 1);
    CHECK (ui.view.getWindowTitle() == "Crashed - Spinus DAW");
}

TEST_CASE ("Without a project document the project commands are disabled", "[ui][document]")
{
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    MainView view { session };

    juce::ApplicationCommandInfo info (0);
    for (auto id : { MainView::newProject, MainView::openProject, MainView::saveProject, MainView::saveProjectAs })
    {
        view.getCommandInfo (id, info);
        CHECK (((unsigned)info.flags & (unsigned)juce::ApplicationCommandInfo::isDisabled) != 0);
    }
    CHECK (view.getWindowTitle() == "Spinus DAW");
}

TEST_CASE ("After a plugin crash: offered to run it separately; a failed plugin can be reloaded", "[ui][sandbox]")
{
    Ui ui;
    plugins::SandboxList list (ui.root.getChildFile ("run-separately.json"));
    ui.session.setSandboxList (&list);

    // "Not now": nothing changes, and what comes next still runs.
    bool next = false;
    ui.answers = { 1 };
    ui.view.offerToRunSeparately ("CLAP:/plugins/Boom.clap", "Boom", [&] { next = true; });
    CHECK (ui.questions.strings.getLast() == "Run Boom in a separate process?");
    CHECK (next);
    CHECK (! list.contains ("CLAP:/plugins/Boom.clap"));

    // "Run separately": remembered.
    ui.answers = { 0 };
    ui.view.offerToRunSeparately ("CLAP:/plugins/Boom.clap", "Boom", {});
    CHECK (list.contains ("CLAP:/plugins/Boom.clap"));

    // A failure notice asks; "Leave it off" does nothing.
    ui.answers = { 1 };
    ui.view.showPluginFailure (model::DeviceId { 99 });
    CHECK (ui.questions.strings.getLast() == "A plugin stopped working");
    ui.session.setSandboxList (nullptr);
}

namespace
{
/** A demo folder with songs that load (empty projects), as the packages install them, and a README beside. */
juce::File makeDemoFolder (const juce::File& at)
{
    for (auto name : { "Night Drive", "Siskin", "Acid Morning" })
        REQUIRE (project::saveProject (at.getChildFile (name), {}).wasOk());
    at.getChildFile ("README.md").replaceWithText ("# Demo songs");
    return at;
}

juce::StringArray rowTexts (Browser& browser, Browser::RowKind kind, const juce::String& key = {})
{
    juce::StringArray texts;
    for (auto& r : browser.getRows())
        if (r.kind == kind && (key.isEmpty() || r.key == key))
            texts.add (r.text);
    return texts;
}

int rowOf (Browser& browser, const juce::String& text)
{
    const auto& rows = browser.getRows();
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].text == text)
            return (int)i;
    FAIL ("no row " << text);
    return -1;
}
} // namespace

TEST_CASE ("The Browser lists the demo songs; opening one opens a copy in the Music folder", "[ui][document][demo]")
{
    Ui ui;
    const auto demos = makeDemoFolder (ui.root.getChildFile ("share/spinus/demo"));
    const auto music = ui.root.getChildFile ("Music");
    ui.view.setDemoFolders (demos, music);
    auto& browser = ui.view.getBrowser();

    // Every song in the demo folder, Siskin first.
    CHECK (rowTexts (browser, Browser::RowKind::project, "demo") ==
           juce::StringArray { "Siskin (demo)", "Acid Morning (demo)", "Night Drive (demo)" });

    // Unsaved changes are asked about first; then the copy opens.
    ui.edit();
    ui.answers = { dontSave };
    browser.useRow (rowOf (browser, "Night Drive (demo)"));
    CHECK (ui.questions == juce::StringArray { "Save changes?" });
    CHECK (ui.document.getFolder() == music.getChildFile ("Night Drive"));
    CHECK (ui.view.getWindowTitle() == "Night Drive - Spinus DAW");
    CHECK (ui.view.getHintBar().getText().contains ("your own copy"));
    ui.document.dispatchPendingMessages(); // the Browser lists what was opened

    // Again: the same copy, said so. The first song by itself.
    ui.document.newProject();
    REQUIRE (ui.view.openDemo (demos.getChildFile ("Night Drive")));
    CHECK (ui.document.getFolder() == music.getChildFile ("Night Drive"));
    CHECK (ui.view.getHintBar().getText().contains ("your copy"));
    REQUIRE (ui.view.openTheDemoSong());
    CHECK (ui.document.getFolder() == music.getChildFile ("Siskin"));

    // The copies opened are listed as projects of the user's own.
    ui.document.dispatchPendingMessages();
    CHECK (ui.view.getBrowser().getProjects().getOpened().size() == 2);

    // No demo songs: none listed, and the problem is shown.
    ui.document.newProject();
    ui.view.setDemoFolders (ui.root.getChildFile ("missing"), music);
    CHECK (rowTexts (browser, Browser::RowKind::project, "demo").isEmpty());
    ui.answers = { 0 };
    CHECK (! ui.view.openTheDemoSong());
    CHECK (ui.questions[ui.questions.size() - 1] == "Could not open the demo song");
}

TEST_CASE ("Projects opened from the file system are listed in the Browser and open from there",
           "[ui][document][projects]")
{
    Ui ui;
    ui.view.setDemoFolders (ui.root.getChildFile ("no demos"), ui.root.getChildFile ("Music"));
    auto& browser = ui.view.getBrowser();
    browser.setProjectsFile (ui.root.getChildFile ("settings/projects.json"));
    browser.getProjects().waitForScan();

    // Saved (Save As) and opened (File › Open, Open recent, the command line): listed, newest first.
    ui.edit();
    REQUIRE (ui.view.saveProjectTo (ui.root.getChildFile ("Songs/Beat")));
    ui.document.dispatchPendingMessages();
    ui.document.newProject();
    ui.edit();
    REQUIRE (ui.view.saveProjectTo (ui.root.getChildFile ("Songs/Ballad")));
    ui.document.dispatchPendingMessages();
    REQUIRE (ui.view.openProjectFile (ui.root.getChildFile ("Songs/Beat/Beat.spn")));
    ui.document.dispatchPendingMessages();
    CHECK (rowTexts (browser, Browser::RowKind::project) == juce::StringArray { "Beat", "Ballad" });
    CHECK (rowTexts (browser, Browser::RowKind::group).contains ("Opened  (2)"));

    // Double-click opens it, after asking about unsaved changes.
    ui.edit();
    ui.answers = { dontSave };
    browser.useRow (rowOf (browser, "Ballad"));
    CHECK (ui.questions == juce::StringArray { "Save changes?" });
    CHECK (ui.document.getName() == "Ballad");

    // Searching finds projects by name.
    browser.setSearchText ("bal");
    CHECK (rowTexts (browser, Browser::RowKind::project) == juce::StringArray { "Ballad" });
    browser.setSearchText ({});

    // One that is gone shows as missing and does not open; it can be removed from the list.
    ui.document.newProject();
    ui.root.getChildFile ("Songs/Beat").deleteRecursively();
    browser.getProjects().rescan();
    browser.getProjects().waitForScan();
    const auto beat = rowOf (browser, "Beat");
    CHECK (! browser.getRows()[(size_t)beat].available);
    CHECK (browser.getRows()[(size_t)beat].note == "missing");
    ui.questions.clear();
    browser.useRow (beat);
    CHECK (ui.questions.isEmpty());
    CHECK (ui.document.isUntitled());
    CHECK (browser.getStatusText().contains ("not there any more"));
    browser.getProjects().remove (ui.root.getChildFile ("Songs/Beat"));
    CHECK (rowTexts (browser, Browser::RowKind::project) == juce::StringArray { "Ballad" });

    // A folder of projects: its projects, with the subfolder each is in.
    const auto library = ui.root.getChildFile ("Library");
    REQUIRE (project::saveProject (library.getChildFile ("2024/Summer"), {}).wasOk());
    REQUIRE (project::saveProject (library.getChildFile ("Winter"), {}).wasOk());
    REQUIRE (browser.addProjectFolder (library));
    browser.getProjects().waitForScan();
    CHECK (rowTexts (browser, Browser::RowKind::group).contains ("Library  (2)"));
    CHECK (rowTexts (browser, Browser::RowKind::project) == juce::StringArray { "Ballad", "Summer", "Winter" });
    CHECK (browser.getRows()[(size_t)rowOf (browser, "Summer")].note == "2024");
    CHECK (rowTexts (browser, Browser::RowKind::action).contains ("+ Add a folder of projects..."));
    browser.toggleOpen ("projects.folder:" + library.getFullPathName());
    CHECK (! rowTexts (browser, Browser::RowKind::project).contains ("Summer"));
}

TEST_CASE ("Help: the Manual (F1) and About Spinus", "[ui][help]")
{
    Ui ui;
    const auto help = ui.view.getMenuBarNames().indexOf ("Help");
    REQUIRE (help >= 0);
    juce::Array<int> items;
    const auto menu = ui.view.getMenuForIndex (help, "Help");
    for (juce::PopupMenu::MenuItemIterator i (menu); i.next();)
        if (i.getItem().itemID != 0)
            items.add (i.getItem().itemID);
    CHECK (items == juce::Array<int> { MainView::showManual, MainView::showAbout });

    // F1 opens the manual (no other command has it).
    auto* mappings = ui.view.getCommandManager().getKeyMappings();
    CHECK (mappings->getKeyPressesAssignedToCommand (MainView::showManual)
               .contains (juce::KeyPress (juce::KeyPress::F1Key)));
    CHECK (mappings->findCommandForKeyPress (juce::KeyPress (juce::KeyPress::F1Key)) == MainView::showManual);

    // Both open, without AI.
    ui.view.setManualFolder (juce::File (SPINUS_TEST_FILES_DIR).getSiblingFile ("docs/user"));
    REQUIRE (invoke (ui.view, MainView::showManual));
    REQUIRE (ManualWindow::getOpen() != nullptr);
    CHECK (ManualWindow::getOpen()->getManual().getPages().size() > 30);
    REQUIRE (invoke (ui.view, MainView::showAbout));
    REQUIRE (AboutSpinus::getOpen() != nullptr);
    CHECK (AboutSpinus::getOpen()->getText().contains (versionString));
    ManualWindow::closeWindow();
    AboutSpinus::closeWindow();
    CHECK (ManualWindow::getOpen() == nullptr);
}
