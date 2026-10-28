#include "SoundIndex.h"

#include "InstrumentPresets.h"
#include "ProgramFolder.h"
#include "SoundDescriptors.h"
#include "core/ChildProcess.h"
#include "core/Log.h"
#include "model/StockDevices.h"
#include "plugins/DeviceList.h"
#include "plugins/PluginDownloads.h"
#include "plugins/windows/PluginDialogs.h"

#include <SpinusVersion.h>

#include <set>

namespace spinus
{

namespace
{
constexpr const char* indexKind = "spinus.sound-index";

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : items)
        o->setProperty (k, v);
    return juce::var (o);
}

bool writeJson (const juce::File& file, const juce::var& value)
{
    file.getParentDirectory().createDirectory();
    // Written beside it, then moved over it: a crash while writing leaves the old one whole.
    const juce::TemporaryFile temporary (file);
    return temporary.getFile().replaceWithText (juce::JSON::toString (value, true) + "\n") &&
           temporary.overwriteTargetFileWithTemporary();
}

juce::var sourceToVar (const SoundSource& s)
{
    return object ({ { "key", s.key },
                     { "name", s.name },
                     { "format", s.format },
                     { "file", s.file },
                     { "pluginName", s.pluginName },
                     { "uniqueId", s.uniqueId },
                     { "stockType", s.stockType },
                     { "badge", s.badge },
                     { "fingerprint", s.fingerprint },
                     { "hostLibraries", s.hostLibraries } });
}

SoundSource sourceFromVar (const juce::var& v)
{
    SoundSource s;
    s.key = v["key"].toString();
    s.name = v["name"].toString();
    s.format = v["format"].toString();
    s.file = v["file"].toString();
    s.pluginName = v["pluginName"].toString();
    s.uniqueId = (int)v["uniqueId"];
    s.stockType = v["stockType"].toString();
    s.badge = v["badge"].toString();
    s.fingerprint = v["fingerprint"].toString();
    s.hostLibraries = (bool)v["hostLibraries"];
    return s;
}

juce::String fileFingerprint (const juce::File& file)
{
    return juce::String (file.getSize()) + ":" + juce::String (file.getLastModificationTime().toMilliseconds());
}

bool isSampleFile (const juce::File& file)
{
    const auto extension = file.getFileExtension().toLowerCase();
    for (auto* e : { ".wav", ".aif", ".aiff", ".flac", ".ogg", ".mp3" })
        if (extension == e)
            return true;
    return false;
}

juce::String noteName (int note)
{
    static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (names[((note % 12) + 12) % 12]) + juce::String (note / 12 - 1);
}

juce::String timeText (double seconds)
{
    return seconds < 1.0 ? juce::String (juce::roundToInt (seconds * 1000.0)) + " ms"
                         : juce::String (seconds, 1) + " s";
}
} // namespace

//==============================================================================
std::vector<SoundSource> findSoundSources (const plugins::DeviceList& list)
{
    std::vector<SoundSource> sources;
    for (const auto& e : list.getEntries())
    {
        if (! e.instrument || ! e.available)
            continue;
        SoundSource s;
        s.name = e.name;
        s.badge = e.badge;
        s.key = InstrumentPresets::libraryOf (e.device);
        s.stockType = e.device.kind == model::Device::Kind::stock ? e.device.stockType : juce::String();
        switch (e.origin)
        {
        case plugins::DeviceList::Origin::spinus:
        {
            // The Sampler and the Beat Slicer play the samples they are given: nothing to measure by themselves.
            const auto* own = model::findSpinusDevice (e.device.stockType);
            if (own == nullptr || s.stockType == "spinus.sampler" || s.stockType == "spinus.beat-slicer")
                continue;
            s.format = "Spinus";
            s.file = own->identifier;
            s.badge = "Spinus";
            s.fingerprint = juce::String ("Spinus ") + versionString;
            break;
        }
        case plugins::DeviceList::Origin::bundled:
        {
            const auto* bundled = model::findBundledDevice (e.device.stockType);
            if (bundled == nullptr || ! list.getBundledFolders())
                continue;
            const auto file = plugins::PluginDownloads::findFile (*list.getBundledFolders(), bundled->file);
            if (file == juce::File())
                continue;
            s.format = file.getFileExtension().equalsIgnoreCase (".vst3") ? "VST3" : "CLAP";
            s.file = file.getFullPathName();
            s.pluginName = bundled->pluginName;
            s.badge = s.format;
            s.fingerprint = fileFingerprint (file);
            break;
        }
        case plugins::DeviceList::Origin::installed:
        {
            // The plugin list's own description: its file as this Spinus loads it (the Flatpak's /run/host/usr),
            // its version and the time of its file.
            auto* catalog = list.getCatalog();
            if (catalog == nullptr)
                continue;
            const auto& ref = e.device.plugin;
            std::optional<juce::PluginDescription> found;
            for (const auto& type : catalog->getKnownPlugins().getTypes())
                if (type.pluginFormatName == ref.format && type.name == ref.name &&
                    toPortablePluginPath (type.fileOrIdentifier) == ref.fileOrIdentifier)
                    found = type;
            if (! found)
                continue;
            s.format = found->pluginFormatName;
            s.file = found->fileOrIdentifier;
            s.pluginName = found->name;
            s.uniqueId = found->uniqueId;
            const juce::File file (found->fileOrIdentifier);
            s.fingerprint = found->version + "|" + juce::String (found->lastFileModTime.toMilliseconds()) + "|" +
                            juce::String (juce::File::isAbsolutePath (found->fileOrIdentifier) && file.existsAsFile()
                                              ? file.getSize()
                                              : 0);
            s.hostLibraries = catalog->needsHostLibraries (found->fileOrIdentifier);
            break;
        }
        }
        sources.push_back (std::move (s));
    }
    return sources;
}

