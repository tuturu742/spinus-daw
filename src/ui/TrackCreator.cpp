#include "TrackCreator.h"
#include "ai/AnalysisJobs.h"

#include "AnalysisLibraryView.h"
#include "Theme.h"
#include "session/SongPreparation.h"

#include <algorithm>
#include <map>

namespace spinus::ui
{

namespace
{
const Theme& themeOf (const juce::Component& c)
{
    static const Theme fallback = Theme::dark();
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&c.getLookAndFeel());
    return lf != nullptr ? lf->getTheme() : fallback;
}

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : items)
        o->setProperty (k, v);
    return juce::var (o);
}

constexpr const char* separationModel = "htdemucs"; // 4 stems: what the style needs
} // namespace

//==============================================================================
/** Reads a song's contents (its digest names it in the analysis library) and, when its style was not measured
    before, decodes it, off the message thread. */
class TrackCreator::Preparer final : public juce::Thread
{
public:
    Preparer (TrackCreator& c, juce::File song, AnalysisLibrary lib, juce::File work, juce::String known)
        : juce::Thread ("Spinus track creator preparer"), creator (c), file (std::move (song)),
          library (std::move (lib)), workFolderRoot (std::move (work)), digest (std::move (known))
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
        if (const auto& found = entry["features"]; found.isObject() && (int)found["version"] == analysisVersion)
            features = found;
        else
        {
            prepared = preparedSongFile (workFolderRoot.getChildFile (digest.substring (0, 16)), file, digest);
            result = prepareSong (file, prepared, [this] { return threadShouldExit(); }, progress);
        }
        if (! threadShouldExit())
            creator.triggerAsyncUpdate();
    }

    TrackCreator& creator;
    juce::File file;
    AnalysisLibrary library;
    juce::File workFolderRoot;
    juce::String digest;
    juce::var entry;    // what the library has of the song
    juce::var features; // its style, measured before
    juce::File prepared;
    juce::Result result = juce::Result::ok();
    std::atomic<double> progress { 0.0 };
};

/** What is known so far of the song being analyzed. */
struct TrackCreator::Work
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
class TrackCreator::Rows final : public juce::ListBoxModel
{
public:
    explicit Rows (TrackCreator& c) : creator (c) {}
    int getNumRows() override { return (int)creator.references.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= getNumRows())
            return;
        const auto& theme = themeOf (creator);
        const auto& reference = creator.references[(size_t)row];
        if (selected)
            g.fillAll (theme.get ("accent").withAlpha (0.25f));
        g.setFont (juce::FontOptions (14.0f));
        g.setColour (theme.get ("text"));
        auto area = juce::Rectangle<int> (0, 0, width, height).reduced (6, 0);
        const auto right = area.removeFromRight (juce::jmin (300, width / 2));
        g.drawFittedText (reference.file.getFileName(), area, juce::Justification::centredLeft, 1);
        g.setColour (reference.status == Status::failed ? theme.get ("accent") : theme.get ("textDim"));
        g.drawFittedText (creator.describe (reference), right, juce::Justification::centredRight, 1);
    }
    void selectedRowsChanged (int) override { creator.updateButtons(); }

private:
    TrackCreator& creator;
};

