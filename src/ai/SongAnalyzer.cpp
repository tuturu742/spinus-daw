#include "SongAnalyzer.h"

#include "AnalysisJobs.h"
#include "session/PartSounds.h"
#include "session/SongPreparation.h"

#include <algorithm>
#include <map>

namespace spinus::ai
{

namespace
{
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : items)
        o->setProperty (k, v);
    return juce::var (o);
}

using parts::separationModel; // 6 stems: guitars and piano apart from the other instruments
} // namespace

//==============================================================================
/** Reads a song's contents (its digest names it in the analysis library) and, when its style was not measured
    before, decodes it, off the message thread. */
class SongAnalyzer::Preparer final : public juce::Thread
{
public:
    Preparer (juce::File songFile, AnalysisLibrary lib, juce::File work, juce::String known,
              std::function<void()> whenDone)
        : juce::Thread ("Spinus song preparer"), file (std::move (songFile)), library (std::move (lib)),
          workFolderRoot (std::move (work)), digest (std::move (known)), done (std::move (whenDone))
    {
        startThread();
    }

    ~Preparer() override { stopThread (10000); }

    void run() override
    {
        if (digest.isEmpty())
            digest = contentDigest (file);
        if (threadShouldExit())
            return;
        entry = library.read (digest);
        if (const auto& found = entry["features"]; found.isObject() && (int)found["version"] == featuresVersion)
            features = found;
        else
        {
            prepared = preparedSongFile (workFolderRoot.getChildFile (digest.substring (0, 16)), file, digest);
            result = prepareSong (file, prepared, [this] { return threadShouldExit(); }, progress);
        }
        if (! threadShouldExit())
            juce::MessageManager::callAsync (done);
    }

    juce::File file;
    AnalysisLibrary library;
    juce::File workFolderRoot;
    juce::String digest;
    std::function<void()> done;
    juce::var entry;    // what the library has of the song
    juce::var features; // its style, measured before
    juce::File prepared;
    juce::Result result = juce::Result::ok();
    std::atomic<double> progress { 0.0 };
};

/** What is known so far of the song being analyzed. */
struct SongAnalyzer::Work
{
    juce::String digest, name;
    juce::var entry; // what the analysis library had of it
    juce::File prepared, folder;
    juce::var rhythm, key, loudness;
    std::map<juce::String, juce::File> stems;
    juce::StringArray queue; // stems still to listen to
    juce::DynamicObject::Ptr parts = new juce::DynamicObject();
    juce::var drums = juce::Array<juce::var>();
};

//==============================================================================
SongAnalyzer::SongAnalyzer (AiService* s, AnalysisLibrary l, juce::File root)
    : service (s), library (std::move (l)), workRoot (std::move (root))
{
}

SongAnalyzer::~SongAnalyzer()
{
    callbacks = {};
    stop();
}

juce::File SongAnalyzer::getWorkFolder (const juce::String& digest) const
{
    return workRoot.getChildFile (digest.substring (0, 16));
}

juce::StringArray SongAnalyzer::neededModels() const
{
    return { beatModel, separationModel, "basic_pitch", "drum_transcriber" };
}

void SongAnalyzer::start (const juce::File& file, const juce::String& knownDigest, Callbacks c)
{
    stop();
    callbacks = std::move (c);
    song = file;
    busy = true;
    say ("Preparing...");
    preparer = std::make_unique<Preparer> (file, library, workRoot, knownDigest,
                                           [this, token = run]
                                           {
                                               if (*token)
                                                   prepared();
                                           });
}

void SongAnalyzer::stop()
{
    *run = false;
    run = std::make_shared<bool> (true);
    if (service != nullptr && job.isNotEmpty() && job != "starting")
        service->call ("ai.v1.job.cancel", object ({ { "job", job } }), {});
    clear (true);
}

void SongAnalyzer::clear (bool deleteWork)
{
    job.clear();
    task = nullptr;
    preparer = nullptr;
    if (work != nullptr && deleteWork)
        work->folder.deleteRecursively(); // the decoded song and its stems as WAV: large, and kept as FLAC
    work = nullptr;
    busy = false;
}

void SongAnalyzer::say (const juce::String& words, double progress)
{
    if (callbacks.step)
        callbacks.step (words, progress);
}