//==============================================================================
juce::File SoundIndex::getDefaultFolder() { return getPluginListFolder().getChildFile ("ai/sounds"); }

juce::File SoundIndex::instrumentFile (const juce::String& key) const
{
    return root.getChildFile ("instruments")
        .getChildFile (key.retainCharacters ("abcdefghijklmnopqrstuvwxyz"
                                             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                             "0123456789-._") +
                       ".json");
}

void SoundIndex::load()
{
    instruments.clear();
    samples.clear();
    sampleFolders.clear();
    updated = {};
    roleExamples = 0;
    const auto index = juce::JSON::parse (root.getChildFile ("index.json"));
    // Another version, or measured another way: as if there were none (everything is measured again).
    if (index["kind"].toString() != indexKind || (int)index["version"] != version ||
        (int)index["descriptorsVersion"] != sounds::descriptorsVersion)
        return;
    updated = juce::Time::fromISO8601 (index["updated"].toString());
    roleExamples = (int)index["roleExamples"];
    if (const auto* folders = index["sampleFolders"].getArray())
        for (const auto& f : *folders)
            sampleFolders.add (f.toString());
    if (const auto* list = index["instruments"].getArray())
        for (const auto& v : *list)
        {
            Instrument i;
            i.source = sourceFromVar (v);
            if (i.source.key.isEmpty())
                continue;
            i.complete = (bool)v["complete"];
            i.presetFiles = (bool)v["presetFiles"];
            i.problem = v["problem"].toString();
            i.measuredWith = (int)v["measuredWith"];
            i.updated = juce::Time::fromISO8601 (v["updated"].toString());
            if (const auto* bad = v["problemPresets"].getArray())
                for (const auto& b : *bad)
                    i.problemPresets.add (b.toString());
            const auto file = juce::JSON::parse (instrumentFile (i.source.key));
            if ((int)file["version"] == version && file["key"].toString() == i.source.key)
            {
                if (const auto* sounds = file["sounds"].getArray())
                    i.sounds = *sounds;
            }
            else
                i.complete = false; // its file is missing or older: measured again
            instruments[i.source.key] = std::move (i);
        }
    const auto kept = juce::JSON::parse (root.getChildFile ("samples.json"));
    if ((int)kept["version"] == version)
        if (const auto* files = kept["files"].getArray())
            for (const auto& f : *files)
            {
                Sample s;
                s.path = f["path"].toString();
                s.size = (juce::int64)f["size"];
                s.modified = (juce::int64)f["modified"];
                s.info = f;
                if (s.path.isNotEmpty())
                    samples.push_back (std::move (s));
            }
}