//==============================================================================
TrackCreator::TrackCreator (Session& s, ai::AiService* a)
    : Panel ("trackCreator", "Track creator (experimental)"), session (s),
      workRoot (juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("spinus-track-creator-" + juce::Uuid().toString()))
{
    setHelpText ("Track creator: add songs you like to see what their style has in common");
    intro.setText ("Add a few songs you like, or an album (a folder). Spinus studies what they have in common: "
                   "tempo, key, chords, drum groove, parts, sections and loudness. Making a new song in that "
                   "style comes in a later version.",
                   juce::dontSendNotification);
    intro.setJustificationType (juce::Justification::topLeft);
    notice.setText ("Analyzing songs is for study and personal use. Only the style is measured, never the songs' "
                    "melodies, words or sound; if you release music, you are responsible for the rights.",
                    juce::dontSendNotification);
    for (auto* l : std::initializer_list<juce::Label*> { &status, &notice, &needsText, &summaryTitle })
        l->setJustificationType (juce::Justification::centredLeft);
    summaryTitle.setText ("What the songs have in common", juce::dontSendNotification);
    summaryText.setMultiLine (true, true);
    summaryText.setReadOnly (true);
    summaryText.setScrollbarsShown (true);
    summaryText.setCaretVisible (false);
    summaryText.setTextToShowWhenEmpty ("The summary appears here once the songs are analyzed.", juce::Colours::grey);

    rows = std::make_unique<Rows> (*this);
    list.setModel (rows.get());
    list.setRowHeight (24);
    list.setMultipleSelectionEnabled (false);

    addButton.setTooltip ("Add audio files (WAV, FLAC, MP3, Ogg...)");
    addButton.onClick = [this]
    {
        chooser = std::make_unique<core::FileDialog> ("Choose songs to study", juce::File(), songFilePatterns());
        chooser->launchAsync ((int)((unsigned)juce::FileBrowserComponent::openMode |
                                    (unsigned)juce::FileBrowserComponent::canSelectFiles |
                                    (unsigned)juce::FileBrowserComponent::canSelectMultipleItems),
                              [safe = juce::Component::SafePointer (this)] (const core::FileDialog& c)
                              {
                                  if (safe != nullptr && ! c.getResults().isEmpty())
                                      safe->addReferences (c.getResults());
                              });
    };
    folderButton.setTooltip ("Add every song in a folder (an album), and in the folders inside it");
    folderButton.onClick = [this]
    {
        chooser = std::make_unique<core::FileDialog> ("Choose a folder of songs (an album)");
        chooser->launchAsync ((int)((unsigned)juce::FileBrowserComponent::openMode |
                                    (unsigned)juce::FileBrowserComponent::canSelectDirectories),
                              [safe = juce::Component::SafePointer (this)] (const core::FileDialog& c)
                              {
                                  if (safe != nullptr && c.getResult().isDirectory())
                                      safe->addReferences ({ c.getResult() });
                              });
    };
    removeButton.onClick = [this] { removeReference (list.getSelectedRow()); };
    clearButton.onClick = [this] { clearReferences(); };
    cancelButton.onClick = [this] { cancel(); };
    againButton.setTooltip ("Analyze the songs that are waiting or had a problem");
    againButton.onClick = [this] { analyzeAgain(); };
    downloadButton.onClick = [this] { downloadModels(); };
    engineButton.onClick = [this]
    {
        if (onSetUpEngine)
            onSetUpEngine();
    };
    saveButton.setTooltip ("Save the summary as a JSON file");
    saveButton.onClick = [this]
    {
        chooser = std::make_unique<core::FileDialog> (
            "Save the style summary",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Style summary.json"),
            "*.json");
        chooser->launchAsync ((int)((unsigned)juce::FileBrowserComponent::saveMode |
                                    (unsigned)juce::FileBrowserComponent::canSelectFiles |
                                    (unsigned)juce::FileBrowserComponent::warnAboutOverwriting),
                              [safe = juce::Component::SafePointer (this)] (const core::FileDialog& c)
                              {
                                  if (safe == nullptr || c.getResult() == juce::File())
                                      return;
                                  const auto file = c.getResult().withFileExtension ("json");
                                  safe->status.setText (safe->saveSummary (file)
                                                            ? "Saved " + file.getFullPathName() + "."
                                                            : "Spinus cannot write " + file.getFullPathName() + ".",
                                                        juce::dontSendNotification);
                              });
    };

    libraryButton.setTooltip ("The songs Spinus has analyzed (here and in the Analyzer), the space they take, and "
                              "deleting them");
    libraryButton.onClick = [this] { showLibrary(); };

    for (auto* c : std::initializer_list<juce::Component*> { &intro, &addButton, &folderButton, &removeButton,
                                                             &clearButton, &list, &status, &summaryTitle, &summaryText,
                                                             &saveButton, &libraryButton, &notice })
        addAndMakeVisible (c);
    for (auto* c : std::initializer_list<juce::Component*> { &cancelButton, &againButton, &needsText, &downloadButton,
                                                             &engineButton })
        addChildComponent (c);

    setService (a);
    updateButtons();
}

