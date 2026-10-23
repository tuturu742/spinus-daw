#include "WindowsPlugins.h"
#include "core/ChildProcess.h"

#include "plugins/PluginScanner.h"

#include <map>
#include <set>
#include <vector>

// Wine and yabridge exist only on Linux; on Windows, Windows plugins are native and Spinus never creates
// a WindowsPlugins (this file still compiles there, so the dialog and its callers need no #ifs).
#if ! JUCE_WINDOWS
#include <grp.h>
#include <pwd.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace spinus::plugins
{

namespace
{
// The plugin folders installers use inside drive_c (matched ignoring case, as Windows does).
const char* const vst3Folder = "Program Files/Common Files/VST3";
const char* const clapFolder = "Program Files/Common Files/CLAP";
const char* const vst2Folders[] = { "Program Files/VstPlugins",
                                    "Program Files/Steinberg/VstPlugins",
                                    "Program Files/Common Files/VST2",
                                    "Program Files/Common Files/Steinberg/VST2",
                                    "Program Files/VST2",
                                    "Program Files/VST" };

const char* chainloaderFor (WindowsPlugins::Format f)
{
    switch (f)
    {
    case WindowsPlugins::Format::vst2:
        return "libyabridge-chainloader-vst2.so";
    case WindowsPlugins::Format::vst3:
        return "libyabridge-chainloader-vst3.so";
    case WindowsPlugins::Format::clap:
        break;
    }
    return "libyabridge-chainloader-clap.so";
}

/** The child of `dir` named `name`, ignoring case (the first match), or the exact path if none exists. */
juce::File childIgnoringCase (const juce::File& dir, const juce::String& name)
{
    if (dir.isDirectory())
        for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFilesAndDirectories))
            if (entry.getFile().getFileName().equalsIgnoreCase (name))
                return entry.getFile();
    return dir.getChildFile (name);
}

/** A path inside drive_c, ignoring case at each step. */
juce::File insideDriveC (const juce::File& prefix, const juce::String& path)
{
    auto file = prefix.getChildFile ("drive_c");
    for (auto& part : juce::StringArray::fromTokens (path, "/", ""))
        file = childIgnoringCase (file, part);
    return file;
}

/** The original PATH and WINELOADER, before applyEnvironment() changed them. */
struct OriginalEnvironment
{
    juce::String path = juce::SystemStats::getEnvironmentVariable ("PATH", "/usr/local/bin:/usr/bin:/bin");
    std::optional<juce::String> wineloader = []() -> std::optional<juce::String>
    {
        if (const char* v = getenv ("WINELOADER")) // NOLINT(concurrency-mt-unsafe)
            return juce::String (v);
        return std::nullopt;
    }();
    std::optional<juce::String> dllOverrides = []() -> std::optional<juce::String>
    {
        if (const char* v = getenv ("WINEDLLOVERRIDES")) // NOLINT(concurrency-mt-unsafe)
            return juce::String (v);
        return std::nullopt;
    }();
};

// No Mono or Gecko install prompts (they block a plugin), and no menu entries: Windows installers in Spinus's
// own prefix would otherwise add their programs to the desktop's application menu.
constexpr const char* wineDllOverrides = "mscoree,mshtml=;winemenubuilder.exe=d";

const OriginalEnvironment& original()
{
    static const OriginalEnvironment env;
    return env;
}

juce::String pathFor (const WindowsPlugins::Runtime& r)
{
    if (r.kind == WindowsPlugins::Runtime::Kind::spinus)
        return r.yabridge.getFullPathName() + ":" + r.wine.getParentDirectory().getFullPathName() + ":" +
               original().path;
    return original().path;
}

bool copyIfDifferent (const juce::File& source, const juce::File& target)
{
    if (target.existsAsFile() && target.hasIdenticalContentTo (source))
        return true;
    target.getParentDirectory().createDirectory();
    if (! source.copyFileTo (target))
        return false;
    target.setExecutePermission (true);
    return true;
}

bool linkTo (const juce::File& target, const juce::File& link)
{
    if (link.isSymbolicLink() && link.getLinkedTarget() == target)
        return true;
    link.getParentDirectory().createDirectory();
    if (link.exists() || link.isSymbolicLink())
        link.deleteRecursively (false);
    return target.createSymbolicLink (link, true);
}

/** The Flatpak runtime's branch ("25.08"), from /.flatpak-info; the manifest's if it cannot be read. */
juce::String flatpakRuntimeBranch()
{
    juce::StringArray lines;
    lines.addLines (juce::File ("/.flatpak-info").loadFileAsString());
    for (auto& line : lines)
        if (line.startsWith ("runtime=runtime/"))
            return line.fromLastOccurrenceOf ("/", false, false).trim();
    return "25.08";
}

//==============================================================================
// Just enough of the PE format to tell 64-bit DLLs and VST2 entry points apart.
struct PeInfo
{
    uint16_t machine = 0;
    std::vector<juce::String> exports;
};

