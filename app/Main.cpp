#include <juce_gui_basics/juce_gui_basics.h>

#include <SpinusVersion.h>

#if JUCE_LINUX || JUCE_BSD
#include <dlfcn.h>
#elif JUCE_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "commands/CommandRegistry.h"
#include "core/FileDialog.h"
#include "core/Log.h"
#include "core/FileDialogPortal.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "engine/PluginCrashGuard.h"
#include "session/ProgramFolder.h"
#include "session/AudioDeviceLog.h"
#include "session/SelfTest.h"
#include "session/SystemInfo.h"
#include "session/TextLocale.h"
#include "plugins/SandboxList.h"
#include "plugins/PluginCatalog.h"
#include "plugins/PluginDownloads.h"
#include "plugins/windows/PluginDialogs.h"
#include "plugins/windows/WindowsPlugins.h"
#include "ai/AiService.h"
#include "ui/About.h"
#include "ui/AiSettingsDialog.h"
#include "ui/Manual.h"
#include "session/ProjectDocument.h"
#include "ui/ChannelRack.h"
#include "ui/Logo.h"
#include "ui/Browser.h"
#include "ui/MainView.h"
#include "ui/ManualDemo.h"
#include "ui/Mixer.h"
#include "ui/PianoRoll.h"
#include "ui/PluginDownloadDialog.h"
#include "ui/WindowsPluginsDialog.h"
#include "ui/Playlist.h"

