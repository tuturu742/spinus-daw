#include <catch2/catch_test_macros.hpp>

#include "core/ChildProcess.h"
#include "ui/Theme.h"
#include "ui/WindowsPluginsDialog.h"

#include <iostream>

using namespace spinus;
using namespace spinus::ui;
using plugins::WindowsPlugins;

namespace
{
struct TempFolder
{
    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-windows-dialog") + "-" + juce::Uuid().toString());
};

/** Spinus's runtime as if downloaded (files present, no real Wine) and a prefix with one plugin. */
void fakeReady (WindowsPlugins& w)
{
    const auto& pack = WindowsPlugins::getRuntimePack();
    const auto dir = w.getDownloads().getPackFolder (pack);
    for (auto& f : pack.files)
        REQUIRE (dir.getChildFile (f).create().wasOk());
    REQUIRE (dir.getChildFile (".spinus-pack").replaceWithText (pack.version));
    REQUIRE (w.findSpinusRuntime().has_value());
    w.setChosenRuntime (WindowsPlugins::Runtime::Kind::spinus);

    REQUIRE (w.getPrefix().getChildFile ("system.reg").create().wasOk());
    // A 64-bit PE header is enough to be listed.
    juce::MemoryBlock pe (0x200, true);
    auto* b = static_cast<uint8_t*> (pe.getData());
    b[0] = 'M', b[1] = 'Z', b[0x3c] = 0x40, b[0x40] = 'P', b[0x41] = 'E', b[0x44] = 0x64, b[0x45] = 0x86;
    const auto clap = w.getPrefix().getChildFile ("drive_c/Program Files/Common Files/CLAP/Synth.clap");
    REQUIRE (clap.getParentDirectory().createDirectory().wasOk());
    REQUIRE (clap.replaceWithData (pe.getData(), pe.getSize()));
}

juce::File scanner()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile)
        .getParentDirectory()
        .getParentDirectory()
        .getChildFile ("spinus-plugin-scanner");
}
} // namespace

TEST_CASE ("The Windows plugins window starts with Set up, and lists plugins once ready", "[ui][windows]")
{
    TempFolder t;
    WindowsPlugins windows (t.root);
    {
        WindowsPluginsDialog dialog (windows, scanner());
        CHECK (dialog.getPage() == WindowsPluginsDialog::Page::notSetUp);
        CHECK (dialog.getStatusText().startsWith ("Not set up yet"));
        CHECK (! dialog.isBusy());
    }

    fakeReady (windows);
    WindowsPluginsDialog dialog (windows, scanner());
    CHECK (dialog.getPage() == WindowsPluginsDialog::Page::ready);
    CHECK (dialog.getPluginNames() == juce::StringArray { "Synth (CLAP)" });
    CHECK (dialog.getStatusText() == "Ready: 1 Windows plugins.");
}

TEST_CASE ("A Windows plugin is removed from the window, unless the project uses it", "[ui][windows]")
{
    TempFolder t;
    WindowsPlugins windows (t.root);
    fakeReady (windows);
    WindowsPluginsDialog dialog (windows, scanner());
    REQUIRE (dialog.getPluginNames() == juce::StringArray { "Synth (CLAP)" });
    int changes = 0;
    dialog.onPluginsChanged = [&changes] { ++changes; };

    // In use in the open project: refused, and said why.
    dialog.whoUses = [] (const juce::Array<juce::File>&) { return juce::StringArray { "Lead" }; };
    dialog.removePlugin (0, true);
    CHECK (dialog.getStatusText().contains ("is used in this project (Lead)"));
    CHECK (dialog.getPluginNames().size() == 1);
    CHECK (changes == 0);

    dialog.whoUses = [] (const juce::Array<juce::File>&) { return juce::StringArray(); };
    dialog.removePlugin (0, true);
    CHECK (dialog.getStatusText() == "Removed Synth (CLAP).");
    CHECK (dialog.getPluginNames().isEmpty());
    CHECK (changes == 1); // the Browser scans again and drops it
    CHECK (! windows.getPrefix().getChildFile ("drive_c/Program Files/Common Files/CLAP/Synth.clap").exists());
}

