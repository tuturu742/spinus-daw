#pragma once

#include "plugins/PluginDownloads.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <optional>

namespace spinus::plugins
{

/** Windows plugins through Wine and yabridge, set up and managed by Spinus.

    Everything lives in one folder (normally ~/.local/share/Spinus DAW/windows):
      prefix/          Spinus's Windows folder (a Wine prefix); installers run here
      bridged/         yabridge's Linux side of each Windows plugin, scanned like any plugin folder
      runtime/         Spinus's own Wine and yabridge, if downloaded
      settings.json    which runtime is used and the last test's result

    The long operations (download, creating the prefix, installers, the test) block: run them on a
    background thread. Each takes an optional cancel flag. */
class WindowsPlugins
{
public:
    /** Wine plus yabridge. */
    struct Runtime
    {
        enum class Kind
        {
            system, // installed on this computer (e.g. Arch's wine-staging and yabridge packages)
            spinus  // downloaded by Spinus into runtime/
        };

        Kind kind = Kind::system;
        juce::File wine;     // the wine binary
        juce::File yabridge; // the folder with libyabridge-chainloader-*.so
        juce::File host;     // yabridge-host.exe

        juce::String describe() const;
        bool operator== (const Runtime&) const = default;
    };

    /** The last test, saved in settings.json. */
    struct TestResult
    {
        bool passed = false;
        Runtime::Kind kind = Runtime::Kind::system;
        juce::String wineVersion; // e.g. "wine-11.19 (Staging)"
        juce::String message;     // why it failed, in plain words
        juce::Time when;
    };

    enum class Format
    {
        vst2,
        vst3,
        clap
    };

    /** A Windows plugin found in the prefix. */
    struct Found
    {
        Format format = Format::clap;
        juce::File file; // the .dll, the .vst3 bundle or file, or the .clap
        juce::String name;
        bool operator== (const Found&) const = default;
    };

    /** `sandboxed`: Spinus runs as a Flatpak. The Windows folder is shared with a native Spinus then, but
        the Flatpak cannot use the Wine installed on the computer: it keeps its own runtime choice and test
        (Spinus's tested Wine only). */
    explicit WindowsPlugins (juce::File folder, bool sandboxed = false);

    bool isSandboxed() const noexcept { return sandboxed; }

    juce::File getFolder() const { return folder; }
    juce::File getPrefix() const { return folder.getChildFile ("prefix"); }
    juce::File getBridgedFolder() const { return folder.getChildFile ("bridged"); }
    juce::File getBridgedFolder (Format) const;
    bool isPrefixReady() const;

    //==============================================================================
    // Runtimes

    /** Wine and yabridge installed on this computer, if both are there. Folders inside `ignore` (Spinus's
        own runtime, which Spinus may have put on PATH) do not count. `searchPath`: PATH as Spinus
        started (empty), or another one (tests). */
    static std::optional<Runtime> findSystemRuntime (const juce::File& ignore = {},
                                                     const juce::String& searchPath = {});
    /** Spinus's own runtime, if downloaded. Its Wine is the 64-bit wine64 (the 32-bit `wine` starter needs
        32-bit system libraries, which a Flatpak has only with its Compat.i386 extension); wine64 hands
        32-bit programs to `wine` itself. */
    std::optional<Runtime> findSpinusRuntime() const;
    /** The Wine and yabridge on this computer as this Spinus can use them: none in the Flatpak. */
    std::optional<Runtime> findUsableSystemRuntime() const;

    /** Wine Staging 9.21 and yabridge 5.1.1, pinned; and the Windows test plugin. */
    static const DownloadPack& getRuntimePack();
    static const DownloadPack& getTestPluginPack();
    PluginDownloads getDownloads() const { return PluginDownloads (folder); }

    /** "wine-11.19 (Staging)", or empty if it does not run. */
    static juce::String getWineVersion (const Runtime&);

    /** "5.1.1" (yabridgectl's), or empty if unknown. Spinus's own runtime has the pinned one. */
    static juce::String getYabridgeVersion (const Runtime&);
    /** A problem this Wine and yabridge are known to have, in plain words, or empty. yabridge 5.1.1 and
        earlier with Wine 9.22 or later: once a plugin's window is moved, the plugin is told it is at the
        top-left corner of the screen, so clicks land in the wrong place (wine bug 57603). */
    static juce::String getKnownProblem (const juce::String& wineVersion, const juce::String& yabridgeVersion);

