#include "PluginCatalog.h"

#include "clap/ClapPluginFormat.h"
#include "core/Log.h"
#include "session/ProgramFolder.h"

namespace spinus::plugins
{

PluginCatalog::PluginCatalog (juce::File settingsFolder, juce::File executableFolder)
    : settings (std::move (settingsFolder)), executables (std::move (executableFolder))
{
}

void PluginCatalog::load()
{
    if (auto xml = juce::parseXML (settings.getChildFile ("plugin-list.xml")))
        known.recreateFromXml (*xml);

    if (auto xml = juce::parseXML (settings.getChildFile ("problem-plugins.xml")))
        problems.loadFromXml (*xml);
}

void PluginCatalog::save() const
{
    settings.createDirectory();

    if (auto xml = known.createXml())
        xml->writeTo (settings.getChildFile ("plugin-list.xml"));

    problems.toXml()->writeTo (settings.getChildFile ("problem-plugins.xml"));
}

void PluginCatalog::addSearchFolder (const juce::String& formatName, const juce::File& folder)
{
    auto& folders = extraFolders[formatName];
    if (std::find (folders.begin(), folders.end(), folder) == folders.end())
        folders.push_back (folder);
}

void PluginCatalog::addFormats (juce::AudioPluginFormatManager& formats)
{
    juce::addDefaultFormatsToManager (formats);
    formats.addFormat (std::make_unique<ClapPluginFormat>());
}

std::map<juce::String, juce::FileSearchPath>
PluginCatalog::getSearchPaths (juce::AudioPluginFormatManager& formats) const
{
    std::map<juce::String, juce::FileSearchPath> paths;
    const auto bundled = executables.getChildFile ("plugins");

    for (auto* format : formats.getFormats())
    {
        auto path = format->getDefaultLocationsToSearch();
        if (bundled.isDirectory() && (format->getName() == "VST3" || format->getName() == ClapPluginFormat::formatName))
            path.addIfNotAlreadyThere (bundled);
        // In the Flatpak, the plugins the computer's package manager installed (read-only, at /run/host/usr).
        for (const auto& folder : getHostPluginFolders (format->getName()))
            path.addIfNotAlreadyThere (folder);
        if (auto it = extraFolders.find (format->getName()); it != extraFolders.end())
            for (auto& folder : it->second)
                path.addIfNotAlreadyThere (folder);
        // In the Flatpak, plugins come as extensions (org.freedesktop.LinuxAudio.Plugins.*), mounted here.
        static const std::map<juce::String, juce::String> extensionFolders {
            { "VST3", "vst3" }, { "LV2", "lv2" }, { "VST", "vst" }, { ClapPluginFormat::formatName, "clap" }
        };
        if (const juce::File extensions ("/app/extensions/Plugins"); extensions.isDirectory())
            if (auto it = extensionFolders.find (format->getName()); it != extensionFolders.end())
                path.addIfNotAlreadyThere (extensions.getChildFile (it->second));
        paths[format->getName()] = path;
    }

    return paths;
}

PluginScanner::Summary PluginCatalog::scan (const std::function<void (const PluginScanner::Progress&)>& onProgress,
                                            const std::atomic<bool>* cancel)
{
    juce::AudioPluginFormatManager formats;
    addFormats (formats);

    const PluginScanner scanner ({ getHelperProgram (executables, "spinus-plugin-scanner") });
    core::Logs::info ("scan", "Scanning plugins");
    const auto started = juce::Time::getMillisecondCounter();
    auto summary = scanner.scanAll (formats, known, problems, getSearchPaths (formats), onProgress, cancel);
    save();
    core::Logs::info ("scan", juce::String (cancel != nullptr && cancel->load() ? "Scan stopped: " : "Scanned: ") +
                                  juce::String (summary.scanned) + " files, " + juce::String (summary.pluginsFound) +
                                  " plugins found, " + juce::String (summary.newProblems) + " new problem plugins, " +
                                  juce::String (summary.skippedUnchanged) + " unchanged and " +
                                  juce::String (summary.skippedProblems) + " known problems skipped; " +
                                  juce::String (known.getNumTypes()) + " plugins known (" +
                                  juce::String ((juce::Time::getMillisecondCounter() - started) / 1000.0, 1) + " s)");
    return summary;
}

void PluginCatalog::refreshFormats (juce::AudioPluginFormatManager& formats)
{
    for (auto* format : formats.getFormats())
        if (format->getName() == "LV2")
        {
            auto path = format->getDefaultLocationsToSearch();
            for (const auto& folder : getHostPluginFolders ("LV2"))
                path.addIfNotAlreadyThere (folder);
            [[maybe_unused]] auto found = format->searchPathsForPlugins (path, true);
        }
}

void PluginCatalog::addKnownTypesTo (juce::KnownPluginList& target) const
{
    for (auto& type : known.getTypes())
        target.addType (type);
}

} // namespace spinus::plugins
