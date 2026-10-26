#include "Analyzer.h"
#include "AnalysisLibraryView.h"
#include "session/SoundIndex.h"
#include "ai/AnalysisJobs.h"
#include "project/AudioImport.h"

#include "Theme.h"
#include "dsp/drums/DrumSynth.h"
#include "commands/CommandRegistry.h"
#include "dsp/sampler/SampleStorage.h"
#include "project/ProjectFiles.h"
#include "session/SongPreparation.h"
#include "model/ModelJson.h"
#include "plugins/sandbox/SandboxPluginFormat.h"

#include <set>

namespace spinus::ui
{

namespace
{
juce::File presetCountsFile (ai::AiService* service)
{
    return service != nullptr ? service->getFolder().getChildFile ("instrument-presets.json") : juce::File();
}

/** An installed plugin's file changes when it is updated: what Spinus found then is looked at again. */
juce::String pluginVersionOf (const model::Device& device)
{
    const juce::File file (device.plugin.fileOrIdentifier);
    return juce::File::isAbsolutePath (device.plugin.fileOrIdentifier) && file.exists()
               ? juce::String (file.getLastModificationTime().toMilliseconds())
               : juce::String();
}

juce::String presetVersion (const juce::File& file)
{
    return juce::String (file.getSize()) + ":" + juce::String (file.getLastModificationTime().toMilliseconds());
}

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

const char* const noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

/** What the Analyzer names a stem's channel, audio clip and Playlist track: the stem first, so that it shows
    where names are cut short ("Drums (My Song)" for its notes, "Drums stem (My Song)" for its audio). */
juce::String stemLabel (const juce::String& stem, const juce::String& song)
{
    return stem.substring (0, 1).toUpperCase() + stem.substring (1) + " (" + song + ")";
}

} // namespace

//==============================================================================
/** Builds one undo step from several commands, each tried on a copy of the project first to learn the IDs
    it makes (a new track's ID for its clip, a channel's for its notes). */
class Analyzer::Steps
{
public:
    Steps (const model::Project& project, const juce::String& description)
        : trial (project), transaction (std::make_unique<commands::Transaction> (description))
    {
    }

    /** The command's result (e.g. {"lane": 12}), or void if it cannot be made (getError() then says why;
        nothing is added to the project once one has failed). */
    juce::var add (const juce::String& name, const juce::var& params)
    {
        if (error.isNotEmpty())
            return {};
        auto& registry = commands::CommandRegistry::get();
        juce::String problem;
        auto onCopy = registry.create (name, params, problem);
        auto real = registry.create (name, params, problem);
        if (onCopy == nullptr || real == nullptr)
        {
            error = name + ": " + problem;
            return {};
        }
        auto* command = onCopy.get();
        if (const auto tried = trial.perform (std::move (onCopy)); tried.failed())
        {
            error = name + ": " + tried.getErrorMessage();
            return {};
        }
        transaction->add (std::move (real));
        const auto result = command->getResult();
        return result.isVoid() ? juce::var (true) : result;
    }

    /** Something else went wrong: nothing is added. Returns false. */
    bool fail (const juce::String& why)
    {
        if (error.isEmpty())
            error = why;
        return false;
    }

    /** The first command that could not be made, and why. */
    juce::String getError() const { return error; }

    juce::Result performOn (commands::CommandHistory& history)
    {
        if (error.isNotEmpty())
            return juce::Result::fail (error);
        return history.perform (std::move (transaction));
    }

private:
    commands::CommandHistory trial;
    std::unique_ptr<commands::Transaction> transaction;
    juce::String error;
};

//==============================================================================
/** Decodes the song to a stereo 32-bit float WAV, off the message thread. */
class Analyzer::Decoder final : public juce::Thread
{
public:
    Decoder (Analyzer& a, juce::File from, juce::File to)
        : juce::Thread ("Spinus analyzer decoder"), analyzer (a), source (std::move (from)),
          destination (std::move (to))
    {
        startThread();
    }

    ~Decoder() override { stopThread (10000); }

    void run() override
    {
        result = decode();
        if (! threadShouldExit())
            analyzer.triggerAsyncUpdate();
    }

    juce::Result decode()
    {
        return prepareSong (source, destination, [this] { return threadShouldExit(); }, progress);
    }

    Analyzer& analyzer;
    juce::File source, destination;
    juce::Result result = juce::Result::ok();
    std::atomic<double> progress { 0.0 };
};

//==============================================================================
Analyzer::Analyzer (Session& s, ai::AiService* a) : Panel ("analyzer", "Analyzer (experimental)"), session (s)
{
    setHelpText ("Analyzer: drop a song here to find its tempo, key and stems");
    intro.setText ("Drop a song here (WAV, FLAC, Ogg, AIFF or MP3), or choose one. Spinus finds its tempo, beats "
                   "and key, and can split it into stems to learn from.",
                   juce::dontSendNotification);
    notice.setText ("The analysis and separation use freely available open-source models made by others, not "
                    "affiliated with Spinus. Analyzing songs is for study and personal use; if you release music made "
                    "from someone else's song, you are responsible for the rights.",
                    juce::dontSendNotification);
    for (auto* l : std::initializer_list<juce::Label*> { &intro, &songText, &status, &tempoText, &keyText, &stemsText,
                                                         &needsText, &notice })
        l->setJustificationType (juce::Justification::centredLeft);
    tempoLabel.setText ("Tempo", juce::dontSendNotification);
    keyLabel.setText ("Key", juce::dontSendNotification);
    stemsLabel.setText ("Stems", juce::dontSendNotification);
    stemCount.addItem ("4 stems", 4);
    stemCount.addItem ("6 stems (with piano and guitar)", 6);
    stemCount.addItem ("Many instruments (up to 53, slow)", manyInstruments);
    stemCount.setTooltip ("4 stems: vocals, drums, bass and the rest. 6 adds piano and guitar. Many instruments "
                          "tells apart up to 53 kinds (lead and backing vocals, synth, strings, flute, accordion...) "
                          "and keeps those it hears; it is a bigger download (1.4 GB) and slower, best on a "
                          "graphics card with 16 GB of memory.");
    stemCount.setSelectedId (4, juce::dontSendNotification);
    // What the window says it needs to download follows the choice at once.
    stemCount.onChange = [this]
    {
        showNeeds();
        updateButtons();
    };

    chooseButton.onClick = [this]
    {
        chooser = std::make_unique<core::FileDialog> ("Choose a song to analyze", juce::File(), songFilePatterns());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe = juce::Component::SafePointer (this)] (const core::FileDialog& c)
                              {
                                  if (safe != nullptr && c.getResult() != juce::File())
                                      safe->analyze (c.getResult());
                              });
    };
    useTempoButton.onClick = [this] { useTempo(); };
    useKeyButton.onClick = [this] { useKey(); };
    separateButton.onClick = [this] { separate (stemCount.getSelectedId()); };
    addButton.setHelpText ("Add the stems as chosen below: replicated as notes on new channels, or as audio on "
                           "Playlist tracks (one undo step)");
    addButton.onClick = [this] { addChosen(); };
    drumFolderButton.setTooltip ("A folder of drum samples (searched with its subfolders) to suggest from, besides "
                                 "Spinus's Synth Kit");
    drumFolderButton.onClick = [this]
    {
        chooser = std::make_unique<core::FileDialog> ("Choose a folder of drum samples", drumSamplesFolder);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer<Analyzer> (this)] (const core::FileDialog& c)
                              {
                                  if (safe != nullptr && c.getResult().isDirectory())
                                      safe->setDrumSamplesFolder (c.getResult());
                              });
    };
    downloadButton.onClick = [this] { downloadModels(); };
    sourcesButton.setTooltip ("Where each model file comes from and its licence: opens its page");
    sourcesButton.onClick = [this] { showSources(); };
    engineButton.onClick = [this]
    {
        if (onSetUpEngine)
            onSetUpEngine();
    };
    cancelButton.onClick = [this] { cancel(); };
    continueButton.setTooltip ("Use the tempo, key and stems found when this song was analyzed before");
    continueButton.onClick = [this] { continueFromEarlier(); };
    anewButton.setTooltip ("Analyze the song again from the start");
    anewButton.onClick = [this] { analyzeAnew(); };
    libraryButton.setTooltip ("The songs Spinus has analyzed (here and in the Track creator), the space they take, "
                              "and deleting them");
    libraryButton.onClick = [this] { showLibrary(); };

    for (auto* c : std::initializer_list<juce::Component*> {
             &intro, &songText, &status, &tempoLabel, &tempoText, &keyLabel, &keyText, &stemsLabel, &stemsText, &notice,
             &chooseButton, &useTempoButton, &useKeyButton, &separateButton, &stemCount, &sourcesButton,
             &libraryButton })
        addAndMakeVisible (c);
    for (auto* c : std::initializer_list<juce::Component*> { &needsText, &downloadButton, &engineButton, &bar,
                                                             &cancelButton, &addButton, &continueButton, &anewButton })
        addChildComponent (c);
    // The stems' rows scroll: separating into many instruments can give a dozen or more.
    stemRows.addChildComponent (drumFolderButton);
    stemsView.setViewedComponent (&stemRows, false);
    stemsView.setScrollBarsShown (true, false);
    addChildComponent (stemsView);

    setService (a);
    // Instruments installed or downloaded later (a plugin scan, Options > Plugins) join the sound lists.
    session.getDeviceList().addChangeListener (this);
    updateButtons();
}

Analyzer::~Analyzer()
{
    session.getDeviceList().removeChangeListener (this);
    *alive = false;
    stemTask = nullptr;
    decoder = nullptr;
    if (service != nullptr && listener != 0)
        service->removeListener (listener);
    if (tempFolder != juce::File())
        tempFolder.deleteRecursively();
}

void Analyzer::setService (ai::AiService* newService)
{
    if (service != nullptr && listener != 0)
        service->removeListener (listener);
    listener = 0;
    service = newService;
    presetCounts.clear();
    // The parsed file is kept while it is read: its object goes with it.
    if (const auto counts = presetCountsFile (service); counts.existsAsFile())
        if (const auto parsed = juce::JSON::parse (counts); auto* found = parsed.getDynamicObject())
            for (const auto& item : found->getProperties())
                presetCounts[item.name.toString()] = (int)item.value;
    if (const auto settings = getSettingsFile(); settings.existsAsFile())
        if (const auto path = juce::JSON::parse (settings)["drumSamples"].toString(); juce::File::isAbsolutePath (path))
            drumSamplesFolder = juce::File (path);
    if (service != nullptr)
        listener = service->addListener (
            [this] (const juce::String& method, const juce::var& params)
            {
                if (method != "ai.v1.job.update" || job.isEmpty() || params["job"].toString() != job)
                    return;
                if (params["kind"].toString() == "progress")
                {
                    const auto total = (double)params["total"];
                    progress = total > 0.0 ? (double)params["done"] / total : -1.0;
                    if (params["step"].toString().isNotEmpty())
                        status.setText (params["step"].toString() + "...", juce::dontSendNotification);
                }
            });
}

juce::File Analyzer::getCacheFolder() const
{
    if (const auto folder = session.getProjectFolder(); folder != juce::File())
        return folder.getChildFile ("cache").getChildFile ("analysis");
    if (tempFolder == juce::File())
        tempFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("spinus-analysis-" + juce::Uuid().toString());
    return tempFolder;
}