TrackCreator::~TrackCreator()
{
    *alive = false;
    *run = false;
    task = nullptr;
    preparer = nullptr;
    list.setModel (nullptr);
    workRoot.deleteRecursively();
}

void TrackCreator::setService (ai::AiService* newService)
{
    service = newService;
    if (service != nullptr && ! recalled)
        recall();
}

AnalysisLibrary TrackCreator::getLibrary() const
{
    return AnalysisLibrary::inAiFolder (service != nullptr ? service->getFolder() : juce::File());
}

juce::File TrackCreator::getListFile() const
{
    return service != nullptr ? service->getFolder().getChildFile ("track-creator.json") : juce::File();
}

void TrackCreator::showLibrary() { AnalysisLibraryView::show (getLibrary(), this); }

void TrackCreator::remember() const
{
    const auto file = getListFile();
    if (file == juce::File() || ! recalled)
        return;
    juce::Array<juce::var> songs;
    for (const auto& r : references)
        songs.add (object ({ { "path", r.file.getFullPathName() },
                             { "digest", r.digest },
                             { "size", r.size },
                             { "modified", r.modified } }));
    file.getParentDirectory().createDirectory();
    file.replaceWithText (
        juce::JSON::toString (object (
            { { "version", 1 }, { "references", songs }, { "summary", summary }, { "summarized", summarized } })) +
        "\n");
}

void TrackCreator::recall()
{
    recalled = true;
    const auto saved = juce::JSON::parse (getListFile());
    const auto library = getLibrary();
    if (auto* songs = saved["references"].getArray())
        for (const auto& song : *songs)
        {
            Reference r;
            r.file = juce::File (song["path"].toString());
            if (! juce::File::isAbsolutePath (song["path"].toString()) ||
                std::any_of (references.begin(), references.end(),
                             [&r] (const Reference& o) { return o.file == r.file; }))
                continue;
            // Unchanged since its contents were read: what the library has of it counts.
            if (r.file.existsAsFile() && r.file.getSize() == (juce::int64)song["size"] &&
                r.file.getLastModificationTime().toMilliseconds() == (juce::int64)song["modified"])
            {
                r.digest = song["digest"].toString();
                r.size = (juce::int64)song["size"];
                r.modified = (juce::int64)song["modified"];
                if (const auto found = library.read (r.digest)["features"];
                    found.isObject() && (int)found["version"] == analysisVersion)
                {
                    r.features = found;
                    r.status = Status::done;
                    r.keptBefore = true;
                }
            }
            if (! r.file.existsAsFile())
            {
                r.status = Status::failed;
                r.step = "Spinus cannot find this file.";
            }
            references.push_back (std::move (r));
        }
    // The summary of the same songs comes back as it was, without asking the AI service.
    summary = saved["summary"];
    summarized = saved["summarized"].toString();
    juce::StringArray done;
    for (const auto& r : references)
        if (r.status == Status::done)
            done.add (r.digest + r.file.getFileName());
    if (done.joinIntoString (",") != summarized || ! summary.isObject())
    {
        summary = juce::var();
        summarized.clear();
    }
    if (std::any_of (references.begin(), references.end(),
                     [] (const Reference& r) { return r.status == Status::waiting; }))
        status.setText ("Some songs are not analyzed yet: click Analyze again.", juce::dontSendNotification);
    list.updateContent();
    showSummary();
}

juce::File TrackCreator::getWorkFolder (const juce::String& digest) const
{
    return workRoot.getChildFile (digest.substring (0, 16));
}