TEST_CASE ("In the Flatpak, setting up says the computer's Wine cannot be used", "[ui][windows]")
{
    TempFolder t;
    WindowsPlugins windows (t.root, true);
    WindowsPluginsDialog dialog (windows, scanner());
    CHECK (dialog.getStatusText().contains ("In the Flatpak version"));
    dialog.setUp();
    dialog.finishForTests();
    CHECK (dialog.getPage() == WindowsPluginsDialog::Page::offerDownload);
    CHECK (dialog.getStatusText().contains ("cannot use the Wine installed on this computer"));
}

TEST_CASE ("A step can be cancelled, and a failed download says why", "[ui][windows]")
{
    TempFolder t;
    WindowsPlugins windows (t.root);
    WindowsPluginsDialog dialog (windows, scanner());
    dialog.downloadRuntime();
    CHECK (dialog.isBusy());
    dialog.cancel();
    dialog.finishForTests();
    CHECK (! dialog.isBusy());
    CHECK (dialog.getStatusText().startsWith ("Could not download Wine and yabridge"));
    CHECK (dialog.getPage() == WindowsPluginsDialog::Page::notSetUp);
}

// A real setup with the Wine on this computer (or Spinus's download), then adding the test plugin:
//   spinus_tests "[.windows-e2e]"
TEST_CASE ("Windows plugins window end to end: set up, then add a plugin", "[.windows-e2e]")
{
    const auto folder =
        juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("spinus-windows-ui-e2e");
    folder.deleteRecursively();
    WindowsPlugins windows (folder);
    WindowsPluginsDialog dialog (windows, scanner());

    dialog.setUp();
    dialog.finishForTests();
    if (dialog.getPage() == WindowsPluginsDialog::Page::offerDownload)
    {
        std::cout << "Set up offered the download: " << dialog.getStatusText() << "\n";
        dialog.downloadRuntime();
        dialog.finishForTests();
    }
    INFO (dialog.getStatusText());
    REQUIRE (dialog.getPage() == WindowsPluginsDialog::Page::ready);
    std::cout << "Set up: " << dialog.getStatusText() << "\n";

    const auto plugin = windows.getDownloads()
                            .getPackFolder (WindowsPlugins::getTestPluginPack())
                            .getChildFile ("DragonflyRoomReverb.clap");
    dialog.addPlugin (plugin);
    dialog.finishForTests();
    CHECK (dialog.getStatusText().startsWith ("Added DragonflyRoomReverb (CLAP)"));
    CHECK (dialog.getPluginNames() == juce::StringArray { "DragonflyRoomReverb (CLAP)" });
    CHECK (windows.getBridgedFolder (WindowsPlugins::Format::clap)
               .getChildFile ("DragonflyRoomReverb.clap")
               .existsAsFile());

    dialog.retest();
    dialog.finishForTests();
    CHECK (dialog.getStatusText() == "Windows plugins work.");

    // A Windows program run as an "installer" (one that installs nothing): Spinus waits for it, then says
    // no new plugins were found. Meanwhile a plugin's Windows side runs (ping standing in for yabridge's
    // host, which runs as long as a plugin is open): the installer does not wait for it.
    const auto runtime = windows.getRuntime();
    REQUIRE (runtime.has_value());
    const auto wine = runtime.value_or (plugins::WindowsPlugins::Runtime {});
    const auto fakeHost = windows.getPrefix().getChildFile ("drive_c/yabridge-host.exe");
    REQUIRE (windows.getPrefix().getChildFile ("drive_c/windows/system32/ping.exe").copyFileTo (fakeHost));
    spinus::core::ChildProcess pluginHost;
    REQUIRE (
        pluginHost.start (windows.wineCommand (wine, { fakeHost.getFullPathName(), "-n", "120", "127.0.0.1" }), 0));
    juce::Thread::sleep (3000);
    const auto program = windows.getPrefix().getChildFile ("drive_c/windows/system32/hostname.exe");
    REQUIRE (program.existsAsFile());
    const auto started = juce::Time::getMillisecondCounter();
    dialog.addPlugin (program);
    dialog.finishForTests();
    CHECK (dialog.getStatusText().startsWith ("The installer finished and added no new plugin"));
    CHECK (juce::Time::getMillisecondCounter() - started < 30000u);
    CHECK (pluginHost.isRunning()); // still running, untouched
    spinus::core::ChildProcess stopHost;
    REQUIRE (stopHost.start (windows.wineCommand (wine, { "taskkill", "/F", "/IM", "yabridge-host.exe" })));
    CHECK (stopHost.waitForProcessToFinish (20000));
    CHECK (pluginHost.waitForProcessToFinish (20000));

    // An installer that hangs with its window open (Notepad stands in): Cancel ends it, not just Spinus's
    // waiting, so nothing is left running in Wine.
    const auto frozen = folder.getChildFile ("Frozen Setup.exe");
    REQUIRE (windows.getPrefix().getChildFile ("drive_c/windows/system32/notepad.exe").copyFileTo (frozen));
    dialog.addPlugin (frozen);
    juce::Thread::sleep (4000);
    REQUIRE (dialog.isBusy());
    dialog.cancel();
    dialog.finishForTests();
    CHECK (! dialog.isBusy());
    spinus::core::ChildProcess waiting;
    REQUIRE (waiting.start (windows.wineserverCommand (wine, { "-w" })));
    CHECK (waiting.waitForProcessToFinish (15000)); // everything in the prefix has ended
    if (waiting.isRunning())
        waiting.kill();
}