//==============================================================================
bool Analyzer::analyze (const juce::File& file)
{
    if (isBusy())
        return false;
    if (! file.existsAsFile())
    {
        songText.setText (file.getFileName(), juce::dontSendNotification);
        status.setText ("Spinus cannot find " + file.getFullPathName() + ".", juce::dontSendNotification);
        return false;
    }
    song = file;
    resume = nullptr;
    clapWanted = false;
    rhythm.reset();
    key.reset();
    stems.clear();
    transcribed.clear();
    updateNoteChoices();
    tempoText.setText ({}, juce::dontSendNotification);
    keyText.setText ({}, juce::dontSendNotification);
    stemsText.setText ({}, juce::dontSendNotification);
    songText.setText (file.getFileName(), juce::dontSendNotification);

    // Named by the song's contents, so a song analyzed before is not decoded again.
    songDigest = contentDigest (file);
    prepared = preparedSongFile (getCacheFolder(), file, songDigest);
    stemsModel.clear();
    choosing = false;
    earlier = getLibrary().read (songDigest);
    if (! earlier["rhythm"].isObject() && ! earlier["key"].isObject() && getLibrary().stemModels (songDigest).isEmpty())
        earlier = juce::var();
    setStep (Step::preparing, "Preparing the song...");
    progress = 0.0;
    decoder = std::make_unique<Decoder> (*this, file, prepared);
    return true;
}

void Analyzer::handleAsyncUpdate()
{
    if (decoder == nullptr || step != Step::preparing)
        return;
    const auto result = decoder->result;
    decoder = nullptr;
    if (result.failed())
        return fail (result.getErrorMessage());
    if (earlier.isObject())
    {
        // Analyzed before: continue from it, or anew.
        juce::StringArray what;
        if (earlier["rhythm"].isObject())
            what.add ("tempo");
        if (earlier["key"].isObject())
            what.add ("key");
        for (const auto& model : getLibrary().stemModels (songDigest))
            what.add (model == "htdemucs_6s" ? "6 stems" : model == "mvsep_mega_53" ? "many instruments" : "4 stems");
        choosing = true;
        setStep (Step::idle, "Spinus analyzed this song before (" + what.joinIntoString (", ") +
                                 "). Continue from that, or analyze it anew?");
        return;
    }
    checkReady ([this] { findTempo(); });
}

AnalysisLibrary Analyzer::getLibrary() const
{
    return AnalysisLibrary::inAiFolder (service != nullptr ? service->getFolder() : juce::File());
}

void Analyzer::showLibrary() { AnalysisLibraryView::show (getLibrary(), this, SoundIndex::getDefaultFolder()); }

bool Analyzer::hasEarlierAnalysis() const { return choosing && earlier.isObject(); }

bool Analyzer::continueFromEarlier()
{
    if (! hasEarlierAnalysis() || isBusy())
        return false;
    choosing = false;
    if (earlier["rhythm"].isObject())
        applyTempo (earlier["rhythm"]);
    if (earlier["key"].isObject())
        applyKey (earlier["key"]);
    // What is still missing is found as usual (it needs the AI service).
    auto rest = [this]
    {
        if (! rhythm && beatTimes.empty() && ! earlier["rhythm"].isObject())
            return checkReady ([this] { findTempo(); });
        if (! key)
            return checkReady ([this] { findKey(); });
        setStep (Step::done, stems.empty() ? "Continued from the earlier analysis. Split it into stems to hear and "
                                             "learn its parts."
                                           : "Continued from the earlier analysis. Choose what to do with each stem "
                                             "below, then click Add to the project.");
    };
    // The stems of the separation the Stems box names, or else any kept.
    const auto models = getLibrary().stemModels (songDigest);
    auto model = separationModel (stemCount.getSelectedId());
    if (! models.contains (model) && ! models.isEmpty())
        model = models[0];
    if (! models.contains (model))
        return rest(), true;
    stemCount.setSelectedId (model == "mvsep_mega_53" ? manyInstruments
                             : model == "htdemucs_6s" ? 6
                                                      : 4,
                             juce::dontSendNotification);
    useKeptStems (model, rest);
    return true;
}

bool Analyzer::analyzeAnew()
{
    if (! hasEarlierAnalysis() || isBusy())
        return false;
    choosing = false;
    // The notes found before belong to the stems found before: found again with the new ones.
    getLibrary().update (songDigest, song.getFileName(),
                         [] (juce::DynamicObject& o)
                         {
                             o.removeProperty ("notes");
                             o.removeProperty ("drums");
                         });
    earlier = juce::var();
    setStep (Step::tempo);
    checkReady ([this] { findTempo(); });
    return true;
}

void Analyzer::useKeptStems (const juce::String& model, const std::function<void()>& then)
{
    setStep (Step::separating, "Using the stems kept before...");
    auto restored = std::make_shared<std::map<juce::String, juce::File>>();
    stemTask = std::make_unique<BackgroundTask> (
        "Spinus analyzer stems",
        [library = getLibrary(), digest = songDigest, folder = getCacheFolder().getChildFile ("kept-stems"), model,
         restored] (const std::function<bool()>& stop)
        { return library.restoreStems (digest, model, folder, *restored, stop); },
        [this, restored, model, then] (const juce::Result& result)
        {
            stemTask = nullptr;
            if (result.failed() || restored->empty())
                return fail ("The stems kept before cannot be used: " + result.getErrorMessage());
            stems = *restored;
            showStems (model);
            then();
        });
}

void Analyzer::showStems (const juce::String& model)
{
    stemsModel = model;
    juce::StringArray names;
    for (const auto& [name, file] : stems)
        names.add (name);
    stemsText.setText (names.joinIntoString (", "), juce::dontSendNotification);
    updateNoteChoices();
}

/** AI on, the analysis engine installed and the models downloaded; otherwise says what is missing. */
void Analyzer::checkReady (const std::function<void()>& then, const juce::StringArray& alsoNeeded)
{
    if (service == nullptr || ! service->isEnabled())
    {
        needsShowModels = false;
        needsText.setText ("The Analyzer uses AI that runs on this computer: turn on AI in Options > AI.",
                           juce::dontSendNotification);
        return fail ("AI is off.");
    }
    setStep (step, "Checking the analysis engine...");
    ai::checkAnalysisEngine (*service, alive,
                             [this, then, alsoNeeded] (const ai::EngineCheck& engine)
                             {
                                 if (engine.error.isNotEmpty())
                                     return fail (engine.error);
                                 if (! engine.installed)
                                 {
                                     needsShowModels = false;
                                     needsText.setText (
                                         "The Analyzer needs the analysis engine (PyTorch) installed first.",
                                         juce::dontSendNotification);
                                     engineButton.setVisible (true);
                                     return fail ("The analysis engine is not installed.");
                                 }
                                 beatModel = engine.gpu ? "beat_this_final0" : "beat_this_small0";
                                 readModels (engine.models);
                                 engineButton.setVisible (false);
                                 needsText.setText ({}, juce::dontSendNotification); // AI and engine are fine
                                 showNeeds (alsoNeeded);
                                 if (! missingModels.empty())
                                     return fail ("Model files are missing.");
                                 resume = nullptr; // it is under way
                                 then();
                             });
}

void Analyzer::readModels (const juce::var& list)
{
    allSources.clear();
    if (auto* models = list["models"].getArray())
        for (auto& m : *models)
            allSources.push_back ({ m["name"].toString(), m["license"].toString(), (juce::int64)m["size"],
                                    m["page"].toString(), (bool)m["downloaded"], m["id"].toString() });
}

juce::String Analyzer::separationModel (int count)
{
    return count == manyInstruments ? juce::String ("mvsep_mega_53")
           : count == 6             ? juce::String ("htdemucs_6s")
                                    : juce::String ("htdemucs");
}

juce::StringArray Analyzer::neededModels() const
{
    juce::StringArray needed;
    if (beatModel.isNotEmpty())
        needed.add (beatModel);
    needed.add ("basic_pitch");
    needed.add (separationModel (stemCount.getSelectedId()));
    // Closest presets and drum samples, and synths tuned from their closest preset, compare sounds by ear (CLAP).
    bool clap = clapWanted;
    for (auto& [name, row] : rows)
        if (const auto* source = row.chosen(); source != nullptr && replicates (name))
        {
            const auto way = row.chosenWay();
            clap = clap || way == Way::closestPreset || (source->startsFromPreset && way == Way::tuned);
        }
    if (clap)
        needed.add ("clap_music");
    return needed;
}

void Analyzer::showNeeds (const juce::StringArray& alsoNeeded)
{
    // Another problem (AI off, no engine) is said until it is solved; while downloading, the list stays.
    if ((needsText.getText().isNotEmpty() && ! needsShowModels) || step == Step::downloading || allSources.empty())
        return;
    auto needed = neededModels();
    needed.addArray (alsoNeeded);
    missingModels.clear();
    missingSources.clear();
    downloadSize = 0;
    juce::StringArray names;
    for (const auto& m : allSources)
        if (needed.contains (m.id) && ! m.downloaded)
        {
            missingModels.push_back (m.id);
            downloadSize += m.size;
            names.add (m.name);
            missingSources.push_back (m);
        }
    const bool wasMissing = needsShowModels && needsText.getText().isNotEmpty();
    needsShowModels = ! missingModels.empty();
    if (missingModels.empty())
    {
        needsText.setText ({}, juce::dontSendNotification);
        downloadButton.setVisible (false);
        // The song was waiting for model files the choice no longer needs: on with it.
        if (wasMissing && ! isBusy())
        {
            status.setText ("The model files needed are there.", juce::dontSendNotification);
            if (song.existsAsFile() && prepared.existsAsFile() && ! rhythm)
            {
                setStep (Step::tempo);
                return checkReady ([this] { findTempo(); });
            }
        }
        updateButtons();
        return;
    }
    const auto megabytes = juce::String (juce::roundToInt ((double)downloadSize / 1e6));
    needsText.setText ("The Analyzer needs " + names.joinIntoString (" and ") + " (downloaded once, " + megabytes +
                           " MB). Sources and licences (at the bottom) says where each comes from.",
                       juce::dontSendNotification);
    downloadButton.setButtonText ("Download (" + megabytes + " MB)");
    downloadButton.setVisible (true);
    updateButtons();
}

void Analyzer::showSources()
{
    // Every model the Analyzer may use, its licence and size; choosing one opens its page to check for yourself.
    auto show = [this]
    {
        juce::PopupMenu menu;
        menu.addSectionHeader ("Open-source models made by others, not by Spinus");
        for (const auto& source : allSources)
            menu.addItem (source.name + "  -  " + source.license + "  -  " +
                              juce::String (juce::roundToInt ((double)source.size / 1e6)) + " MB" +
                              (source.downloaded ? ", downloaded" : "") + "  -  " +
                              source.page.fromFirstOccurrenceOf ("://", false, false),
                          [page = source.page] { juce::URL (page).launchInDefaultBrowser(); });
        if (allSources.empty())
            menu.addItem ("Turn on AI in Options > AI to see the list", false, false, [] {});
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&sourcesButton));
    };
    if (service == nullptr || ! service->isEnabled())
        return show();
    service->call ("ai.v1.stems.models", {},
                   [this, a = alive, show] (const juce::var& list, const juce::String& error)
                   {
                       if (! *a)
                           return;
                       if (error.isEmpty())
                           readModels (list);
                       show();
                   });
}

