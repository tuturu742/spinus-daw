#pragma once

#include <juce_core/juce_core.h>

#include <optional>
#include <set>

namespace spinus::engine
{

/** Notices which plugin took Spinus down, so the next start can offer to run it in a separate
    process.

    When a plugin is made, the libraries it loaded are noted (ModuleWatch). If Spinus then crashes
    (Linux: SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT; Windows: an unhandled exception, or abort()), a
    handler walks the crashed thread's stack, and the first frame inside a noted library names the plugin:
    it writes that plugin to a small file and lets the crash carry on as before. Only what is safe after a
    crash happens there: the table is filled beforehand, and the file is written with the system's own
    calls (open/write, CreateFile/WriteFile). */
class PluginCrashGuard
{
public:
    /** Installs the handlers (once per process); a crash in a plugin is written to `record`. */
    static void install (const juce::File& record);

    /** A plugin's library: a path (its file, or its folder for bundles) and the plugin it belongs to.
        `own`: the plugin's own file, which wins over libraries it merely loaded (shared ones may be used by
        other code too). */
    static void add (const juce::String& libraryPath, const juce::String& key, const juce::String& name,
                     bool own = true);

    struct Crash
    {
        juce::String key, name; // SandboxList::keyOf, and its name
    };
    /** The plugin a crash was written for last time, if any; the record is deleted. */
    static std::optional<Crash> takeRecorded (const juce::File& record);

    /** The libraries loaded between its making and add(): what a plugin brought in. */
    class ModuleWatch
    {
    public:
        ModuleWatch();
        /** `pluginFile`: the plugin's file or bundle, when it is a path. */
        void addNewModules (const juce::String& key, const juce::String& name, const juce::String& pluginFile) const;

    private:
        std::set<std::string> before;
    };
};

} // namespace spinus::engine