std::optional<PeInfo> readPe (const juce::File& file, bool wantExports)
{
    juce::FileInputStream in (file);
    if (! in.openedOk() || in.getTotalLength() < 0x40)
        return std::nullopt;

    auto u16 = [&in] (juce::int64 at) { return in.setPosition (at) ? (uint16_t)in.readShort() : (uint16_t)0; };
    auto u32 = [&in] (juce::int64 at) { return in.setPosition (at) ? (uint32_t)in.readInt() : 0u; };

    if (u16 (0) != 0x5a4d) // "MZ"
        return std::nullopt;
    const auto pe = (juce::int64)u32 (0x3c);
    if (pe <= 0 || pe > in.getTotalLength() - 24 || u32 (pe) != 0x00004550) // "PE\0\0"
        return std::nullopt;

    PeInfo info;
    info.machine = u16 (pe + 4);
    if (! wantExports)
        return info;

    const auto sections = u16 (pe + 6);
    const auto optionalSize = u16 (pe + 20);
    const auto optional = pe + 24;
    if (u16 (optional) != 0x20b) // PE32+ only (64-bit)
        return info;

    const auto exportRva = u32 (optional + 112);
    if (exportRva == 0)
        return info;

    struct Section
    {
        uint32_t address, size, offset;
    };
    std::vector<Section> table;
    for (int i = 0; i < juce::jmin<int> (sections, 96); ++i)
    {
        const auto at = optional + optionalSize + (juce::int64)i * 40;
        table.push_back ({ u32 (at + 12), juce::jmax (u32 (at + 8), u32 (at + 16)), u32 (at + 20) });
    }
    auto offsetOf = [&table] (uint32_t rva) -> juce::int64
    {
        for (auto& s : table)
            if (rva >= s.address && rva < s.address + s.size)
                return (juce::int64)s.offset + (rva - s.address);
        return -1;
    };

    const auto directory = offsetOf (exportRva);
    if (directory < 0)
        return info;
    const auto count = juce::jmin<uint32_t> (u32 (directory + 24), 100000);
    const auto names = offsetOf (u32 (directory + 32));
    for (uint32_t i = 0; names >= 0 && i < count; ++i)
    {
        const auto at = offsetOf (u32 (names + (juce::int64)i * 4));
        if (at < 0 || ! in.setPosition (at))
            continue;
        juce::String name;
        for (int c = 0; c < 64; ++c)
        {
            const auto ch = in.readByte();
            if (ch == 0)
                break;
            name += (juce::juce_wchar)(unsigned char)ch;
        }
        info.exports.push_back (name);
    }
    return info;
}

/** The Windows module of a VST3 plugin: the file itself, or the 64-bit module inside a bundle. */
juce::File vst3Module (const juce::File& plugin)
{
    if (plugin.isDirectory())
        return plugin.getChildFile ("Contents/x86_64-win").getChildFile (plugin.getFileName());
    return plugin;
}

juce::String nameOf (const WindowsPlugins::Found& f) { return f.file.getFileNameWithoutExtension(); }
} // namespace

//==============================================================================
juce::String WindowsPlugins::Runtime::describe() const
{
    return kind == Kind::spinus ? "Spinus's tested Wine and yabridge" : "the Wine and yabridge on this computer";
}

WindowsPlugins::WindowsPlugins (juce::File f, bool inSandbox) : folder (std::move (f)), sandboxed (inSandbox) {}

juce::String WindowsPlugins::key (const char* name) const
{
    return sandboxed
               ? "flatpak" + juce::String (name).substring (0, 1).toUpperCase() + juce::String (name).substring (1)
               : juce::String (name);
}

juce::File WindowsPlugins::getBridgedFolder (Format f) const
{
    switch (f)
    {
    case Format::vst2:
        return getBridgedFolder().getChildFile ("vst2");
    case Format::vst3:
        return getBridgedFolder().getChildFile ("vst3");
    case Format::clap:
        break;
    }
    return getBridgedFolder().getChildFile ("clap");
}

bool WindowsPlugins::isPrefixReady() const
{
    return getPrefix().getChildFile ("drive_c").isDirectory() && getPrefix().getChildFile ("system.reg").existsAsFile();
}

//==============================================================================
std::optional<WindowsPlugins::Runtime> WindowsPlugins::findSystemRuntime (const juce::File& ignore,
                                                                          const juce::String& searchPath)
{
    auto pathFolders = juce::StringArray::fromTokens (searchPath.isNotEmpty() ? searchPath : original().path, ":", "");
    auto onPath = [&] (const juce::String& name) -> juce::File
    {
        for (auto& dir : pathFolders)
        {
            const juce::File d (dir);
            if (dir.isEmpty() || (ignore != juce::File() && (d == ignore || d.isAChildOf (ignore))))
                continue;
#if JUCE_WINDOWS
            if (auto f = d.getChildFile (name); f.existsAsFile())
                return f;
#else
            if (auto f = d.getChildFile (name); f.existsAsFile() && access (f.getFullPathName().toRawUTF8(), X_OK) == 0)
                return f;
#endif
        }
        return {};
    };

    Runtime r;
    r.kind = Runtime::Kind::system;
    r.wine = onPath ("wine");
    if (r.wine == juce::File())
        return std::nullopt;

    // Where yabridge's chainloaders look for its libraries: the user's own copy, then the
    // system's library folders.
    const auto xdg = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});
    const juce::File userCopy =
        (xdg.isNotEmpty() ? juce::File (xdg) : juce::File ("~/.local/share")).getChildFile ("yabridge");
    for (const auto& dir :
         { userCopy, juce::File ("/usr/lib"), juce::File ("/usr/lib64"), juce::File ("/usr/local/lib"),
           juce::File ("/usr/lib/x86_64-linux-gnu"), juce::File ("/usr/local/lib64") })
        if (dir.getChildFile (chainloaderFor (Format::clap)).existsAsFile() &&
            dir.getChildFile (chainloaderFor (Format::vst2)).existsAsFile() &&
            dir.getChildFile (chainloaderFor (Format::vst3)).existsAsFile())
        {
            r.yabridge = dir;
            break;
        }
    if (r.yabridge == juce::File())
        return std::nullopt;

    r.host = r.yabridge.getChildFile ("yabridge-host.exe").existsAsFile()
                 ? r.yabridge.getChildFile ("yabridge-host.exe")
                 : onPath ("yabridge-host.exe");
    if (r.host == juce::File())
        return std::nullopt;
    return r;
}

std::optional<WindowsPlugins::Runtime> WindowsPlugins::findSpinusRuntime() const
{
    const auto& pack = getRuntimePack();
    const auto dir = getDownloads().getPackFolder (pack);
    if (! getDownloads().isInstalled (pack))
        return std::nullopt;
    Runtime r;
    r.kind = Runtime::Kind::spinus;
    // wine64: `wine` is a 32-bit starter that needs 32-bit system libraries (not in a Flatpak); yabridge's
    // 64-bit host runs with wine64 directly.
    r.wine = dir.getChildFile ("wine/bin/wine64").existsAsFile() ? dir.getChildFile ("wine/bin/wine64")
                                                                 : dir.getChildFile ("wine/bin/wine");
    r.yabridge = dir.getChildFile ("yabridge");
    r.host = r.yabridge.getChildFile ("yabridge-host.exe");
    return r;
}

