#include <catch2/catch_test_macros.hpp>

#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "project/ProjectFiles.h"
#include "session/DemoSong.h"
#include "session/ProjectDocument.h"
#include "ui/Browser.h"
#include "ui/ChannelRack.h"
#include "ui/MainView.h"
#include "ui/Manual.h"
#include "ui/ManualDemo.h"
#include "ui/PianoRoll.h"
#include "ui/Playlist.h"

using namespace spinus;
using namespace spinus::ui;

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

std::unique_ptr<engine::DeviceFactory> makeDevices (tracktion::Engine& engine)
{
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerBundledDevices (*devices, engine, { bundledPlugins() });
    return devices;
}

void write (const juce::Image& image, const juce::String& name)
{
    const auto file = juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/" + name);
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}

/** A panel with its title bar, as it sits in the workspace. */
juce::Image snapshotPanel (MainView& view, const juce::String& id)
{
    view.getWorkspace().showPanel (id);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    auto* panel = view.getWorkspace().getPanel (id);
    REQUIRE (panel != nullptr);
    auto* frame = panel->getParentComponent();
    REQUIRE (frame != nullptr);
    return view.createComponentSnapshot (view.getLocalArea (frame, frame->getLocalBounds()), true, 1.0f);
}

/** Ctrl+mouse wheel over the component, as a user zooms: `steps` clicks out (negative) or in. */
void ctrlWheel (juce::Component& c, int steps)
{
    const auto centre = c.getLocalBounds().getCentre().toFloat();
    const auto now = juce::Time::getCurrentTime();
    const juce::MouseEvent e (juce::Desktop::getInstance().getMainMouseSource(), centre,
                              juce::ModifierKeys::ctrlModifier, juce::MouseInputSource::defaultPressure, 0.0f, 0.0f,
                              0.0f, 0.0f, &c, &c, now, centre, now, 0, false);
    const juce::MouseWheelDetails wheel { 0.0f, steps > 0 ? 0.5f : -0.5f, false, false, false };
    for (int i = 0; i < std::abs (steps); ++i)
        c.mouseWheelMove (e, wheel);
}

/** Scrolls the component's horizontal scroll bar to `start` (in the bar's own units). */
void scrollTo (juce::Component& c, double start)
{
    for (auto* child : c.getChildren())
        if (auto* bar = dynamic_cast<juce::ScrollBar*> (child); bar != nullptr && ! bar->isVertical())
            bar->setCurrentRangeStart (start, juce::sendNotificationSync);
}

/** The id of the channel or pattern with this name. */
template <typename Items> auto idOf (const Items& items, const juce::String& name)
{
    for (const auto& item : items)
        if (item.name == name)
            return item.id;
    FAIL ("no " << name);
    return items.front().id;
}
} // namespace