void SongAnalyzer::finish (const juce::var& features, bool keptBefore)
{
    clear (true);
    if (const auto done = callbacks.done)
        done (features, keptBefore);
}

void SongAnalyzer::fail (const juce::String& why)
{
    clear (true);
    if (const auto failed = callbacks.failed)
        failed (why);
}

void SongAnalyzer::hold (const Needs& needs)
{
    clear (false); // the decoded song stays in the work folder: not decoded again
    if (const auto held = callbacks.held)
        held (needs);
}

//==============================================================================
void SongAnalyzer::prepared()
{
    if (preparer == nullptr)
        return;
    const auto finished = std::move (preparer);
    if (callbacks.read)
        callbacks.read (finished->digest);
    if (finished->features.isObject())
        return finish (finished->features, true);
    if (finished->result.failed())
        return fail (finished->result.getErrorMessage());
    work = std::make_unique<Work>();
    work->digest = finished->digest;
    work->prepared = finished->prepared;
    work->folder = getWorkFolder (finished->digest);
    work->entry = finished->entry;
    work->name = song.getFileName();
    checkReady ([this] { findTempo(); });
}

/** AI on, the analysis engine installed and the models downloaded; otherwise says what is missing. */
void SongAnalyzer::checkReady (const std::function<void()>& then)
{
    if (service == nullptr || ! service->isEnabled())
        return hold ({ Needs::What::ai, "AI is off.", {}, 0 });
    // What the library has already (from the Analyzer, or an earlier time) needs no models.
    const auto& entry = work->entry;
    const auto kept = library.findStems (work->digest, separationModel);
    const bool allKept =
        entry["rhythm"].isObject() && ! kept.empty() &&
        std::all_of (
            kept.begin(), kept.end(),
            [&entry] (const auto& stem)
            {
                return stem.first == "drums"
                           ? entry["drums"][separationModel].isArray()
                           : entry["notes"][juce::Identifier (juce::String (separationModel) + "/" + stem.first)]
                                 .isArray();
            });
    if (allKept)
        return then();
    say ("Checking the analysis engine...");
    checkAnalysisEngine (*service, run,
                         [this, then] (const EngineCheck& engine)
                         {
                             if (engine.error.isNotEmpty())
                                 return hold ({ Needs::What::service, engine.error, {}, 0 });
                             if (! engine.installed)
                                 return hold ({ Needs::What::engine, "The analysis engine is not installed.", {}, 0 });
                             beatModel = engine.gpu ? "beat_this_final0" : "beat_this_small0";
                             Needs needs { Needs::What::models, "Model files are missing.", {}, 0 };
                             const auto needed = neededModels();
                             if (auto* models = engine.models["models"].getArray())
                                 for (auto& m : *models)
                                     if (needed.contains (m["id"].toString()) && ! (bool)m["downloaded"])
                                     {
                                         needs.models.push_back (
                                             { m["id"].toString(), m["name"].toString(), (juce::int64)m["size"] });
                                         needs.downloadSize += (juce::int64)m["size"];
                                     }
                             if (! needs.models.empty())
                                 return hold (needs);
                             then();
                         });
}

void SongAnalyzer::runStep (const juce::String& method, const juce::var& params, const juce::String& words,
                            const std::function<void (const juce::var&)>& done)
{
    say (words);
    job = "starting";
    if (callbacks.ran)
        callbacks.ran (method);
    runJob (
        *service, method, params, run, [this] (const juce::String& started) { job = started; },
        [this, done] (const juce::var& result, const juce::String& error)
        {
            job.clear();
            if (error.isNotEmpty())
                return fail (error);
            done (result);
        },
        [this, words] (double doneSoFar, double total, const juce::String& step)
        { say (step.isNotEmpty() ? step + "..." : words, total > 0.0 ? doneSoFar / total : -1.0); });
}

void SongAnalyzer::runTask (const juce::String& words, BackgroundTask::Work what, const std::function<void()>& then)
{
    say (words);
    task = std::make_unique<BackgroundTask> ("Spinus song analysis stems", std::move (what),
                                             [then, token = run] (const juce::Result&)
                                             {
                                                 if (*token)
                                                     then();
                                             });
}