std::optional<WindowsPlugins::Runtime> WindowsPlugins::findUsableSystemRuntime() const
{
    // The Flatpak sees only its own /usr: a Wine of the computer is not there (and could not run inside).
    if (sandboxed)
        return std::nullopt;
    return findSystemRuntime (folder);
}

const DownloadPack& WindowsPlugins::getRuntimePack()
{
    // Pinned: the pair yabridge 5.1.1 is documented to work with. The hashes are of the files
    // on the projects' GitHub release pages.
    static const DownloadPack pack {
        "runtime",
        "Wine and yabridge",
        "wine-9.21-staging+yabridge-5.1.1",
        "Runs Windows plugins on Linux: Wine Staging 9.21 (portable build) and yabridge 5.1.1",
        "LGPL-2.1-or-later (Wine), GPL-3.0-or-later (yabridge)",
        "https://www.winehq.org, https://github.com/robbert-vdh/yabridge",
        {
            { "https://github.com/Kron4ek/Wine-Builds/releases/download/9.21/wine-9.21-staging-amd64.tar.xz",
              "bf94c1f3271e6e8d7faf259ecaf3043ba01986191473350b4e4d3c3e344809ce",
              61870072,
              { { "wine-9.21-staging-amd64/", "wine" } } },
            { "https://github.com/robbert-vdh/yabridge/releases/download/5.1.1/yabridge-5.1.1.tar.gz",
              "d11f0307412b5566b09a75ed4aa191266db56d22f4de4d20e4fbde6296cd1a47",
              4351433,
              { { "yabridge/", "yabridge" } } },
        },
        { "wine/bin/wine", "wine/bin/wineserver", "yabridge/yabridge-host.exe", "yabridge/libyabridge-clap.so",
          "yabridge/libyabridge-chainloader-clap.so", "yabridge/libyabridge-chainloader-vst2.so",
          "yabridge/libyabridge-chainloader-vst3.so" },
    };
    return pack;
}

const DownloadPack& WindowsPlugins::getTestPluginPack()
{
    static const DownloadPack pack {
        "test-plugin",
        "Windows test plugin",
        "dragonfly-reverb-3.2.10-win64",
        "The Windows build of Dragonfly Room Reverb, used to test that Windows plugins work",
        "GPL-3.0-or-later",
        "https://michaelwillis.github.io/dragonfly-reverb/",
        { { "https://github.com/michaelwillis/dragonfly-reverb/releases/download/3.2.10/"
            "dragonfly-reverb-3.2.10-win64.zip",
            "ca5f35f9dcd9a33f06490d3c92f04061a1a798cb81257d5ca5851bdda0824c66",
            22447365,
            { { "dragonfly-reverb-3.2.10/DragonflyRoomReverb.clap", "DragonflyRoomReverb.clap" } } } },
        { "DragonflyRoomReverb.clap" },
    };
    return pack;
}

juce::String WindowsPlugins::getWineVersion (const Runtime& r)
{
    core::ChildProcess process;
    if (! process.start (juce::StringArray { "env", "PATH=" + pathFor (r), r.wine.getFullPathName(), "--version" },
                         core::ChildProcess::wantStdOut))
        return {};
    const auto text = process.readAllProcessOutput().trim();
    process.waitForProcessToFinish (10000);
    return text.startsWithIgnoreCase ("wine") ? text.upToFirstOccurrenceOf ("\n", false, false) : juce::String();
}

juce::String WindowsPlugins::getYabridgeVersion (const Runtime& r)
{
    if (r.kind == Runtime::Kind::spinus)
        return "5.1.1";
    core::ChildProcess process;
    if (! process.start (juce::StringArray { "env", "PATH=" + pathFor (r), "yabridgectl", "--version" },
                         core::ChildProcess::wantStdOut))
        return {};
    const auto text = process.readAllProcessOutput().trim();
    process.waitForProcessToFinish (10000);
    return text.startsWith ("yabridgectl ") ? text.fromFirstOccurrenceOf (" ", false, false).trim() : juce::String();
}

juce::String WindowsPlugins::getKnownProblem (const juce::String& wineVersion, const juce::String& yabridgeVersion)
{
    // "wine-11.20 (Staging)" -> {11, 20}; "5.1.1" -> {5, 1, 1}.
    auto numbers = [] (const juce::String& text)
    {
        const auto digits = text.fromFirstOccurrenceOf ("-", false, false).upToFirstOccurrenceOf (" ", false, false);
        std::vector<int> parts;
        for (const auto& p : juce::StringArray::fromTokens (digits.isEmpty() ? text : digits, ".", {}))
            parts.push_back (p.getIntValue());
        parts.resize (3, 0);
        return parts;
    };
    if (! wineVersion.startsWithIgnoreCase ("wine"))
        return {};
    const auto wine = numbers (wineVersion);
    if (wine < std::vector<int> { 9, 22, 0 })
        return {};
    // Unknown yabridge: as the released ones (5.1.1 is the latest release without the fix).
    if (yabridgeVersion.isNotEmpty() && numbers (yabridgeVersion) > std::vector<int> { 5, 1, 1 })
        return {};
    return "With " + wineVersion.upToFirstOccurrenceOf (" ", false, false) + " and yabridge " +
           (yabridgeVersion.isNotEmpty() ? yabridgeVersion : juce::String ("5.1.1 or older")) +
           ", clicks land in the wrong place once you move a plugin's window (a known yabridge problem with "
           "Wine 9.22 and later). Spinus's tested Wine (9.21) does not have it.";
}

