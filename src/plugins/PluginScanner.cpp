#include "PluginScanner.h"
#include "core/ChildProcess.h"
#include "core/Log.h"

#include "clap/ClapPluginFormat.h"
#include "plugins/windows/PluginDialogs.h"
#include "session/ProgramFolder.h"

#if JUCE_LINUX || JUCE_BSD
#include <dlfcn.h>
#endif

namespace spinus::plugins
{

//==============================================================================
void ProblemPlugins::add (Problem p)
{
    remove (p.format, p.file);
    problems.push_back (std::move (p));
}

void ProblemPlugins::remove (const juce::String& format, const juce::String& file)
{
    std::erase_if (problems, [&] (const Problem& p) { return p.format == format && p.file == file; });
}

bool ProblemPlugins::contains (const juce::String& format, const juce::String& file) const
{
    return std::any_of (problems.begin(), problems.end(),
                        [&] (const Problem& p) { return p.format == format && p.file == file; });
}

std::unique_ptr<juce::XmlElement> ProblemPlugins::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("PROBLEM_PLUGINS");
    for (auto& p : problems)
    {
        auto* e = xml->createNewChildElement ("PROBLEM");
        e->setAttribute ("format", p.format);
        e->setAttribute ("file", p.file);
        e->setAttribute ("reason", p.reason);
        e->setAttribute ("when", p.when.toISO8601 (true));
    }
    return xml;
}

void ProblemPlugins::loadFromXml (const juce::XmlElement& xml)
{
    problems.clear();
    for (auto* e : xml.getChildWithTagNameIterator ("PROBLEM"))
        problems.push_back ({ e->getStringAttribute ("format"), e->getStringAttribute ("file"),
                              e->getStringAttribute ("reason"),
                              juce::Time::fromISO8601 (e->getStringAttribute ("when")) });
}

//==============================================================================
namespace
{
juce::Array<juce::PluginDescription> typesForFile (const juce::KnownPluginList& known, const juce::String& file)
{
    juce::Array<juce::PluginDescription> result;
    for (auto& type : known.getTypes())
        if (type.fileOrIdentifier == file)
            result.add (type);
    return result;
}
} // namespace

PluginScanner::PluginScanner (Options o) : options (std::move (o)) {}

juce::Result PluginScanner::scanFile (const juce::String& formatName, const juce::String& fileOrIdentifier,
                                      juce::OwnedArray<juce::PluginDescription>& found) const
{
    if (! options.scannerExecutable.existsAsFile())
        return juce::Result::fail ("the plugin scanner program is missing: " +
                                   options.scannerExecutable.getFullPathName());

    const juce::TemporaryFile output (".xml");
    core::ChildProcess process;

    // A different environment (e.g. testing another Wine for Windows plugins) goes through env(1).
    juce::StringArray command;
    if (! options.environment.isEmpty())
        command.add ("env");
    command.addArray (options.environment);
    command.addArray ({ options.scannerExecutable.getFullPathName(), "--format", formatName, "--file", fileOrIdentifier,
                        "--output", output.getFile().getFullPathName() });

    if (! process.start (command, 0))
        return juce::Result::fail ("could not start the plugin scanner");

    // A Windows plugin may ask something in a window of its own (e.g. to register) before it loads:
    // brought to the front while the scanner waits.
    std::optional<PluginDialogs::ScopedWait> waitForWindowsPlugin;
    if (PluginDialogs::isWindowsPlugin (fileOrIdentifier))
        waitForWindowsPlugin.emplace();

    if (! process.waitForProcessToFinish (options.timeoutMs))
    {
        process.kill();
        return juce::Result::fail ("did not finish loading within " + juce::String (options.timeoutMs / 1000) + " s");
    }

    // The scanner writes its result only after a clean scan, so a missing file means it crashed.
    const auto xml = juce::parseXML (output.getFile());
    if (xml == nullptr || ! xml->hasTagName ("PLUGINS"))
    {
        const auto code = (int)process.getExitCode();
        core::Logs::warning ("scan", "The plugin scanner ended with exit code " + juce::String (code) +
                                         (code > 128 ? " (signal " + juce::String (code - 128) + ")" : juce::String()) +
                                         " while loading " + fileOrIdentifier);
        return juce::Result::fail ("crashed while loading");
    }
    if (xml->getNumChildElements() == 0 && xml->hasAttribute ("error"))
        return juce::Result::fail (xml->getStringAttribute ("error"));

    for (auto* e : xml->getChildIterator())
    {
        auto desc = std::make_unique<juce::PluginDescription>();
        if (desc->loadFromXml (*e))
            found.add (desc.release());
    }

    return juce::Result::ok();
}