bool SoundIndex::saveIndex() const
{
    juce::Array<juce::var> list;
    for (const auto& [key, i] : instruments)
    {
        auto v = sourceToVar (i.source);
        auto* o = v.getDynamicObject();
        o->setProperty ("complete", i.complete);
        o->setProperty ("presetFiles", i.presetFiles);
        if (i.problem.isNotEmpty())
            o->setProperty ("problem", i.problem);
        juce::Array<juce::var> bad;
        for (const auto& b : i.problemPresets)
            bad.add (b);
        o->setProperty ("problemPresets", bad);
        o->setProperty ("sounds", i.sounds.size());
        o->setProperty ("measuredWith", i.measuredWith);
        o->setProperty ("updated", i.updated.toISO8601 (true));
        list.add (v);
    }
    juce::Array<juce::var> folders;
    for (const auto& f : sampleFolders)
        folders.add (f);
    const auto counts = summarize();
    return writeJson (root.getChildFile ("index.json"),
                      object ({ { "kind", indexKind },
                                { "version", version },
                                { "descriptorsVersion", sounds::descriptorsVersion },
                                { "updated", updated.toISO8601 (true) },
                                { "instruments", list },
                                { "sampleFolders", folders },
                                { "roleExamples", roleExamples },
                                { "summary", object ({ { "instruments", counts.instruments },
                                                       { "presets", counts.presets },
                                                       { "samples", counts.samples },
                                                       { "problems", counts.problems } }) } }));
}

bool SoundIndex::saveInstrument (const Instrument& i) const
{
    return writeJson (instrumentFile (i.source.key),
                      object ({ { "version", version }, { "key", i.source.key }, { "sounds", i.sounds } }));
}

bool SoundIndex::saveSamples() const
{
    juce::Array<juce::var> files;
    for (const auto& s : samples)
    {
        juce::var v = s.info.isObject() ? s.info : juce::var (new juce::DynamicObject());
        // A copy, so the in-memory entry keeps its own object.
        auto copy = juce::JSON::parse (juce::JSON::toString (v, true));
        auto* o = copy.getDynamicObject();
        if (o == nullptr)
            continue;
        o->setProperty ("path", s.path);
        o->setProperty ("size", s.size);
        o->setProperty ("modified", s.modified);
        files.add (copy);
    }
    return writeJson (root.getChildFile ("samples.json"), object ({ { "version", version }, { "files", files } }));
}

bool SoundIndex::removeAll()
{
    instruments.clear();
    samples.clear();
    sampleFolders.clear();
    updated = {};
    roleExamples = 0;
    return ! root.exists() || root.deleteRecursively();
}

SoundIndex::Summary SoundIndex::summarize() const
{
    Summary s;
    s.updated = updated;
    for (const auto& [key, i] : instruments)
    {
        if (i.problem.isNotEmpty())
            ++s.problems;
        else if (! i.sounds.isEmpty())
            ++s.instruments;
        s.problems += i.problemPresets.size();
        for (const auto& sound : i.sounds)
            if (! sound.hasProperty ("problem"))
                ++s.presets;
    }
    for (const auto& sample : samples)
    {
        if (sample.info.hasProperty ("problem"))
            ++s.problems;
        else if (sample.info["kind"].toString() != "long")
            ++s.samples;
    }
    return s;
}

SoundIndex::Summary SoundIndex::readSummary (const juce::File& folder)
{
    Summary s;
    const auto index = juce::JSON::parse (folder.getChildFile ("index.json"));
    if (index["kind"].toString() != indexKind || (int)index["version"] != version ||
        (int)index["descriptorsVersion"] != sounds::descriptorsVersion)
        return s;
    const auto& counts = index["summary"];
    s.instruments = counts["instruments"];
    s.presets = counts["presets"];
    s.samples = counts["samples"];
    s.problems = counts["problems"];
    s.updated = juce::Time::fromISO8601 (index["updated"].toString());
    return s;
}

juce::int64 SoundIndex::getDiskUse() const
{
    juce::int64 bytes = 0;
    if (root.isDirectory())
        for (const auto& entry : juce::RangedDirectoryIterator (root, true, "*", juce::File::findFiles))
            bytes += entry.getFileSize();
    return bytes;
}

bool SoundIndex::Instrument::silentOrFailed() const
{
    if (problem.isNotEmpty() || ! problemPresets.isEmpty())
        return true;
    for (const auto& sound : sounds)
        if ((bool)sound["silent"] || sound.hasProperty ("problem"))
            return true;
    return false;
}

bool SoundIndex::needsVisit (const SoundSource& source) const
{
    const auto found = instruments.find (source.key);
    if (found == instruments.end())
        return true;
    const auto& known = found->second;
    if (known.source.fingerprint != source.fingerprint)
        return true;
    // Silent or failed with an older helper, which may have measured it too soon or given up too early.
    if (known.measuredWith != helperRevision && known.silentOrFailed())
        return true;
    if (known.problem.isNotEmpty())
        return false; // it did not load: not again until it changes
    return ! known.complete || known.presetFiles;
}

