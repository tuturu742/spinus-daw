#include "HostLibraries.h"
#include "PluginScanner.h"

#include "session/ProgramFolder.h"

#include <algorithm>

#if JUCE_LINUX || JUCE_BSD
#include <gnu/libc-version.h>
#endif

namespace spinus::plugins
{

namespace
{
/** "2.44" from the C library's own text ("GNU C Library (GNU libc) stable release version 2.44."). */
juce::String readCVersion (const juce::File& libc)
{
    juce::MemoryBlock data;
    if (! libc.loadFileAsData (data))
        return {};
    static constexpr char marker[] = "release version ";
    const auto* begin = static_cast<const char*> (data.getData());
    const auto* end = begin + data.getSize();
    const auto* found = std::search (begin, end, marker, marker + sizeof (marker) - 1);
    if (found == end)
        return {};
    juce::String version;
    for (const auto* c = found + sizeof (marker) - 1; c < end && (juce::CharacterFunctions::isDigit (*c) || *c == '.');
         ++c)
        version += *c;
    return version.trimCharactersAtEnd (".");
}

int compareVersions (const juce::String& a, const juce::String& b)
{
    const auto x = juce::StringArray::fromTokens (a, ".", ""), y = juce::StringArray::fromTokens (b, ".", "");
    for (int i = 0; i < juce::jmax (x.size(), y.size()); ++i)
        if (const auto d = x[i].getIntValue() - y[i].getIntValue(); d != 0)
            return d;
    return 0;
}
} // namespace

juce::StringArray HostLibraries::command (const juce::File& program, const juce::StringArray& args) const
{
    juce::StringArray result;
    if (! environment.isEmpty())
    {
        result.add ("env");
        result.addArray (environment);
    }
    result.addArray (
        { loader.getFullPathName(), "--library-path", folders.joinIntoString (":"), program.getFullPathName() });
    result.addArray (args);
    return result;
}

std::optional<HostLibraries> findHostLibraries (const juce::File& root, const juce::String& ownCVersion)
{
    HostLibraries libraries;
    // Debian and Ubuntu, Fedora and openSUSE, Arch.
    for (const auto* path : { "usr/lib/x86_64-linux-gnu", "usr/lib64", "usr/lib" })
        if (const auto folder = root.getChildFile (path); folder.getChildFile ("libc.so.6").existsAsFile())
            libraries.folders.addIfNotAlreadyThere (folder.getFullPathName());
    for (const auto& folder : libraries.folders)
        if (const auto loader = juce::File (folder).getChildFile ("ld-linux-x86-64.so.2"); loader.existsAsFile())
        {
            libraries.loader = loader;
            break;
        }
    if (libraries.loader == juce::File())
        return std::nullopt;

    const auto version = readCVersion (juce::File (libraries.folders[0]).getChildFile ("libc.so.6"));
    if (version.isEmpty() || compareVersions (version, ownCVersion) < 0)
        return std::nullopt;

    // The computer's OpenGL drivers, with its libGL: the Flatpak's would not match it.
    if (const auto vendors = root.getChildFile ("usr/share/glvnd/egl_vendor.d"); vendors.isDirectory())
        libraries.environment.add ("__EGL_VENDOR_LIBRARY_DIRS=" + vendors.getFullPathName());
    for (const auto& folder : libraries.folders)
        if (const auto dri = juce::File (folder).getChildFile ("dri"); dri.isDirectory())
        {
            libraries.environment.add ("LIBGL_DRIVERS_PATH=" + dri.getFullPathName());
            break;
        }
    for (const auto& folder : libraries.folders)
        if (const auto gbm = juce::File (folder).getChildFile ("gbm"); gbm.isDirectory())
        {
            libraries.environment.add ("GBM_BACKENDS_PATH=" + gbm.getFullPathName());
            break;
        }
    return libraries;
}

const std::optional<HostLibraries>& getHostLibraries()
{
    static const auto libraries = []() -> std::optional<HostLibraries>
    {
#if JUCE_LINUX || JUCE_BSD
        if (isFlatpak())
            return findHostLibraries (juce::File ("/run/host"), gnu_get_libc_version());
#endif
        return std::nullopt;
    }();
    return libraries;
}

bool isHostPlugin (const juce::String& formatName, const juce::String& fileOrIdentifier)
{
    if (! isFlatpak())
        return false;
    if (fileOrIdentifier.startsWith ("/run/host/"))
        return true;
    return formatName == "LV2" && ! juce::File::isAbsolutePath (fileOrIdentifier) &&
           findHostLv2Binary (fileOrIdentifier) != juce::File();
}

void addPluginFoldersToLv2Path()
{
#if JUCE_LINUX || JUCE_BSD
    if (! isFlatpak())
        return;
    juce::StringArray path;
    path.addTokens (juce::SystemStats::getEnvironmentVariable ("LV2_PATH", {}), ":", "");
    path.removeEmptyStrings();
    for (const auto& folder : getHostPluginFolders ("LV2"))
        path.addIfNotAlreadyThere (folder.getFullPathName());
    if (const juce::File extensions ("/app/extensions/Plugins/lv2"); extensions.isDirectory())
        path.addIfNotAlreadyThere (extensions.getFullPathName());
    if (! path.isEmpty())
        setenv ("LV2_PATH", path.joinIntoString (":").toRawUTF8(), 1);
#endif
}

//==============================================================================
HostLibraryPlugins::HostLibraryPlugins (juce::File f) : file (std::move (f)) {}

void HostLibraryPlugins::load()
{
    const std::scoped_lock guard (lock);
    plugins.clear();
    const auto json = juce::JSON::parse (file);
    if (const auto* list = json["plugins"].getArray())
        for (const auto& plugin : *list)
            plugins.insert (plugin.toString());
}

void HostLibraryPlugins::save() const
{
    juce::Array<juce::var> list;
    {
        const std::scoped_lock guard (lock);
        for (const auto& plugin : plugins)
            list.add (plugin);
    }
    auto* object = new juce::DynamicObject();
    object->setProperty ("plugins", list);
    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::JSON::toString (juce::var (object)));
}

bool HostLibraryPlugins::contains (const juce::String& fileOrIdentifier) const
{
    const std::scoped_lock guard (lock);
    return plugins.contains (fileOrIdentifier);
}

void HostLibraryPlugins::set (const juce::String& fileOrIdentifier, bool withHostLibraries)
{
    const std::scoped_lock guard (lock);
    if (withHostLibraries)
        plugins.insert (fileOrIdentifier);
    else
        plugins.erase (fileOrIdentifier);
}

} // namespace spinus::plugins