PluginScanner::Summary PluginScanner::scanAll (juce::AudioPluginFormatManager& formats, juce::KnownPluginList& known,
                                               ProblemPlugins& problems,
                                               const std::map<juce::String, juce::FileSearchPath>& searchPaths,
                                               const std::function<void (const Progress&)>& onProgress,
                                               const std::atomic<bool>* cancel) const
{
    struct Job
    {
        juce::AudioPluginFormat* format;
        juce::String file;
    };

    std::vector<Job> jobs;
    Summary summary;
    forgetVanishedPlugins (known, &problems);

    for (auto* format : formats.getFormats())
    {
        auto path = searchPaths.contains (format->getName()) ? searchPaths.at (format->getName())
                                                             : format->getDefaultLocationsToSearch();

        for (auto& file : format->searchPathsForPlugins (path, true, false))
        {
            if (problems.contains (format->getName(), file))
            {
                ++summary.skippedProblems;
                continue;
            }

            // Unchanged since the last scan: keep the known descriptions.
            const auto types = typesForFile (known, file);
            if (! types.isEmpty() && types[0].pluginFormatName == format->getName() &&
                ! format->pluginNeedsRescanning (types[0]))
            {
                ++summary.skippedUnchanged;
                continue;
            }

            jobs.push_back ({ format, file });
        }
    }

    for (size_t i = 0; i < jobs.size(); ++i)
    {
        if (cancel != nullptr && cancel->load())
            break;

        auto& job = jobs[i];
        if (onProgress)
            onProgress ({ (int)i, (int)jobs.size(), job.file });

        juce::OwnedArray<juce::PluginDescription> found;
        const auto result = scanFile (job.format->getName(), job.file, found);
        ++summary.scanned;

        // Replace whatever was known about this file.
        for (auto& old : typesForFile (known, job.file))
            known.removeType (old);

        if (result.failed())
        {
            core::Logs::warning ("scan", "Problem plugin (" + job.format->getName() + ") " + job.file + ": " +
                                             result.getErrorMessage());
            problems.add ({ job.format->getName(), job.file, result.getErrorMessage(), juce::Time::getCurrentTime() });
            ++summary.newProblems;
            continue;
        }

        for (auto* desc : found)
        {
            known.addType (*desc);
            ++summary.pluginsFound;
        }
    }

    if (onProgress)
        onProgress ({ (int)jobs.size(), (int)jobs.size(), {} });

    return summary;
}

//==============================================================================
int forgetVanishedPlugins (juce::KnownPluginList& known, ProblemPlugins* problems)
{
    auto vanished = [] (const juce::String& file)
    { return juce::File::isAbsolutePath (file) && ! juce::File (file).exists(); };
    int removed = 0;
    for (const auto& type : known.getTypes())
        if (vanished (type.fileOrIdentifier))
        {
            known.removeType (type);
            ++removed;
        }
    if (problems != nullptr)
    {
        std::vector<std::pair<juce::String, juce::String>> gone;
        for (const auto& p : problems->getAll())
            if (vanished (p.file))
                gone.emplace_back (p.format, p.file);
        for (const auto& [format, file] : gone)
            problems->remove (format, file);
    }
    return removed;
}

namespace
{
/** The program file of the LV2 plugin `uri` among the system's plugins seen from the Flatpak (read from
    the bundles' manifest.ttl: `<uri> a lv2:Plugin ; lv2:binary <file.so>`), or none. LV2 plugins are
    listed from their description files alone, so their program is only loaded when used. */
juce::File findHostLv2Binary (const juce::String& uri)
{
    for (const auto& folder : getHostPluginFolders ("LV2"))
        for (const auto& bundle : folder.findChildFiles (juce::File::findDirectories, false, "*.lv2"))
        {
            const auto text = bundle.getChildFile ("manifest.ttl").loadFileAsString();
            if (text.isEmpty() || ! text.contains ("lv2:binary"))
                continue;
            // @prefix names, then each statement (ended by " ." at a line's end): its subject and binary.
            std::map<juce::String, juce::String> prefixes;
            for (const auto& line : juce::StringArray::fromLines (text))
                if (const auto l = line.trim(); l.startsWith ("@prefix"))
                    prefixes[l.fromFirstOccurrenceOf ("@prefix", false, false)
                                 .upToFirstOccurrenceOf (":", false, false)
                                 .trim()] =
                        l.fromFirstOccurrenceOf ("<", false, false).upToFirstOccurrenceOf (">", false, false);
            for (const auto& statement : juce::StringArray::fromTokens (text.replace (" .\n", "\x01"), "\x01", ""))
            {
                const auto s = statement.trim();
                if (s.startsWith ("@") || ! s.contains ("lv2:Plugin") || ! s.contains ("lv2:binary"))
                    continue;
                auto subject = s.upToFirstOccurrenceOf (" ", false, false)
                                   .upToFirstOccurrenceOf ("\n", false, false)
                                   .upToFirstOccurrenceOf ("\t", false, false);
                if (subject.startsWith ("<"))
                    subject = subject.substring (1, subject.length() - 1);
                else if (subject.contains (":"))
                    subject = prefixes[subject.upToFirstOccurrenceOf (":", false, false)] +
                              subject.fromFirstOccurrenceOf (":", false, false);
                if (subject == uri)
                    return bundle.getChildFile (s.fromFirstOccurrenceOf ("lv2:binary", false, false)
                                                    .fromFirstOccurrenceOf ("<", false, false)
                                                    .upToFirstOccurrenceOf (">", false, false));
            }
        }
    return {};
}
} // namespace