bool SoundIndex::measureAgain (const juce::String& key)
{
    const auto found = instruments.find (key);
    if (found == instruments.end())
        return false;
    auto& i = found->second;
    i.sounds.clear();
    i.problemPresets.clear();
    i.problem.clear();
    i.complete = false;
    saveInstrument (i);
    saveIndex();
    return true;
}

int SoundIndex::measureAgainSilentOrFailed()
{
    juce::StringArray keys;
    for (const auto& [key, i] : instruments)
        if (i.silentOrFailed())
            keys.add (key);
    for (const auto& key : keys)
        measureAgain (key);
    return keys.size();
}

std::vector<SoundIndex::SampleFile> SoundIndex::listSampleFiles (const juce::Array<juce::File>& folders,
                                                                 const std::function<bool()>& shouldStop)
{
    constexpr juce::int64 largest = (juce::int64)200 * 1024 * 1024;
    std::map<juce::String, SampleFile> found;
    for (const auto& folder : folders)
    {
        if (! folder.isDirectory())
            continue;
        for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
        {
            if (shouldStop && shouldStop())
                return {};
            const auto& file = entry.getFile();
            if (! isSampleFile (file) || entry.getFileSize() <= 0 || entry.getFileSize() > largest || entry.isHidden())
                continue;
            found[file.getFullPathName()] = { file, entry.getFileSize(), entry.getModificationTime().toMilliseconds() };
        }
    }
    std::vector<SampleFile> result;
    result.reserve (found.size());
    for (auto& [path, f] : found)
        result.push_back (f);
    return result;
}

std::vector<SoundIndex::SampleFile> SoundIndex::changedSamples (const std::vector<SampleFile>& files) const
{
    std::map<juce::String, const Sample*> known;
    for (const auto& s : samples)
        known[s.path] = &s;
    std::vector<SampleFile> changed;
    for (const auto& f : files)
    {
        const auto k = known.find (f.file.getFullPathName());
        if (k == known.end() || k->second->size != f.size || k->second->modified != f.modified)
            changed.push_back (f);
    }
    return changed;
}

int SoundIndex::learnRoles()
{
    sounds::RoleGuess guess;
    std::vector<std::pair<juce::var, int>> all; // each sound, and its place among the examples (-1: none)
    for (auto& [key, instrument] : instruments)
        for (auto& sound : instrument.sounds)
        {
            if (sound.hasProperty ("problem") || (bool)sound["silent"])
                continue;
            int at = -1;
            if (sound["roleFrom"].toString() == "name")
            {
                at = guess.size();
                guess.add (sound, sound["role"].toString());
            }
            all.emplace_back (sound, at);
        }
    roleExamples = guess.canLearn() ? guess.size() : 0;
    guess.prepare();
    int changed = 0;
    std::set<const juce::DynamicObject*> touched;
    for (auto& [sound, at] : all)
    {
        auto* o = sound.getDynamicObject();
        const auto rules =
            sound.hasProperty ("ruleRole") ? sound["ruleRole"].toString() : sound["soundRole"].toString();
        // An example is guessed from the others only, so Show sounds tells honestly where they differ.
        const auto learnt = roleExamples > 0 ? guess.guess (sound, 15, at) : juce::String();
        const auto role = learnt.isNotEmpty() ? learnt : rules;
        const auto from = learnt.isNotEmpty() ? "learnt" : "rules";
        if (sound["soundRole"].toString() == role && sound["soundRoleFrom"].toString() == from)
            continue;
        o->setProperty ("ruleRole", rules);
        o->setProperty ("soundRole", role);
        o->setProperty ("soundRoleFrom", from);
        if (sound["roleFrom"].toString() != "name")
            o->setProperty ("role", role);
        touched.insert (o);
        ++changed;
    }
    for (auto& [key, instrument] : instruments)
        if (std::any_of (instrument.sounds.begin(), instrument.sounds.end(),
                         [&] (const juce::var& v) { return touched.contains (v.getDynamicObject()); }))
            saveInstrument (instrument);
    return changed;
}