    /** Makes this process (and the scanner and plugins it starts) use the runtime: Spinus's own runtime
        goes first on PATH and becomes WINELOADER; the system's leaves the environment as Spinus started. */
    static void applyEnvironment (const Runtime&);

    //==============================================================================
    // Settings

    std::optional<Runtime::Kind> getChosenRuntime() const;
    void setChosenRuntime (std::optional<Runtime::Kind>);
    std::optional<TestResult> getLastTest() const;

    /** The runtime to use now: the chosen one if it is still there, else none. */
    std::optional<Runtime> getRuntime() const;

    /** True when the system's Wine changed since its last passed test (so it must be tested again). */
    bool needsRetest() const;

    /** Plugin windows drawn without the graphics card: Wine's Direct3D off in the prefix, so plugins that
        draw with Direct2D or Direct3D (e.g. those made with JUCE 8) fall back to plain drawing, the way
        older plugins draw. For windows that do not update on some graphics drivers. Applies to plugin
        windows opened afterwards; some plugins may then show no picture at all. */
    bool isDrawingWithoutGraphicsCard() const;
    juce::Result setDrawingWithoutGraphicsCard (const Runtime&, bool) const;

    //==============================================================================
    // Long operations (background thread)

    using Progress = std::function<void (const juce::String& stage)>;

    /** Stops everything running in the prefix (Wine's server and the plugins' Windows side), whichever
        Wine started it: a Wine of another version cannot use the prefix while it runs. Windows plugins
        loaded in Spinus stop working until they are loaded again. */
    void stopWine() const;

    /** Creates the prefix if needed (a minute or less, sometimes a few), and applies Spinus's registry
        settings (getPrefixSettingsCommands) once, to existing prefixes too, and again when they change. */
    juce::Result createPrefix (const Runtime&, const std::atomic<bool>* cancel = nullptr) const;

    /** The Wine `reg` commands (arguments after wine) that Spinus's prefix settings are made of: Wine's
        own Arial (from this runtime) as the font for text in fonts the prefix doesn't have, never a symbol
        font an installer added. */
    static std::vector<juce::StringArray> getPrefixSettingsCommands (const Runtime&);

    /** Runs a Windows installer (.exe or .msi) in the prefix and waits until it and the programs it
        started have finished; Wine's own programs and the Windows side of plugins open in Spinus
        (yabridge's hosts) do not count. In the Flatpak, a 32-bit installer is refused with what to install
        when Flatpak's 32-bit support (org.freedesktop.Platform.Compat.i386) is missing. */
    juce::Result runInstaller (const Runtime&, const juce::File& installer, const std::atomic<bool>* cancel = nullptr,
                               const Progress& = {}) const;

    /** The programs in Wine's `tasklist /fo csv /nh` output that are neither Wine's own (services,
        explorer, winedevice...) nor yabridge's plugin hosts: what an installer left running. */
    static juce::StringArray findInstallerPrograms (const juce::String& tasklistCsv);

    /** Copies a Windows plugin file (.dll, .vst3 file or bundle, .clap) into the prefix's plugin folders. */
    juce::Result addPluginFile (const juce::File&) const;

    /** Tests the runtime with the Windows test plugin (downloaded if needed) through Spinus's scanner, and
        saves the result. `stopRunningWine`: stop what runs in the prefix first (needed when it may be
        another Wine version; not when retesting the Wine in use, which would stop open plugins). */
    TestResult test (const Runtime&, const juce::File& scannerExecutable, const Progress& = {},
                     const std::atomic<bool>* cancel = nullptr, bool stopRunningWine = true);

    //==============================================================================
    // Plugins

    /** 64-bit Windows plugins in the prefix's usual plugin folders. */
    std::vector<Found> findWindowsPlugins() const;

    /** The files in bridged/ that stand for this plugin (what Spinus's plugin list knows it by: the VST2
        .so, the VST3 bundle, the CLAP .clap), whichever runtime made them. */
    juce::Array<juce::File> findBridgedFiles (const Found&) const;