TrackCreator::Reference* TrackCreator::currentReference()
{
    return current >= 0 && current < (int)references.size() ? &references[(size_t)current] : nullptr;
}

//==============================================================================
int TrackCreator::addReferences (const juce::Array<juce::File>& files)
{
    juce::Array<juce::File> songs;
    for (const auto& file : files)
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
                    songs.add (song);
        }
        else if (file.existsAsFile() && isSongFile (file))
            songs.add (file);
    }
    int added = 0;
    for (const auto& song : songs)
        if (std::none_of (references.begin(), references.end(),
                          [&song] (const Reference& r) { return r.file == song; }))
        {
            references.push_back ({ song });
            ++added;
        }
    if (added == 0)
        status.setText (files.isEmpty() ? juce::String() : "No new songs there (WAV, FLAC, MP3, Ogg...).",
                        juce::dontSendNotification);
    list.updateContent();
    remember();
    startNext();
    updateButtons();
    return added;
}

bool TrackCreator::removeReference (int index)
{
    if (index < 0 || index >= (int)references.size())
        return false;
    if (index == current)
        stopCurrent();
    else if (current > index)
        --current;
    references.erase (references.begin() + index);
    list.updateContent();
    list.deselectAllRows();
    remember();
    startNext();
    updateButtons();
    return true;
}

void TrackCreator::clearReferences()
{
    stopCurrent();
    references.clear();
    list.updateContent();
    remember();
    startNext(); // nothing left: no summary
    updateButtons();
}

void TrackCreator::cancel()
{
    if (auto* reference = currentReference())
    {
        reference->status = Status::failed;
        reference->step = "Stopped.";
    }
    stopCurrent();
    status.setText ("Stopped. Analyze again carries on.", juce::dontSendNotification);
    list.repaint();
    updateButtons();
}

void TrackCreator::analyzeAgain()
{
    for (auto& reference : references)
        if (reference.status == Status::failed)
            reference.status = Status::waiting;
    needsText.setText ({}, juce::dontSendNotification);
    list.repaint();
    startNext();
    updateButtons();
}

void TrackCreator::stopCurrent()
{
    *run = false;
    run = std::make_shared<bool> (true);
    if (service != nullptr && job.isNotEmpty() && job != "starting")
        service->call ("ai.v1.job.cancel", object ({ { "job", job } }), {});
    job.clear();
    task = nullptr;
    preparer = nullptr;
    cancelPendingUpdate();
    if (work != nullptr)
        work->folder.deleteRecursively();
    work = nullptr;
    current = -1;
}

//==============================================================================
void TrackCreator::startNext()
{
    if (current >= 0 || downloading)
        return;
    const auto next = std::find_if (references.begin(), references.end(),
                                    [] (const Reference& r) { return r.status == Status::waiting; });
    if (next == references.end())
        return summarize();
    current = (int)(next - references.begin());
    next->status = Status::preparing;
    next->step = "Preparing...";
    next->progress = -1.0;
    // Unchanged since its contents were read: they are not read again.
    const bool same = next->digest.isNotEmpty() && next->file.getSize() == next->size &&
                      next->file.getLastModificationTime().toMilliseconds() == next->modified;
    preparer =
        std::make_unique<Preparer> (*this, next->file, getLibrary(), workRoot, same ? next->digest : juce::String());
    list.repaint();
    updateButtons();
}

void TrackCreator::handleAsyncUpdate()
{
    auto* reference = currentReference();
    if (preparer == nullptr || reference == nullptr || reference->status != Status::preparing)
        return;
    const auto finished = std::move (preparer);
    reference->digest = finished->digest;
    reference->size = reference->file.getSize();
    reference->modified = reference->file.getLastModificationTime().toMilliseconds();
    if (finished->features.isObject())
        return finishSong (finished->features, true);
    if (finished->result.failed())
        return failSong (finished->result.getErrorMessage());
    work = std::make_unique<Work>();
    work->digest = finished->digest;
    work->prepared = finished->prepared;
    work->folder = getWorkFolder (finished->digest);
    work->entry = finished->entry;
    work->name = reference->file.getFileName();
    checkReady ([this] { findTempo(); });
}