void SongAnalyzer::findTempo()
{
    if (const auto& kept = work->entry["rhythm"]; kept.isObject())
    {
        work->rhythm = kept;
        return findKey();
    }
    runStep (
        "ai.v1.analyze.tempo",
        object ({ { "file", work->prepared.getFullPathName() }, { "cacheFolder", work->folder.getFullPathName() } }),
        "Finding the beat...",
        [this] (const juce::var& result)
        {
            work->rhythm = result;
            library.set (work->digest, work->name, "rhythm", result);
            findKey();
        });
}

void SongAnalyzer::findKey()
{
    if (const auto& kept = work->entry["key"]; kept.isObject())
    {
        work->key = kept;
        return measureLoudness();
    }
    runStep ("ai.v1.analyze.key", object ({ { "file", work->prepared.getFullPathName() } }), "Finding the key...",
             [this] (const juce::var& result)
             {
                 work->key = result;
                 library.set (work->digest, work->name, "key", result);
                 measureLoudness();
             });
}

void SongAnalyzer::measureLoudness()
{
    if (const auto& kept = work->entry["loudness"]; kept.isObject())
    {
        work->loudness = kept;
        return separate();
    }
    juce::Array<juce::var> inserts { object ({ { "name", "song" }, { "file", work->prepared.getFullPathName() } }) };
    runStep ("ai.v1.mix.analyze", object ({ { "inserts", inserts } }), "Measuring the loudness...",
             [this] (const juce::var& result)
             {
                 work->loudness = result["inserts"]["song"];
                 library.set (work->digest, work->name, "loudness", work->loudness);
                 separate();
             });
}

void SongAnalyzer::separate()
{
    // Stems kept from before (here or in the Analyzer, with the same model) are used again.
    if (! library.findStems (work->digest, separationModel).empty())
    {
        auto restored = std::make_shared<std::map<juce::String, juce::File>>();
        return runTask (
            "Using the stems kept before...",
            [lib = library, digest = work->digest, folder = work->folder, restored] (const std::function<bool()>& stop)
            { return lib.restoreStems (digest, separationModel, folder, *restored, stop); },
            [this, restored]
            {
                task = nullptr;
                if (restored->empty())
                    return fail ("The stems kept before cannot be read.");
                work->stems = *restored;
                for (const auto& [name, file] : work->stems)
                    work->queue.add (name);
                checkCount();
            });
    }
    runStep ("ai.v1.stems.separate",
             object ({ { "file", work->prepared.getFullPathName() },
                       { "model", separationModel },
                       { "cacheFolder", work->folder.getFullPathName() },
                       { "quiet", callbacks.playing && callbacks.playing() } }),
             "Separating...",
             [this] (const juce::var& result)
             {
                 if (auto* o = result["stems"].getDynamicObject())
                     for (auto& property : o->getProperties())
                     {
                         work->stems[property.name.toString()] = juce::File (property.value.toString());
                         work->queue.add (property.name.toString());
                     }
                 keepStems();
             });
}

void SongAnalyzer::keepStems()
{
    // Kept as FLAC in the library, for the Analyzer and the next time (not needed to go on, if it fails).
    runTask (
        "Keeping the stems...",
        [lib = library, digest = work->digest, stems = work->stems] (const std::function<bool()>& stop)
        { return lib.keepStems (digest, separationModel, stems, stop); },
        [this]
        {
            task = nullptr;
            checkCount();
        });
}

void SongAnalyzer::checkCount()
{
    const auto& r = work->rhythm;
    if ((double)r["tempo"] < 140.0 || (int)r["beatsPerBar"] != 4 || r["counted"].isString() ||
        ! work->stems.contains ("drums"))
        return transcribeNext();
    runStep ("ai.v1.analyze.count",
             object ({ { "file", work->stems["drums"].getFullPathName() },
                       { "beats", r["beats"] },
                       { "firstDownbeat", r["firstDownbeat"] },
                       { "beatsPerBar", r["beatsPerBar"] },
                       { "tempo", r["tempo"] },
                       { "cacheFolder", work->folder.getFullPathName() } }),
             "Checking the tempo...",
             [this] (const juce::var& result)
             {
                 if ((bool)result["slower"] && result["rhythm"].isObject())
                 {
                     work->rhythm = result["rhythm"];
                     library.set (work->digest, work->name, "rhythm", work->rhythm);
                 }
                 transcribeNext();
             });
}