TEST_CASE ("Main window and panel screenshots for the manual", "[.screenshot][manual]")
{
    // The demo song (Browser › Projects), as a user sees it: its copy opened with the free plugins
    // it uses (Surge XT, Surge XT Effects, Dragonfly) from the build's plugins folder.
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-manual-shots-" + juce::Uuid().toString());
    const auto shipped = juce::File (SPINUS_TEST_FILES_DIR).getSiblingFile ("demo").getChildFile (demo::demoSongName);
    const auto copy = demo::copyDemo (shipped, root.getChildFile ("Music"));
    REQUIRE (copy.result.wasOk());
    {
        EngineHost host { { .openAudioDevice = false } };
        Session session (host.getEngine(), makeDevices (host.getEngine()));
        session.getDeviceList().setBundledFolders ({ bundledPlugins() });
        ProjectDocument document (session, root.getChildFile ("settings"));
        MainView view (session, &document);
        juce::StringArray questions;
        view.setMessageBoxHandler ([&] (const juce::MessageBoxOptions& options, const std::function<void (int)>&)
                                   { questions.add (options.getTitle()); });
        view.setSize (1280, 800);
        REQUIRE (view.openProjectFile (copy.projectFile));
        juce::MessageManager::getInstance()->runDispatchLoopUntil (2000);
        INFO (questions.joinIntoString ("; "));
        REQUIRE (questions.isEmpty());

        // The lead's melody in the Piano Roll, the lead channels in the Channel Rack, the first drop on the
        // Playlist.
        const auto& project = session.getProject();
        session.setCurrentPattern (idOf (project.patterns, "Lead B"));
        session.setSelectedChannel (idOf (project.channels, "Lead (Surge XT)"));
        auto& workspace = view.getWorkspace();
        REQUIRE (view.useLayout ("Compose"));
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        auto* rack = dynamic_cast<ChannelRack*> (workspace.getPanel ("channelRack"));
        auto* roll = dynamic_cast<PianoRoll*> (workspace.getPanel ("pianoRoll"));
        auto* playlist = dynamic_cast<Playlist*> (workspace.getPanel ("playlist"));
        REQUIRE (rack != nullptr);
        REQUIRE (roll != nullptr);
        REQUIRE (playlist != nullptr);
        rack->showGroup ("Leads");
        roll->handleUpdateNowIfNeeded();
        roll->scrollToPitch (78);
        scrollTo (*playlist, 32.0 * 3840.0);

        view.getCommandManager().commandStatusChanged();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (500);
        write (view.createComponentSnapshot (view.getLocalBounds(), true, 1.0f), "main-window.png");

        write (snapshotPanel (view, "channelRack"), "channel-rack.png");
        write (snapshotPanel (view, "pianoRoll"), "piano-roll.png");
        REQUIRE (view.useLayout ("Arrange")); // the Playlist over the whole workspace
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        ctrlWheel (*playlist, -9); // the song's first half: intro, verse, build and drop
        scrollTo (*playlist, 0.0);
        write (snapshotPanel (view, "playlist"), "playlist.png");
    }
    root.deleteRecursively();
}

TEST_CASE ("Browser projects and Manual screenshots for the manual", "[.screenshot][manual]")
{
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-projects-shot-" + juce::Uuid().toString());
    {
        EngineHost host { { .openAudioDevice = false } };
        Session session (host.getEngine(), makeDevices (host.getEngine()));
        SpinusLookAndFeel lf { Theme::dark() };

        // Projects: the demo songs, three opened ones (one gone since), and a folder of projects.
        Browser browser (session);
        browser.setLookAndFeel (&lf);
        browser.setSize (300, 560);
        browser.setDemos (demo::listDemos (juce::File (SPINUS_TEST_FILES_DIR).getSiblingFile ("demo")));
        browser.setProjectsFile (root.getChildFile ("settings/projects.json"));
        for (auto name : { "Old Remix", "Rainy Loop", "Late Night Beat" })
        {
            REQUIRE (project::saveProject (root.getChildFile ("Music/" + juce::String (name)), {}).wasOk());
            browser.getProjects().addOpened (root.getChildFile ("Music/" + juce::String (name)));
        }
        const auto library = root.getChildFile ("Beats");
        for (auto name : { "2025/Summer Groove", "2025/Club Edit", "2026/Lo-fi Sketch", "Synthwave Demo" })
            REQUIRE (project::saveProject (library.getChildFile (name), {}).wasOk());
        REQUIRE (browser.addProjectFolder (library));
        root.getChildFile ("Music/Old Remix").deleteRecursively();
        browser.getProjects().rescan();
        browser.getProjects().waitForScan();
        browser.toggleOpen ("samples");
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        write (browser.createComponentSnapshot (browser.getLocalBounds(), true, 1.0f), "browser-projects.png");
        browser.setLookAndFeel (nullptr);

        // The manual, searched.
        ManualWindow manual (juce::File (SPINUS_TEST_FILES_DIR).getSiblingFile ("docs/user"));
        manual.setLookAndFeel (&lf);
        manual.setSize (1060, 700);
        manual.setSearchText ("freeze");
        manual.openResult (0);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        write (manual.createComponentSnapshot (manual.getLocalBounds(), true, 1.0f), "manual.png");
        manual.setLookAndFeel (nullptr);
    }
    root.deleteRecursively();
}
