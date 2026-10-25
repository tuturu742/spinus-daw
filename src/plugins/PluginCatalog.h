#pragma once

#include "PluginScanner.h"

namespace spinus::plugins
{

/** The plugins Spinus knows about: the scan results, the problem list and (in the Flatpak) the plugins
    that need the computer's own libraries, saved in the user's settings folder, plus where to look. Bundled plugins
   (Surge XT, LSP, Dragonfly) are found in the `plugins/` folder next to the Spinus executable. */
class PluginCatalog
{
public:
    /** `settingsFolder` holds plugin-list.xml and problem-plugins.xml; `executableFolder` holds
        spinus-plugin-scanner and plugins/. */
    PluginCatalog (juce::File settingsFolder, juce::File executableFolder);

    void load();
    void save() const;

    /** Formats Spinus hosts: VST3, LV2, VST (2), LADSPA from JUCE, plus Spinus's CLAP host. */
    static void addFormats (juce::AudioPluginFormatManager&);

    /** Also searches `folder` for this format (e.g. the bridged Windows plugins). */
    void addSearchFolder (const juce::String& formatName, const juce::File& folder);

    /** Where each format is searched: its default locations plus the bundled plugins folder. */
    std::map<juce::String, juce::FileSearchPath> getSearchPaths (juce::AudioPluginFormatManager&) const;

    PluginScanner::Summary scan (const std::function<void (const PluginScanner::Progress&)>& onProgress = {},
                                 const std::atomic<bool>* cancel = nullptr);

    juce::KnownPluginList& getKnownPlugins() noexcept { return known; }
    const juce::KnownPluginList& getKnownPlugins() const noexcept { return known; }

    /** Where the bundled plugins are: `plugins/` next to the executable. */
    juce::File getBundledFolder() const { return executables.getChildFile ("plugins"); }

    /** Makes the plugins found by scans loadable: adds them to the engine's list (Tracktion creates
        plugins from it). */
    void addKnownTypesTo (juce::KnownPluginList&) const;

    /** Makes plugins installed since the formats were created loadable by them. JUCE's LV2 host finds
        plugins by URI among the bundles it loaded when it was created, so it loads its folders again
        (call after a scan). Other formats load plugins by file and need nothing. */
    static void refreshFormats (juce::AudioPluginFormatManager&);
    ProblemPlugins& getProblems() noexcept { return problems; }

    /** In the Flatpak, whether a plugin of the computer's loads only with the computer's own libraries (found
        so by a scan): it runs in a process of its own started with them. Any thread. */
    bool needsHostLibraries (const juce::String& fileOrIdentifier) const
    {
        return hostLibraryPlugins.contains (fileOrIdentifier);
    }

private:
    juce::File settings, executables;
    std::map<juce::String, std::vector<juce::File>> extraFolders;
    juce::KnownPluginList known;
    ProblemPlugins problems;
    HostLibraryPlugins hostLibraryPlugins;
};

} // namespace spinus::plugins
