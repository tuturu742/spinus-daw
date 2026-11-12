#include <catch2/catch_test_macros.hpp>

#include "ai/AiService.h"
#include "commands/CommandRegistry.h"
#include "engine/EngineHost.h"
#include "plugins/PluginDownloads.h"
#include "plugins/windows/WindowsPlugins.h"
#include "ui/About.h"
#include "ui/AiSettingsDialog.h"
#include "ui/ExportDialog.h"
#include "ui/MainView.h"
#include "ui/Manual.h"
#include "ui/PluginDownloadDialog.h"
#include "ui/WindowsPluginsDialog.h"

#include <set>

using namespace spinus;
using namespace spinus::ui;

// Every item of the main window's menus (File, Edit, View, Options, Help) either does something, or is greyed out
// in a state where it can't: without a project document, AI, the free plugins or Windows plugins, with nothing to
// undo or redo, nothing moved to automate, no saved layouts or recent projects.

namespace
{
struct Item
{
    juce::String path; // "File > Open recent > Clear recent projects"
    int id = 0;        // the command, or the item's own number (recent projects)
    bool isCommand = false, enabled = false;
    std::function<void()> action;
};

void collect (const juce::PopupMenu& menu, const juce::String& path, std::vector<Item>& items)
{
    for (juce::PopupMenu::MenuItemIterator i (menu); i.next();)
    {
        const auto& item = i.getItem();
        if (item.isSeparator || item.isSectionHeader)
            continue;
        const auto here = path + " > " + item.text;
        if (item.subMenu != nullptr)
        {
            // A sub-menu with nothing in it is greyed out.
            if (item.isEnabled)
                CHECK (item.subMenu->getNumItems() > 0);
            collect (*item.subMenu, here, items);
            continue;
        }
        items.push_back ({ here, item.itemID, item.commandManager != nullptr, item.isEnabled, item.action });
    }
}

std::vector<Item> allItems (MainView& view)
{
    std::vector<Item> items;
    const auto names = view.getMenuBarNames();
    for (int m = 0; m < names.size(); ++m)
        collect (view.getMenuForIndex (m, names[m]), names[m], items);
    return items;
}

std::set<juce::String> disabledItems (MainView& view)
{
    std::set<juce::String> paths;
    for (auto& item : allItems (view))
        if (! item.enabled)
            paths.insert (item.path);
    return paths;
}

std::unique_ptr<commands::Command> make (const juce::String& name, const juce::String& params)
{
    juce::String error;
    auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
    INFO (error);
    REQUIRE (command != nullptr);
    return command;
}

/** What a command might change, to tell that it did something. */
juce::String stateOf (MainView& view, Session& session, ProjectDocument* document, int questions, int quits)
{
    juce::String s;
    juce::Array<juce::CommandID> ids;
    view.getAllCommands (ids);
    for (auto id : ids)
    {
        juce::ApplicationCommandInfo info (id);
        view.getCommandInfo (id, info);
        s << (((unsigned)info.flags & (unsigned)juce::ApplicationCommandInfo::isTicked) != 0 ? "1" : "0");
    }
    s << "|" << view.getHintBar().getText() << "|" << juce::TopLevelWindow::getNumTopLevelWindows() << "|"
      << juce::ModalComponentManager::getInstance()->getNumModalComponents() << "|"
      << (document != nullptr ? document->getRecentProjects().size() : 0) << "|"
      << session.getHistory().getUndoDescriptions().size() << "|" << session.getHistory().getRedoDescriptions().size()
      << "|" << questions << "|" << quits << "|" << view.getWindowTitle() << "|" << (int)session.getMode() << "|"
      << view.getWindowLayouts().getNames().joinIntoString (",") << "|"
      << view.getCurrentLayout().toXmlString().hashCode64();
    return s;
}

juce::Array<juce::Component*> windowsNow()
{
    juce::Array<juce::Component*> list;
    for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
        list.add (juce::TopLevelWindow::getTopLevelWindow (i));
    return list;
}

/** Closes the windows a command opened. */
void closeWindows (const juce::Array<juce::Component*>& before)
{
    ExportDialog::closeWindow();
    ManualWindow::closeWindow();
    AboutSpinus::closeWindow();
    AiSettingsDialog::closeWindow();
    WindowsPluginsDialog::closeWindow();
    PluginDownloadDialog::closeWindow();
    for (auto* window : windowsNow())
        if (! before.contains (window))
            delete window; // launchAsync's dialogs and alert windows: they own their content
    juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
}

struct Fixture
{
    Fixture (bool withDocument, bool withServices)
    {
        root.createDirectory();
        if (withDocument)
            document = std::make_unique<ProjectDocument> (session, root.getChildFile ("settings"));
        view = std::make_unique<MainView> (session, document.get());
        if (withServices)
        {
            ai = std::make_unique<ai::AiService> (root.getChildFile ("ai"));
            downloads = std::make_unique<plugins::PluginDownloads> (root.getChildFile ("plugins"));
            windows = std::make_unique<plugins::WindowsPlugins> (root.getChildFile ("windows"));
            view->setAiService (ai.get());
            view->setPluginDownloads (downloads.get(), {});
            view->setWindowsPlugins (windows.get(), {});
            view->getCommandManager().commandStatusChanged();
        }
        view->setSize (1280, 800);
        view->setMessageBoxHandler (
            [this] (const juce::MessageBoxOptions& options, const std::function<void (int)>& done)
            {
                ++questions;
                titles.add (options.getTitle());
                // The last button: Cancel where there is one.
                if (done)
                    done (juce::jmax (0, options.getNumButtons() - 1));
            });
        view->setFileChooserHandler (
            [this] (const juce::String& title, const juce::File&, bool, const std::function<void (juce::File)>&)
            {
                ++questions;
                titles.add (title); // cancelled: no file chosen
            });
        view->onQuit = [this] { ++quits; };
        view->reportFolder = root.getChildFile ("reports");
        view->openUrl = [this] (const juce::URL&) { ++questions; };
        view->showFile = [this] (const juce::File&) { ++questions; };
        view->setManualFolder (juce::File (SPINUS_TEST_FILES_DIR).getSiblingFile ("docs/user"));
        view->setLayoutsFile (root.getChildFile ("layouts.json"));
    }