namespace spinus
{

class SpinusApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return productName; }
    const juce::String getApplicationVersion() override { return versionString; }
    bool moreThanOneInstanceAllowed() override { return true; }

    /** For the commands that print to the terminal (--self-test, --scan-plugins): only Spinus's own lines.
        Tracktion logs its start-up steps, and JACK's library prints errors when no JACK server runs (the
        engine looks for one), which read like failures. */
    static void quietTerminal()
    {
        struct SilentLogger final : juce::Logger
        {
            void logMessage (const juce::String&) override {}
        };
        static SilentLogger silent;
        juce::Logger::setCurrentLogger (&silent);

#if JUCE_LINUX || JUCE_BSD
        // JUCE opens libjack itself; the same library's message functions are set here (kept loaded).
        if (auto* jack = dlopen ("libjack.so.0", RTLD_LAZY))
        {
            using SetFunction = void (*) (void (*) (const char*));
            for (auto* name : { "jack_set_error_function", "jack_set_info_function" })
                if (auto* set = reinterpret_cast<SetFunction> (dlsym (jack, name)))
                    set ([] (const char*) {});
        }
#endif
    }

    void initialise (const juce::String& commandLine) override
    {
        session::ensureUtf8ForChildProcesses();

#if JUCE_WINDOWS
        // A Windows program with a window has no terminal of its own: the commands that print use the one
        // Spinus was started from, unless their output already goes somewhere (a pipe, a file).
        const auto output = GetStdHandle (STD_OUTPUT_HANDLE);
        if ((commandLine.contains ("--version") || commandLine.contains ("--self-test") ||
             commandLine.contains ("--scan-plugins")) &&
            (output == nullptr || output == INVALID_HANDLE_VALUE))
            if (AttachConsole (ATTACH_PARENT_PROCESS))
            {
                FILE* stream = nullptr;
                freopen_s (&stream, "CONOUT$", "w", stdout);
                freopen_s (&stream, "CONOUT$", "w", stderr);
            }
#endif
        if (commandLine.contains ("--version"))
        {
            std::cout << productName << ' ' << versionString << '\n';
            quit();
            return;
        }

        const auto args = juce::StringArray::fromTokens (commandLine, true);

        if (const auto selfTest = args.indexOf ("--self-test"); selfTest >= 0)
        {
            // Packages are checked with this on other systems: no audio device, no window, and
            // engine settings of its own, apart from the user's.
            quietTerminal();
            EngineHost::setSettingsName ("Spinus DAW Self Test");
            EngineHost host ({ .openAudioDevice = false });
            const auto folder =
                args[selfTest + 1].isNotEmpty()
                    ? juce::File::getCurrentWorkingDirectory().getChildFile (args[selfTest + 1].unquoted())
                    : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("spinus-self-test");
            const bool passed = runSelfTest (host.getEngine(), folder, [] (const juce::String& line)
                                             { std::cout << line << '\n'
                                                         << std::flush; });
            // Where a tester finds the logs to attach to a report (the self-test itself writes none).
            std::cout << "Logs of Spinus's last runs (Help > Open log folder): " << getLogFolder().getFullPathName()
                      << '\n';
            std::cout << (passed ? "Spinus works here." : "Some checks failed.") << '\n' << std::flush;
            setApplicationReturnValue (passed ? 0 : 1);
            quit();
            return;
        }

        if (args.contains ("--scan-plugins"))
        {
            quietTerminal();
            setApplicationReturnValue (scanPlugins());
            quit();
            return;
        }
        if (const auto choose = args.indexOf ("--choose-file"); choose >= 0)
        {
            chooseFileCheck (args[choose + 1].unquoted());
            return;
        }
        const auto snapshotIndex = args.indexOf ("--snapshot");

        // This run's log (Help > Open log folder); not for snapshots, which are made by tools.
        const auto started = juce::Time::getMillisecondCounter();
        if (snapshotIndex < 0)
            startLog (commandLine);

        // A plugin that took Spinus down last time is named here; from now on a crash in one is noted.
        const auto crashRecord = getSettingsFolder().getChildFile ("plugin-crash.json");
        const auto crashed = engine::PluginCrashGuard::takeRecorded (crashRecord);
        getSettingsFolder().createDirectory();
        engine::PluginCrashGuard::install (crashRecord);
        if (crashed)
            core::Logs::warning ("start", "A plugin took Spinus down last time: " + crashed->name +
                                              ". Spinus offers to run it in a separate process.");

        EngineHost::Options options;
        options.openAudioDevice = ! args.contains ("--no-audio") && snapshotIndex < 0;

        engineHost = std::make_unique<EngineHost> (options);
        if (snapshotIndex < 0)
        {
            core::Logs::info ("start", options.openAudioDevice
                                           ? juce::String ("Audio engine ready")
                                           : juce::String ("Audio engine ready, without a device (--no-audio)"));
            if (engineHost->getFirstStartAudioChoice().isNotEmpty())
                core::Logs::info ("audio", "First start: chose " + engineHost->getFirstStartAudioChoice());
            if (options.openAudioDevice)
                audioLog = std::make_unique<session::AudioDeviceLog> (
                    engineHost->getEngine().getDeviceManager().deviceManager);
        }

        // The free open-source plugins: downloaded on request into the user's data folder,
        // or found in a development build's plugins/ folder or the system's CLAP folders.
        downloads = std::make_unique<plugins::PluginDownloads> (getDataFolder().getChildFile ("plugins"));
        const auto bundledFolders = downloads->getSearchFolders ({ getProgramFolder().getChildFile ("plugins") });
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerBundledDevices (*devices, engineHost->getEngine(), bundledFolders);
        // Plugins the user runs in a separate process.
        sandboxList =
            std::make_unique<plugins::SandboxList> (getPluginListFolder().getChildFile ("plugins-run-separately.json"));
        devices->setRunSeparately (
            [list = sandboxList.get()] (const juce::PluginDescription& d)
            { return list->contains (plugins::SandboxList::keyOf (d.pluginFormatName, d.fileOrIdentifier)); });
        session = std::make_unique<Session> (engineHost->getEngine(), std::move (devices));
        session->setSandboxList (sandboxList.get());
        session->getDeviceList().setBundledFolders (bundledFolders);

        if (snapshotIndex >= 0)
        {
            setApplicationReturnValue (writeSnapshot (args[snapshotIndex + 1].unquoted()) ? 0 : 1);
            quit();
            return;
        }

        // Plugins found by earlier scans: listed in the Browser and loadable in projects. (Not for
        // snapshots, which look the same on every machine.)
        catalog = std::make_unique<plugins::PluginCatalog> (getPluginListFolder(), getProgramFolder());
        catalog->load();
        core::Logs::info ("plugins", "Plugin list: " + juce::String (catalog->getKnownPlugins().getNumTypes()) +
                                         " plugins, " + juce::String ((int)catalog->getProblems().getAll().size()) +
                                         " problem plugins (" + getPluginListFolder().getFullPathName() + ")");
#if ! JUCE_WINDOWS // Windows plugins through Wine; on Windows they are simply plugins
        windows = std::make_unique<plugins::WindowsPlugins> (getWindowsPluginsFolder(), isFlatpak());
        useWindowsPlugins (*windows, *catalog);
        // A Windows plugin asking something in a window of its own (e.g. to register) while Spinus waits for
        // it: that window is brought to the front.
        pluginDialogs = std::make_unique<plugins::PluginDialogs>();
        pluginDialogs->watchMessageThread();
#endif
        // The catalog is the plugin list; Tracktion's own saved copy may hold plugins that are gone.
        engineHost->getEngine().getPluginManager().knownPluginList.clear();
        catalog->addKnownTypesTo (engineHost->getEngine().getPluginManager().knownPluginList);
        session->getDeviceList().setPreferencesFile (getPluginListFolder().getChildFile ("plugin-formats.json"));
        session->getDeviceList().setCatalog (catalog.get());

        document = std::make_unique<ProjectDocument> (*session, getSettingsFolder());
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *session, *document);
        windowIcons = std::make_unique<ui::WindowIcons>();
        auto& view = mainWindow->getView();
        view.onQuit = [] { quit(); };
        view.setPluginDownloads (downloads.get(), bundledFolders);
        view.getBrowser().setSampleFoldersFile (getSettingsFolder().getChildFile ("sample-folders.txt"));
        view.getBrowser().setProjectsFile (getSettingsFolder().getChildFile ("projects.json"));
        view.setShortcutsFile (getSettingsFolder().getChildFile ("shortcuts.xml"));
        view.setLayoutsFile (getSettingsFolder().getChildFile ("layouts.xml"));
        aiService = std::make_unique<ai::AiService> (getSettingsFolder());
        view.setAiService (aiService.get());
        view.setWindowsPlugins (windows.get(), getHelperProgram ("spinus-plugin-scanner"));

        // `spinus "Song/Song.spn"` (or the folder) opens that project; otherwise offer crashed work, if any.
        for (auto& arg : args)
            if (! arg.startsWith ("-") &&
                juce::File::getCurrentWorkingDirectory().getChildFile (arg.unquoted()).exists())
            {
                view.openProjectFile (juce::File::getCurrentWorkingDirectory().getChildFile (arg.unquoted()));
                break;
            }
        if (crashed)
            view.offerToRunSeparately (crashed->key, crashed->name, [v = &view] { v->offerRecovery(); });
        else
            view.offerRecovery();

        // The Wine on this computer changed since Windows plugins were last tested with it: test again.
        if (windows != nullptr && windows->getChosenRuntime() == plugins::WindowsPlugins::Runtime::Kind::system &&
            windows->needsRetest())
            view.showWindowsPlugins (true);

        // No plugin list yet (a first start, or the Flatpak's first start beside a native Spinus): look for the
        // installed plugins now rather than show an empty Browser.
        if (! getPluginListFolder().getChildFile ("plugin-list.xml").existsAsFile())
            view.getBrowser().startScan();

        core::Logs::info ("start",
                          "Started in " + juce::String (juce::Time::getMillisecondCounter() - started) + " ms");

        // First launch: offer the free plugins (once; later from Options > Get free plugins).
        const auto offered = getPluginListFolder().getChildFile ("plugin-downloads-offered");
        if (! offered.exists() && view.arePluginDownloadsMissing())
        {
            getPluginListFolder().createDirectory();
            [[maybe_unused]] const auto created = offered.create();
            view.showPluginDownloads();
        }
    }

    void shutdown() override
    {
        core::Logs::info ("quit", "Quitting");
        checkDialog = nullptr;
        checkWindow = nullptr;
        ui::PluginDownloadDialog::closeWindow(); // cancels downloads
        ui::WindowsPluginsDialog::closeWindow(); // cancels a running setup or test
        ui::AiSettingsDialog::closeWindow();
        ui::ManualWindow::closeWindow();
        ui::AboutSpinus::closeWindow();
        windowIcons = nullptr;
        mainWindow = nullptr;
        aiService = nullptr; // stops the AI service
        document = nullptr;
        session = nullptr;
        sandboxList = nullptr;
        catalog = nullptr;
        audioLog = nullptr;
        engineHost = nullptr;
        core::Logs::info ("quit", "Audio engine closed");
        downloads = nullptr;
        windows = nullptr;
        pluginDialogs = nullptr;
        core::Logs::stop();
    }

    /** An exception that reached JUCE's message loop: noted before Spinus goes on (or ends). */
    void unhandledException (const std::exception* e, const juce::String& sourceFile, int line) override
    {
        core::Logs::error ("crash", "Nothing caught " +
                                        (e != nullptr ? "a C++ exception: " + juce::String (e->what())
                                                      : juce::String ("an exception that is not a std::exception")) +
                                        (sourceFile.isNotEmpty() ? " (" + juce::File (sourceFile).getFileName() + ":" +
                                                                       juce::String (line) + ")"
                                                                 : juce::String()));
    }

    /** The window's close button, Ctrl+Q, or the desktop asking: offer to save first. */
    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
            mainWindow->getView().quitApp();
        else
            quit();
    }