void SongAnalyzer::transcribeNext()
{
    if (work->queue.isEmpty())
        return measureStyle();
    const auto name = work->queue[0];
    work->queue.remove (0);
    const bool drums = name == "drums";
    const auto notesKey = juce::String (separationModel) + "/" + name;
    const auto known =
        drums ? work->entry["drums"][separationModel] : work->entry["notes"][juce::Identifier (notesKey)];
    if (! drums && known.isArray())
    {
        // Found before: nothing to listen to.
        work->parts->setProperty (name, known);
        return transcribeNext();
    }
    auto params = object (
        { { "file", work->stems[name].getFullPathName() },
          { "bass", name == "bass" },
          { "beats", work->rhythm["beats"] },
          { "firstDownbeat", work->rhythm["firstDownbeat"].isVoid() ? juce::var (0.0) : work->rhythm["firstDownbeat"] },
          { "cacheFolder", work->folder.getFullPathName() } });
    if (drums && known.isArray())
    {
        work->drums = known;
        return transcribeNext();
    }
    runStep (drums ? "ai.v1.transcribe.drums" : "ai.v1.transcribe.notes", params,
             drums ? juce::String ("Listening to the drums...") : "Listening for the notes of the " + name + "...",
             [this, name, drums, notesKey] (const juce::var& result)
             {
                 if (drums)
                 {
                     work->drums = result["hits"].isArray() ? result["hits"] : juce::var (juce::Array<juce::var>());
                     library.setIn (work->digest, work->name, "drums", separationModel, work->drums);
                 }
                 else
                 {
                     const auto seconds =
                         result["seconds"].isArray() ? result["seconds"] : juce::var (juce::Array<juce::var>());
                     work->parts->setProperty (name, seconds);
                     library.setIn (work->digest, work->name, "notes", notesKey, seconds);
                 }
                 transcribeNext();
             });
}

void SongAnalyzer::measureStyle()
{
    const auto& rhythm = work->rhythm;
    // The stems themselves too: the drums' grid comes from the drum stem, the key and chords also from the rest.
    auto* stemFiles = new juce::DynamicObject();
    for (const auto& [name, file] : work->stems)
        stemFiles->setProperty (name, file.getFullPathName());
    juce::Array<juce::var> songs { object ({
        { "name", song.getFileNameWithoutExtension() },
        { "file", work->prepared.getFullPathName() },
        { "tempo", rhythm["tempo"] },
        { "beatsPerBar", rhythm["beatsPerBar"] },
        { "firstDownbeat", rhythm["firstDownbeat"] },
        { "beats", rhythm["beats"] },
        { "downbeats", rhythm["downbeats"] },
        { "key", work->key },
        { "loudness", work->loudness },
        { "parts", juce::var (work->parts.get()) },
        { "drums", work->drums },
        { "stems", stemFiles },
    }) };
    runStep ("ai.v1.style.summarize", object ({ { "songs", songs } }), "Measuring its style...",
             [this] (const juce::var& result)
             {
                 const auto features = result["perSong"][0];
                 if (! features.isObject())
                     return fail ("The AI service did not say what it measured.");
                 library.set (work->digest, work->name, "features", features);
                 finish (features, false);
             });
}

//==============================================================================
void downloadModels (AiService& service, std::vector<SongAnalyzer::Needs::Model> models,
                     const std::shared_ptr<bool>& alive,
                     std::function<void (const juce::String& name, double fraction)> progress,
                     std::function<void (const juce::String& error)> done)
{
    if (models.empty())
        return done ({});
    const auto next = models.front();
    models.erase (models.begin());
    if (progress)
        progress (next.name, -1.0);
    runJob (
        service, "ai.v1.stems.download", object ({ { "model", next.id } }), alive, {},
        [&service, models, alive, progress, done] (const juce::var&, const juce::String& error)
        {
            if (error.isNotEmpty())
                return done (error);
            downloadModels (service, models, alive, progress, done);
        },
        [progress, name = next.name] (double soFar, double total, const juce::String&)
        {
            if (progress && total > 0.0)
                progress (name, soFar / total);
        });
}

//==============================================================================
SongAnalysisQueue::SongAnalysisQueue (AiService* service, AnalysisLibrary library, juce::File workRoot)
    : analyzer (service, std::move (library), std::move (workRoot))
{
}