std::vector<SoundIndex::Row> SoundIndex::rows() const
{
    std::vector<Row> result;
    for (const auto& [key, i] : instruments)
        for (const auto& s : i.sounds)
        {
            Row row;
            row.name = s["name"].toString();
            row.source = i.source.name;
            if (s.hasProperty ("problem"))
            {
                row.role = "problem";
                row.detail = s["problem"].toString();
            }
            else
            {
                row.role = s["role"].toString();
                const auto d = sounds::descriptorsFromVar (s["sound"]);
                juce::StringArray detail;
                if (! (bool)s["silent"])
                {
                    detail.add ("attack " + timeText (d.attack));
                    detail.add (d.sustain >= 0.25 ? juce::String ("sustained") : "decays in " + timeText (d.decay));
                    detail.add ("brightness " + juce::String (juce::roundToInt (d.brightness)) + " Hz");
                    detail.add ((bool)s["tracksPitch"] ? noteName ((int)s["low"]) + "-" + noteName ((int)s["high"])
                                                       : juce::String ("unpitched"));
                    if ((bool)s["polyphonic"])
                        detail.add ("chords");
                    if (s["roleFrom"].toString() == "name" && s["soundRole"].toString() != row.role)
                        detail.add ("sounds like " + s["soundRole"].toString());
                }
                else
                    detail.add ("silent");
                row.detail = detail.joinIntoString (", ");
            }
            result.push_back (row);
        }
    for (const auto& sample : samples)
    {
        Row row;
        const juce::File file (sample.path);
        row.name = file.getFileName();
        row.source = file.getParentDirectory().getFileName();
        row.sample = true;
        const auto& info = sample.info;
        if (info.hasProperty ("problem"))
        {
            row.role = "problem";
            row.detail = info["problem"].toString();
        }
        else
        {
            row.role = info["class"].toString();
            juce::StringArray detail;
            detail.add (info["kind"].toString());
            detail.add (timeText ((double)info["duration"]));
            if (info.hasProperty ("tempo"))
                detail.add (juce::String ((double)info["tempo"], 1) + " BPM");
            if (info.hasProperty ("key"))
                detail.add (info["key"].toString());
            if (info["classFrom"].toString() == "name" && info["soundClass"].toString() != row.role)
                detail.add ("sounds like " + info["soundClass"].toString());
            row.detail = detail.joinIntoString (", ");
        }
        result.push_back (row);
    }
    return result;
}

//==============================================================================
std::atomic<int> SoundIndexer::running { 0 };

SoundIndexer::SoundIndexer (juce::File f, Options o)
    : juce::Thread ("Spinus sound index"), folder (std::move (f)), options (std::move (o))
{
}

SoundIndexer::~SoundIndexer()
{
    cancel();
    cancelPendingUpdate();
}

bool SoundIndexer::start (std::vector<SoundSource> s, juce::Array<juce::File> f,
                          std::function<void (const Progress&)> onProgress,
                          std::function<void (const Result&)> onFinished)
{
    if (isThreadRunning())
        return false;
    sources = std::move (s);
    folders = std::move (f);
    progressCallback = std::move (onProgress);
    finishedCallback = std::move (onFinished);
    {
        const std::scoped_lock l (lock);
        progress = {};
        result.reset();
    }
    ++running;
    if (! startThread (juce::Thread::Priority::low))
    {
        --running;
        return false;
    }
    return true;
}

void SoundIndexer::cancel()
{
    if (isThreadRunning())
        stopThread (20000);
}

SoundIndexer::Progress SoundIndexer::getProgress() const
{
    const std::scoped_lock l (lock);
    return progress;
}

void SoundIndexer::run()
{
    SoundIndex index (folder);
    index.load();
    auto finished = update (
        index, options, sources, folders, [this] { return threadShouldExit(); },
        [this] (const Progress& p)
        {
            {
                const std::scoped_lock l (lock);
                progress = p;
            }
            triggerAsyncUpdate();
        });
    {
        const std::scoped_lock l (lock);
        result = finished;
    }
    --running;
    triggerAsyncUpdate();
}

void SoundIndexer::handleAsyncUpdate()
{
    Progress p;
    std::optional<Result> r;
    {
        const std::scoped_lock l (lock);
        p = progress;
        r = result;
        if (r)
            result.reset();
    }
    if (progressCallback)
        progressCallback (p);
    if (r && finishedCallback)
        finishedCallback (*r);
}