void WindowsPlugins::applyEnvironment (const Runtime& r)
{
#if JUCE_WINDOWS
    juce::ignoreUnused (r);
#else
    // NOLINTBEGIN(concurrency-mt-unsafe): message thread, before plugins and the scanner start.
    const auto& o = original();
    setenv ("PATH", pathFor (r).toRawUTF8(), 1);
    // No .NET or Internet Explorer install questions when a plugin starts Wine (they block the plugin);
    // a choice of the user's own stays.
    if (! o.dllOverrides)
        setenv ("WINEDLLOVERRIDES", wineDllOverrides, 1);
    if (r.kind == Runtime::Kind::spinus)
        setenv ("WINELOADER", r.wine.getFullPathName().toRawUTF8(), 1);
    else if (o.wineloader)
        setenv ("WINELOADER", o.wineloader->toRawUTF8(), 1);
    else
        unsetenv ("WINELOADER");
    // NOLINTEND(concurrency-mt-unsafe)
#endif
}

juce::StringArray WindowsPlugins::wineCommand (const Runtime& r, const juce::StringArray& args) const
{
    juce::StringArray command { "env",
                                "WINEPREFIX=" + getPrefix().getFullPathName(),
                                "WINEDEBUG=-all",
                                "WINEDLLOVERRIDES=" + juce::String (wineDllOverrides),
                                "PATH=" + pathFor (r),
                                "WINELOADER=" + r.wine.getFullPathName(),
                                r.wine.getFullPathName() };
    command.addArray (args);
    return command;
}

juce::StringArray WindowsPlugins::wineserverCommand (const Runtime& r, const juce::StringArray& args) const
{
    auto command = wineCommand (r, args);
    command.set (command.indexOf (r.wine.getFullPathName(), false, 1),
                 r.wine.getSiblingFile ("wineserver").getFullPathName());
    return command;
}

//==============================================================================
juce::var WindowsPlugins::loadSettings() const
{
    auto v = juce::JSON::parse (folder.getChildFile ("settings.json"));
    return v.isObject() ? v : juce::var (new juce::DynamicObject());
}

void WindowsPlugins::saveSettings (const juce::var& v) const
{
    folder.createDirectory();
    folder.getChildFile ("settings.json").replaceWithText (juce::JSON::toString (v));
}

std::optional<WindowsPlugins::Runtime::Kind> WindowsPlugins::getChosenRuntime() const
{
    const auto text = loadSettings()[key ("runtime").toRawUTF8()].toString();
    if (text == "spinus")
        return Runtime::Kind::spinus;
    if (text == "system" && ! sandboxed)
        return Runtime::Kind::system;
    // The Flatpak uses Spinus's tested Wine once it is downloaded (by either Spinus): it is the only one it
    // can run.
    if (sandboxed && findSpinusRuntime())
        return Runtime::Kind::spinus;
    return std::nullopt;
}

void WindowsPlugins::setChosenRuntime (std::optional<Runtime::Kind> kind)
{
    auto settings = loadSettings();
    settings.getDynamicObject()->setProperty (key ("runtime"), ! kind                           ? juce::var()
                                                               : *kind == Runtime::Kind::spinus ? juce::var ("spinus")
                                                                                                : juce::var ("system"));
    saveSettings (settings);
}

std::optional<WindowsPlugins::TestResult> WindowsPlugins::getLastTest() const
{
    const auto settings = loadSettings();
    auto t = settings[key ("test").toRawUTF8()];
    // The Flatpak has not tested yet: a native Spinus's test of the same (Spinus's) Wine counts.
    if (sandboxed && ! t.isObject() && settings["test"]["runtime"].toString() == "spinus")
        t = settings["test"];
    if (! t.isObject())
        return std::nullopt;
    TestResult r;
    r.passed = (bool)t["passed"];
    r.kind = t["runtime"].toString() == "spinus" ? Runtime::Kind::spinus : Runtime::Kind::system;
    r.wineVersion = t["wine"].toString();
    r.message = t["message"].toString();
    r.when = juce::Time::fromISO8601 (t["when"].toString());
    return r;
}

std::optional<WindowsPlugins::Runtime> WindowsPlugins::getRuntime() const
{
    const auto kind = getChosenRuntime();
    if (! kind)
        return std::nullopt;
    return *kind == Runtime::Kind::spinus ? findSpinusRuntime() : findUsableSystemRuntime();
}

bool WindowsPlugins::needsRetest() const
{
    const auto runtime = getRuntime();
    const auto last = getLastTest();
    if (! runtime || ! last || ! last->passed || last->kind != runtime->kind)
        return true;
    return runtime->kind == Runtime::Kind::system && getWineVersion (*runtime) != last->wineVersion;
}

bool WindowsPlugins::isDrawingWithoutGraphicsCard() const { return (bool)loadSettings()["drawWithoutGraphicsCard"]; }

juce::Result WindowsPlugins::setDrawingWithoutGraphicsCard (const Runtime& r, bool on) const
{
    if (! isPrefixReady())
        return juce::Result::fail ("Set up Windows plugins first.");
    // Wine's Direct3D renderer "gdi" (no3d): Direct3D devices cannot be made, so Direct2D and Direct3D
    // drawing fails and plugins that can fall back draw with plain GDI.
    const auto args =
        on ? juce::StringArray { "reg", "add", "HKCU\\Software\\Wine\\Direct3D", "/v", "renderer", "/d", "gdi", "/f" }
           : juce::StringArray { "reg", "delete", "HKCU\\Software\\Wine\\Direct3D", "/v", "renderer", "/f" };
    juce::String output;
    if (auto result = run (wineCommand (r, args), 60000, nullptr, &output); result.failed())
        return result;
    // Deleting a value that is not there "fails" harmlessly; adding must work.
    if (on && ! output.containsIgnoreCase ("success"))
        return juce::Result::fail ("Wine could not change the setting: " + output.trim());
    auto settings = loadSettings();
    settings.getDynamicObject()->setProperty ("drawWithoutGraphicsCard", on);
    saveSettings (settings);
    return juce::Result::ok();
}