void Analyzer::downloadModels()
{
    if (service == nullptr || missingModels.empty() || isBusy())
        return;
    const auto next = missingModels.front();
    setStep (Step::downloading, "Downloading...");
    runJob ("ai.v1.stems.download", object ({ { "model", next } }), "Downloading",
            [this, next] (const juce::var&)
            {
                missingModels.erase (missingModels.begin());
                for (auto& m : allSources)
                    if (m.id == next)
                        m.downloaded = true;
                if (! missingModels.empty())
                {
                    step = Step::idle; // the next one
                    return downloadModels();
                }
                step = Step::idle;
                // The choices may have changed while downloading: whatever they need now.
                showNeeds();
                if (! missingModels.empty())
                    return setStep (Step::idle, "Downloaded. The choices made since need more.");
                if (song.existsAsFile() && prepared.existsAsFile() && ! rhythm)
                {
                    setStep (Step::tempo);
                    return checkReady ([this] { findTempo(); });
                }
                // On with what was asked for (separating, adding the stems), which needed them.
                if (auto asked = std::move (resume))
                {
                    resume = nullptr;
                    setStep (Step::done, "Downloaded.");
                    return asked();
                }
                setStep (Step::done, "Downloaded.");
            });
}

void Analyzer::runJob (const juce::String& method, const juce::var& params, const juce::String& what,
                       const std::function<void (const juce::var&)>& done)
{
    progress = 0.0;
    job = "starting";
    updateButtons();
    // Its progress reaches the window through the listener setService() adds.
    ai::runJob (
        *service, method, params, alive,
        [this] (const juce::String& started)
        {
            job = started;
            updateButtons();
        },
        [this, done] (const juce::var& result, const juce::String& error)
        {
            if (error.isNotEmpty())
                return fail (error);
            job.clear();
            progress = -1.0;
            done (result);
        });
    juce::ignoreUnused (what);
}

void Analyzer::findTempo()
{
    setStep (Step::tempo, "Finding the beat...");
    runJob ("ai.v1.analyze.tempo",
            object ({ { "file", prepared.getFullPathName() }, { "cacheFolder", getCacheFolder().getFullPathName() } }),
            "Finding the beat",
            [this] (const juce::var& result)
            {
                getLibrary().set (songDigest, song.getFileName(), "rhythm", result);
                applyTempo (result);
                findKey();
            });
}

void Analyzer::applyTempo (const juce::var& result)
{
    beatTimes.clear();
    if (auto* list = result["beats"].getArray())
        for (auto& b : *list)
            beatTimes.push_back ((double)b);
    if (! result["tempo"].isVoid())
    {
        rhythm = Rhythm { (double)result["tempo"], (double)result["firstDownbeat"],
                          juce::jmax (1, (int)result["beatsPerBar"]) };
        tempoText.setText (juce::String (rhythm->tempo, 1) + " BPM, " + juce::String (rhythm->beatsPerBar) +
                               " beats per bar",
                           juce::dontSendNotification);
    }
    else
        tempoText.setText ("No steady beat found.", juce::dontSendNotification);
}

void Analyzer::findKey()
{
    setStep (Step::key, "Finding the key...");
    runJob ("ai.v1.analyze.key", object ({ { "file", prepared.getFullPathName() } }), "Finding the key",
            [this] (const juce::var& result)
            {
                getLibrary().set (songDigest, song.getFileName(), "key", result);
                applyKey (result);
                setStep (Step::done, "Done. Split it into stems to hear and learn its parts.");
            });
}

void Analyzer::applyKey (const juce::var& result)
{
    key = model::Key { (int)result["tonic"], result["scale"].toString() };
    const auto confidence = (double)result["confidence"];
    keyText.setText (result["name"].toString() + (confidence < 0.3 ? juce::String (" (not sure; also maybe ") +
                                                                         result["others"][0]["name"].toString() + ")"
                                                                   : juce::String()),
                     juce::dontSendNotification);
}

juce::String Analyzer::stemDisplayName (const juce::String& stem)
{
    static const std::map<juce::String, juce::String> names {
        { "hh", "hi-hat" },
        { "vocal", "vocals" },
        { "lead-vocal", "lead vocals" },
        { "back-vocal", "backing vocals" },
        { "bowed_strings", "bowed strings" },
        { "keys", "keys" },
    };
    if (const auto found = names.find (stem); found != names.end())
        return found->second;
    return stem.replaceCharacter ('_', ' ').replaceCharacter ('-', ' ');
}

bool Analyzer::separate (int count)
{
    if (isBusy() || ! prepared.existsAsFile() || service == nullptr)
        return false;
    const auto model = separationModel (count);
    stemCount.setSelectedId (count, juce::dontSendNotification); // what the window then says it needs
    resume = [this] { separate (stemCount.getSelectedId()); };
    // Separated with this model before (here or in the Track creator): those stems, kept in the library.
    if (earlier.isObject() && ! getLibrary().findStems (songDigest, model).empty())
    {
        useKeptStems (model,
                      [this]
                      {
                          setStep (Step::done, "Separated before. Choose what to do with each stem below, then click "
                                               "Add to the project.");
                      });
        return true;
    }
    setStep (Step::separating, "Separating...");
    checkReady (
        [this, model]
        {
            setStep (Step::separating, "Separating (this takes about a tenth of the song's length on a graphics "
                                       "card, longer on the CPU)...");
            runJob ("ai.v1.stems.separate",
                    object ({ { "file", prepared.getFullPathName() },
                              { "model", model },
                              { "cacheFolder", getCacheFolder().getFullPathName() },
                              { "quiet", session.isPlaying() } }),
                    "Separating",
                    [this, model] (const juce::var& result)
                    {
                        stems.clear();
                        if (auto* o = result["stems"].getDynamicObject())
                            for (auto& property : o->getProperties())
                                stems[property.name.toString()] = juce::File (property.value.toString());
                        showStems (model);
                        // Kept in the library as FLAC (in the background), for the next time and the Track creator.
                        if (getLibrary().isAvailable() && songDigest.isNotEmpty())
                        {
                            earlier = getLibrary().read (songDigest);
                            stemTask = std::make_unique<BackgroundTask> (
                                "Spinus analyzer stems",
                                [library = getLibrary(), digest = songDigest, model,
                                 kept = stems] (const std::function<bool()>& stop)
                                { return library.keepStems (digest, model, kept, stop); },
                                [] (const juce::Result&) {});
                        }
                        setStep (Step::done, "Separated. Choose what to do with each stem below, then click "
                                             "Add to the project.");
                    });
        },
        juce::StringArray { model });
    return true;
}

void Analyzer::setStemChoice (int count) { stemCount.setSelectedId (count, juce::sendNotificationSync); }

std::vector<Analyzer::Source> Analyzer::makeSources (bool forDrums) const
{
    using Kind = Source::Kind;
    std::vector<Source> sources;
    if (forDrums)
    {
        // The drums' notes are the Drum Machine's pads: its Synth Kit or the closest samples, or the song's hits.
        sources.push_back (
            { "Drum Machine", "drum-machine", Kind::drumMachine, {}, { Way::asIs, Way::closestPreset }, false });
        sources.push_back ({ "Its own hits (Drum Machine)", "own", Kind::own, {}, { Way::asIs }, false });
        return sources;
    }
    sources.push_back ({ "Its own phrases (Sampler)", "own", Kind::own, {}, { Way::asIs }, false });
    // Every instrument there is to play notes, each once: Spinus's own synth, the free open-source ones that
    // are downloaded, then the installed plugins (one listed already, e.g. Surge XT installed by the system
    // too, is not listed again).
    std::set<juce::String> names;
    using Origin = plugins::DeviceList::Origin;
    for (auto origin : { Origin::spinus, Origin::bundled, Origin::installed })
        for (const auto& entry : session.getDeviceList().getEntries())
        {
            if (entry.origin != origin || ! entry.instrument || ! entry.available)
                continue;
            Source source;
            source.device = entry.device;
            source.name = entry.name;
            if (origin == Origin::spinus)
            {
                // The Sampler and the Beat Slicer play recordings and the Drum Machine pads: not notes made
                // from a melodic part (its own phrases are the Sampler's choice above).
                if (entry.device.stockType != "spinus.3x-osc")
                    continue;
                source.kind = Kind::threeOsc;
                source.key = entry.device.stockType;
                source.ways = { Way::asIs, Way::tuned };
            }
            else if (origin == Origin::bundled)
            {
                source.key = entry.device.stockType;
                if (entry.device.stockType == "bundled.surge-xt")
                {
                    source.kind = Kind::surge;
                    source.ways = { Way::asIs, Way::closestPreset, Way::tuned };
                    source.startsFromPreset = true;
                }
                else
                {
                    // Odin 2 and OB-Xf have presets and profiles of Spinus's own; any other is tuned by what
                    // its settings are called (whether there are enough is known once it is loaded),
                    // and its presets are looked for when they are asked for.
                    source.ways = { Way::asIs, Way::tuned };
                    if (mayHavePresets (entry.device))
                        source.ways.insert (source.ways.begin() + 1, Way::closestPreset);
                    source.experimental = ! SynthProfile::isTested (entry.device);
                    const auto profile = SynthProfile::find (entry.device);
                    source.startsFromPreset = profile && profile->start == "closestPreset";
                }
            }
            else
            {
                source.key = instrumentKey (entry.device);
                if (entry.badge.isNotEmpty())
                    source.name << " (" << entry.badge << ")";
                // Its presets: offered until Spinus has looked and found none (they are known only once it
                // is loaded).
                source.ways = { Way::asIs, Way::tuned };
                if (mayHavePresets (entry.device))
                    source.ways.insert (source.ways.begin() + 1, Way::closestPreset);
                source.experimental = true;
            }
            if (! names.insert (entry.name.toLowerCase()).second)
                continue;
            sources.push_back (std::move (source));
        }
    // One list in alphabetical order after its own phrases, whoever made the instrument.
    std::stable_sort (sources.begin() + 1, sources.end(),
                      [] (const Source& a, const Source& b) { return a.name.compareNatural (b.name) < 0; });
    return sources;
}

const Analyzer::Source* Analyzer::StemRow::chosen() const
{
    const auto index = source != nullptr ? source->getSelectedId() - 1 : -1;
    return juce::isPositiveAndBelow (index, (int)sources.size()) ? &sources[(size_t)index] : nullptr;
}

Analyzer::Way Analyzer::StemRow::chosenWay() const
{
    const auto* from = chosen();
    if (from == nullptr || way == nullptr)
        return Way::asIs;
    const auto picked = (Way)(way->getSelectedId() - 1);
    return std::find (from->ways.begin(), from->ways.end(), picked) != from->ways.end() ? picked : Way::asIs;
}

namespace
{
juce::String wayName (bool drums, Analyzer::Way way, bool experimental)
{
    using Way = Analyzer::Way;
    if (drums)
        return way == Way::closestPreset ? "Closest drum samples" : "Synth Kit";
    return way == Way::closestPreset ? "Closest preset"
           : way == Way::tuned       ? (experimental ? "Tuned to it (experimental)" : "Tuned to it (slow)")
                                     : "As is (default sound)";
}
} // namespace