juce::StringArray TrackCreator::neededModels() const { return { beatModel, separationModel, "basic_pitch" }; }

/** AI on, the analysis engine installed and the models downloaded; otherwise says what is missing. */
void TrackCreator::checkReady (const std::function<void()>& then)
{
    if (service == nullptr || ! service->isEnabled())
    {
        needsText.setText ("The Track creator uses AI that runs on this computer: turn on AI in Options > AI.",
                           juce::dontSendNotification);
        return holdQueue ("AI is off.");
    }
    // What the library has already (from the Analyzer, or an earlier time) needs no models.
    const auto& entry = work->entry;
    const auto kept = getLibrary().findStems (work->digest, separationModel);
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
    if (auto* reference = currentReference())
    {
        reference->status = Status::analyzing;
        reference->step = "Checking the analysis engine...";
    }
    list.repaint();
    ai::checkAnalysisEngine (
        *service, run,
        [this, then] (const ai::EngineCheck& engine)
        {
            if (engine.error.isNotEmpty())
                return holdQueue (engine.error);
            engineButton.setVisible (! engine.installed);
            if (! engine.installed)
            {
                needsText.setText ("The Track creator needs the analysis engine (PyTorch) installed first.",
                                   juce::dontSendNotification);
                return holdQueue ("The analysis engine is not installed.");
            }
            beatModel = engine.gpu ? "beat_this_final0" : "beat_this_small0";
            missing.clear();
            downloadSize = 0;
            juce::StringArray names;
            const auto needed = neededModels();
            if (auto* models = engine.models["models"].getArray())
                for (auto& m : *models)
                    if (needed.contains (m["id"].toString()) && ! (bool)m["downloaded"])
                    {
                        missing.push_back ({ m["id"].toString(), m["name"].toString(), (juce::int64)m["size"] });
                        downloadSize += (juce::int64)m["size"];
                        names.add (m["name"].toString());
                    }
            if (! missing.empty())
            {
                const auto megabytes = juce::String (juce::roundToInt ((double)downloadSize / 1e6));
                needsText.setText ("The Track creator needs " + names.joinIntoString (" and ") +
                                       " (open-source models made by others, downloaded once, " + megabytes +
                                       " MB). The Analyzer's Sources and licences says where each comes from.",
                                   juce::dontSendNotification);
                downloadButton.setButtonText ("Download (" + megabytes + " MB)");
                return holdQueue ("Model files are missing.");
            }
            needsText.setText ({}, juce::dontSendNotification);
            updateButtons();
            then();
        });
}

void TrackCreator::holdQueue (const juce::String& why)
{
    if (auto* reference = currentReference())
    {
        reference->status = Status::waiting;
        reference->step.clear();
    }
    job.clear();
    work = nullptr; // the decoded song stays in the work folder: not decoded again
    current = -1;
    status.setText (why, juce::dontSendNotification);
    list.repaint();
    updateButtons();
}

void TrackCreator::runStep (const juce::String& method, const juce::var& params, const juce::String& words,
                            const std::function<void (const juce::var&)>& done)
{
    if (auto* reference = currentReference())
    {
        reference->status = Status::analyzing;
        reference->step = words;
        reference->progress = -1.0;
    }
    list.repaint();
    job = "starting";
    methodsRun.add (method);
    updateButtons();
    ai::runJob (
        *service, method, params, run, [this] (const juce::String& started) { job = started; },
        [this, done] (const juce::var& result, const juce::String& error)
        {
            job.clear();
            if (error.isNotEmpty())
                return failSong (error);
            done (result);
        },
        [this] (double doneSoFar, double total, const juce::String& step)
        {
            if (auto* reference = currentReference())
            {
                reference->progress = total > 0.0 ? doneSoFar / total : -1.0;
                if (step.isNotEmpty())
                    reference->step = step + "...";
            }
            list.repaint();
        });
}