    ~Fixture()
    {
        view = nullptr;
        if (ai != nullptr)
            ai->setEnabled (false);
        document = nullptr;
        root.deleteRecursively();
    }

    /** Invokes every enabled item of every menu: each one must do something. Help > Open log folder is left
        out: it opens the system's file manager. */
    void invokeEveryEnabledItem (const std::set<int>& skip = {})
    {
        const auto before = windowsNow();
        for (auto& item : allItems (*view))
        {
            if (! item.enabled || skip.contains (item.id) || item.id == MainView::openLogFolder)
                continue;
            INFO (item.path);
            if (item.isCommand)
            {
                // An earlier item may have changed what this one can do (Undo removes what was moved last).
                juce::ApplicationCommandInfo info (item.id);
                view->getCommandInfo (item.id, info);
                if (((unsigned)info.flags & (unsigned)juce::ApplicationCommandInfo::isDisabled) != 0)
                    continue;
            }
            const auto was = stateOf (*view, session, document.get(), questions, quits);
            if (item.isCommand)
                CHECK (view->getCommandManager().invokeDirectly (item.id, false));
            else if (item.action)
                item.action();
            else if (auto* bar = menuBar(); bar != nullptr && bar->getModel() != nullptr)
                bar->getModel()->menuItemSelected (item.id, 0); // File > Open recent's projects
            else
                FAIL ("an enabled item with neither a command nor an action");
            // Right away too: a dialog has its window now (headless, a modal one is dismissed at once).
            const auto right = stateOf (*view, session, document.get(), questions, quits);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            const auto after = stateOf (*view, session, document.get(), questions, quits);
            CHECK ((right != was || after != was));
            closeWindows (before);
        }
    }

    juce::MenuBarComponent* menuBar() const
    {
        for (auto* child : view->getChildren())
            if (auto* bar = dynamic_cast<juce::MenuBarComponent*> (child))
                return bar;
        return nullptr;
    }

    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-menus-" + juce::Uuid().toString());
    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine() };
    std::unique_ptr<ProjectDocument> document;
    std::unique_ptr<ai::AiService> ai;
    std::unique_ptr<plugins::PluginDownloads> downloads;
    std::unique_ptr<plugins::WindowsPlugins> windows;
    std::unique_ptr<MainView> view;
    int questions = 0, quits = 0;
    juce::StringArray titles;
};
} // namespace