void Analyzer::fillSources (const juce::String& stem, StemRow& row, const juce::String& keepKey)
{
    const bool drums = stem == "drums";
    row.sources = makeSources (drums);
    row.source->clear (juce::dontSendNotification);
    int selected = 0, fallback = 0;
    for (size_t i = 0; i < row.sources.size(); ++i)
    {
        const auto& source = row.sources[i];
        row.source->addItem (source.name, (int)i + 1);
        if (source.key == keepKey)
            selected = (int)i + 1;
        // The first time: the Drum Machine for the drums, 3x Osc for the rest.
        if (source.kind == Source::Kind::drumMachine || source.kind == Source::Kind::threeOsc)
            fallback = (int)i + 1;
    }
    row.source->setSelectedId (selected > 0 ? selected : juce::jmax (1, fallback), juce::dontSendNotification);
    fillWays (stem, row);
}

void Analyzer::fillWays (const juce::String& stem, StemRow& row)
{
    // The ways the chosen source supports; the one chosen before when it still can.
    const auto before = row.way->getNumItems() > 0 ? (Way)(row.way->getSelectedId() - 1) : Way::asIs;
    row.way->clear (juce::dontSendNotification);
    const auto* source = row.chosen();
    if (source == nullptr)
        return;
    for (auto way : source->ways)
        row.way->addItem (wayName (stem == "drums", way, source->experimental), 1 + (int)way);
    if (stem != "drums")
    {
        const bool presets =
            std::find (source->ways.begin(), source->ways.end(), Way::closestPreset) != source->ways.end();
        row.way->setTooltip ("How it plays them: as it is" +
                             juce::String (presets ? "; with the " + source->name +
                                                         " preset that sounds most like the " + stemDisplayName (stem) +
                                                         (source->kind == Source::Kind::surge || ! source->experimental
                                                              ? juce::String()
                                                              : " (if it has presets Spinus can choose from)")
                                                   : juce::String()) +
                             "; or tuned until it sounds like it (about half a minute)" +
                             (source->experimental
                                  ? ". Experimental: Spinus has not tried this plugin. Its presets are played and it "
                                    "is tuned inside Spinus, so a plugin that crashes takes Spinus down with it: "
                                    "save first."
                                  : ""));
    }
    const bool canKeep = std::find (source->ways.begin(), source->ways.end(), before) != source->ways.end();
    row.way->setSelectedId (1 + (int)(canKeep ? before : Way::asIs), juce::dontSendNotification);
}

void Analyzer::updateNoteChoices()
{
    for (auto& [name, row] : rows)
        for (auto* box : { row.use.get(), row.source.get(), row.way.get() })
            stemRows.removeChildComponent (box);
    rows.clear();
    clapWanted = false;
    for (auto& [name, file] : stems)
    {
        // What happens to it: the drums and the bass are replicated, the rest added as audio.
        const auto shown = stemDisplayName (name);
        auto& row = rows[name];
        row.use = std::make_unique<juce::ComboBox>();
        row.use->addItem ("Leave out the " + shown, 1 + (int)StemUse::leaveOut);
        row.use->addItem ("Replicate the " + shown, 1 + (int)StemUse::replicate);
        row.use->addItem ("Add the " + shown + " as audio", 1 + (int)StemUse::audio);
        row.use->setSelectedId (1 + (int)(name == "drums" || name == "bass" ? StemUse::replicate : StemUse::audio),
                                juce::dontSendNotification);
        row.use->setTooltip ("Replicate: notes on a new channel, played by the sound chosen beside it. As audio: "
                             "the stem itself on a Playlist track. Or leave it out.");

        // Replicated: what plays its notes, then how.
        row.source = std::make_unique<juce::ComboBox>();
        row.way = std::make_unique<juce::ComboBox>();
        if (name == "drums")
        {
            row.source->setTooltip ("What plays the drums: the Drum Machine, or the song's own hits on its pads");
            row.way->setTooltip ("The Drum Machine's sounds: its Synth Kit, or the samples that sound most like "
                                 "each of the song's drums (from the Synth Kit and your folder)");
        }
        else
        {
            row.source->setTooltip ("What plays the " + shown +
                                    "'s notes: its own phrases in a Sampler, Spinus's 3x Osc, a free "
                                    "open-source synth or one of your own instruments");
        }
        fillSources (name, row, {});
        row.use->onChange = [this]
        {
            clapWanted = false;
            showNeeds();
            updateButtons();
        };
        row.source->onChange = [this, name]
        {
            if (auto found = rows.find (name); found != rows.end())
                fillWays (name, found->second);
            clapWanted = false;
            showNeeds();
            updateButtons();
        };
        row.way->onChange = row.use->onChange;
        for (auto* box : { row.use.get(), row.source.get(), row.way.get() })
            stemRows.addChildComponent (*box);
        row.use->setVisible (true);
    }
    showNeeds();
    updateButtons(); // shows the stems' rows
    resized();
}

void Analyzer::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The same choices, from the new list (an instrument that is gone: the first choice again).
    for (auto& [name, row] : rows)
    {
        const auto* chosen = row.chosen();
        fillSources (name, row, chosen != nullptr ? chosen->key : juce::String());
    }
    updateButtons();
}

bool Analyzer::replicates (const juce::String& stem) const
{
    const auto found = rows.find (stem);
    return found != rows.end() && found->second.use->getSelectedId() == 1 + (int)StemUse::replicate;
}

Analyzer::Sounds Analyzer::soundFor (const Source& source, Way way)
{
    switch (source.kind)
    {
    case Source::Kind::own:
        return Sounds::song;
    case Source::Kind::drumMachine:
        return way == Way::closestPreset ? Sounds::presets : Sounds::spinus;
    case Source::Kind::threeOsc:
        return way == Way::tuned ? Sounds::tunedThreeOsc : Sounds::spinus;
    case Source::Kind::surge:
        return way == Way::closestPreset ? Sounds::presets
               : way == Way::tuned       ? Sounds::tunedSurge
                                         : Sounds::instrument;
    case Source::Kind::device:
        break;
    }
    return way == Way::tuned           ? Sounds::tunedInstrument
           : way == Way::closestPreset ? Sounds::presets
                                       : Sounds::instrument;
}

std::map<juce::String, Analyzer::StemUse> Analyzer::getChosenUses() const
{
    std::map<juce::String, StemUse> chosen;
    for (auto& [name, row] : rows)
        chosen[name] = (StemUse)(row.use->getSelectedId() - 1);
    return chosen;
}

std::map<juce::String, Analyzer::Sounds> Analyzer::getChosenSounds() const
{
    std::map<juce::String, Sounds> chosen;
    for (auto& [name, row] : rows)
        if (const auto* source = row.chosen())
            chosen[name] = soundFor (*source, row.chosenWay());
    return chosen;
}

std::map<juce::String, model::Device> Analyzer::getChosenInstruments() const
{
    std::map<juce::String, model::Device> chosen;
    for (auto& [name, row] : rows)
        if (const auto* source = row.chosen(); source != nullptr && name != "drums")
            if (const auto sound = soundFor (*source, row.chosenWay());
                sound == Sounds::instrument || sound == Sounds::tunedInstrument || sound == Sounds::presets)
                chosen[name] = source->device;
    return chosen;
}

void Analyzer::setChosenUse (const juce::String& stem, StemUse use)
{
    if (auto found = rows.find (stem); found != rows.end())
        found->second.use->setSelectedId (1 + (int)use, juce::sendNotificationSync);
}

juce::StringArray Analyzer::getSoundSources (bool forDrums) const
{
    juce::StringArray names;
    for (auto& source : makeSources (forDrums))
        names.add (source.name);
    return names;
}

std::vector<Analyzer::Way> Analyzer::getWays (const juce::String& stem) const
{
    const auto found = rows.find (stem);
    const auto* source = found != rows.end() ? found->second.chosen() : nullptr;
    return source != nullptr ? source->ways : std::vector<Way> {};
}

juce::String Analyzer::getChosenSource (const juce::String& stem) const
{
    const auto found = rows.find (stem);
    const auto* source = found != rows.end() ? found->second.chosen() : nullptr;
    return source != nullptr ? source->name : juce::String();
}

juce::StringArray Analyzer::getWayNames (const juce::String& stem) const
{
    juce::StringArray names;
    if (const auto found = rows.find (stem); found != rows.end())
        for (int i = 0; i < found->second.way->getNumItems(); ++i)
            names.add (found->second.way->getItemText (i));
    return names;
}

bool Analyzer::setChosenSound (const juce::String& stem, const juce::String& sourceName, Way way)
{
    const auto found = rows.find (stem);
    if (found == rows.end())
        return false;
    auto& row = found->second;
    for (size_t i = 0; i < row.sources.size(); ++i)
        if (row.sources[i].name == sourceName)
        {
            const auto& ways = row.sources[i].ways;
            if (std::find (ways.begin(), ways.end(), way) == ways.end())
                return false;
            row.source->setSelectedId ((int)i + 1, juce::sendNotificationSync);
            row.way->setSelectedId (1 + (int)way, juce::sendNotificationSync);
            return true;
        }
    return false;
}

void Analyzer::cannotAdd (const juce::String& why)
{
    status.setText ("Nothing was added: " + why, juce::dontSendNotification);
}

bool Analyzer::addChosen()
{
    if (isBusy())
        return false;
    if (rows.empty())
    {
        cannotAdd ("separate the song into stems first.");
        return false;
    }
    const auto uses = getChosenUses();
    if (std::none_of (uses.begin(), uses.end(), [] (auto& u) { return u.second != StemUse::leaveOut; }))
    {
        cannotAdd ("every stem is left out.");
        return false;
    }
    if (! addStems (uses, getChosenSounds(), getChosenInstruments()))
        return false;                 // it says why
    resume = [this] { addChosen(); }; // with the window's choices then, when model files are downloaded first
    return true;
}

bool Analyzer::addStems (const std::map<juce::String, StemUse>& usePerStem,
                         const std::map<juce::String, Sounds>& soundPerStem,
                         const std::map<juce::String, model::Device>& instrumentPerStem)
{
    if (isBusy())
        return false;
    juce::StringArray asAudio;
    std::map<juce::String, Sounds> replicated;
    for (auto& [name, use] : usePerStem)
    {
        if (! stems.contains (name))
        {
            cannotAdd ("this song has no " + stemDisplayName (name) + " stem; separate it again.");
            return false;
        }
        if (use == StemUse::audio)
            asAudio.add (name);
        else if (use == StemUse::replicate)
            replicated[name] = soundPerStem.contains (name) ? soundPerStem.at (name) : Sounds::spinus;
    }
    if (replicated.empty())
    {
        if (asAudio.isEmpty())
            cannotAdd ("every stem is left out.");
        return addStemsAsAudio (asAudio);
    }
    // The audio goes in with the notes, once they are made, as one undo step.
    if (! makeNotesWith (replicated, instrumentPerStem))
        return false;
    audioStems = std::move (asAudio);
    resume = [this, usePerStem, soundPerStem, instrumentPerStem]
    { addStems (usePerStem, soundPerStem, instrumentPerStem); };
    return true;
}