//==============================================================================
SoundIndexer::Run SoundIndexer::runHelper (const Options& options, const juce::var& request, bool withHostLibraries,
                                           const std::function<void (const juce::var&)>& onLine,
                                           const std::function<bool()>& shouldStop)
{
    Run run;
    if (! options.helper.existsAsFile())
    {
        run.exitCode = -1;
        return run;
    }
    const juce::TemporaryFile requestFile (".json"), outputFile (".jsonl");
    requestFile.getFile().replaceWithText (juce::JSON::toString (request, true));
    const juce::StringArray args { "--sound-index", requestFile.getFile().getFullPathName(), "--output",
                                   outputFile.getFile().getFullPathName() };
    juce::StringArray command;
    if (withHostLibraries && options.hostLibraries)
        command = options.hostLibraries->command (options.helper, args);
    else
    {
        command.add (options.helper.getFullPathName());
        command.addArray (args);
    }
    core::ChildProcess process;
    if (! process.start (command, 0))
    {
        run.exitCode = -1;
        return run;
    }
    // A Windows plugin may ask something in a window of its own while it loads: brought to the front.
    std::optional<plugins::PluginDialogs::ScopedWait> waitForWindowsPlugin;
    if (plugins::PluginDialogs::isWindowsPlugin (request["file"].toString()))
        waitForWindowsPlugin.emplace();

    juce::int64 offset = 0;
    std::string pending;
    auto lastNews = juce::Time::getMillisecondCounterHiRes();
    auto readNew = [&]
    {
        juce::FileInputStream in (outputFile.getFile());
        if (! in.openedOk() || in.getTotalLength() <= offset)
            return;
        in.setPosition (offset);
        juce::MemoryBlock bytes;
        in.readIntoMemoryBlock (bytes);
        offset += (juce::int64)bytes.getSize();
        pending.append (static_cast<const char*> (bytes.getData()), bytes.getSize());
        for (auto end = pending.find ('\n'); end != std::string::npos; end = pending.find ('\n'))
        {
            const auto line = juce::String::fromUTF8 (pending.data(), (int)end);
            pending.erase (0, end + 1);
            const auto v = juce::JSON::parse (line);
            if (! v.isObject())
                continue;
            run.started = true;
            lastNews = juce::Time::getMillisecondCounterHiRes();
            if (v.hasProperty ("begin"))
                run.stuckOn = v["begin"].toString();
            if (v.hasProperty ("sound") || v.hasProperty ("sample") || v.hasProperty ("rendered"))
                run.stuckOn.clear();
            if (v.hasProperty ("done"))
                run.finished = true;
            if (onLine)
                onLine (v);
        }
    };
    for (;;)
    {
        const bool ended = process.waitForProcessToFinish (20);
        readNew();
        if (ended || run.finished)
            break;
        if (shouldStop && shouldStop())
        {
            process.kill();
            break;
        }
        const auto timeout = run.started ? options.soundTimeoutMs : options.loadTimeoutMs;
        if (juce::Time::getMillisecondCounterHiRes() - lastNews > timeout)
        {
            process.kill();
            run.hung = true;
            break;
        }
    }
    process.waitForProcessToFinish (5000);
    if (! process.isRunning())
        run.exitCode = (int)process.getExitCode();
    else
        process.kill();
    return run;
}