//==============================================================================
juce::Result WindowsPlugins::run (const juce::StringArray& command, int timeoutMs, const std::atomic<bool>* cancel,
                                  juce::String* output) const
{
    core::ChildProcess process;
    if (! process.start (command,
                         output != nullptr ? core::ChildProcess::wantStdOut | core::ChildProcess::wantStdErr : 0))
        return juce::Result::fail ("Could not start " + command[command.size() > 7 ? 7 : 0]);

    if (output != nullptr)
        *output = process.readAllProcessOutput();

    const auto start = juce::Time::getMillisecondCounter();
    while (process.isRunning())
    {
        if (cancel != nullptr && cancel->load())
        {
            process.kill();
            return juce::Result::fail ("Cancelled.");
        }
        if (timeoutMs > 0 && juce::Time::getMillisecondCounter() - start > (juce::uint32)timeoutMs)
        {
            process.kill();
            return juce::Result::fail ("Wine did not finish in time.");
        }
        juce::Thread::sleep (100);
    }
    return juce::Result::ok();
}

void WindowsPlugins::stopWine() const
{
    // Only a wineserver of the same version can stop a running one: try each runtime there is.
    std::vector<Runtime> runtimes;
    if (auto own = findSpinusRuntime())
        runtimes.push_back (*own);
    if (auto system = findUsableSystemRuntime())
        runtimes.push_back (*system);
    for (auto& r : runtimes)
    {
        [[maybe_unused]] const auto killed = run (wineserverCommand (r, { "-k" }), 20000, nullptr);
        [[maybe_unused]] const auto waited = run (wineserverCommand (r, { "-w" }), 20000, nullptr);
    }
}

juce::Result WindowsPlugins::createPrefix (const Runtime& r, const std::atomic<bool>* cancel) const
{
    // The Wine that last made or updated the prefix. Another Wine updates it on its first start, and asks
    // to install .NET and Internet Explorer support then, with a window that blocks every Windows program
    // (and plugin) until it is answered: Spinus updates it first, without those questions.
    const auto marker = getPrefix().getChildFile (".spinus-wine");
    const auto version = getWineVersion (r);
    const bool ready = isPrefixReady();
    if (! ready || marker.loadFileAsString().trim() != version)
    {
        getPrefix().getParentDirectory().createDirectory();
        if (auto result = run (wineCommand (r, { "wineboot", ready ? "-u" : "-i" }), 10 * 60 * 1000, cancel);
            result.failed())
            return result;
        // The registry files are written when Wine's server exits: wait for that.
        if (auto result = run (wineserverCommand (r, { "-w" }), 2 * 60 * 1000, cancel); result.failed())
            return result;
        if (! isPrefixReady())
            return juce::Result::fail ("Wine could not create Spinus's Windows folder.");
        [[maybe_unused]] const auto written = marker.replaceWithText (version);
    }

    // Spinus's own registry settings, also for prefixes made before they existed. Without them the prefix
    // still works (some text may look wrong): one Wine refuses is tried again the next time.
    // The marker holds the settings applied: other settings (a new Spinus, another Wine) are applied again.
    const auto settings = getPrefixSettingsCommands (r);
    juce::StringArray applied;
    for (auto& args : settings)
        applied.add (args.joinIntoString (" "));
    const auto settingsMarker = getPrefix().getChildFile (".spinus-settings");
    if (settingsMarker.loadFileAsString().trim() == applied.joinIntoString ("\n").trim())
        return juce::Result::ok();
    bool allSet = true;
    for (auto& args : settings)
    {
        juce::String output;
        if (auto result = run (wineCommand (r, args), 60000, cancel, &output); result.failed())
            return result;
        allSet = allSet && output.containsIgnoreCase ("success");
    }
    if (! allSet)
        return juce::Result::ok();
    // No waiting for Wine's server here (plugins open in Spinus may keep it running): it has the
    // settings now and writes them to the registry files when it stops.
    [[maybe_unused]] const auto written = settingsMarker.replaceWithText (applied.joinIntoString ("\n"));
    return juce::Result::ok();
}

std::vector<juce::StringArray> WindowsPlugins::getPrefixSettingsCommands (const Runtime& r)
{
    // The font for text in a font the prefix doesn't have. Programs made with JUCE (most plugins and their
    // installers) then take the first font of Windows's font list, which Wine keeps in alphabetical order:
    // a symbol font an installer added can come first (Ample Sound's AmpleSoundTab: every letter a
    // guitar-tab sign). An entry named to sort first makes that Wine's own Arial. Other programs already
    // get Wine's Arial, Courier New or Times New Roman.
    const auto arial = r.wine.getParentDirectory().getSiblingFile ("share/wine/fonts/arial.ttf");
    if (! arial.existsAsFile())
        return {};
    return { { "reg", "add", "HKLM\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", "/v",
               " Spinus default (TrueType)", "/t", "REG_SZ", "/d",
               "Z:" + arial.getFullPathName().replaceCharacter ('/', '\\'), "/f" } };
}

juce::StringArray WindowsPlugins::findInstallerPrograms (const juce::String& tasklistCsv)
{
    // Wine's own programs, always running in a prefix, and the Windows side of plugins open in Spinus.
    static const juce::StringArray ignored { "services.exe",         "winedevice.exe",      "explorer.exe",
                                             "plugplay.exe",         "svchost.exe",         "rpcss.exe",
                                             "conhost.exe",          "start.exe",           "tasklist.exe",
                                             "wineboot.exe",         "winemenubuilder.exe", "yabridge-host.exe",
                                             "yabridge-host-32.exe", "yabridge-group.exe",  "yabridge-group-32.exe" };
    juce::StringArray programs;
    juce::StringArray lines;
    lines.addLines (tasklistCsv);
    for (auto& line : lines)
    {
        // "Image Name","PID","Session Name","Session#","Mem Usage"
        if (! line.trim().startsWithChar ('"'))
            continue;
        const auto image = line.trim().substring (1).upToFirstOccurrenceOf ("\"", false, false).trim();
        if (image.isNotEmpty() && ! ignored.contains (image, true))
            programs.add (image);
    }
    return programs;
}

