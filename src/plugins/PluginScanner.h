#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <map>

namespace spinus::plugins
{

/** Plugin files that crashed or hung while being scanned. They are
    skipped by later scans until the user retries them. */
class ProblemPlugins
{
public:
    struct Problem
    {
        juce::String format, file, reason;
        juce::Time when;
    };

    void add (Problem);
    void remove (const juce::String& format, const juce::String& file);
    bool contains (const juce::String& format, const juce::String& file) const;
    const std::vector<Problem>& getAll() const noexcept { return problems; }

    std::unique_ptr<juce::XmlElement> toXml() const;
    void loadFromXml (const juce::XmlElement&);

private:
    std::vector<Problem> problems;
};

/** Scans plugins out of process: each plugin file is opened by the separate
    `spinus-plugin-scanner` program, so a plugin that crashes or hangs while loading cannot take
    Spinus down; it lands in ProblemPlugins instead.
*/
class PluginScanner
{
public:
    struct Options
    {
        juce::File scannerExecutable;  // spinus-plugin-scanner
        int timeoutMs = 60000;         // per plugin file
        juce::StringArray environment; // for the scanner process, as env(1) takes it ("NAME=value", "-u", "NAME")
    };

    explicit PluginScanner (Options);

    /** Scans one file in a fresh scanner process. Fails with a readable reason ("crashed while
        scanning", "did not finish within 60 s", …). */
    juce::Result scanFile (const juce::String& formatName, const juce::String& fileOrIdentifier,
                           juce::OwnedArray<juce::PluginDescription>& found) const;

    struct Progress
    {
        int done = 0, total = 0;
        juce::String currentFile;
    };

    struct Summary
    {
        int scanned = 0, skippedUnchanged = 0, skippedProblems = 0, newProblems = 0, pluginsFound = 0;
    };

    /** Scans every plugin file of the given formats: new and changed files are scanned, unchanged ones
        (already in `known` with the same modification time) and known problems are skipped. Paths not
        given use each format's default locations. Runs on the calling thread; `cancel` stops it early. */
    Summary scanAll (juce::AudioPluginFormatManager&, juce::KnownPluginList& known, ProblemPlugins& problems,
                     const std::map<juce::String, juce::FileSearchPath>& searchPaths = {},
                     const std::function<void (const Progress&)>& onProgress = {},
                     const std::atomic<bool>* cancel = nullptr) const;

private:
    Options options;
};

/** The scanner program's side (app/PluginScannerMain.cpp): scans one file and writes the plugin
    descriptions to `output` as XML. Returns the process exit code. */
int runScannerProcess (const juce::String& formatName, const juce::String& fileOrIdentifier, const juce::File& output);

/** Removes plugins whose file is gone (uninstalled, or a Windows plugin removed in Spinus) from a plugin
    list, and from the problem list if given. Identifiers that are not files (LV2 URIs) stay. Returns how
    many were removed. */
int forgetVanishedPlugins (juce::KnownPluginList&, ProblemPlugins* = nullptr);

/** Why the plugin's program file does not load, in plain words, or empty if it loads (or this cannot be
    told: LV2, Windows). Uses the system's own loader (e.g. "libfoo.so.1: cannot open shared object file").
    In the Flatpak, a plugin of the computer's own system (under /run/host) says so and points to the
    native version. */
juce::String whyPluginDoesNotLoad (const juce::String& formatName, const juce::String& fileOrIdentifier);

} // namespace spinus::plugins