namespace
{
/** Measures one instrument's new and changed presets, restarting the helper after a crash or hang. */
void visitInstrument (SoundIndex& index, const SoundIndexer::Options& options, const SoundSource& source,
                      const std::function<bool()>& shouldStop, SoundIndexer::Progress& progress,
                      const std::function<void (const SoundIndexer::Progress&)>& report, SoundIndexer::Result& result)
{
    auto& entry = index.instruments[source.key];
    if (entry.source.fingerprint != source.fingerprint)
    {
        // A new or changed plugin: everything measured again.
        entry.sounds.clear();
        entry.problemPresets.clear();
    }
    else if (entry.measuredWith != SoundIndex::helperRevision && entry.silentOrFailed())
    {
        // Measured by an older helper: its silent sounds and the presets it gave up on are measured again.
        for (int i = entry.sounds.size(); --i >= 0;)
            if ((bool)entry.sounds[i]["silent"] || entry.sounds[i].hasProperty ("problem"))
                entry.sounds.remove (i);
        entry.problemPresets.clear();
    }
    entry.measuredWith = SoundIndex::helperRevision;
    entry.source = source;
    entry.problem.clear();
    entry.complete = false;

    std::map<juce::String, int> at; // a preset's place in entry.sounds
    for (int i = 0; i < entry.sounds.size(); ++i)
        at[entry.sounds[i]["id"].toString()] = i;
    std::optional<std::set<juce::String>> listed;
    int sinceSaved = 0;
    progress.current = source.name;
    report (progress);

    bool defaultOnly = false;
    for (int attempt = 0;; ++attempt)
    {
        auto* known = new juce::DynamicObject();
        for (const auto& s : entry.sounds)
            known->setProperty (s["id"].toString(), s["version"]);
        juce::Array<juce::var> skip;
        for (const auto& b : entry.problemPresets)
            skip.add (b);
        const auto request = object ({ { "mode", "instrument" },
                                       { "format", source.format },
                                       { "file", source.file },
                                       { "name", source.pluginName.isNotEmpty() ? source.pluginName : source.name },
                                       { "uniqueId", source.uniqueId },
                                       { "stockType", source.stockType },
                                       { "known", juce::var (known) },
                                       { "skip", skip },
                                       { "defaultOnly", defaultOnly } });
        const auto run = SoundIndexer::runHelper (
            options, request, source.hostLibraries,
            [&] (const juce::var& line)
            {
                if (const auto& presets = line["presets"]; presets.isObject())
                {
                    listed.emplace();
                    if (const auto* list = presets["list"].getArray())
                        for (const auto& p : *list)
                            listed->insert (p["id"].toString());
                    entry.presetFiles = (bool)presets["presetFiles"];
                }
                if (line.hasProperty ("error"))
                    entry.problem = line["error"].toString();
                if (const auto& sound = line["sound"]; sound.isObject())
                {
                    const auto id = sound["id"].toString();
                    if (const auto found = at.find (id); found != at.end())
                        entry.sounds.set (found->second, sound);
                    else
                    {
                        at[id] = entry.sounds.size();
                        entry.sounds.add (sound);
                    }
                    ++result.soundsMeasured;
                    ++progress.sounds;
                    progress.current = source.name + ": " + sound["name"].toString();
                    report (progress);
                    // Long ones are kept as they go, so a stop (or a crash of Spinus) loses little.
                    if (++sinceSaved >= 50)
                    {
                        index.saveInstrument (entry);
                        sinceSaved = 0;
                    }
                }
            },
            shouldStop);

        if (run.finished)
        {
            entry.complete = entry.problem.isEmpty();
            if (entry.problem.isNotEmpty())
                ++result.problems;
            break;
        }
        if (shouldStop && shouldStop())
        {
            result.cancelled = true;
            break;
        }
        const auto why = run.exitCode == -1 ? juce::String ("Spinus's helper program is missing")
                         : run.hung         ? "stopped answering"
                                            : "crashed";
        if (run.stuckOn.isNotEmpty() && attempt < options.retries)
        {
            // That preset is left out; the rest are measured in a new helper.
            core::Logs::warning ("sounds", source.name + " " + why + " on the preset " + run.stuckOn);
            entry.problemPresets.addIfNotAlreadyThere (run.stuckOn);
            ++result.problems;
            continue;
        }
        if (run.stuckOn.isEmpty() && ! defaultOnly && attempt < options.retries)
        {
            // It crashed or hung before any preset was played (loading, finding its presets, or its own
            // loading in the background): tried once more with only its sound as it opens, so it can still
            // be chosen.
            core::Logs::warning ("sounds", source.name + " " + why +
                                               " before its first preset: trying its "
                                               "default sound only");
            defaultOnly = true;
            continue;
        }
        entry.problem = run.stuckOn.isNotEmpty() ? source.name + " " + why + " on too many presets"
                        : run.started            ? source.name + " " + why + " while listing its presets"
                                                 : source.name + " " + why + " while loading";
        core::Logs::warning ("sounds", entry.problem);
        ++result.problems;
        break;
    }

    // Presets that are gone (only known when the helper listed them).
    if (listed)
        for (int i = entry.sounds.size(); --i >= 0;)
            if (! listed->contains (entry.sounds[i]["id"].toString()))
            {
                entry.sounds.remove (i);
                ++result.soundsRemoved;
            }
    entry.updated = juce::Time::getCurrentTime();
    index.saveInstrument (entry);
    ++result.instrumentsVisited;
}
} // namespace