juce::Result WindowsPlugins::runInstaller (const Runtime& r, const juce::File& installer,
                                           const std::atomic<bool>* cancel, const Progress& progress) const
{
    if (! installer.existsAsFile())
        return juce::Result::fail ("The installer " + installer.getFullPathName() + " does not exist.");
    if (const auto problem = getInstallerProblem (installer); problem.isNotEmpty())
        return juce::Result::fail (problem);
    if (auto result = createPrefix (r, cancel); result.failed())
        return result;

    const bool msi = installer.hasFileExtension ("msi");
    const auto args = msi ? juce::StringArray { "msiexec", "/i", installer.getFullPathName() }
                          : juce::StringArray { installer.getFullPathName() };
    auto result = run (wineCommand (r, args), 0, cancel);

    // Installers often start helpers and return early: wait until those have finished too. Not with
    // `wineserver -w`, which waits for everything in the prefix, so also for the Windows side of plugins open
    // in Spinus, which runs until they are closed. Twice in a row, since one helper may start the next.
    for (int quiet = 0; result.wasOk() && quiet < 2;)
    {
        juce::String tasks;
        if (auto listed = run (wineCommand (r, { "tasklist", "/fo", "csv", "/nh" }), 30000, cancel, &tasks);
            listed.failed())
        {
            result = cancel != nullptr && cancel->load() ? listed : juce::Result::ok();
            break; // no list (an old Wine): nothing more to wait for
        }
        const auto programs = findInstallerPrograms (tasks);
        quiet = programs.isEmpty() ? quiet + 1 : 0;
        if (! programs.isEmpty() && progress)
            progress ("Waiting for the installer's programs to finish: " + programs.joinIntoString (", "));
        for (int i = 0; i < 15 && quiet < 2; ++i)
        {
            if (cancel != nullptr && cancel->load())
            {
                result = juce::Result::fail ("Cancelled.");
                break;
            }
            juce::Thread::sleep (100);
        }
    }

    if (cancel != nullptr && cancel->load())
    {
        // Stopping our wine command leaves the installer's window (and what it started) running inside
        // Wine: end that program and its children, and nothing else (Windows plugins open in the project
        // run in the same Wine).
        const auto image = msi ? juce::String ("msiexec.exe") : installer.getFileName();
        [[maybe_unused]] const auto ended =
            run (wineCommand (r, { "taskkill", "/F", "/T", "/IM", image }), 20000, nullptr);
    }
    return result;
}

juce::Result WindowsPlugins::addPluginFile (const juce::File& file) const
{
    if (! isPrefixReady())
        return juce::Result::fail ("Set up Windows plugins first.");

    juce::File target;
    if (file.hasFileExtension ("clap"))
        target = insideDriveC (getPrefix(), clapFolder).getChildFile (file.getFileName());
    else if (file.hasFileExtension ("vst3"))
        target = insideDriveC (getPrefix(), vst3Folder).getChildFile (file.getFileName());
    else if (file.hasFileExtension ("dll"))
        target = insideDriveC (getPrefix(), vst2Folders[0]).getChildFile (file.getFileName());
    else
        return juce::Result::fail (file.getFileName() + " is not a Windows plugin (.dll, .vst3 or .clap).");

    const auto module = file.hasFileExtension ("vst3") ? vst3Module (file) : file;
    if (! isWindows64 (module))
        return juce::Result::fail (file.getFileName() + " is not a 64-bit Windows plugin. 32-bit plugins are "
                                                        "not supported yet.");
    if (file.hasFileExtension ("dll") && ! exportsVst2Entry (file))
        return juce::Result::fail (file.getFileName() + " is a Windows library but not a VST plugin.");

    target.getParentDirectory().createDirectory();
    if (target.exists())
        target.deleteRecursively();
    const bool copied = file.isDirectory() ? file.copyDirectoryTo (target) : file.copyFileTo (target);
    return copied ? juce::Result::ok() : juce::Result::fail ("Could not copy " + file.getFileName() + ".");
}

//==============================================================================
bool WindowsPlugins::isWindows64 (const juce::File& file)
{
    const auto pe = readPe (file, false);
    return pe && pe->machine == 0x8664;
}

bool WindowsPlugins::isWindows32 (const juce::File& file)
{
    const auto pe = readPe (file, false);
    return pe && pe->machine == 0x14c;
}

juce::String WindowsPlugins::getFlatpak32BitProblem (const juce::File& installer, const juce::File& loader32,
                                                     const juce::String& runtimeBranch)
{
    // An .msi runs in Wine's 64-bit msiexec; only a 32-bit .exe needs the 32-bit starter up front (a 64-bit
    // installer's 32-bit helpers cannot be known in advance).
    if (! installer.hasFileExtension ("exe") || ! isWindows32 (installer) || loader32.existsAsFile())
        return {};
    return installer.getFileName() +
           " is a 32-bit Windows program. Spinus as a Flatpak runs 32-bit Windows programs only with "
           "Flatpak's 32-bit support, which is not installed on this computer. To install it, open a "
           "terminal and run:\n\n    " +
           getFlatpak32BitCommand (runtimeBranch) +
           "\n\nThen quit Spinus, start it again and add the installer again. (Spinus from the Arch package "
           "or the AppImage runs it without this.)";
}

juce::String WindowsPlugins::getFlatpak32BitCommand (const juce::String& runtimeBranch)
{
    return "flatpak install flathub org.freedesktop.Platform.Compat.i386//" +
           (runtimeBranch.isNotEmpty() ? runtimeBranch : flatpakRuntimeBranch());
}

juce::String WindowsPlugins::getInstallerProblem (const juce::File& installer) const
{
    // The runtime's /lib/ld-linux.so.2 leads to /app/lib/i386-linux-gnu, where the extension appears.
    return sandboxed ? getFlatpak32BitProblem (installer, loader32, flatpakRuntimeBranch()) : juce::String();
}

bool WindowsPlugins::exportsVst2Entry (const juce::File& file)
{
    const auto pe = readPe (file, true);
    return pe && std::any_of (pe->exports.begin(), pe->exports.end(), [] (const juce::String& e)
                              { return e == "VSTPluginMain" || e == "main" || e == "VstPluginMain"; });
}