bool Analyzer::makeNotes (const juce::StringArray& names, Sounds withSounds)
{
    std::map<juce::String, Sounds> chosen;
    for (auto& name : names)
        chosen[name] = withSounds;
    return makeNotesWith (chosen);
}

Analyzer::Sounds Analyzer::soundOf (const juce::String& stem) const
{
    const auto found = stemSounds.find (stem);
    return found != stemSounds.end() ? found->second : Sounds::spinus;
}

bool Analyzer::makeNotesWith (const std::map<juce::String, Sounds>& soundPerStem,
                              const std::map<juce::String, model::Device>& instrumentPerStem)
{
    juce::StringArray names;
    for (auto& [name, sound] : soundPerStem)
        names.add (name);
    if (isBusy() || names.isEmpty())
        return false;
    if (service == nullptr)
    {
        cannotAdd ("the Analyzer uses AI that runs on this computer: turn on AI in Options > AI.");
        return false;
    }
    for (auto& [name, sound] : soundPerStem)
    {
        if (! stems.contains (name))
        {
            cannotAdd ("this song has no " + stemDisplayName (name) + " stem; separate it again.");
            return false;
        }
        if ((sound == Sounds::instrument || sound == Sounds::tunedInstrument) &&
            (name == "drums" || ! instrumentPerStem.contains (name)))
        {
            cannotAdd ("no instrument is chosen for the " + stemDisplayName (name) + ".");
            return false;
        }
    }
    notesQueue = names;
    audioStems.clear();
    transcribed.clear();
    pieces.clear();
    closest.clear();
    madeChannels.clear();
    tuned.clear();
    tuneNotes.clear();
    heard.clear();
    updatePresetChoices();
    stemSounds = soundPerStem;
    stemInstruments = instrumentPerStem;
    setStep (Step::transcribing, "Listening for notes...");
    drumGroups.clear();
    drumSuggestions.clear();
    // An instrument's presets are measured when a melodic stem plays with its closest preset (or is tuned from
    // it); CLAP is needed for that and for suggesting drum samples.
    std::vector<model::Device> withPresets;
    std::set<juce::String> listed;
    bool clap = false;
    for (auto& name : names)
    {
        if (usesPresets (name))
            if (const auto device = instrumentOf (name); listed.insert (instrumentKey (device)).second)
                withPresets.push_back (device);
        clap = clap || usesClap (name);
    }
    presetSets.clear();
    clapWanted = clap; // what the window says it needs, until a choice changes
    resume = [this, soundPerStem, instrumentPerStem] { makeNotesWith (soundPerStem, instrumentPerStem); };
    if (! withPresets.empty() || clap)
    {
        checkReady ([this, withPresets] { measurePresets (withPresets, 0, [this] { transcribeNext(); }); },
                    { "clap_music" });
        return true;
    }
    checkReady ([this] { transcribeNext(); });
    return true;
}

bool Analyzer::usesClap (const juce::String& stem) const
{
    // Presets and the drum samples are compared by sound (CLAP); tuning on its own is not.
    const auto sound = soundOf (stem);
    return usesPresets (stem) || (stem == "drums" && (sound == Sounds::presets || sound == Sounds::tunedSurge ||
                                                      sound == Sounds::tunedThreeOsc));
}

bool Analyzer::usesPresets (const juce::String& stem) const
{
    if (stem == "drums")
        return false;
    const auto sound = soundOf (stem);
    if (sound == Sounds::presets || sound == Sounds::tunedSurge)
        return true;
    if (sound != Sounds::tunedInstrument || ! stemInstruments.contains (stem))
        return false;
    const auto profile = SynthProfile::find (stemInstruments.at (stem));
    return profile && profile->start == "closestPreset";
}

model::Device Analyzer::instrumentOf (const juce::String& stem) const
{
    const auto sound = soundOf (stem);
    if (const auto chosen = stemInstruments.find (stem);
        chosen != stemInstruments.end() &&
        (sound == Sounds::instrument || sound == Sounds::tunedInstrument || sound == Sounds::presets))
        return chosen->second;
    model::Device device;
    device.kind = model::Device::Kind::stock;
    device.stockType = sound == Sounds::presets || sound == Sounds::tunedSurge ? "bundled.surge-xt"
                       : sound == Sounds::song                                 ? "spinus.sampler"
                                                                               : "spinus.3x-osc";
    return device;
}

juce::String Analyzer::instrumentKey (const model::Device& device)
{
    return device.kind == model::Device::Kind::stock
               ? device.stockType
               : device.plugin.format + "|" + device.plugin.fileOrIdentifier + "|" + device.plugin.name;
}

juce::String Analyzer::instrumentName (const model::Device& device) const
{
    if (auto profile = SynthProfile::find (device); profile && ! profile->automatic)
        return profile->name;
    for (const auto& entry : session.getDeviceList().getEntries())
        if (entry.device.kind == device.kind && entry.device.stockType == device.stockType &&
            entry.device.plugin == device.plugin)
            return entry.name;
    return device.kind == model::Device::Kind::plugin ? device.plugin.name : device.stockType;
}

bool Analyzer::mayHavePresets (const model::Device& device) const
{
    if (device.kind != model::Device::Kind::plugin)
        return true;
    const auto found = presetCounts.find (instrumentKey (device) + "|" + pluginVersionOf (device));
    return found == presetCounts.end() || found->second > 1;
}

void Analyzer::rememberPresets (const model::Device& device, int count)
{
    if (device.kind != model::Device::Kind::plugin)
        return;
    presetCounts[instrumentKey (device) + "|" + pluginVersionOf (device)] = count;
    if (const auto file = presetCountsFile (service); file != juce::File())
    {
        auto* o = new juce::DynamicObject();
        for (auto& [plugin, n] : presetCounts)
            o->setProperty (plugin, n);
        file.replaceWithText (juce::JSON::toString (juce::var (o)));
    }
}

void Analyzer::transcribeNext()
{
    if (notesQueue.isEmpty())
    {
        clapWanted = false;
        addNotesToProject(); // it says what it added, or why it could not
        return setStep (Step::done, status.getText());
    }
    const auto name = notesQueue[0];
    notesQueue.remove (0);
    setStep (Step::transcribing,
             (soundOf (name) == Sounds::song ? "Cutting samples from the " : "Listening for the notes of the ") + name +
                 "...");
    juce::Array<juce::var> beats;
    for (auto b : beatTimes)
        beats.add (b);
    const bool ownSounds = soundOf (name) == Sounds::song;
    const auto* method = ownSounds         ? "ai.v1.samples.make"
                         : name == "drums" ? "ai.v1.transcribe.drums"
                                           : "ai.v1.transcribe.notes";
    auto params = object ({ { "file", stems[name].getFullPathName() },
                            { "drums", name == "drums" },
                            { "beats", beats },
                            { "firstDownbeat", rhythm ? rhythm->firstDownbeat : 0.0 },
                            { "beatsPerBar", rhythm ? rhythm->beatsPerBar : 4 },
                            { "cacheFolder", getCacheFolder().getFullPathName() } });
    // Found before in these stems (the library keeps them by separation): not listened for again.
    const auto notesKey = stemsModel + "/" + name;
    if (! ownSounds && stemsModel.isNotEmpty())
    {
        const auto entry = getLibrary().read (songDigest);
        if (const auto& hits = entry["drums"][juce::Identifier (stemsModel)]; name == "drums" && hits.isArray())
            params.getDynamicObject()->setProperty ("hits", hits);
        if (const auto& known = entry["notes"][juce::Identifier (notesKey)]; name != "drums" && known.isArray())
            params.getDynamicObject()->setProperty ("known", known);
    }
    runJob (method, params, ownSounds ? "Cutting samples" : "Listening for notes",
            [this, name, ownSounds, notesKey] (const juce::var& result)
            {
                if (! ownSounds && stemsModel.isNotEmpty())
                {
                    if (name == "drums" && result["hits"].isArray())
                        getLibrary().setIn (songDigest, song.getFileName(), "drums", stemsModel, result["hits"]);
                    else if (name != "drums" && result["seconds"].isArray())
                        getLibrary().setIn (songDigest, song.getFileName(), "notes", notesKey, result["seconds"]);
                }
                if (auto* list = result["samples"].getArray())
                {
                    auto& cut = pieces[name];
                    for (auto& p : *list)
                        cut.push_back (
                            { (int)(p.hasProperty ("pad") ? p["pad"] : p["key"]), (double)p["start"], (double)p["end"],
                              song.getFileNameWithoutExtension() + " - " +
                                  (p.hasProperty ("kind") ? p["kind"].toString().replace ("_", " ")
                                                          : "phrase " + juce::String ((int)cut.size() + 1)) });
                }
                if (auto* groups = result["groups"].getArray())
                    for (auto& g : *groups)
                        drumGroups.push_back ({ (int)g["pad"], g["kind"].toString(), g["name"].toString(),
                                                (int)g["count"], (double)g["start"], (double)g["end"] });
                auto& notes = transcribed[name];
                if (auto* list = result["notes"].getArray())
                    for (auto& n : *list)
                        notes.push_back ({ (model::Ticks)(juce::int64)n["start"],
                                           (model::Ticks)(juce::int64)n["length"],
                                           (int)n["pitch"],
                                           (float)(double)n["velocity"],
                                           0.5f,
                                           0.0f,
                                           0.0f,
                                           {} });
                if (auto* list = result["seconds"].getArray())
                    for (auto& n : *list)
                        heard[name].push_back ({ (int)n["pitch"], (double)n["start"],
                                                 (double)n["end"] - (double)n["start"], (float)(double)n["velocity"] });
                const auto sound = soundOf (name);
                const bool tune =
                    sound == Sounds::tunedSurge || sound == Sounds::tunedThreeOsc || sound == Sounds::tunedInstrument;
                if (name == "drums" && usesClap (name) && ! drumGroups.empty())
                    return measureDrumSamples ([this] { matchDrumGroups (0, [this] { transcribeNext(); }); });
                if (name == "drums" || (! usesPresets (name) && ! tune))
                    return transcribeNext();
                // The closest presets first (to play, or to tune from); an instrument without any plays as it
                // is (measurePresets said so).
                const auto set = presetSets.find (instrumentKey (instrumentOf (name)));
                if (! usesPresets (name) || set == presetSets.end() || set->second.presets.empty())
                    return tune ? tuneSynth (name) : transcribeNext();
                juce::Array<juce::var> only;
                for (const auto& p : set->second.presets)
                    only.add (p.id);
                setStep (Step::transcribing,
                         "Finding the " + set->second.name + " presets closest to the " + name + "...");
                runJob ("ai.v1.presets.match",
                        object ({ { "library", set->second.library },
                                  { "file", stems[name].getFullPathName() },
                                  { "top", 5 },
                                  { "only", only } }),
                        "Finding presets",
                        [this, name, key = set->first, tune] (const juce::var& found)
                        {
                            auto& list = closest[name];
                            if (const auto known = presetSets.find (key); known != presetSets.end())
                                if (auto* presets = found["presets"].getArray())
                                    for (auto& p : *presets)
                                        for (auto& preset : known->second.presets)
                                            if (preset.id == p["id"].toString())
                                                list.push_back (preset);
                            if (tune)
                                return tuneSynth (name);
                            transcribeNext();
                        });
            });
}

