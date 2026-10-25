#pragma once

#include <juce_core/juce_core.h>

#include <mutex>
#include <optional>
#include <set>

namespace spinus::plugins
{

/** The computer's own libraries seen from the Flatpak (--filesystem=host-os:ro puts them under /run/host).
    A plugin the system's package manager built against libraries newer than the Flatpak's cannot load
    inside Spinus there, but it runs in a process of its own started by the computer's loader
    (ld-linux-x86-64.so.2) with the computer's library folders: Spinus's helper programs need only an older
    C library and libstdc++, freetype and fontconfig, which the computer has too. */
struct HostLibraries
{
    juce::File loader;             // <root>/usr/lib/ld-linux-x86-64.so.2 or the like
    juce::StringArray folders;     // where the loader looks, before anything of the Flatpak's
    juce::StringArray environment; // "NAME=value": the computer's graphics drivers too (for OpenGL plugin windows)

    /** The command that runs `program` with these libraries. */
    juce::StringArray command (const juce::File& program, const juce::StringArray& args) const;
};

/** The libraries of the computer whose root is `root` (the Flatpak's /run/host), when its C library is at
    least `ownCVersion` ("2.42", this process's): an older one cannot start Spinus's helpers, and its plugins
    load inside the Flatpak anyway. None when the loader or the C library is not found. */
std::optional<HostLibraries> findHostLibraries (const juce::File& root, const juce::String& ownCVersion);

/** findHostLibraries for this process: in the Flatpak, /run/host and this process's C library; none
    outside the Flatpak. Worked out once. */
const std::optional<HostLibraries>& getHostLibraries();

/** In the Flatpak, whether a plugin is one of the computer's (under /run/host/; an LV2 URI: whose bundle is
    there). */
bool isHostPlugin (const juce::String& formatName, const juce::String& fileOrIdentifier);

/** In the Flatpak, adds the computer's LV2 folders (and the Flatpak's plugin extensions') to LV2_PATH, so
    that a helper program finds an LV2 plugin of the computer's by its URI. Call at the start of main(). */
void addPluginFoldersToLv2Path();

/** The plugins the last scans found loadable only with the computer's libraries (by their file, or an LV2
    URI), kept in a JSON file ({"plugins": [...]}) next to the plugin list. They run in a process of their own.
    Thread-safe: scans run on a background thread. */
class HostLibraryPlugins
{
public:
    explicit HostLibraryPlugins (juce::File file);

    void load();
    void save() const;
    bool contains (const juce::String& fileOrIdentifier) const;
    void set (const juce::String& fileOrIdentifier, bool withHostLibraries);

private:
    juce::File file;
    mutable std::mutex lock;
    std::set<juce::String> plugins;
};

} // namespace spinus::plugins