std::vector<WindowsPlugins::Found> WindowsPlugins::findWindowsPlugins() const
{
    std::vector<Found> found;
    const auto prefix = getPrefix();

    auto search = [&] (const juce::File& root, Format format)
    {
        if (! root.isDirectory())
            return;
        std::vector<juce::File> pending { root };
        while (! pending.empty())
        {
            const auto dir = pending.back();
            pending.pop_back();
            for (const auto& entry :
                 juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFilesAndDirectories))
            {
                const auto& f = entry.getFile();
                const auto isVst3 = f.getFileExtension().equalsIgnoreCase (".vst3");
                if (entry.isDirectory() && ! isVst3)
                {
                    pending.push_back (f);
                    continue;
                }
                bool ok = false;
                switch (format)
                {
                case Format::vst3:
                    ok = isVst3 && isWindows64 (vst3Module (f));
                    break;
                case Format::clap:
                    ok = ! entry.isDirectory() && f.getFileExtension().equalsIgnoreCase (".clap") && isWindows64 (f);
                    break;
                case Format::vst2:
                    ok = ! entry.isDirectory() && f.getFileExtension().equalsIgnoreCase (".dll") && isWindows64 (f) &&
                         exportsVst2Entry (f);
                    break;
                }
                if (ok)
                    found.push_back ({ format, f, f.getFileNameWithoutExtension() });
            }
        }
    };

    search (insideDriveC (prefix, vst3Folder), Format::vst3);
    search (insideDriveC (prefix, clapFolder), Format::clap);
    std::vector<juce::File> vst2Roots;
    for (auto* path : vst2Folders)
        if (auto root = insideDriveC (prefix, path);
            root.isDirectory() && std::find (vst2Roots.begin(), vst2Roots.end(), root) == vst2Roots.end())
            vst2Roots.push_back (root);
    for (auto& root : vst2Roots)
        search (root, Format::vst2);

    std::sort (found.begin(), found.end(),
               [] (const Found& a, const Found& b) { return a.file.getFullPathName() < b.file.getFullPathName(); });
    return found;
}

juce::Array<juce::File> WindowsPlugins::findBridgedFiles (const Found& plugin) const
{
    juce::Array<juce::File> result;
    const auto dir = getBridgedFolder (plugin.format);
    if (! dir.isDirectory())
        return result;
    // The bridge's link points at the Windows file (for a VST3 bundle, the module inside it).
    const auto windowsFile = plugin.format == Format::vst3 ? vst3Module (plugin.file) : plugin.file;
    auto linksHere = [&windowsFile] (const juce::File& link)
    { return link.isSymbolicLink() && juce::File (link.getLinkedTarget()) == windowsFile; };

    for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFilesAndDirectories))
    {
        const auto& f = entry.getFile();
        switch (plugin.format)
        {
        case Format::vst2:
            if (f.hasFileExtension ("dll") && linksHere (f))
                result.add (f.withFileExtension ("so"));
            break;
        case Format::vst3:
            if (f.hasFileExtension ("vst3") && f.isDirectory() &&
                linksHere (f.getChildFile ("Contents/x86_64-win/" + f.getFileNameWithoutExtension() + ".vst3")))
                result.add (f);
            break;
        case Format::clap:
            if (f.getFileExtension() == ".clap-win" && linksHere (f))
                result.add (f.withFileExtension ("clap"));
            break;
        }
    }
    return result;
}

juce::Result WindowsPlugins::removePlugin (const Found& plugin, juce::Array<juce::File>* removedBridges) const
{
    // Only what Spinus found in the prefix's plugin folders: never anything outside drive_c.
    const auto driveC = getPrefix().getChildFile ("drive_c");
    if (! plugin.file.isAChildOf (driveC) || ! plugin.file.exists())
        return juce::Result::fail (plugin.name + " is not in Spinus's Windows folder.");

    const auto bridges = findBridgedFiles (plugin);
    if (! plugin.file.deleteRecursively (false))
        return juce::Result::fail ("Could not delete " + plugin.file.getFullPathName() + ".");

    for (const auto& bridge : bridges)
    {
        bridge.deleteRecursively (false);
        switch (plugin.format)
        {
        case Format::vst2:
            bridge.withFileExtension ("dll").deleteFile();
            break;
        case Format::clap:
            bridge.withFileExtension ("clap-win").deleteFile();
            break;
        case Format::vst3:
            break;
        }
        if (removedBridges != nullptr)
            removedBridges->add (bridge);
    }
    return juce::Result::ok();
}

int WindowsPlugins::syncBridges (const Runtime& r) const
{
    int count = 0;
    for (auto format : { Format::vst2, Format::vst3, Format::clap })
    {
        const auto dir = getBridgedFolder (format);
        dir.createDirectory();
        const auto chainloader = r.yabridge.getChildFile (chainloaderFor (format));

        // One entry per plugin; a second plugin with the same file name gets " (2)" and so on.
        std::set<juce::String> keep;
        std::map<juce::String, int> used;
        for (auto& plugin : findWindowsPlugins())
        {
            if (plugin.format != format)
                continue;
            auto name = nameOf (plugin);
            if (const auto n = ++used[name.toLowerCase()]; n > 1)
                name << " (" << n << ")";

            bool ok = false;
            switch (format)
            {
            case Format::vst2:
                ok = copyIfDifferent (chainloader, dir.getChildFile (name + ".so")) &&
                     linkTo (plugin.file, dir.getChildFile (name + ".dll"));
                keep.insert (name + ".so");
                keep.insert (name + ".dll");
                break;
            case Format::vst3:
            {
                const auto bundle = dir.getChildFile (name + ".vst3");
                ok = copyIfDifferent (chainloader, bundle.getChildFile ("Contents/x86_64-linux/" + name + ".so")) &&
                     linkTo (vst3Module (plugin.file), bundle.getChildFile ("Contents/x86_64-win/" + name + ".vst3"));
                keep.insert (name + ".vst3");
                break;
            }
            case Format::clap:
                ok = copyIfDifferent (chainloader, dir.getChildFile (name + ".clap")) &&
                     linkTo (plugin.file, dir.getChildFile (name + ".clap-win"));
                keep.insert (name + ".clap");
                keep.insert (name + ".clap-win");
                break;
            }
            count += ok ? 1 : 0;
        }

        // Plugins that were uninstalled (or renamed) leave.
        for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFilesAndDirectories))
            if (! keep.contains (entry.getFile().getFileName()))
                entry.getFile().deleteRecursively (false);
    }
    return count;
}