juce::File Analyzer::getSettingsFile() const
{
    return service != nullptr ? service->getFolder().getChildFile ("analyzer.json") : juce::File();
}

void Analyzer::setDrumSamplesFolder (const juce::File& folder)
{
    drumSamplesFolder = folder;
    if (const auto file = getSettingsFile(); file != juce::File())
        file.replaceWithText (juce::JSON::toString (object ({ { "drumSamples", folder.getFullPathName() } })));
}

void Analyzer::measureDrumSamples (const std::function<void()>& then)
{
    // The candidates: Spinus's Synth Kit (written as files once) and the drum samples folder's short sounds.
    drumCandidates.clear();
    const auto kit = service->getFolder().getChildFile ("drum-samples/Synth Kit");
    for (auto& drum : dsp::makeSynthKit (48000.0))
    {
        const auto file = kit.getChildFile (drum.name + ".wav");
        if (! file.existsAsFile())
        {
            kit.createDirectory();
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
            if (auto writer = juce::WavAudioFormat().createWriterFor (
                    stream,
                    juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (
                        24)))
                writer->writeFromAudioSampleBuffer (drum.audio, 0, drum.audio.getNumSamples());
        }
        drumCandidates[file.getFullPathName()] = file;
    }
    if (drumSamplesFolder.isDirectory())
    {
        int looked = 0;
        for (const auto& entry : juce::RangedDirectoryIterator (
                 drumSamplesFolder, true, "*.wav;*.flac;*.aif;*.aiff;*.ogg", juce::File::findFiles))
        {
            if (++looked > 5000)
                break;
            std::unique_ptr<juce::AudioFormatReader> reader (dsp::getAudioFormats().createReaderFor (entry.getFile()));
            if (reader != nullptr && reader->sampleRate > 0.0 &&
                (double)reader->lengthInSamples / reader->sampleRate <= 3.0)
                drumCandidates[entry.getFile().getFullPathName()] = entry.getFile();
        }
    }
    juce::Array<juce::var> list;
    for (auto& [id, file] : drumCandidates)
        list.add (object ({ { "id", id }, { "version", presetVersion (file) } }));
    setStep (Step::transcribing, "Checking which drum samples are measured...");
    service->call ("ai.v1.presets.missing", object ({ { "library", "drum-samples" }, { "presets", list } }),
                   [this, a = alive, then] (const juce::var& result, const juce::String& error)
                   {
                       if (! *a)
                           return;
                       if (error.isNotEmpty())
                           return fail (error);
                       std::vector<juce::String> missing;
                       if (auto* ids = result["missing"].getArray())
                           for (auto& id : *ids)
                               missing.push_back (id.toString());
                       addSounds (std::move (missing), 0, then);
                   });
}

void Analyzer::addSounds (std::vector<juce::String> missing, size_t from, const std::function<void()>& then)
{
    if (from >= missing.size() || step != Step::transcribing)
    {
        if (step == Step::transcribing)
            then();
        return;
    }
    constexpr size_t batch = 64;
    juce::Array<juce::var> sent;
    const auto end = juce::jmin (missing.size(), from + batch);
    for (auto i = from; i < end; ++i)
        sent.add (object (
            { { "id", missing[i] }, { "version", presetVersion (juce::File (missing[i])) }, { "file", missing[i] } }));
    setStep (Step::transcribing, "Listening to the drum samples, once: " + juce::String ((int)from) + " of " +
                                     juce::String ((int)missing.size()) + "...");
    runJob (
        "ai.v1.presets.add",
        object ({ { "library", "drum-samples" }, { "previews", sent }, { "oneShot", true }, { "keepFiles", true } }),
        "Measuring drum samples", [this, missing, end, then] (const juce::var&) { addSounds (missing, end, then); });
}

void Analyzer::matchDrumGroups (size_t index, const std::function<void()>& then)
{
    if (index >= drumGroups.size())
        return then();
    const auto group = drumGroups[index];
    setStep (Step::transcribing, "Finding drum samples like the song's " + group.name + "...");
    juce::Array<juce::var> only;
    for (auto& [id, file] : drumCandidates)
        only.add (id);
    runJob ("ai.v1.presets.match",
            object ({ { "library", "drum-samples" },
                      { "file", stems["drums"].getFullPathName() },
                      { "start", group.start },
                      { "end", group.end },
                      { "top", 5 },
                      { "only", only } }),
            "Finding drum samples",
            [this, index, group, then] (const juce::var& found)
            {
                auto& list = drumSuggestions[group.pad];
                if (auto* matches = found["presets"].getArray())
                    for (auto& m : *matches)
                        list.push_back (juce::File (m["id"].toString()));
                matchDrumGroups (index + 1, then);
            });
}

bool Analyzer::useDrumSuggestion (int pad, int index)
{
    const auto found = drumSuggestions.find (pad);
    if (found == drumSuggestions.end() || ! madeChannels.contains ("drums") ||
        ! juce::isPositiveAndBelow (index, (int)found->second.size()))
        return false;
    return loadDrumPadFile (session, madeChannels["drums"], pad, found->second[(size_t)index]).wasOk();
}

void Analyzer::tuneSynth (const juce::String& name)
{
    // Which synth: Surge XT, 3x Osc, or the instrument chosen for the stem; with its profile, or one made
    // from its settings' names.
    const auto sound = soundOf (name);
    if (sound == Sounds::tunedInstrument && ! stemInstruments.contains (name))
        return transcribeNext();
    const auto device = instrumentOf (name);
    auto profile = SynthProfile::find (device);
    const auto excerpt = chooseExcerpt (heard[name]);
    if ((! profile && sound != Sounds::tunedInstrument) || excerpt.notes.empty())
        return transcribeNext(); // nothing to tune to: the synth plays as it is
    const auto target = readExcerpt (stems[name], excerpt.start, excerpt.length);
    const auto synth = profile ? profile->name : instrumentName (device);
    matcher = std::make_unique<SynthMatcher> (session, device, profile);
    if (matcher->getProblem().isNotEmpty())
    {
        matcher = nullptr;
        if (sound != Sounds::tunedInstrument)
            return fail (synth + " is not installed.");
        tuneNotes.add (synth + " could not be loaded to tune it; it plays its own sound.");
        return transcribeNext();
    }
    if (matcher->getNumSettings() < SynthProfile::fewestSettings)
    {
        // Too few settings Spinus can tune: it plays as it is.
        matcher = nullptr;
        tuneNotes.add (synth + " has no settings Spinus can tune; it plays its own sound.");
        return transcribeNext();
    }
    if (matcher->getProfile().start == "closestPreset" && ! closest[name].empty())
        matcher->loadPreset (closest[name].front());
    const auto tries = tryLimit > 0 ? tryLimit : matcher->getProfile().getTries();
    setStep (Step::transcribing, "Tuning " + synth + " to the " + name + "...");
    progress = 0.0;
    matcher->start (
        target, excerpt.notes, tries,
        [this, name, synth, tries] (int done, double)
        {
            progress = (double)done / (double)tries;
            status.setText ("Tuning " + synth + " to the " + name + ": " + juce::String (done) + " of " +
                                juce::String (tries) + " tries...",
                            juce::dontSendNotification);
        },
        [this, a = alive, name]
        {
            tuned[name] = matcher->getResult();
            // The matcher is done; let it go once this call has returned.
            juce::MessageManager::callAsync (
                [this, a]
                {
                    if (! *a)
                        return;
                    matcher = nullptr;
                    progress = -1.0;
                    if (step == Step::transcribing)
                        transcribeNext();
                });
        });
}

void Analyzer::measurePresets (std::vector<model::Device> instruments, size_t index, const std::function<void()>& then)
{
    if (index >= instruments.size() || step != Step::transcribing)
    {
        previews = nullptr;
        if (step == Step::transcribing)
            then();
        return;
    }
    // Every preset is measured once (kept by the AI service); afterwards only new or changed ones.
    const auto& device = instruments[index];
    const auto instrument = instrumentKey (device);
    const auto name = instrumentName (device);
    const bool surge = device.kind == model::Device::Kind::stock && device.stockType == "bundled.surge-xt";
    const auto next = [this, instruments, index, then] { measurePresets (instruments, index + 1, then); };
    // Said with the result when a stem was to play with one of its presets (a stem tuned from its closest
    // preset just starts from its default sound).
    const auto playsAsIs = [this, &instrument] (const juce::String& why)
    {
        for (auto& [stem, sound] : stemSounds)
            if (sound == Sounds::presets && stem != "drums" && instrumentKey (instrumentOf (stem)) == instrument)
            {
                if (! tuneNotes.contains (why))
                    tuneNotes.add (why);
                return;
            }
    };
    setStep (Step::transcribing, "Looking for " + name + "'s presets...");
    previews = std::make_unique<PresetPreviews> (session, device);
    auto* instance = previews->getInstance();
    if (instance == nullptr)
    {
        previews = nullptr;
        if (surge)
        {
            needsText.setText ("Closest presets need Surge XT: download it in Options > Plugins.",
                               juce::dontSendNotification);
            return fail ("Surge XT is not installed.");
        }
        playsAsIs (name + " could not be loaded to choose a preset; it plays its own sound.");
        return next();
    }
    auto& set = presetSets[instrument];
    set.name = name;
    set.library = InstrumentPresets::libraryOf (device);
    set.presets = InstrumentPresets::find (*instance, device);
    // Remembered only for the plugin itself: one run separately shows no programs.
    if (instance->getPluginDescription().pluginFormatName != plugins::sandbox::SandboxPluginFormat::formatName)
        rememberPresets (device, (int)set.presets.size());
    if (presetLimit >= 0 && (int)set.presets.size() > presetLimit)
        set.presets.resize ((size_t)presetLimit);
    if (set.presets.empty())
    {
        previews = nullptr;
        // OB-Xf downloaded before its patches came with it.
        playsAsIs (device.kind == model::Device::Kind::stock && device.stockType == "bundled.ob-xf"
                       ? "OB-Xf's factory patches are missing (remove OB-Xf in Options > Plugins and download it "
                         "again to get them); it plays its own sound."
                       : name + " has no presets Spinus can choose from; it plays its own sound.");
        return next();
    }

    juce::Array<juce::var> list;
    for (auto& p : set.presets)
        list.add (object ({ { "id", p.id }, { "version", p.version } }));
    setStep (Step::transcribing, "Checking which " + name + " presets are measured...");
    service->call ("ai.v1.presets.missing", object ({ { "library", set.library }, { "presets", list } }),
                   [this, a = alive, instrument, next] (const juce::var& result, const juce::String& error)
                   {
                       if (! *a)
                           return;
                       if (error.isNotEmpty())
                           return fail (error);
                       std::vector<juce::String> missing;
                       if (auto* ids = result["missing"].getArray())
                           for (auto& id : *ids)
                               missing.push_back (id.toString());
                       measureBatch (instrument, std::move (missing), 0, next);
                   });
}

