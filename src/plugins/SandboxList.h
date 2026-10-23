#pragma once

#include "session/ProgramFolder.h"

#include <juce_core/juce_core.h>

namespace spinus::plugins
{

/** Plugins Spinus runs in a separate process: offered after a plugin took Spinus down, and
    switched per plugin from its menu. A setting of this computer, not of projects, kept in a JSON file
    ({"plugins": [{"key", "name"}]}). Message thread only. */
class SandboxList
{
public:
    explicit SandboxList (juce::File file);

    /** "CLAP:/usr/lib/clap/Foo.clap": the plugin's format and file (or identifier), as the computer's path
        (the same for the Flatpak's /run/host/usr/...). */
    static juce::String keyOf (const juce::String& format, const juce::String& fileOrIdentifier)
    {
        return format + ":" + toPortablePluginPath (fileOrIdentifier);
    }

    bool contains (const juce::String& key) const { return names.find (key) != names.end(); }
    /** Adds or removes a plugin and saves the file. */
    void set (const juce::String& key, bool runSeparately, const juce::String& name);
    std::vector<std::pair<juce::String, juce::String>> getPlugins() const; // key, name

private:
    void save() const;

    juce::File file;
    std::map<juce::String, juce::String> names;
};

} // namespace spinus::plugins