// The manual's picture (docs/user/images/windows-plugins.png) is a real capture: Dexed added through this
// window, with Spinus's own Wine. This draws the window the same way, from stand-ins.
TEST_CASE ("The Windows plugins window lists the plugins and Spinus's Wine version", "[ui][windows]")
{
    TempFolder t;
    WindowsPlugins windows (t.root);
    fakeReady (windows);
    const auto clap = windows.getPrefix().getChildFile ("drive_c/Program Files/Common Files/CLAP/Synth.clap");
    for (auto name : { "Big Synth.clap", "Vintage Keys.clap" })
        REQUIRE (clap.copyFileTo (clap.getSiblingFile (name)));
    REQUIRE (clap.getSiblingFile ("Synth.clap").deleteFile());
    // Spinus's own Wine, as after its download (a stand-in that only says its version).
    const auto wine = windows.findSpinusRuntime().value_or (WindowsPlugins::Runtime {}).wine;
    REQUIRE (wine.replaceWithText ("#!/bin/sh\necho 'wine-9.21 (Staging)'\n", false, false, "\n"));
    REQUIRE (wine.setExecutePermission (true));
    CHECK (WindowsPlugins::getWineVersion (windows.findSpinusRuntime().value_or (WindowsPlugins::Runtime {})) ==
           "wine-9.21 (Staging)");

    SpinusLookAndFeel lookAndFeel { Theme::dark() };
    WindowsPluginsDialog dialog (windows, scanner());
    dialog.setLookAndFeel (&lookAndFeel);
    CHECK (dialog.getPluginNames() == juce::StringArray { "Big Synth (CLAP)", "Vintage Keys (CLAP)" });
    CHECK (dialog.getStatusText() == "Ready: 2 Windows plugins.");
    const auto image = dialog.createComponentSnapshot (dialog.getLocalBounds(), true, 1.0f);
    CHECK (image.isValid());
    dialog.setLookAndFeel (nullptr);
}

namespace
{
/** A stand-in for Wine: an "installer" puts a CLAP plugin in the prefix, then exits, or (named "Hang...")
    keeps running until stopped. Wine's own commands do nothing. */
void fakeWine (WindowsPlugins& windows, const juce::File& root)
{
    const auto runtime = windows.getRuntime();
    REQUIRE (runtime.has_value());
    const auto wine = runtime.value_or (WindowsPlugins::Runtime {}).wine;
    const auto plugin = root.getChildFile ("NewSynth.clap");
    REQUIRE (
        windows.getPrefix().getChildFile ("drive_c/Program Files/Common Files/CLAP/Synth.clap").copyFileTo (plugin));
    REQUIRE (wine.replaceWithText ("#!/bin/sh\n"
                                   "case \"$1\" in\n"
                                   "  --version) echo 'wine-9.21 (Staging)' ;;\n"
                                   "  wineboot|tasklist|taskkill) ;;\n"
                                   "  reg) echo 'reg: The operation completed successfully' ;;\n"
                                   "  *) cp '" +
                                       plugin.getFullPathName() +
                                       "' \"$WINEPREFIX/drive_c/Program Files/"
                                       "Common Files/CLAP/\"\n"
                                       "     case \"$1\" in *Hang*) exec sleep 60 ;; esac ;;\n"
                                       "esac\n",
                                   false, false, "\n"));
    REQUIRE (wine.getSiblingFile ("wineserver").replaceWithText ("#!/bin/sh\n", false, false, "\n"));
    REQUIRE (wine.setExecutePermission (true));
    REQUIRE (wine.getSiblingFile ("wineserver").setExecutePermission (true));
}
} // namespace