//==============================================================================
WindowsPlugins::TestResult WindowsPlugins::test (const Runtime& r, const juce::File& scannerExecutable,
                                                 const Progress& progress, const std::atomic<bool>* cancel,
                                                 bool stopRunningWine)
{
    TestResult result;
    result.kind = r.kind;
    result.when = juce::Time::getCurrentTime();
    result.wineVersion = getWineVersion (r);

    auto finish = [&] (const juce::String& message)
    {
        result.passed = message.isEmpty();
        result.message = message;
        auto* t = new juce::DynamicObject();
        t->setProperty ("passed", result.passed);
        t->setProperty ("runtime", r.kind == Runtime::Kind::spinus ? "spinus" : "system");
        t->setProperty ("wine", result.wineVersion);
        t->setProperty ("message", result.message);
        t->setProperty ("when", result.when.toISO8601 (true));
        auto settings = loadSettings();
        settings.getDynamicObject()->setProperty (key ("test"), juce::var (t));
        saveSettings (settings);
        return result;
    };
    auto say = [&] (const juce::String& stage)
    {
        if (progress)
            progress (stage);
    };

    if (result.wineVersion.isEmpty())
        return finish ("Wine does not start (" + r.wine.getFullPathName() + ").");

    const auto& pack = getTestPluginPack();
    const auto downloads = getDownloads();
    if (! downloads.isInstalled (pack))
    {
        say ("Downloading the test plugin");
        if (auto installed = downloads.install (pack, {}, cancel); installed.failed())
            return finish ("Could not download the test plugin: " + installed.getErrorMessage());
    }

    // A Wine of another version may still be running in the prefix (e.g. the other runtime's last test).
    if (stopRunningWine)
        stopWine();

    say ("Preparing Spinus's Windows folder");
    if (auto prefix = createPrefix (r, cancel); prefix.failed())
        return finish (prefix.getErrorMessage());

    // The test plugin inside the prefix (yabridge picks the prefix the plugin is in), and its bridge
    // outside bridged/ so it never shows in the Browser.
    const auto windowsFile = insideDriveC (getPrefix(), "Spinus/Test").getChildFile ("DragonflyRoomReverb.clap");
    const auto bridge = folder.getChildFile ("test-bridge/Spinus Test.clap");
    if (! copyIfDifferent (downloads.getPackFolder (pack).getChildFile ("DragonflyRoomReverb.clap"), windowsFile) ||
        ! copyIfDifferent (r.yabridge.getChildFile (chainloaderFor (Format::clap)), bridge) ||
        ! linkTo (windowsFile, bridge.withFileExtension ("clap-win")))
        return finish ("Could not set up the test plugin in " + folder.getFullPathName() + ".");

    say ("Starting a Windows plugin");
    PluginScanner::Options options;
    options.scannerExecutable = scannerExecutable;
    options.timeoutMs = 120000; // the first start of a prefix can take a while
    options.environment = { "-u", "WINEPREFIX", "PATH=" + pathFor (r), "WINELOADER=" + r.wine.getFullPathName(),
                            "WINEDLLOVERRIDES=" + juce::String (wineDllOverrides) };
    juce::OwnedArray<juce::PluginDescription> found;
    const auto scanned = PluginScanner (options).scanFile ("CLAP", bridge.getFullPathName(), found);
    if (scanned.failed())
        return finish ("The Windows test plugin did not start: " + scanned.getErrorMessage() + ".");
    if (found.isEmpty())
        return finish ("The Windows test plugin started but showed no plugin. This Wine and yabridge do not "
                       "work together.");
    return finish ({});
}

//==============================================================================
WindowsPlugins::Realtime WindowsPlugins::checkRealtime()
{
    Realtime r;
#if JUCE_WINDOWS
    r.memlockOk = true;
    return r;
#else
    rlimit limit {};
    if (getrlimit (RLIMIT_MEMLOCK, &limit) == 0)
    {
        r.memlockKiB = limit.rlim_cur == RLIM_INFINITY ? -1 : (juce::int64)(limit.rlim_cur / 1024);
        r.memlockOk = limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur >= (rlim_t)512 * 1024 * 1024;
    }

    juce::String user;
    if (const auto* pw = getpwuid (getuid())) // NOLINT(concurrency-mt-unsafe)
        user = pw->pw_name;

    if (const auto* group = getgrnam ("realtime")) // NOLINT(concurrency-mt-unsafe)
    {
        r.groupExists = true;
        for (auto** member = group->gr_mem; member != nullptr && *member != nullptr; ++member)
            r.addedToGroup = r.addedToGroup || user == *member;

        const auto gid = group->gr_gid;
        std::vector<gid_t> groups ((size_t)juce::jmax (1, getgroups (0, nullptr)));
        const auto n = getgroups ((int)groups.size(), groups.data());
        r.inGroup = getgid() == gid || std::find (groups.begin(), groups.begin() + juce::jmax (0, n), gid) !=
                                           groups.begin() + juce::jmax (0, n);
    }

    if (r.memlockOk)
        return r;
    if (! r.groupExists)
        r.advice = "Windows plugins run more smoothly with real-time privileges. In a terminal, run: sudo pacman -S "
                   "realtime-privileges && sudo gpasswd -a " +
                   user + " realtime - then log out and back in.";
    else if (! r.addedToGroup)
        r.advice = "Windows plugins run more smoothly with real-time privileges. In a terminal, run: sudo gpasswd "
                   "-a " +
                   user + " realtime - then log out and back in.";
    else if (! r.inGroup)
        r.advice = "You are in the realtime group, but this session started before that: log out and back in.";
    else
        r.advice = "The locked-memory limit is low (" + juce::String (r.memlockKiB) +
                   " KiB) even though you are in the realtime group. Restart the computer.";
    return r;
#endif
}

} // namespace spinus::plugins