void SongAnalysisQueue::add (const juce::File& song)
{
    for (auto& s : songs)
        if (s.file == song)
        {
            if (s.state == Song::State::done || s.state == Song::State::failed)
            {
                s.state = Song::State::waiting;
                s.step.clear();
                s.progress = -1.0;
            }
            return startNext();
        }
    Song added;
    added.file = song;
    songs.push_back (added);
    startNext();
}

void SongAnalysisQueue::stop()
{
    analyzer.stop();
    current = -1;
    for (auto& s : songs)
        if (s.state == Song::State::waiting || s.state == Song::State::analyzing)
        {
            s.state = Song::State::failed;
            s.step = "Stopped.";
        }
}

void SongAnalysisQueue::startNext()
{
    if (current >= 0)
        return;
    const auto next =
        std::find_if (songs.begin(), songs.end(), [] (const Song& s) { return s.state == Song::State::waiting; });
    if (next == songs.end())
        return;
    current = (int)(next - songs.begin());
    next->state = Song::State::analyzing;
    needs.reset();
    const auto changed = [this]
    {
        if (onChange && current >= 0)
            onChange (songs[(size_t)current]);
    };
    // Each ends the song: tell, then on with the next (after `current` is free again).
    const auto ended = [this] (Song::State state, const juce::String& words)
    {
        auto& s = songs[(size_t)current];
        s.state = state;
        s.step = words;
        s.progress = -1.0;
        const auto finished = s;
        current = -1;
        if (onChange)
            onChange (finished);
    };
    SongAnalyzer::Callbacks callbacks;
    callbacks.step = [this, changed] (const juce::String& words, double progress)
    {
        songs[(size_t)current].step = words;
        songs[(size_t)current].progress = progress;
        changed();
    };
    callbacks.read = [this] (const juce::String& digest) { songs[(size_t)current].digest = digest; };
    callbacks.done = [this, ended] (const juce::var&, bool keptBefore)
    {
        songs[(size_t)current].keptBefore = keptBefore;
        ended (Song::State::done, {});
        startNext();
    };
    callbacks.failed = [this, ended] (const juce::String& why)
    {
        ended (Song::State::failed, why);
        startNext();
    };
    callbacks.held = [this, ended] (const SongAnalyzer::Needs& missing)
    {
        // The songs after it would stop at the same thing.
        needs = missing;
        juce::StringArray names;
        for (const auto& m : missing.models)
            names.add (m.name);
        const auto words = missing.what == SongAnalyzer::Needs::What::models
                               ? "Model files are missing: " + names.joinIntoString (", ") + " (" +
                                     juce::String (juce::roundToInt ((double)missing.downloadSize / 1e6)) + " MB)."
                               : missing.text;
        ended (Song::State::failed, words);
        for (auto& s : songs)
            if (s.state == Song::State::waiting)
            {
                s.state = Song::State::failed;
                s.step = words;
                if (onChange)
                    onChange (s);
            }
    };
    callbacks.playing = playing;
    analyzer.start (next->file, {}, std::move (callbacks));
}

juce::var SongAnalysisQueue::describe() const
{
    juce::Array<juce::var> list;
    for (const auto& s : songs)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", s.file.getFileName());
        switch (s.state)
        {
        case Song::State::waiting:
            o->setProperty ("state", "waiting");
            break;
        case Song::State::analyzing:
            o->setProperty ("state", "analyzing");
            if (s.step.isNotEmpty())
                o->setProperty ("step", s.step);
            if (s.progress >= 0.0)
                o->setProperty ("percent", juce::roundToInt (100.0 * s.progress));
            break;
        case Song::State::done:
            o->setProperty ("state", "done");
            if (s.keptBefore)
                o->setProperty ("analyzedBefore", true);
            break;
        case Song::State::failed:
            o->setProperty ("state", "failed");
            o->setProperty ("problem", s.step);
            break;
        }
        if (s.digest.isNotEmpty())
            o->setProperty ("song", AnalysisLibrary::keyOf (s.digest));
        list.add (juce::var (o));
    }
    return list;
}

