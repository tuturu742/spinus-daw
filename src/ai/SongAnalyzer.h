#pragma once

#include "AiService.h"
#include "session/AnalysisLibrary.h"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace spinus::ai
{

/** Analyzes one song at a time, without a window: what the Track creator measures of a reference song, kept in
    the analysis library. The Track creator, `spinus --analyze` and agents (analyze_song) all use it.

    The steps: the song is read and decoded (a thread of its own); then, through the AI service, its beat, key
    and loudness, its stems (kept as FLAC), the count checked on the drums, the notes of each stem and the drum
    hits, and its style (the `features` agents and the Track creator read). What the library has from before is
    not found again, and a song whose style was measured before (by this version) is done at once, with AI off
    too.

    Message thread only. Nothing is called back after stop() or once it is deleted. */
class SongAnalyzer
{
public:
    /** The version of the measured style kept in the library (its `features`); change it with
        spinus_ai.style.FEATURES_VERSION (a test checks they agree), so songs measured by an older Spinus are
        measured again. */
    static constexpr int featuresVersion = 6;

    /** What is missing before a song can be analyzed. */
    struct Needs
    {
        enum class What
        {
            ai,      // AI is off
            service, // the AI service did not answer (`text` says why)
            engine,  // the analysis engine (PyTorch) is not installed
            models   // model files are not downloaded (`models`)
        };
        struct Model
        {
            juce::String id, name;
            juce::int64 size = 0;
        };
        What what = What::ai;
        juce::String text;         // in a few words ("AI is off.")
        std::vector<Model> models; // What::models: those missing
        juce::int64 downloadSize = 0;
    };

    struct Callbacks
    {
        /** What is being done ("Finding the beat..."), and how far the step is (0 to 1; -1: unknown). */
        std::function<void (const juce::String& words, double progress)> step;
        /** The song's contents were read: its digest (it names the song in the library). */
        std::function<void (const juce::String& digest)> read;
        /** Done: its style. `keptBefore`: it was in the library already. */
        std::function<void (const juce::var& features, bool keptBefore)> done;
        std::function<void (const juce::String& why)> failed;
        /** Something is missing: the song was not analyzed (the decoded song is kept for the next try). */
        std::function<void (const Needs&)> held;
        /** An AI step was run (tests: what the library saved). */
        std::function<void (const juce::String& method)> ran;
        /** Whether the song plays now: the separation then leaves the processor some room. */
        std::function<bool()> playing;
    };

    /** `service` may be null (no AI): only songs measured before are then done. `workRoot`: where a song's large
        files are made while it is analyzed (deleted after). */
    SongAnalyzer (AiService* service, AnalysisLibrary library, juce::File workRoot);
    ~SongAnalyzer();

    void setService (AiService* s) { service = s; }
    void setLibrary (AnalysisLibrary l) { library = std::move (l); }

    /** Starts on a song (stopping the one before). `knownDigest`: its contents' digest when known and the file
        has not changed since (it is then not read again). */
    void start (const juce::File& song, const juce::String& knownDigest, Callbacks);
    /** Stops the song being analyzed, if any; its large files are deleted. */
    void stop();
    bool isBusy() const noexcept { return busy; }

    /** Where a song's large files are made while it is analyzed. */
    juce::File getWorkFolder (const juce::String& digest) const;
    /** The model files the analysis needs, by id (the beat model depends on the processor: after a check). */
    juce::StringArray neededModels() const;

private:
    class Preparer;
    struct Work;

    void prepared();
    void checkReady (const std::function<void()>& then);
    void runStep (const juce::String& method, const juce::var& params, const juce::String& words,
                  const std::function<void (const juce::var&)>& done);
    void runTask (const juce::String& words, BackgroundTask::Work, const std::function<void()>& then);
    void findTempo();
    void findKey();
    void measureLoudness();
    void separate();
    void keepStems();
    /** A song counted twice too fast (its snare on one beat of each bar at 140 BPM or more) is counted at half the
        tempo (ai.v1.analyze.count, as in the Analyzer), then transcribeNext(). */
    void checkCount();
    void transcribeNext();
    void measureStyle();
    void say (const juce::String& words, double progress = -1.0);
    void finish (const juce::var& features, bool keptBefore);
    void fail (const juce::String& why);
    void hold (const Needs&);
    void clear (bool deleteWork);

    AiService* service = nullptr;
    AnalysisLibrary library;
    juce::File workRoot;
    Callbacks callbacks;
    bool busy = false;
    juce::File song;
    std::shared_ptr<bool> run = std::make_shared<bool> (true); // the song being analyzed (false: stopped)
    std::unique_ptr<Preparer> preparer;
    std::unique_ptr<BackgroundTask> task; // keeping or restoring the song's stems
    std::unique_ptr<Work> work;
    juce::String job; // the AI service's job for the current step
    juce::String beatModel;
};

/** Downloads model files one after the other (ai.v1.stems.download). `progress`: the model's name and how far
    (0 to 1); `done`: an error, empty when all are there. Nothing is called once `alive` is false. */
void downloadModels (AiService&, std::vector<SongAnalyzer::Needs::Model> models, const std::shared_ptr<bool>& alive,
                     const std::function<void (const juce::String& name, double fraction)>& progress,
                     const std::function<void (const juce::String& error)>& done);

/** Songs analyzed one after the other, without a window: `spinus --analyze` and agents (analyze_song). A song
    that needs something missing (AI off, no engine, no model files) fails with those words, and so do the songs
    after it. Message thread only. */
class SongAnalysisQueue
{
public:
    struct Song
    {
        enum class State
        {
            waiting,
            analyzing,
            done,
            failed
        };
        juce::File file;
        State state = State::waiting;
        juce::String step;      // what is being done, or the problem
        double progress = -1.0; // of the step
        bool keptBefore = false;
        juce::String digest;
    };

    SongAnalysisQueue (AiService* service, AnalysisLibrary library, juce::File workRoot);

    /** Adds a song (once: one waiting or being analyzed is not added again; a finished one is analyzed anew,
        which is quick when nothing changed). Analyzing starts at once. */
    void add (const juce::File& song);
    /** Stops, and forgets the songs still waiting. */
    void stop();
    bool isBusy() const noexcept { return analyzer.isBusy() || current >= 0; }
    const std::vector<Song>& getSongs() const noexcept { return songs; }
    /** What was missing when a song could not start (void when nothing was). */
    const std::optional<SongAnalyzer::Needs>& getNeeds() const noexcept { return needs; }

    /** The songs for an agent: [{name, state, step?, percent?, problem?, song (its library key, when read)}],
        named by their file names only. */
    juce::var describe() const;

    /** Called when a song's state or step changes. */
    std::function<void (const Song&)> onChange;
    std::function<bool()> playing;

private:
    void startNext();

    SongAnalyzer analyzer;
    std::vector<Song> songs;
    int current = -1;
    std::optional<SongAnalyzer::Needs> needs;
};

/** The songs among files and folders (a folder: its songs and those one folder down, as an album with CD 1 and
    CD 2), sorted by name, each once. */
juce::Array<juce::File> songsIn (const juce::Array<juce::File>& filesAndFolders);

/** `spinus --analyze`: analyzes the songs and waits for them (the message loop runs meanwhile). `say` gets JSON
    lines: {"song", "step"} as each step starts, {"song", "ok", "key" | "error"} as each song ends, and a last
    {"ok", "analyzed", "failed"}. With `download`, model files that are missing are downloaded first (a
    {"downloading", "percent"} line now and then). Returns the exit status: 0 when every song was analyzed. */
int analyzeSongs (AiService&, const juce::Array<juce::File>& songs, bool download,
                  const std::function<void (const juce::String& line)>& say);

} // namespace spinus::ai