TEST_CASE ("After an installer, even a cancelled one, its plugins are bridged and the Browser scans", "[ui][windows]")
{
    TempFolder t;
    WindowsPlugins windows (t.root.getChildFile ("windows"));
    fakeReady (windows);
    fakeWine (windows, t.root);
    WindowsPluginsDialog dialog (windows, scanner());
    REQUIRE (dialog.getPage() == WindowsPluginsDialog::Page::ready);
    int scans = 0;
    dialog.onPluginsChanged = [&scans] { ++scans; };
    const auto bridged = windows.getBridgedFolder (WindowsPlugins::Format::clap).getChildFile ("NewSynth.clap");
    const auto added = windows.getPrefix().getChildFile ("drive_c/Program Files/Common Files/CLAP/NewSynth.clap");

    SECTION ("finished")
    {
        const auto installer = t.root.getChildFile ("Setup.exe");
        REQUIRE (installer.replaceWithText ("MZ"));
        dialog.addPlugin (installer);
        dialog.finishForTests();
        INFO (dialog.getStatusText());
        CHECK (dialog.getStatusText().startsWith ("Added NewSynth (CLAP)"));
    }
    SECTION ("cancelled while it still runs")
    {
        const auto installer = t.root.getChildFile ("Hang Setup.exe");
        REQUIRE (installer.replaceWithText ("MZ"));
        dialog.addPlugin (installer);
        for (int i = 0; i < 100 && ! added.existsAsFile(); ++i)
            juce::Thread::sleep (50);
        REQUIRE (added.existsAsFile());
        juce::Thread::sleep (300);
        CHECK (dialog.isBusy());
        dialog.cancel();
        dialog.finishForTests();
        CHECK (dialog.getStatusText().startsWith ("The installer was stopped. It had added NewSynth (CLAP)"));
    }
    CHECK (dialog.getPluginNames().contains ("NewSynth (CLAP)"));
    CHECK (bridged.exists()); // bridged now, not after a restart
    CHECK (scans == 1);       // and the Browser scans it in
}

TEST_CASE ("In the Flatpak without 32-bit support, a 32-bit installer is not started and the window says why",
           "[ui][windows]")
{
    TempFolder t;
    WindowsPlugins windows (t.root, true);
    fakeReady (windows);
    windows.set32BitLoaderForTests (t.root.getChildFile ("no-32-bit-loader"));
    WindowsPluginsDialog dialog (windows, scanner());
    REQUIRE (dialog.getPage() == WindowsPluginsDialog::Page::ready);

    // A 32-bit PE header (Intel i386), as in many plugin installers.
    juce::MemoryBlock pe (0x200, true);
    auto* b = static_cast<uint8_t*> (pe.getData());
    b[0] = 'M', b[1] = 'Z', b[0x3c] = 0x40, b[0x40] = 'P', b[0x41] = 'E', b[0x44] = 0x4c, b[0x45] = 0x01;
    const auto installer = t.root.getChildFile ("AME_Installer.exe");
    REQUIRE (installer.replaceWithData (pe.getData(), pe.getSize()));

    auto& modal = *juce::ModalComponentManager::getInstance();
    const auto before = modal.getNumModalComponents();
    const auto windowsBefore = juce::TopLevelWindow::getNumTopLevelWindows();
    dialog.addPlugin (installer);
    CHECK (! dialog.isBusy()); // nothing started
    CHECK (dialog.getStatusText().contains ("AME_Installer.exe was not started: it is a 32-bit Windows program"));
    for (int i = 0; i < 250 && modal.getNumModalComponents() == before; ++i) // the message box opens
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    CHECK (modal.getNumModalComponents() == before + 1); // the message with the command
    CHECK (! windows.getPrefix().getChildFile ("drive_c/windows").exists());
    modal.cancelAllModalComponents();
    // Closed message boxes are deleted a message or two later: wait for that, however busy the machine.
    for (int i = 0; i < 250 && juce::TopLevelWindow::getNumTopLevelWindows() > windowsBefore; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    CHECK (juce::TopLevelWindow::getNumTopLevelWindows() == windowsBefore);
}