void Analyzer::measureBatch (const juce::String& instrument, std::vector<juce::String> missing, size_t from,
                             const std::function<void()>& then)
{
    const auto set = presetSets.find (instrument);
    if (from >= missing.size() || step != Step::transcribing || set == presetSets.end() || previews == nullptr)
    {
        if (step == Step::transcribing)
            then();
        return;
    }
    // Spinus records a batch, then the AI service measures it (and deletes the recordings).
    constexpr size_t batch = 48;
    const auto folder = getCacheFolder().getChildFile ("preset-previews");
    std::map<juce::String, const InstrumentPreset*> byId;
    for (const auto& p : set->second.presets)
        byId[p.id] = &p;
    std::vector<PresetPreviews::Item> items;
    juce::Array<juce::var> sent;
    auto end = from;
    for (; end < missing.size() && items.size() < batch; ++end)
    {
        const auto found = byId.find (missing[end]);
        if (found == byId.end())
            continue;
        const auto output = folder.getChildFile (juce::String ((int)end) + ".wav");
        items.push_back ({ *found->second, output });
        sent.add (object (
            { { "id", missing[end] }, { "version", found->second->version }, { "file", output.getFullPathName() } }));
    }
    const auto total = (int)missing.size();
    setStep (Step::transcribing, "Listening to " + set->second.name + "'s presets, once: " + juce::String ((int)from) +
                                     " of " + juce::String (total) + "...");
    progress = (double)from / (double)total;
    previews->render (std::move (items), {},
                      [this, a = alive, instrument, library = set->second.library, missing, end, sent,
                       then] (const juce::Result& rendered)
                      {
                          if (! *a)
                              return;
                          if (rendered.failed())
                              return fail (rendered.getErrorMessage());
                          runJob ("ai.v1.presets.add", object ({ { "library", library }, { "previews", sent } }),
                                  "Measuring presets", [this, instrument, missing, end, then] (const juce::var&)
                                  { measureBatch (instrument, missing, end, then); });
                      });
}

bool Analyzer::useClosestPreset (const juce::String& stem, int index)
{
    if (! closest.contains (stem) || ! madeChannels.contains (stem) ||
        ! juce::isPositiveAndBelow (index, (int)closest[stem].size()))
        return false;
    return loadInstrumentPreset (session, madeChannels[stem], closest[stem][(size_t)index]).wasOk();
}

void Analyzer::updatePresetChoices()
{
    for (auto& [name, box] : presetChoices)
        stemRows.removeChildComponent (box.get());
    presetChoices.clear();
    for (auto& [name, presets] : closest)
    {
        if (presets.empty())
            continue;
        auto box = std::make_unique<juce::ComboBox>();
        for (size_t i = 0; i < presets.size(); ++i)
            box->addItem (stemDisplayName (name) + ": " + presets[i].name +
                              (presets[i].category.isNotEmpty() ? " (" + presets[i].category + ")" : juce::String()),
                          (int)i + 1);
        box->setSelectedId (1, juce::dontSendNotification);
        const auto set = presetSets.find (instrumentKey (instrumentOf (name)));
        box->setTooltip ("The " + (set != presetSets.end() ? set->second.name : juce::String ("instrument's")) +
                         " presets that sound most like the " + stemDisplayName (name) + ", closest first");
        box->onChange = [this, n = name, b = box.get()] { useClosestPreset (n, b->getSelectedId() - 1); };
        stemRows.addAndMakeVisible (*box);
        presetChoices[name] = std::move (box);
    }
    for (auto& group : drumGroups)
    {
        const auto found = drumSuggestions.find (group.pad);
        if (found == drumSuggestions.end() || found->second.empty())
            continue;
        auto box = std::make_unique<juce::ComboBox>();
        for (size_t i = 0; i < found->second.size(); ++i)
        {
            const auto& file = found->second[i];
            box->addItem ("drums, " + group.name + ": " + file.getFileNameWithoutExtension() + " (" +
                              file.getParentDirectory().getFileName() + ")",
                          (int)i + 1);
        }
        box->setSelectedId (1, juce::dontSendNotification);
        box->setTooltip ("The drum samples that sound most like the song's " + group.name + ", closest first");
        box->onChange = [this, pad = group.pad, b = box.get()] { useDrumSuggestion (pad, b->getSelectedId() - 1); };
        stemRows.addAndMakeVisible (*box);
        presetChoices["drums " + juce::String (group.pad)] = std::move (box);
    }
    resized();
}

bool Analyzer::addNotesToProject()
{
    const auto title = song.getFileNameWithoutExtension();
    const auto barLength = session.getProject().settings.timeSignature.barTicks();
    model::Ticks end = barLength;
    for (auto& [name, notes] : transcribed)
        for (auto& n : notes)
            end = juce::jmax (end, n.start + n.length);
    const auto length = (end + barLength - 1) / barLength * barLength;

    Steps steps (session.getProject(), audioStems.isEmpty() ? "Notes from " + title : "Stems of " + title);
    // Nothing goes in when one step cannot be made; the status line says which and why.
    const auto failed = [this, &steps]
    {
        audioStems.clear();
        cannotAdd (steps.getError().isNotEmpty() ? steps.getError() : juce::String ("a stem's file is missing."));
        return false;
    };
    int audioCount = 0;
    if (! addAudioSteps (steps, audioStems, audioCount))
        return failed();
    audioStems.clear();
    const auto pattern = steps.add (
        "pattern.add", object ({ { "name", "Analysis: " + title }, { "length", (juce::int64)length } }))["pattern"];
    if (pattern.isVoid())
        return failed();
    int count = 0;
    std::map<juce::String, model::ChannelId> channels;
    for (auto& [name, notes] : transcribed)
    {
        // Drums play on a Drum Machine (its pads match the hits); the rest on 3x Osc (tuned or not), on a
        // Sampler with the stem's phrases, or on Surge XT with the closest preset (tuned or not).
        // Or on the instrument chosen for it (with its closest preset, tuned or not).
        const auto sound = soundOf (name);
        const auto chosen = stemInstruments.find (name);
        const auto device =
            name == "drums" ? object ({ { "kind", "stock" }, { "stockType", "spinus.drum-machine" } })
            : (sound == Sounds::instrument || sound == Sounds::tunedInstrument || sound == Sounds::presets) &&
                    chosen != stemInstruments.end()
                ? model::deviceToJson (chosen->second)
                : object ({ { "kind", "stock" }, { "stockType", instrumentOf (name).stockType } });
        const auto channel = steps.add (
            "channel.add", object ({ { "name", stemLabel (name, title) }, { "instrument", device } }))["channel"];
        if (channel.isVoid())
            return failed();
        channels[name] = model::ChannelId { (uint32_t)(int)channel };
        // As the notes command takes them (a note found at the very start, or one too short, still plays).
        juce::Array<juce::var> list;
        for (auto& n : notes)
            list.add (object ({ { "start", (juce::int64)juce::jmax<model::Ticks> (0, n.start) },
                                { "length", (juce::int64)juce::jmax<model::Ticks> (1, n.length) },
                                { "pitch", juce::jlimit (0, 127, n.pitch) },
                                { "velocity", juce::jlimit (0.0f, 1.0f, n.velocity) } }));
        if (steps
                .add ("pattern.setNotes",
                      object ({ { "pattern", pattern }, { "channel", channel }, { "notes", list } }))
                .isVoid())
            return failed();
        count += (int)notes.size();
    }
    const auto lane = steps.add ("lane.add", object ({ { "name", stemLabel ("notes", title) } }))["lane"];
    if (lane.isVoid() ||
        steps.add ("clip.addPattern", object ({ { "lane", lane }, { "pattern", pattern }, { "start", 0 } })).isVoid())
        return failed();
    auto result = steps.performOn (session.getHistory());
    if (result.failed())
    {
        cannotAdd (result.getErrorMessage());
        return false;
    }

    // The song's own sounds go into the new instruments' saved state (an undo keeps it).
    for (auto& [name, cut] : pieces)
        if (result.wasOk() && channels.contains (name))
            result = name == "drums" ? loadDrumPieces (session, channels[name], stems[name], cut)
                                     : loadSamplerPieces (session, channels[name], stems[name], cut);
    madeChannels = channels;
    // The closest presets: the best one now, the others offered beside the stem's name.
    for (auto& [name, presets] : closest)
        if (result.wasOk() && ! presets.empty() && channels.contains (name))
            result = loadInstrumentPreset (session, channels[name], presets.front());
    // The tuned settings, on top (the preset choices then start from the preset again).
    for (auto& [name, found] : tuned)
        if (result.wasOk() && channels.contains (name))
            SynthMatcher::apply (session, channels[name], found);
    for (auto it = closest.begin(); it != closest.end();)
        it =
            soundOf (it->first) == Sounds::presets ? std::next (it) : closest.erase (it); // tuned: a list would undo it
    // Drums: each group's closest sample on its pad; a second group without one starts from its kind's sound.
    if (result.wasOk() && channels.contains ("drums"))
        for (auto& group : drumGroups)
        {
            if (auto found = drumSuggestions.find (group.pad);
                found != drumSuggestions.end() && ! found->second.empty())
                loadDrumPadFile (session, channels["drums"], group.pad, found->second.front());
            else if (group.name.endsWith (" 2"))
                for (auto& first : drumGroups)
                    if (first.kind == group.kind && &first != &group)
                        copyDrumPad (session, channels["drums"], first.pad, group.pad, group.name);
        }
    updatePresetChoices();
    status.setText (result.wasOk()
                        ? juce::String (count) + " notes on " + juce::String ((int)transcribed.size()) +
                              " new channels, in the pattern \"Analysis: " + title + "\" (on the Playlist at bar 1)" +
                              (audioCount > 0 ? ", and " + juce::String (audioCount) +
                                                    (audioCount == 1 ? " stem" : " stems") + " as audio"
                                              : juce::String()) +
                              ". Ctrl+Z takes them away." +
                              (tuneNotes.isEmpty() ? juce::String() : " " + tuneNotes.joinIntoString (" "))
                        : "Added, but not every sound could be loaded: " + result.getErrorMessage(),
                    juce::dontSendNotification);
    return result.wasOk();
}

void Analyzer::cancel()
{
    if (stemTask != nullptr && step == Step::separating)
    {
        stemTask = nullptr;
        return fail ("Stopped.");
    }
    if (step == Step::preparing)
    {
        decoder = nullptr;
        return fail ("Stopped.");
    }
    if (previews != nullptr && previews->isRendering())
    {
        previews = nullptr;
        return fail ("Stopped.");
    }
    if (matcher != nullptr)
        return matcher->stop(); // keeps the best so far and carries on
    if (service != nullptr && job.isNotEmpty() && job != "starting")
        service->call ("ai.v1.job.cancel", object ({ { "job", job } }), {});
}

bool Analyzer::useTempo()
{
    if (! rhythm)
        return false;
    auto& registry = commands::CommandRegistry::get();
    auto transaction =
        std::make_unique<commands::Transaction> ("Use the tempo of " + song.getFileNameWithoutExtension());
    juce::String error;
    auto tempo = registry.create ("project.setTempo", object ({ { "bpm", rhythm->tempo } }), error);
    if (tempo == nullptr)
        return false;
    transaction->add (std::move (tempo));
    // A bar of 2-12 beats: the song's time signature in quarter notes, unless it is already that long.
    const auto& sig = session.getProject().settings.timeSignature;
    if (sig.numerator * 4 / juce::jmax (1, sig.denominator) != rhythm->beatsPerBar)
        if (auto signature =
                registry.create ("project.setTimeSignature",
                                 object ({ { "numerator", rhythm->beatsPerBar }, { "denominator", 4 } }), error))
            transaction->add (std::move (signature));
    return session.getHistory().perform (std::move (transaction)).wasOk();
}