    /** Deletes a Windows plugin: its file in the prefix (.dll, .vst3 bundle or .clap) and its entry in
        bridged/. Its sounds and presets elsewhere stay (the plugin's own uninstaller removes those).
        `removedBridges` gets the files the plugin list knew it by. */
    juce::Result removePlugin (const Found&, juce::Array<juce::File>* removedBridges = nullptr) const;

    /** Makes bridged/ match the prefix's plugins: one yabridge entry per plugin (chainloader copy plus a
        link to the Windows file), stale ones removed. Returns the number of bridged plugins. */
    int syncBridges (const Runtime&) const;

    /** True if the file is a 64-bit Windows DLL (and, for VST2, exports VSTPluginMain or main). */
    static bool isWindows64 (const juce::File&);
    static bool exportsVst2Entry (const juce::File&);
    /** True if the file is a 32-bit (x86) Windows program or library. */
    static bool isWindows32 (const juce::File&);

    /** Why `installer` cannot run in the Flatpak, in plain words with what to do, or empty. A 32-bit
        Windows program needs Wine's 32-bit starter, which runs only with Flatpak's 32-bit Linux libraries
        (the org.freedesktop.Platform.Compat.i386 extension, of the runtime's `runtimeBranch`, e.g. "25.08");
        `loader32` is their loader, /lib/ld-linux.so.2 in the sandbox, there only when they are installed. */
    static juce::String getFlatpak32BitProblem (const juce::File& installer, const juce::File& loader32,
                                                const juce::String& runtimeBranch);
    /** The command that installs Flatpak's 32-bit support for this runtime branch (empty: the running
        Flatpak's, read from /.flatpak-info). */
    static juce::String getFlatpak32BitCommand (const juce::String& runtimeBranch = {});
    /** Why this installer cannot run in this Spinus (getFlatpak32BitProblem in the Flatpak), or empty. */
    juce::String getInstallerProblem (const juce::File& installer) const;
    /** Tests: another file stands for the sandbox's 32-bit loader. */
    void set32BitLoaderForTests (const juce::File& loader) { loader32 = loader; }

    //==============================================================================
    // Real-time privileges (yabridge works best with an unlimited locked-memory limit)

    struct Realtime
    {
        bool memlockOk = false;     // the locked-memory limit is unlimited or large
        juce::int64 memlockKiB = 0; // -1: unlimited
        bool inGroup = false;       // this session is in the "realtime" group
        bool addedToGroup = false;  // the user is in /etc/group's "realtime" (but this session is not yet)
        bool groupExists = false;   // the realtime-privileges package is installed
        juce::String advice;        // what to do, in plain words; empty when all is well
    };
    static Realtime checkRealtime();

    /** The command line that runs `args` with Wine in the prefix, with the runtime's environment. */
    juce::StringArray wineCommand (const Runtime&, const juce::StringArray& args) const;
    /** The same for Wine's server (e.g. "-w": wait until everything in the prefix has finished). */
    juce::StringArray wineserverCommand (const Runtime&, const juce::StringArray& args) const;

private:
    juce::Result run (const juce::StringArray& command, int timeoutMs, const std::atomic<bool>* cancel,
                      juce::String* output = nullptr) const;
    // What the public functions of the same name do; those note it in the session log.
    juce::Result createPrefixUnlogged (const Runtime&, const std::atomic<bool>* cancel) const;
    juce::Result runInstallerUnlogged (const Runtime&, const juce::File& installer, const std::atomic<bool>* cancel,
                                       const Progress&) const;
    juce::Result addPluginFileUnlogged (const juce::File&) const;
    juce::Result removePluginUnlogged (const Found&, juce::Array<juce::File>* removedBridges) const;
    juce::var loadSettings() const;
    void saveSettings (const juce::var&) const;

    /** A settings.json key: the Flatpak keeps its own runtime choice and test. */
    juce::String key (const char* name) const;

    juce::File folder;
    bool sandboxed = false;
    juce::File loader32 { "/lib/ld-linux.so.2" }; // in the sandbox: there with Flatpak's 32-bit support
};

} // namespace spinus::plugins