void TrackCreator::runTask (const juce::String& words, BackgroundTask::Work what, const std::function<void()>& then)
{
    if (auto* reference = currentReference())
    {
        reference->status = Status::analyzing;
        reference->step = words;
        reference->progress = -1.0;
    }
    list.repaint();
    task = std::make_unique<BackgroundTask> ("Spinus track creator stems", std::move (what),
                                             [this, then, token = run] (const juce::Result&)
                                             {
                                                 if (*token)
                                                     then();
                                             });
}

void TrackCreator::findTempo()
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
            getLibrary().set (work->digest, work->name, "rhythm", result);
            findKey();
        });
}

void TrackCreator::findKey()
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
                 getLibrary().set (work->digest, work->name, "key", result);
                 measureLoudness();
             });
}

void TrackCreator::measureLoudness()
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
                 getLibrary().set (work->digest, work->name, "loudness", work->loudness);
                 separate();
             });
}

void TrackCreator::separate()
{
    // Stems kept from before (here or in the Analyzer, with the same model) are used again.
    if (! getLibrary().findStems (work->digest, separationModel).empty())
    {
        auto restored = std::make_shared<std::map<juce::String, juce::File>>();
        return runTask (
            "Using the stems kept before...",
            [library = getLibrary(), digest = work->digest, folder = work->folder,
             restored] (const std::function<bool()>& stop)
            { return library.restoreStems (digest, separationModel, folder, *restored, stop); },
            [this, restored]
            {
                task = nullptr;
                if (restored->empty())
                    return failSong ("The stems kept before cannot be read.");
                work->stems = *restored;
                for (const auto& [name, file] : work->stems)
                    work->queue.add (name);
                transcribeNext();
            });
    }
    runStep ("ai.v1.stems.separate",
             object ({ { "file", work->prepared.getFullPathName() },
                       { "model", separationModel },
                       { "cacheFolder", work->folder.getFullPathName() },
                       { "quiet", session.isPlaying() } }),
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

void TrackCreator::keepStems()
{
    // Kept as FLAC in the library, for the Analyzer and the next time (not needed to go on, if it fails).
    runTask (
        "Keeping the stems...",
        [library = getLibrary(), digest = work->digest, stems = work->stems] (const std::function<bool()>& stop)
        { return library.keepStems (digest, separationModel, stems, stop); },
        [this]
        {
            task = nullptr;
            transcribeNext();
        });
}

void TrackCreator::transcribeNext()
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
                 auto library = getLibrary();
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

void TrackCreator::measureStyle()
{
    auto* reference = currentReference();
    const auto& rhythm = work->rhythm;
    juce::Array<juce::var> songs { object ({
        { "name", reference != nullptr ? reference->file.getFileNameWithoutExtension() : juce::String() },
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
    }) };
    runStep ("ai.v1.style.summarize", object ({ { "songs", songs } }), "Measuring its style...",
             [this] (const juce::var& result)
             {
                 const auto features = result["perSong"][0];
                 if (! features.isObject())
                     return failSong ("The AI service did not say what it measured.");
                 getLibrary().set (work->digest, work->name, "features", features);
                 finishSong (features, false);
             });
}

void TrackCreator::finishSong (const juce::var& features, bool keptBefore)
{
    auto* reference = currentReference();
    if (reference == nullptr)
        return;
    reference->features = features;
    reference->keptBefore = keptBefore;
    reference->status = Status::done;
    reference->step.clear();
    reference->progress = -1.0;
    if (work != nullptr)
        work->folder.deleteRecursively(); // the decoded song and its stems as WAV: large, and kept as FLAC
    work = nullptr;
    current = -1;
    list.repaint();
    remember();
    startNext();
    updateButtons();
}

void TrackCreator::failSong (const juce::String& why)
{
    if (auto* reference = currentReference())
    {
        reference->status = Status::failed;
        reference->step = why;
    }
    if (work != nullptr)
        work->folder.deleteRecursively();
    work = nullptr;
    job.clear();
    current = -1;
    list.repaint();
    startNext(); // on with the others
    updateButtons();
}

//==============================================================================
void TrackCreator::summarize()
{
    juce::Array<juce::var> songs;
    juce::StringArray digests;
    for (const auto& reference : references)
        if (reference.status == Status::done)
        {
            // Named as the file is now (what was kept may come from a copy under another name).
            auto features = reference.features.clone();
            if (auto* o = features.getDynamicObject())
                o->setProperty ("name", reference.file.getFileNameWithoutExtension());
            songs.add (object ({ { "features", features } }));
            digests.add (reference.digest + reference.file.getFileName());
        }
    const auto key = digests.joinIntoString (",");
    if (summarizing || key == summarized)
        return;
    if (songs.isEmpty())
    {
        summarized = key;
        summary = juce::var();
        return showSummary();
    }
    if (service == nullptr || ! service->isEnabled())
        return summaryText.setText ("Turn on AI in Options > AI to see the summary.", juce::dontSendNotification);
    summarizing = true;
    status.setText ("Summing up what the songs have in common...", juce::dontSendNotification);
    updateButtons();
    ai::runJob (*service, "ai.v1.style.summarize", object ({ { "songs", songs } }), alive, {},
                [this, key] (const juce::var& result, const juce::String& error)
                {
                    summarizing = false;
                    summarized = key; // not asked again for the same songs
                    summary = error.isEmpty() ? result : juce::var();
                    remember();
                    status.setText (error.isEmpty() ? juce::String ("Done.")
                                                    : "The summary could not be made: " + error,
                                    juce::dontSendNotification);
                    showSummary();
                    startNext(); // songs added meanwhile
                    updateButtons();
                });
}

void TrackCreator::showSummary()
{
    juce::StringArray lines;
    if (auto* plain = summary["plain"].getArray())
        for (auto& line : *plain)
            lines.add (line.toString());
    summaryText.setText (lines.joinIntoString ("\n"), juce::dontSendNotification);
    updateButtons();
}

bool TrackCreator::saveSummary (const juce::File& file) const
{
    return summary.isObject() && file.replaceWithText (juce::JSON::toString (summary) + "\n");
}

void TrackCreator::downloadModels()
{
    if (service == nullptr || missing.empty() || isBusy())
        return;
    downloading = true;
    const auto next = missing.front();
    status.setText ("Downloading " + next.name + "...", juce::dontSendNotification);
    updateButtons();
    ai::runJob (
        *service, "ai.v1.stems.download", object ({ { "model", next.id } }), alive, {},
        [this] (const juce::var&, const juce::String& error)
        {
            downloading = false;
            if (error.isNotEmpty())
            {
                status.setText (error, juce::dontSendNotification);
                return updateButtons();
            }
            missing.erase (missing.begin());
            if (! missing.empty())
                return downloadModels();
            needsText.setText ({}, juce::dontSendNotification);
            status.setText ("Downloaded.", juce::dontSendNotification);
            startNext();
            updateButtons();
        },
        [this, name = next.name] (double done, double total, const juce::String&)
        {
            if (total > 0.0)
                status.setText ("Downloading " + name + ": " + juce::String (juce::roundToInt (100.0 * done / total)) +
                                    " %",
                                juce::dontSendNotification);
        });
}

//==============================================================================
juce::String TrackCreator::describe (const Reference& reference) const
{
    switch (reference.status)
    {
    case Status::waiting:
        return "Waiting";
    case Status::preparing:
        return "Preparing...";
    case Status::analyzing:
        return reference.step + (reference.progress >= 0.0
                                     ? " " + juce::String (juce::roundToInt (reference.progress * 100.0)) + " %"
                                     : juce::String());
    case Status::done:
        return reference.keptBefore ? "Done (analyzed before)" : "Done";
    case Status::failed:
        return "Problem: " + reference.step;
    }
    return {};
}

void TrackCreator::updateButtons()
{
    const bool busy = isBusy();
    removeButton.setEnabled (list.getSelectedRow() >= 0 && ! downloading);
    clearButton.setEnabled (! references.empty() && ! downloading);
    cancelButton.setVisible (current >= 0);
    againButton.setVisible (! busy &&
                            std::any_of (references.begin(), references.end(), [] (const Reference& r)
                                         { return r.status == Status::waiting || r.status == Status::failed; }));
    saveButton.setEnabled (summary.isObject());
    needsText.setVisible (needsText.getText().isNotEmpty());
    downloadButton.setVisible (needsText.isVisible() && ! missing.empty());
    downloadButton.setEnabled (! busy);
    if (! needsText.isVisible())
        engineButton.setVisible (false);
    resized();
}

bool TrackCreator::isInterestedInFileDrag (const juce::StringArray& files)
{
    return std::any_of (files.begin(), files.end(),
                        [] (const juce::String& f)
                        {
                            const juce::File file (f);
                            return file.isDirectory() || isSongFile (file);
                        });
}

void TrackCreator::filesDropped (const juce::StringArray& files, int, int)
{
    juce::Array<juce::File> dropped;
    for (const auto& f : files)
        dropped.add (juce::File (f));
    addReferences (dropped);
}

void TrackCreator::paint (juce::Graphics& g)
{
    const auto& theme = themeOf (*this);
    g.fillAll (theme.get ("panel"));
    for (auto* l : { &intro, &summaryTitle })
        l->setColour (juce::Label::textColourId, theme.get ("text"));
    for (auto* l : { &status, &notice })
        l->setColour (juce::Label::textColourId, theme.get ("textDim"));
    needsText.setColour (juce::Label::textColourId, theme.get ("accent"));
    list.setColour (juce::ListBox::backgroundColourId, theme.get ("background"));
}

void TrackCreator::resized()
{
    auto area = getLocalBounds().reduced (10);
    notice.setBounds (area.removeFromBottom (40));
    intro.setBounds (area.removeFromTop (40));
    area.removeFromTop (4);
    {
        auto r = area.removeFromTop (28);
        for (auto* b : { &addButton, &folderButton })
        {
            b->setBounds (r.removeFromLeft (120).reduced (0, 1));
            r.removeFromLeft (6);
        }
        for (auto* b : { &removeButton, &clearButton })
        {
            b->setBounds (r.removeFromLeft (80).reduced (0, 1));
            r.removeFromLeft (6);
        }
        if (cancelButton.isVisible())
            cancelButton.setBounds (r.removeFromRight (80).reduced (0, 1));
        if (againButton.isVisible())
            againButton.setBounds (r.removeFromRight (120).reduced (0, 1));
    }
    area.removeFromTop (6);
    if (needsText.isVisible())
    {
        needsText.setBounds (area.removeFromTop (40));
        auto r = area.removeFromTop (28);
        if (downloadButton.isVisible())
            downloadButton.setBounds (r.removeFromLeft (160).reduced (0, 1));
        if (engineButton.isVisible())
            engineButton.setBounds (r.removeFromLeft (220).reduced (0, 1));
        area.removeFromTop (6);
    }
    list.setBounds (area.removeFromTop (juce::jlimit (96, 240, area.getHeight() * 38 / 100)));
    status.setBounds (area.removeFromTop (26));
    {
        auto r = area.removeFromTop (28);
        saveButton.setBounds (r.removeFromRight (140).reduced (0, 1));
        r.removeFromRight (6);
        libraryButton.setBounds (r.removeFromRight (150).reduced (0, 1));
        summaryTitle.setBounds (r);
    }
    area.removeFromTop (4);
    summaryText.setBounds (area);
}

} // namespace spinus::ui