//==============================================================================
juce::Array<juce::File> songsIn (const juce::Array<juce::File>& filesAndFolders)
{
    juce::Array<juce::File> songs;
    for (const auto& file : filesAndFolders)
    {
        if (file.isDirectory())
        {
            // An album: its songs, and those one folder down (CD 1, CD 2...).
            juce::Array<juce::File> found;
            for (const auto& child : file.findChildFiles (juce::File::findFiles, false))
                found.add (child);
            for (const auto& folder : file.findChildFiles (juce::File::findDirectories, false))
                if (! folder.isHidden())
                    for (const auto& child : folder.findChildFiles (juce::File::findFiles, false))
                        found.add (child);
            found.sort();
            for (const auto& song : found)
                if (isSongFile (song) && ! song.isHidden())
                    songs.addIfNotAlreadyThere (song);
        }
        else if (file.existsAsFile() && isSongFile (file))
            songs.addIfNotAlreadyThere (file);
    }
    return songs;
}

int analyzeSongs (AiService& service, const juce::Array<juce::File>& songs, bool download,
                  const std::function<void (const juce::String& line)>& say)
{
    const auto line = [&say] (std::initializer_list<std::pair<const char*, juce::var>> items)
    { say (juce::JSON::toString (object (items), true)); };
    const auto wait = [] (const std::function<bool()>& busy)
    {
        while (busy())
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    };
    const auto workRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("spinus-analyze-" + juce::Uuid().toString());
    struct Cleanup
    {
        juce::File folder;
        ~Cleanup() { folder.deleteRecursively(); }
    } cleanup { workRoot };

    SongAnalysisQueue queue (&service, AnalysisLibrary::inAiFolder (service.getFolder()), workRoot);
    std::map<juce::File, juce::String> said; // each song's step said last: a step is said once
    queue.onChange = [&] (const SongAnalysisQueue::Song& song)
    {
        if (song.state == SongAnalysisQueue::Song::State::analyzing && song.step.isNotEmpty() &&
            said[song.file] != song.step)
        {
            said[song.file] = song.step;
            line ({ { "song", song.file.getFileName() }, { "step", song.step } });
        }
    };
    const auto run = [&]
    {
        for (const auto& song : songs)
            queue.add (song);
        wait ([&] { return queue.isBusy(); });
    };
    run();

    // Model files missing: downloaded when asked, then the songs again.
    if (const auto needs = queue.getNeeds(); needs && needs->what == SongAnalyzer::Needs::What::models && download)
    {
        auto alive = std::make_shared<bool> (true);
        bool downloading = true;
        juce::String problem;
        int saidPercent = -1;
        juce::String saidName;
        downloadModels (
            service, needs->models, alive,
            [&] (const juce::String& name, double fraction)
            {
                const auto percent = fraction < 0.0 ? 0 : juce::roundToInt (100.0 * fraction) / 10 * 10;
                if (name == saidName && percent == saidPercent)
                    return;
                saidName = name;
                saidPercent = percent;
                line ({ { "downloading", name }, { "percent", percent } });
            },
            [&] (const juce::String& error)
            {
                problem = error;
                downloading = false;
            });
        wait ([&] { return downloading; });
        *alive = false;
        if (problem.isEmpty())
            run();
    }

    int analyzed = 0, failed = 0;
    for (const auto& song : queue.getSongs())
    {
        const bool ok = song.state == SongAnalysisQueue::Song::State::done;
        ++(ok ? analyzed : failed);
        auto result = object ({ { "song", song.file.getFileName() }, { "ok", ok } });
        auto* o = result.getDynamicObject();
        if (ok)
        {
            o->setProperty ("key", AnalysisLibrary::keyOf (song.digest));
            if (song.keptBefore)
                o->setProperty ("analyzedBefore", true);
        }
        else
            o->setProperty ("error", song.step);
        say (juce::JSON::toString (result, true));
    }
    auto summary = object ({ { "ok", failed == 0 }, { "analyzed", analyzed }, { "failed", failed } });
    if (const auto needs = queue.getNeeds(); needs && failed > 0)
    {
        using What = SongAnalyzer::Needs::What;
        summary.getDynamicObject()->setProperty (
            "hint",
            needs->what == What::ai       ? "Turn on AI in Spinus (Options > AI): the analysis runs on this computer."
            : needs->what == What::engine ? "Install the analysis engine in Spinus (Options > AI > Analysis engine)."
            : needs->what == What::models ? "Run it again with --download-models (" +
                                                juce::String (juce::roundToInt ((double)needs->downloadSize / 1e6)) +
                                                " MB, open-source models made by others, downloaded once)."
                                          : juce::String ("The AI service did not answer."));
    }
    say (juce::JSON::toString (summary, true));
    return failed == 0 ? 0 : 1;
}

} // namespace spinus::ai