juce::String whyPluginDoesNotLoad (const juce::String& formatName, const juce::String& fileOrIdentifier)
{
#if JUCE_LINUX || JUCE_BSD
    // An LV2 plugin is known by its URI: in the Flatpak, the system's ones are checked by their program.
    const auto lv2Binary = formatName == "LV2" && ! juce::File::isAbsolutePath (fileOrIdentifier) && isFlatpak()
                               ? findHostLv2Binary (fileOrIdentifier)
                               : juce::File();
    if (! juce::File::isAbsolutePath (fileOrIdentifier) && lv2Binary == juce::File())
        return {}; // an LV2 URI, a Spinus device
    const juce::File plugin (lv2Binary != juce::File() ? lv2Binary.getFullPathName() : fileOrIdentifier);
    juce::File binary = plugin;
    if (formatName == "VST3" && plugin.isDirectory())
    {
        const auto folder = plugin.getChildFile ("Contents/x86_64-linux");
        binary = folder.getChildFile (plugin.getFileNameWithoutExtension() + ".so");
        if (! binary.existsAsFile())
            if (const auto any = folder.findChildFiles (juce::File::findFiles, false, "*.so"); ! any.isEmpty())
                binary = any.getFirst();
    }
    if (! binary.existsAsFile())
        return {};

    juce::String error;
    if (auto* handle = dlopen (binary.getFullPathName().toRawUTF8(), RTLD_LAZY | RTLD_LOCAL))
        dlclose (handle);
    else if (const auto* text = dlerror())
        error = juce::String::fromUTF8 (text).trim();
    if (error.isEmpty())
        return {};

    // The loader names the file first ("/run/host/usr/lib/vst3/X.vst3/.../X.so: libfoo.so.1: cannot open..."):
    // the plugin is named already.
    if (error.startsWith (binary.getFullPathName() + ": "))
        error = error.fromFirstOccurrenceOf (": ", false, false);
    if (isFlatpak() && plugin.getFullPathName().startsWith ("/run/host/"))
        return plugin.getFileNameWithoutExtension() +
               " was installed by your system and cannot run inside the Flatpak version of Spinus (" + error +
               "). Use the native version of Spinus for it.";
    return "does not load: " + error;
#else
    juce::ignoreUnused (formatName, fileOrIdentifier);
    return {};
#endif
}

int runScannerProcess (const juce::String& formatName, const juce::String& fileOrIdentifier, const juce::File& output)
{
    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager (formats);
    formats.addFormat (std::make_unique<ClapPluginFormat>());

    for (auto* format : formats.getFormats())
    {
        if (format->getName() != formatName)
            continue;

        juce::OwnedArray<juce::PluginDescription> found;
        format->findAllTypesForFile (found, fileOrIdentifier);
        // An LV2 plugin is described without loading its program: in the Flatpak, one of the system's whose
        // program cannot load there is a problem plugin, not a plugin that fails later in a project.
        juce::String lv2Problem;
        if (formatName == "LV2" && isFlatpak())
            for (int i = found.size(); --i >= 0;)
                if (auto why = whyPluginDoesNotLoad (formatName, found[i]->fileOrIdentifier); why.isNotEmpty())
                {
                    lv2Problem = why;
                    found.remove (i);
                }

        juce::XmlElement xml ("PLUGINS");
        for (auto* desc : found)
            xml.addChildElement (desc->createXml().release());
        // Nothing found: say why when the system can (a library it needs is missing, or too old), so the
        // plugin is listed as a problem with a reason instead of silently missing.
        if (found.isEmpty())
            if (const auto why =
                    lv2Problem.isNotEmpty() ? lv2Problem : whyPluginDoesNotLoad (formatName, fileOrIdentifier);
                why.isNotEmpty())
                xml.setAttribute ("error", why);

        // Write atomically: the coordinator treats a missing file as a crash.
        juce::TemporaryFile temp (output);
        if (! xml.writeTo (temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
        {
            core::Logs::error ("scan", "Could not write the result for " + fileOrIdentifier + " to " +
                                           output.getFullPathName());
            return 3;
        }

        return 0;
    }

    core::Logs::error ("scan", "Unknown plugin format " + formatName + " (for " + fileOrIdentifier + ")");
    return 2; // unknown format
}

} // namespace spinus::plugins