private:
    /** Starts this run's session log (core::Logs) in the log folder, with a header describing the run. */
    static void startLog (const juce::String& commandLine)
    {
        core::Logs::Options log;
        log.folder = getLogFolder();
        log.header = session::describeRunForLog (log.startTime);
        if (! core::Logs::start (log))
            return;
        core::Logs::installTerminateHandler();
        core::Logs::info ("start", commandLine.trim().isNotEmpty() ? "Starting with: " + commandLine.trim()
                                                                   : juce::String ("Starting"));
    }

    /** Windows plugins: use the chosen Wine and yabridge, refresh the bridges, and scan them. */
    static void useWindowsPlugins (const plugins::WindowsPlugins& windows, plugins::PluginCatalog& catalog)
    {
        using Format = plugins::WindowsPlugins::Format;
        // Not set up (e.g. a Flatpak beside a native Spinus that uses the computer's Wine): the bridged plugins
        // cannot start, so they are not scanned; the Windows plugins window says what to do. Setting up adds
        // the folders (MainView::showWindowsPlugins).
        const auto runtime = windows.getRuntime();
        if (! runtime)
            return;
        plugins::WindowsPlugins::applyEnvironment (*runtime);
        windows.syncBridges (*runtime);
        catalog.addSearchFolder ("VST", windows.getBridgedFolder (Format::vst2));
        catalog.addSearchFolder ("VST3", windows.getBridgedFolder (Format::vst3));
        catalog.addSearchFolder ("CLAP", windows.getBridgedFolder (Format::clap));
    }

    /** Scans all plugin folders out of process and prints a summary (until the browser shows it). */
    static int scanPlugins()
    {
        const auto settings = getPluginListFolder();
        plugins::PluginCatalog catalog (settings, getProgramFolder());
        catalog.load();
#if ! JUCE_WINDOWS
        const plugins::WindowsPlugins windows (getWindowsPluginsFolder(), isFlatpak());
        useWindowsPlugins (windows, catalog);
#endif

        std::cout << "Scanning plugins (each in its own process)...\n";
        const auto summary = catalog.scan (
            [] (const plugins::PluginScanner::Progress& p)
            {
                if (p.currentFile.isNotEmpty())
                    std::cout << "  [" << p.done + 1 << "/" << p.total << "] " << p.currentFile << '\n';
            });

        std::cout << "\nScanned " << summary.scanned << " files, found " << summary.pluginsFound << " plugins; "
                  << summary.skippedUnchanged << " unchanged files skipped.\n"
                  << "Known plugins: " << catalog.getKnownPlugins().getNumTypes() << '\n';

        for (auto& type : catalog.getKnownPlugins().getTypes())
            std::cout << "  " << type.pluginFormatName << "  " << type.name << " (" << type.manufacturerName << ")\n";

        std::cout << "Problem plugins: " << catalog.getProblems().getAll().size() << '\n';
        for (auto& p : catalog.getProblems().getAll())
            std::cout << "  " << p.format << "  " << p.file << ": " << p.reason << '\n';

        std::cout << "Saved in " << settings.getFullPathName() << '\n';
        return 0;
    }

    /** The owner check of file dialogs (tools/owner-checks/file-dialog.sh): shows a small Spinus
        window and the file dialog Spinus uses (open, save or folder) on it, then prints which dialog it
        was and what it gave. For a save, makes the project folder there, as saving a project does, and
        removes it again. */
    void chooseFileCheck (const juce::String& mode)
    {
        using Browser = juce::FileBrowserComponent;
        const bool save = mode == "save", folder = mode == "folder";
        const auto flags = save     ? (int)((unsigned)Browser::saveMode | (unsigned)Browser::canSelectFiles)
                           : folder ? (int)((unsigned)Browser::openMode | (unsigned)Browser::canSelectDirectories)
                                    : (int)((unsigned)Browser::openMode | (unsigned)Browser::canSelectFiles |
                                            (unsigned)Browser::canSelectMultipleItems);
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

        checkWindow = std::make_unique<juce::DocumentWindow> ("Spinus file dialog check", juce::Colour (0xff1e2227),
                                                              juce::DocumentWindow::closeButton);
        auto* label = new juce::Label ({}, "Spinus is showing its file dialog. Choose something, or cancel.");
        label->setSize (520, 80);
        label->setJustificationType (juce::Justification::centred);
        checkWindow->setUsingNativeTitleBar (true);
        checkWindow->setContentOwned (label, true);
        checkWindow->centreWithSize (520, 80);
        checkWindow->setVisible (true);

        checkDialog =
            std::make_unique<core::FileDialog> (save     ? "Save a test project"
                                                : folder ? "Choose a folder"
                                                         : "Open one or more files",
                                                save ? home.getChildFile ("Spinus file dialog check") : home,
                                                save || folder ? "" : "*.wav;*.flac;*.spn", checkWindow.get());
        juce::Timer::callAfterDelay (500,
                                     [this, flags, save]
                                     {
                                         checkDialog->launchAsync (flags,
                                                                   [this, save] (const core::FileDialog& d)
                                                                   {
                                                                       printFileDialogCheck (d, save);
                                                                       checkWindow.reset();
                                                                       quit();
                                                                   });
                                     });
    }

    static void printFileDialogCheck (const core::FileDialog& d, bool save)
    {
        using Kind = core::FileDialog::Kind;
        std::cout << "Spinus " << versionString << (isFlatpak() ? " (Flatpak)" : "") << '\n';
        std::cout << "Dialog: "
                  << (d.getKind() == Kind::portal       ? "the desktop's file dialog (XDG Desktop Portal)"
                      : d.getKind() == Kind::ownBrowser ? "Spinus's own file browser"
                                                        : "the system's dialog")
                  << '\n';
        if (d.getFallbackReason().isNotEmpty())
            std::cout << "Why not the desktop's: " << d.getFallbackReason() << '\n';
        if (d.getResults().isEmpty())
            std::cout << "Chosen: nothing (cancelled)\n";
        const auto documents = core::portal::documentsRoot();
        for (auto& f : d.getResults())
        {
            std::cout << "Chosen: " << f.getFullPathName() << '\n';
            if (documents != juce::File() && f.isAChildOf (documents))
                std::cout << "  This is a document portal path (only this one file is reachable).\n";
            if (! save)
                std::cout << "  " << (f.isDirectory() ? "a folder" : f.existsAsFile() ? "a file" : "not found") << '\n';
        }
        if (save && d.getResult() != juce::File())
        {
            // As saving a project does: a folder named as chosen, holding <name>.spn.
            const auto folder = d.getResult().withFileExtension ({});
            const bool existed = folder.exists();
            const auto file = folder.getChildFile (folder.getFileName() + ".spn");
            const bool ok = folder.createDirectory().wasOk() && file.replaceWithText ("{}");
            std::cout << "Project folder " << folder.getFullPathName() << ": "
                      << (ok ? "could be written" : "could NOT be written") << '\n';
            if (! existed)
                folder.deleteRecursively();
            else if (ok)
                file.deleteFile();
        }
    }

    /** Renders the main window content to a PNG without showing a window.
        Used for screenshots of the manual and for checking UI changes. */
    bool writeSnapshot (const juce::String& path)
    {
        if (path.isEmpty())
        {
            std::cerr << "Usage: spinus --snapshot <file.png>" << '\n';
            return false;
        }

        ui::MainView content (*session);
        content.setSize (1280, 800);
        // SPINUS_SNAPSHOT_DEMO=1: the manual's demo song (ui/ManualDemo).
        if (juce::SystemStats::getEnvironmentVariable ("SPINUS_SNAPSHOT_DEMO", {}) == "1")
            ui::makeManualDemo (content, *session);

        if (juce::SystemStats::getEnvironmentVariable ("SPINUS_SNAPSHOT_DEMO", {}) != "1")
        {
            content.getWorkspace().showPanel ("mixer");
            content.getWorkspace().showPanel ("pianoRoll");
        }
        else
        {
            content.getWorkspace().showPanel ("channelRack");
            content.getWorkspace().showPanel ("pianoRoll");
            if (juce::SystemStats::getEnvironmentVariable ("SPINUS_SNAPSHOT_PANEL", {}).isNotEmpty())
                content.getWorkspace().showPanel (
                    juce::SystemStats::getEnvironmentVariable ("SPINUS_SNAPSHOT_PANEL", {}));
        }
        content.getCommandManager().commandStatusChanged();
        // Command buttons update asynchronously; let pending updates run before capturing.
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        auto image = content.createComponentSnapshot (content.getLocalBounds(), true, 1.0f);

        juce::File file (juce::File::getCurrentWorkingDirectory().getChildFile (path));
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat png;

        if (! out.openedOk() || ! png.writeImageToStream (image, out))
        {
            std::cerr << "Could not write " << file.getFullPathName() << '\n';
            return false;
        }

        std::cout << "Wrote " << file.getFullPathName() << '\n';
        return true;
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, Session& session, ProjectDocument& document)
            : DocumentWindow (name, juce::Colour (0xff1e2227), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new ui::MainView (session, &document), true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        ~MainWindow() override { clearContentComponent(); }

        ui::MainView& getView() { return *static_cast<ui::MainView*> (getContentComponent()); }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<plugins::PluginDownloads> downloads;
    std::unique_ptr<plugins::WindowsPlugins> windows;
    std::unique_ptr<plugins::PluginDialogs> pluginDialogs;
    std::unique_ptr<ai::AiService> aiService;
    std::unique_ptr<EngineHost> engineHost;
    std::unique_ptr<session::AudioDeviceLog> audioLog;
    std::unique_ptr<plugins::PluginCatalog> catalog;
    std::unique_ptr<plugins::SandboxList> sandboxList;
    std::unique_ptr<Session> session;
    std::unique_ptr<ProjectDocument> document;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<juce::DocumentWindow> checkWindow; // --choose-file
    std::unique_ptr<core::FileDialog> checkDialog;
    std::unique_ptr<ui::WindowIcons> windowIcons;
};

} // namespace spinus

START_JUCE_APPLICATION (spinus::SpinusApplication)
