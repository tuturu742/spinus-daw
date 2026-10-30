#pragma once

#include "plugins/HostLibraries.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace spinus
{

namespace plugins
{
class DeviceList;
}

/** An instrument the sound index measures: how to load it, and what changes when it changes. */
struct SoundSource
{
    juce::String key;        // its index name (InstrumentPresets::libraryOf): "surge-xt", "stock-spinus.3x-osc"...
    juce::String name;       // as the Browser shows it
    juce::String format;     // "Spinus" (Spinus's own), "CLAP", "VST3", "LV2", "VST"
    juce::String file;       // the plugin file (or identifier) to load here
    juce::String pluginName; // the plugin's name inside that file
    int uniqueId = 0;
    juce::String stockType;   // Spinus's own and the downloaded ones
    juce::String badge;       // "Spinus", "CLAP", "WIN"...
    juce::String fingerprint; // the plugin's version, file size and time: all its presets are measured again on change
    bool hostLibraries = false; // in the Flatpak: runs with the computer's own libraries
};

/** Sounds the user set up in a plugin's own window and saved (the Track creator's Save this sound): a plugin
    that is silent until a kit or a patch is chosen in its window can be used then, and any other sound the
    user made too. Each is the plugin's state, kept as a file beside the sound index (`<index folder>/../
    saved-sounds/`: `sounds.json` {version, sounds: [{id, instrument, name, version, saved}]} and `<id>.state`),
    so updating or deleting the index keeps them. The index measures each as one more preset of its instrument
    (its preset id is the saved sound's id, "saved:..."; its category "Your saved sounds"). Generic for every
    plugin. Message thread or the indexer's thread, one at a time. */
class SavedSounds
{
public:
    struct Sound
    {
        juce::String id;         // "saved:" and a unique part: its preset id in the index
        juce::String instrument; // the index's key of its instrument (SoundSource::key)
        juce::String name;       // as the user named it
        juce::String version;    // of its state: a sound saved again under its id is measured again
        juce::Time saved;
    };

    /** The folder beside the index folder. */
    explicit SavedSounds (const juce::File& indexFolder);
    static bool isSaved (const juce::String& presetId) { return presetId.startsWith ("saved:"); }
    /** Where a saved sound's state is, for an index folder. */
    static juce::File stateFileIn (const juce::File& indexFolder, const juce::String& id);
    static constexpr const char* category = "Your saved sounds";

    void load();
    bool save() const;
    /** Keeps a new sound (its state written, the list saved); none if it could not be written. */
    std::optional<Sound> add (const juce::String& instrument, const juce::String& name, const juce::MemoryBlock& state);
    bool remove (const juce::String& id);
    /** Deletes every saved sound. */
    bool removeAll();
    juce::File stateFile (const juce::String& id) const;
    const std::vector<Sound>& all() const noexcept { return sounds; }
    std::vector<Sound> forInstrument (const juce::String& key) const;
    juce::File getFolder() const { return folder; }

private:
    juce::File folder;
    std::vector<Sound> sounds;
};

/** The installed instruments the sound index can measure (message thread): Spinus's own that make a sound
    by themselves (3x Osc and the Drum Machine; the Sampler and the Beat Slicer play samples, which are
    indexed as samples), the downloaded ones, and the installed instrument plugins in the Browser's list
    (Windows plugins through Wine among them). */
std::vector<SoundSource> findSoundSources (const plugins::DeviceList&);

/** The sound index (Track creator, milestone 2): how the sounds this computer has sound, so the closest one
    the user owns can be chosen for each part of a reference song. Built on this computer from its own
    instruments and samples; nothing is shipped or sent anywhere.

    Kept in `<plugin list folder>/ai/sounds/` (the settings folder; the Flatpak's own, since what it can load
    differs):
    - `index.json`: `{kind: "spinus.sound-index", version, descriptorsVersion, updated, roleExamples, instruments:
      [{key, name, format, file, pluginName, uniqueId, stockType, badge, fingerprint, complete, presetFiles,
      problem, problemPresets, sounds (how many), measuredWith, updated}], sampleFolders, summary: {instruments,
   presets, samples, problems}}`;
    - `instruments/<key>.json`: `{version, key, sounds: [{id, version, name, category, role, roleFrom,
      soundRole, soundRoleFrom, ruleRole, silent, tracksPitch, polyphonic, low, high, width, registerLevels,
      registerBrightness, sound: {descriptors}, drumMap}]}`;
    - `samples.json`: `{version, files: [{path, size, modified, kind, class, classFrom, soundClass, duration,
      sound, tempo, tempoFrom, bars, key, keyFrom}]}`.
    A file of another `version`, or measured with another sounds::descriptorsVersion, is ignored (measured
    again). Message thread or the indexer's thread, one at a time. */
class SoundIndex
{
public:
    static constexpr int version = 1;
    /** How the helper measures (SoundRender): raised when that changes in a way that can make a sound silent or
        failed before measure now. 2: it waits for instruments that load their sounds late, and tries a plugin
        that crashes listing its presets with its default sound only. */
    static constexpr int helperRevision = 2;

    explicit SoundIndex (juce::File folder) : root (std::move (folder)) {}
    /** <plugin list folder>/ai/sounds. */
    static juce::File getDefaultFolder();
    juce::File getFolder() const { return root; }

    struct Instrument
    {
        SoundSource source;
        juce::Array<juce::var> sounds;    // one per preset
        juce::StringArray problemPresets; // presets that crashed or hung the helper: skipped
        juce::String problem;             // the instrument itself did not load
        bool complete = false;            // every preset measured (not stopped part way)
        bool presetFiles = false;         // its presets are files (more may come): listed again at each update
        int measuredWith = 0;             // the helperRevision that measured it (0: older than revision 2)
        juce::Time updated;

        /** It did not load, a preset crashed or hung the helper, or a sound was silent. */
        bool silentOrFailed() const;
    };
    struct Sample
    {
        juce::String path;
        juce::int64 size = 0, modified = 0;
        juce::var info; // what describeSample found (or {problem})
    };

    /** Reads the index (a missing or older one is empty). */
    void load();
    bool saveIndex() const;
    bool saveInstrument (const Instrument&) const;
    bool saveSamples() const;
    /** Deletes the whole index from disk and memory. */
    bool removeAll();

    /** Guesses again the role of every preset from its sound, learnt from the presets whose name or category
        says their role (sounds::RoleGuess), when there are enough of them; otherwise the rules' guess
        (sounds::roleFromSound) stands. A preset its name decides keeps that role, and gets the sound's guess
        made from the other presets beside it (`soundRole`; `soundRoleFrom` "learnt" or "rules"; `ruleRole`).
        Saves the instruments that changed. Returns how many sounds changed. */
    int learnRoles();
    /** How many examples the last learnRoles() learnt from (0: the rules decided). */
    int roleExamples = 0;

    std::map<juce::String, Instrument> instruments;
    std::vector<Sample> samples; // by path
    juce::StringArray sampleFolders;
    juce::Time updated;

    struct Summary
    {
        int instruments = 0, presets = 0, samples = 0, problems = 0;
        juce::Time updated;
    };
    Summary summarize() const;
    /** The summary kept in index.json (quick: the instruments' and samples' files are not read). */
    static Summary readSummary (const juce::File& folder);
    /** The space the index takes on disk. */
    juce::int64 getDiskUse() const;

    /** Whether an update should start the helper for this instrument: new, changed (its fingerprint), not
        finished, with preset files (which may have been added or changed), with saved sounds (SavedSounds) not
        measured yet or gone, or silent or failed when measured by an older helperRevision (or marked with
        measureAgain). A healthy one measured by an older helper is kept as it is; one that did not load is
        otherwise not tried again until it changes. */
    bool needsVisit (const SoundSource&) const;

    /** Forgets what was measured of this instrument (its sounds and problems), so the next update measures it
        all again. Saves the index. False when it is not in the index. */
    bool measureAgain (const juce::String& key);
    /** measureAgain for every instrument that is silentOrFailed(). Returns how many. */
    int measureAgainSilentOrFailed();

    struct SampleFile
    {
        juce::File file;
        juce::int64 size = 0, modified = 0;
    };
    /** The audio files in these folders and the folders inside them (WAV, AIFF, FLAC, Ogg, MP3; at most
        200 MB each), sorted by path. */
    static std::vector<SampleFile> listSampleFiles (const juce::Array<juce::File>& folders,
                                                    const std::function<bool()>& shouldStop = {});
    /** Which of these files are new or changed (path, size and modification time) since they were measured. */
    std::vector<SampleFile> changedSamples (const std::vector<SampleFile>&) const;

    /** One flat list of every sound (for the panel's list and search): instrument presets and samples. */
    struct Row
    {
        juce::String name, source, role, detail; // role: the role or the sample's class
        bool sample = false;
    };
    std::vector<Row> rows() const;

private:
    juce::File instrumentFile (const juce::String& key) const;
    juce::File root;
};

/** Brings the sound index up to date in the background: each instrument that needs it is loaded in the
    helper process (spinus-plugin-scanner --sound-index, SoundRender), which plays the test notes with every
    new or changed preset; then the new or changed sample files are measured there too, a batch per run.
    Removed instruments, presets and files leave the index.

    A helper that crashes or stops answering (nothing new for `soundTimeoutMs`, or `loadTimeoutMs` while the
    plugin loads) is killed; the preset or file it was on is noted as a problem and skipped, and the helper
    starts again with the rest (at most `retries` times per instrument). An instrument that crashes while
    loading is noted as a problem. Nothing of this reaches Spinus's own process, and the index failing never
    stops anything else: the DAW does not need it.

    Progress and the end are reported on the message thread. Cancel stops at once (the helper is killed);
    what was measured is kept, and the next update carries on from there. */
class SoundIndexer final : private juce::Thread, private juce::AsyncUpdater
{
public:
    struct Options
    {
        juce::File helper; // spinus-plugin-scanner
        std::optional<plugins::HostLibraries> hostLibraries;
        int loadTimeoutMs = 60000, soundTimeoutMs = 30000;
        int samplesPerRun = 200;
        int retries = 8;
    };

    struct Progress
    {
        juce::String stage; // "instruments", "samples"
        int done = 0, total = 0;
        juce::String current; // the instrument (and preset) or file
        int sounds = 0;       // measured so far in this update
    };

    struct Result
    {
        int instrumentsVisited = 0, soundsMeasured = 0, soundsRemoved = 0, samplesMeasured = 0, samplesRemoved = 0;
        int problems = 0; // new problem presets, instruments and files
        bool cancelled = false;
    };

    SoundIndexer (juce::File folder, Options);
    ~SoundIndexer() override;

    /** Starts an update (unless one runs). `onProgress` and `onFinished` are called on the message thread. */
    bool start (std::vector<SoundSource>, juce::Array<juce::File> sampleFolders,
                std::function<void (const Progress&)> onProgress, std::function<void (const Result&)> onFinished);
    void cancel();
    bool isRunning() const { return isThreadRunning(); }
    Progress getProgress() const;

    /** Whether any indexer is updating an index now (the Saved analyses window does not delete it meanwhile). */
    static bool isAnyRunning() noexcept { return running.load() > 0; }

    /** The same update on the calling thread (tests, and the thread itself). */
    static Result update (SoundIndex&, const Options&, const std::vector<SoundSource>&,
                          const juce::Array<juce::File>& sampleFolders, const std::function<bool()>& shouldStop,
                          const std::function<void (const Progress&)>& onProgress);
    /** One instrument brought up to date on the calling thread (a sound just saved), the rest of the index left
        as it is. Saves the index. */
    static Result updateInstrument (SoundIndex&, const Options&, const SoundSource&,
                                    const std::function<bool()>& shouldStop);

    /** One run of the helper: `onLine` gets each line it writes. `stuckOn` is the last "begin" without its
        result when it crashed or hung. */
    struct Run
    {
        bool finished = false; // it wrote "done"
        bool started = false;  // it wrote anything at all ("presets", or a first "begin")
        bool hung = false;
        juce::String stuckOn;
        int exitCode = 0;
    };
    static Run runHelper (const Options&, const juce::var& request, bool withHostLibraries,
                          const std::function<void (const juce::var&)>& onLine,
                          const std::function<bool()>& shouldStop);

private:
    void run() override;
    void handleAsyncUpdate() override;

    static std::atomic<int> running;

    juce::File folder;
    Options options;
    std::vector<SoundSource> sources;
    juce::Array<juce::File> folders;
    std::function<void (const Progress&)> progressCallback;
    std::function<void (const Result&)> finishedCallback;
    mutable std::mutex lock;
    Progress progress;
    std::optional<Result> result;
};

} // namespace spinus