bool Analyzer::useKey()
{
    if (! key)
        return false;
    juce::String error;
    auto command = commands::CommandRegistry::get().create (
        "project.setKey", object ({ { "tonic", key->tonic }, { "scale", key->scale } }), error);
    return command != nullptr && session.getHistory().perform (std::move (command)).wasOk();
}

/** Each stem on a muted track of its own, the song's first downbeat on bar 1, as one undo step. */
bool Analyzer::addStemsAsAudio (const juce::StringArray& names)
{
    if (names.isEmpty() || isBusy())
        return false;
    Steps steps (session.getProject(), "Add the stems of " + song.getFileNameWithoutExtension());
    int added = 0;
    if (! addAudioSteps (steps, names, added))
    {
        cannotAdd (steps.getError().isNotEmpty() ? steps.getError() : juce::String ("a stem's file is missing."));
        return false;
    }
    const auto result = steps.performOn (session.getHistory());
    if (result.failed())
        cannotAdd (result.getErrorMessage());
    else
        status.setText ("Added " + juce::String (added) + (added == 1 ? " stem" : " stems") +
                            " to the Playlist as audio. Ctrl+Z takes them away.",
                        juce::dontSendNotification);
    return result.wasOk();
}

bool Analyzer::addAudioSteps (Steps& steps, const juce::StringArray& names, int& added)
{
    // Each on a track of its own, the song's first downbeat on bar 1.
    const auto title = song.getFileNameWithoutExtension();
    const auto tempo = session.getProject().settings.tempoBpm;
    const auto offset =
        rhythm ? (model::Ticks)std::llround (rhythm->firstDownbeat * tempo / 60.0 * (double)model::ticksPerQuarterNote)
               : (model::Ticks)0;
    for (auto& name : names)
    {
        const auto found = stems.find (name);
        if (found == stems.end())
            return steps.fail ("this song has no " + stemDisplayName (name) + " stem; separate it again.");
        const auto& file = found->second;
        std::unique_ptr<juce::AudioFormatReader> reader (dsp::getAudioFormats().createReaderFor (file));
        if (reader == nullptr || reader->sampleRate <= 0.0)
            return steps.fail ("Spinus cannot read the " + stemDisplayName (name) + " stem (" + file.getFullPathName() +
                               "); separate the song again.");
        const auto length = (model::Ticks)std::llround ((double)reader->lengthInSamples / reader->sampleRate * tempo /
                                                        60.0 * (double)model::ticksPerQuarterNote);
        juce::String path = file.getFullPathName();
        if (const auto folder = session.getProjectFolder(); folder != juce::File())
        {
            juce::String relative;
            if (project::importAudioFile (folder, file, relative).wasOk())
                path = relative;
        }
        const auto lane = steps.add ("lane.add", object ({ { "name", stemLabel (name + " stem", title) } }))["lane"];
        if (lane.isVoid())
            return false;
        if (steps
                .add ("clip.addAudio",
                      object ({ { "lane", lane },
                                { "file", path },
                                { "start", 0 },
                                { "offset", (juce::int64)offset },
                                { "length", (juce::int64)juce::jmax<model::Ticks> (1, length - offset) },
                                { "name", stemLabel (name + " stem", title) } }))
                .isVoid())
            return false;
        ++added;
    }
    return true;
}

void Analyzer::fail (const juce::String& message)
{
    job.clear();
    progress = -1.0;
    setStep (Step::idle, message);
}

void Analyzer::setStep (Step s, const juce::String& message)
{
    step = s;
    if (message.isNotEmpty())
        status.setText (message, juce::dontSendNotification);
    updateButtons();
}

void Analyzer::updateButtons()
{
    useTempoButton.setEnabled (rhythm.has_value() && ! isBusy());
    useKeyButton.setEnabled (key.has_value() && ! isBusy());
    // Separating needs the model files the Stems box names (and those for the song itself).
    const auto separating = juce::StringArray { beatModel, "basic_pitch", separationModel (stemCount.getSelectedId()) };
    separateButton.setEnabled (prepared.existsAsFile() && ! isBusy() &&
                               std::none_of (missingModels.begin(), missingModels.end(),
                                             [&separating] (const juce::String& m)
                                             { return separating.contains (m); }));
    const auto uses = getChosenUses();
    addButton.setVisible (! rows.empty());
    addButton.setEnabled (
        ! isBusy() && std::any_of (uses.begin(), uses.end(), [] (auto& u) { return u.second != StemUse::leaveOut; }));
    // What plays a stem is chosen only when it is replicated, and how only when its source can play it in
    // more than one way.
    for (auto& [name, row] : rows)
    {
        const bool replicated = replicates (name);
        const auto* source = row.chosen();
        row.source->setVisible (replicated);
        row.way->setVisible (replicated && source != nullptr && source->ways.size() > 1);
        for (auto* box : { row.use.get(), row.source.get(), row.way.get() })
            box->setEnabled (! isBusy());
    }
    // Drum samples are suggested (from this folder too) when the drums play on the closest samples.
    const auto drums = rows.find ("drums");
    drumFolderButton.setVisible (drums != rows.end() && replicates ("drums") && drums->second.chosen() != nullptr &&
                                 drums->second.chosen()->kind == Source::Kind::drumMachine &&
                                 drums->second.chosenWay() == Way::closestPreset);
    drumFolderButton.setEnabled (! isBusy());
    chooseButton.setEnabled (! isBusy());
    continueButton.setVisible (hasEarlierAnalysis() && ! isBusy());
    anewButton.setVisible (continueButton.isVisible());
    downloadButton.setEnabled (! isBusy());
    bar.setVisible (isBusy());
    cancelButton.setVisible (isBusy() && step != Step::downloading);
    needsText.setVisible (needsText.getText().isNotEmpty());
    if (needsText.getText().isEmpty())
    {
        downloadButton.setVisible (false);
        engineButton.setVisible (false);
    }
    resized();
}

//==============================================================================
bool Analyzer::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && isSongFile (juce::File (files[0]));
}

void Analyzer::filesDropped (const juce::StringArray& files, int, int)
{
    if (! files.isEmpty())
        analyze (juce::File (files[0]));
}

void Analyzer::paint (juce::Graphics& g)
{
    const auto& theme = themeOf (*this);
    g.fillAll (theme.get ("panel"));
    for (auto* l : { &intro, &songText, &tempoText, &keyText, &stemsText, &tempoLabel, &keyLabel, &stemsLabel })
        l->setColour (juce::Label::textColourId, theme.get ("text"));
    for (auto* l : { &status, &notice })
        l->setColour (juce::Label::textColourId, theme.get ("textDim"));
    needsText.setColour (juce::Label::textColourId, theme.get ("accent"));
}

void Analyzer::resized()
{
    auto area = getLocalBounds().reduced (10);
    {
        auto r = area.removeFromBottom (48);
        sourcesButton.setBounds (r.removeFromRight (180).withSizeKeepingCentre (180, 26));
        r.removeFromRight (8);
        notice.setBounds (r);
    }
    intro.setBounds (area.removeFromTop (36));
    auto top = area.removeFromTop (28);
    chooseButton.setBounds (top.removeFromLeft (140));
    top.removeFromLeft (8);
    libraryButton.setBounds (top.removeFromRight (140));
    top.removeFromRight (8);
    songText.setBounds (top);
    area.removeFromTop (6);
    if (continueButton.isVisible())
    {
        auto r = area.removeFromTop (28);
        continueButton.setBounds (r.removeFromLeft (260).reduced (0, 1));
        r.removeFromLeft (8);
        anewButton.setBounds (r.removeFromLeft (140).reduced (0, 1));
        area.removeFromTop (6);
    }

    if (needsText.isVisible())
    {
        needsText.setBounds (area.removeFromTop (36));
        auto row = area.removeFromTop (28);
        if (downloadButton.isVisible())
            downloadButton.setBounds (row.removeFromLeft (150));
        if (engineButton.isVisible())
            engineButton.setBounds (row.removeFromLeft (220));
        area.removeFromTop (6);
    }

    auto row = [&area] (juce::Label& label, juce::Component& value, juce::Component* button, int width = 140)
    {
        auto r = area.removeFromTop (28);
        label.setBounds (r.removeFromLeft (60));
        if (button != nullptr)
            button->setBounds (r.removeFromRight (width).reduced (0, 1));
        value.setBounds (r);
        area.removeFromTop (4);
    };
    row (tempoLabel, tempoText, &useTempoButton);
    row (keyLabel, keyText, &useKeyButton);
    {
        auto r = area.removeFromTop (28);
        stemsLabel.setBounds (r.removeFromLeft (60));
        stemCount.setBounds (r.removeFromLeft (220).reduced (0, 1));
        r.removeFromLeft (8);
        separateButton.setBounds (r.removeFromLeft (100).reduced (0, 1));
        area.removeFromTop (4);
    }
    {
        auto r = area.removeFromTop (28);
        r.removeFromLeft (60);
        if (addButton.isVisible())
            addButton.setBounds (r.removeFromRight (140).reduced (0, 1));
        stemsText.setBounds (r);
    }
    // The progress bar and the status line at the bottom; the stems' rows scroll in between.
    auto bottom = area.removeFromBottom (40 + 4 + (bar.isVisible() ? 26 : 0));
    status.setBounds (bottom.removeFromBottom (40));
    if (bar.isVisible())
    {
        auto r = bottom.removeFromTop (22);
        if (cancelButton.isVisible())
            cancelButton.setBounds (r.removeFromRight (80));
        bar.setBounds (r.withTrimmedRight (6));
    }
    stemsView.setVisible (! rows.empty());
    if (! rows.empty())
    {
        // One row per stem: what happens to it and, replicated, what plays it and how.
        area.removeFromTop (4);
        stemsView.setBounds (area);
        const auto width = juce::jmax (0, area.getWidth() - stemsView.getScrollBarThickness());
        const auto indent = width >= 600 ? 60 : 0;
        const auto useWidth = juce::jlimit (150, 210, (width - indent) * 34 / 100);
        const auto sourceWidth = juce::jlimit (120, 200, (width - indent - useWidth - 12) * 54 / 100);
        int y = 0;
        for (auto& [name, stem] : rows)
        {
            juce::Rectangle<int> r (0, y, width, 26);
            y += 26;
            r.removeFromLeft (indent);
            stem.use->setBounds (r.removeFromLeft (useWidth).reduced (0, 2));
            r.removeFromLeft (6);
            stem.source->setBounds (r.removeFromLeft (sourceWidth).reduced (0, 2));
            r.removeFromLeft (6);
            const auto wayBounds = r.removeFromLeft (juce::jmin (r.getWidth(), 170)).reduced (0, 2);
            stem.way->setBounds (wayBounds);
            // Beside the drums' way, on a line of its own: where the drum samples come from.
            if (name == "drums" && drumFolderButton.isVisible())
            {
                drumFolderButton.setBounds (wayBounds.withY (y + 2).withHeight (24).withWidth (130));
                y += 28;
            }
        }
        for (auto& [name, box] : presetChoices)
        {
            y += 2;
            box->setBounds (60, y, 360, 26);
            y += 26;
        }
        stemRows.setSize (width, y);
    }
}

} // namespace spinus::ui