TEST_CASE ("Menus: every item works, or is greyed out when it can't (no project document, AI or plugins)",
           "[ui][menus]")
{
    Fixture f (false, false);
    const auto items = allItems (*f.view);
    REQUIRE (items.size() > 30);

    // Each item is named, and each command has its description (the hint bar shows it).
    for (auto& item : items)
    {
        INFO (item.path);
        CHECK (item.path.fromLastOccurrenceOf (" > ", false, false).trim().isNotEmpty());
        if (item.isCommand)
        {
            juce::ApplicationCommandInfo info (item.id);
            f.view->getCommandInfo (item.id, info);
            CHECK (info.shortName.isNotEmpty());
            CHECK (info.description.isNotEmpty());
        }
    }

    // Greyed out: what needs a project document, the AI service, the free plugins or Windows plugins; Undo and
    // Redo with nothing to undo; automating with nothing moved; layouts not saved yet.
    const auto disabled = disabledItems (*f.view);
    std::set<juce::String> expected { "File > New",         "File > Open...",
                                      "File > Open recent", "File > Save",
                                      "File > Save as...",  "Edit > Undo",
                                      "Edit > Redo",        "Options > Get free plugins...",
                                      "Options > AI..." };
#if JUCE_LINUX || JUCE_BSD
    expected.insert ("Options > Windows plugins...");
#endif
    for (auto& path : disabled)
    {
        INFO (path);
        CHECK ((expected.contains (path) || path.startsWith ("Edit > Create automation clip") ||
                path.startsWith ("View > Layouts > Delete layout")));
    }
    for (auto& path : expected)
    {
        INFO (path);
        CHECK ((disabled.contains (path) || path == "File > Open recent")); // a sub-menu: checked in collect
    }

    // A greyed-out command does nothing from its keyboard shortcut either.
    for (auto& item : items)
        if (item.isCommand && ! item.enabled)
        {
            INFO (item.path);
            const auto was = stateOf (*f.view, f.session, nullptr, f.questions, f.quits);
            CHECK (! f.view->getCommandManager().invokeDirectly (item.id, false));
            CHECK (stateOf (*f.view, f.session, nullptr, f.questions, f.quits) == was);
        }

    // And everything else does something.
    f.invokeEveryEnabledItem();
}

TEST_CASE ("Menus: with a project, AI and plugins set up, every item works; undo, redo and automate follow the state",
           "[ui][menus]")
{
    Fixture f (true, true);

    // A recent project and a saved layout fill their sub-menus.
    REQUIRE (f.session.getHistory().perform (make ("pattern.add", R"({"length": 3840})")).wasOk());
    REQUIRE (f.view->saveProjectTo (f.root.getChildFile ("Song")));
    f.view->getWindowLayouts().save ("Mine", f.view->getCurrentLayout());
    REQUIRE (f.session.getHistory().perform (make ("pattern.add", R"({"length": 3840})")).wasOk());
    f.session.getHistory().undo(); // something to undo and to redo

    auto disabled = disabledItems (*f.view);
    for (auto& path : disabled)
    {
        INFO (path);
        CHECK ((path.startsWith ("Edit > Create automation clip") || path.startsWith ("View > Layouts > Layout ")));
    }
    bool recentListed = false;
    for (auto& item : allItems (*f.view))
        recentListed = recentListed || item.path.startsWith ("File > Open recent > Song");
    CHECK (recentListed);

    // A new change: nothing to redo. A fader moved: Automate is there.
    REQUIRE (f.session.getHistory().perform (make ("channel.add", R"({"name": "Lead"})")).wasOk());
    model::ParameterTarget fader;
    fader.channel = f.session.getProject().channels.back().id;
    f.session.noteMovedControl (fader);
    f.view->getCommandManager().commandStatusChanged();
    disabled = disabledItems (*f.view);
    CHECK (disabled.contains ("Edit > Redo"));
    for (auto& path : disabled)
        CHECK (! path.startsWith ("Edit > Create automation clip"));

    // Everything enabled does something. Quit is answered with Cancel when there are unsaved changes; the
    // demo's recent project is opened last (it replaces the project).
    f.invokeEveryEnabledItem();
}
