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
    - `index.json`: `{kind: "spinus.sound-index", version, descriptorsVersion, updated, instruments:
      [{key, name, format, file, pluginName, uniqueId, stockType, badge, fingerprint, complete, presetFiles,
      problem, problemPresets, sounds (how many), updated}], sampleFolders, summary: {instruments, presets,
      samples, problems}}`;
    - `instruments/<key>.json`: `{version, key, sounds: [{id, version, name, category, role, roleFrom,
      soundRole, silent, tracksPitch, polyphonic, low, high, sound: {descriptors}, drumMap}]}`;
    - `samples.json`: `{version, files: [{path, size, modified, kind, class, classFrom, soundClass, duration,
      sound, tempo, tempoFrom, bars, key, keyFrom}]}`.
    A file of another `version`, or measured with another sounds::descriptorsVersion, is ignored (measured
    again). Message thread or the indexer's thread, one at a time. */
class SoundIndex
{
public:
    static constexpr int version = 1;

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
        juce::Time updated;
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

    /** Whether an update should start the helper for this instrument: new, changed (its fingerprint), measured
        by another version, not finished, or with preset files (which may have been added or changed). One
        that did not load is not tried again until it changes. */
    bool needsVisit (const SoundSource&) const;

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