SoundIndexer::Result SoundIndexer::update (SoundIndex& index, const Options& options,
                                           const std::vector<SoundSource>& sources,
                                           const juce::Array<juce::File>& sampleFolders,
                                           const std::function<bool()>& shouldStop,
                                           const std::function<void (const Progress&)>& onProgress)
{
    Result result;
    auto stop = [&] { return shouldStop && shouldStop(); };
    auto report = [&] (const Progress& p)
    {
        if (onProgress)
            onProgress (p);
    };

    // Instruments: those gone leave the index; new and changed ones are visited.
    std::set<juce::String> present;
    std::vector<const SoundSource*> visits;
    for (const auto& s : sources)
        if (present.insert (s.key).second)
        {
            if (index.needsVisit (s))
                visits.push_back (&s);
            else
                index.instruments[s.key].source = s; // its name and place may have changed, not its sound
        }
    for (auto i = index.instruments.begin(); i != index.instruments.end();)
        if (! present.contains (i->first))
        {
            result.soundsRemoved += i->second.sounds.size();
            i = index.instruments.erase (i);
        }
        else
            ++i;
    if (index.getFolder().getChildFile ("instruments").isDirectory())
        for (const auto& file :
             index.getFolder().getChildFile ("instruments").findChildFiles (juce::File::findFiles, false, "*.json"))
            if (! index.instruments.contains (file.getFileNameWithoutExtension()))
                file.deleteFile();

    Progress progress;
    progress.stage = "instruments";
    progress.total = (int)visits.size();
    report (progress);
    for (const auto* source : visits)
    {
        if (stop())
        {
            result.cancelled = true;
            break;
        }
        visitInstrument (index, options, *source, shouldStop, progress, report, result);
        ++progress.done;
        report (progress);
        index.saveIndex();
        if (result.cancelled)
            break;
    }

    // The role guess, learnt again from the presets whose names say their role.
    index.learnRoles();

    // Samples: the files in the Browser's sample folders.
    if (! result.cancelled)
    {
        index.sampleFolders.clear();
        for (const auto& f : sampleFolders)
            index.sampleFolders.add (f.getFullPathName());
        const auto files = SoundIndex::listSampleFiles (sampleFolders, shouldStop);
        if (stop())
            result.cancelled = true;
        else
        {
            std::set<juce::String> onDisk;
            for (const auto& f : files)
                onDisk.insert (f.file.getFullPathName());
            const auto before = index.samples.size();
            std::erase_if (index.samples, [&] (const SoundIndex::Sample& s) { return ! onDisk.contains (s.path); });
            result.samplesRemoved += (int)(before - index.samples.size());

            const auto changed = index.changedSamples (files);
            std::map<juce::String, size_t> at;
            for (size_t i = 0; i < index.samples.size(); ++i)
                at[index.samples[i].path] = i;
            auto keep = [&] (const SoundIndex::SampleFile& f, const juce::var& info)
            {
                SoundIndex::Sample s { f.file.getFullPathName(), f.size, f.modified, info };
                if (const auto found = at.find (s.path); found != at.end())
                    index.samples[found->second] = std::move (s);
                else
                {
                    at[s.path] = index.samples.size();
                    index.samples.push_back (std::move (s));
                }
            };

            progress = {};
            progress.stage = "samples";
            progress.total = (int)changed.size();
            report (progress);
            for (size_t next = 0; next < changed.size();)
            {
                if (stop())
                {
                    result.cancelled = true;
                    break;
                }
                const auto count = juce::jmin (changed.size() - next, (size_t)juce::jmax (1, options.samplesPerRun));
                std::map<juce::String, const SoundIndex::SampleFile*> batch;
                juce::Array<juce::var> paths;
                for (size_t i = next; i < next + count; ++i)
                {
                    batch[changed[i].file.getFullPathName()] = &changed[i];
                    paths.add (changed[i].file.getFullPathName());
                }
                size_t handled = 0;
                const auto run = runHelper (
                    options, object ({ { "mode", "samples" }, { "files", paths } }), false,
                    [&] (const juce::var& line)
                    {
                        if (const auto& sample = line["sample"]; sample.isObject())
                            if (const auto found = batch.find (sample["path"].toString()); found != batch.end())
                            {
                                keep (*found->second, sample);
                                ++handled;
                                ++result.samplesMeasured;
                                ++progress.done;
                                ++progress.sounds;
                                progress.current = found->second->file.getFileName();
                                report (progress);
                            }
                    },
                    shouldStop);
                if (! run.finished && stop())
                {
                    result.cancelled = true;
                    break;
                }
                if (! run.finished && handled < count)
                {
                    // The file it was reading is a problem; the batch carries on after it.
                    const auto stuck = batch.find (run.stuckOn);
                    const auto& file = stuck != batch.end() ? *stuck->second : changed[next + handled];
                    const auto why = run.exitCode == -1 ? juce::String ("Spinus's helper program is missing")
                                     : run.hung         ? "Spinus's helper stopped answering while reading it"
                                                        : "Spinus's helper crashed while reading it";
                    core::Logs::warning ("sounds", file.file.getFullPathName() + ": " + why);
                    keep (file, object ({ { "problem", why } }));
                    ++handled;
                    ++result.problems;
                    ++progress.done;
                    if (run.exitCode == -1)
                        break;
                }
                next += juce::jmax ((size_t)1, handled);
                index.saveSamples();
            }
            std::sort (index.samples.begin(), index.samples.end(),
                       [] (const SoundIndex::Sample& a, const SoundIndex::Sample& b) { return a.path < b.path; });
            index.saveSamples();
        }
    }

    index.updated = juce::Time::getCurrentTime();
    index.saveIndex();
    return result;
}

} // namespace spinus
