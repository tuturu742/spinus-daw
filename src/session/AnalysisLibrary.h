#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace spinus
{

/** What Spinus has learnt about songs, kept for every project: the analysis library, shared by the Analyzer
    and the Track creator, in the settings folder (settings/ai/library), never inside a project.

    One folder per song, named by its contents (the first 20 hex digits of its SHA-256), so a song renamed,
    moved or used in another project is still recognised:

    - `song.json`: `version` (an entry of another version is left out and can be deleted), the song's
      last file name, and what was found: `rhythm` (ai.v1.analyze.tempo's result), `key`, `loudness`
      (ai.v1.mix.analyze's measurements of the whole song), `notes` and `drums` per separation
      ("<model>/<stem>": Basic Pitch's notes in seconds, the drum hits), `stems` (per separation model: its
      stems' names), and `features` (the Track creator's measurements, with their own version).
    - `stems/<model>/<stem>.flac`: the stems, as 24-bit FLAC (smaller than the 32-bit float WAV files the AI
      service writes, and exact enough to listen to and analyze again). A model names its number
      of stems (htdemucs: 4, htdemucs_6s: 6, mvsep_mega_53: many), so stems are reused only for the same
      model.

    Deleting an entry or its stems deletes only these files, never the user's own audio. Reading and
    writing song.json is quick (message thread); keeping and restoring stems takes seconds and runs on
    another thread (BackgroundTask). */
class AnalysisLibrary
{
public:
    /** Change when song.json's meaning changes: older entries are then ignored (and offered for deletion). */
    static constexpr int version = 1;

    explicit AnalysisLibrary (juce::File folder) : root (std::move (folder)) {}
    /** The library of an AI service's settings folder (`aiFolder`/library), or none for an empty folder. */
    static AnalysisLibrary inAiFolder (const juce::File& aiFolder);

    bool isAvailable() const { return root != juce::File(); }
    juce::File getFolder() const { return root; }
    /** A song's key in the library: the first 20 hex digits of its contents' SHA-256. */
    static juce::String keyOf (const juce::String& digest) { return digest.substring (0, 20); }

    /** A song's entry (song.json), or void when there is none of this version. */
    juce::var read (const juce::String& digest) const;
    /** Changes a song's entry (made when missing), and remembers its file name. */
    bool update (const juce::String& digest, const juce::String& songName,
                 const std::function<void (juce::DynamicObject&)>& change);
    /** Sets one of the entry's properties, or one inside an object property (e.g. "notes", "htdemucs/bass"). */
    bool set (const juce::String& digest, const juce::String& songName, const juce::Identifier& property,
              const juce::var& value);
    bool setIn (const juce::String& digest, const juce::String& songName, const juce::Identifier& property,
                const juce::String& name, const juce::var& value);

    /** The kept stems of a separation, by name: all of them, or none (some missing). */
    std::map<juce::String, juce::File> findStems (const juce::String& digest, const juce::String& model) const;
    /** The separation models whose stems are kept for a song. */
    juce::StringArray stemModels (const juce::String& digest) const;
    /** Keeps stems (any audio files Spinus reads) as FLAC. Any thread. */
    juce::Result keepStems (const juce::String& digest, const juce::String& model,
                            const std::map<juce::String, juce::File>& stems,
                            const std::function<bool()>& shouldStop) const;
    /** Decodes kept stems into WAV files in `folder` (what the AI service reads), reusing ones there. Any
        thread. */
    juce::Result restoreStems (const juce::String& digest, const juce::String& model, const juce::File& folder,
                               std::map<juce::String, juce::File>& wavs, const std::function<bool()>& shouldStop) const;

    struct Entry
    {
        juce::String key, name;
        juce::File folder;
        juce::int64 bytes = 0, stemBytes = 0;
        juce::StringArray stemModels;
        bool current = true; // false: made by another version of Spinus
        juce::Time modified;
    };
    /** Every song in the library, the most recent first, with the space it takes. */
    std::vector<Entry> list() const;
    juce::int64 totalBytes() const;

    bool removeSong (const juce::String& key) const;
    /** Deletes a song's stems (all models); what was measured stays. */
    bool removeStems (const juce::String& key) const;
    bool removeAllStems() const;
    bool removeAll() const;

private:
    juce::File songFolder (const juce::String& digest) const { return root.getChildFile (keyOf (digest)); }
    juce::File root;
};

/** Runs work on a thread of its own, then `done` with its result on the message thread, unless the task was
    deleted first (deleting it stops the work and waits). */
class BackgroundTask final : private juce::Thread
{
public:
    using Work = std::function<juce::Result (const std::function<bool()>& shouldStop)>;
    BackgroundTask (const juce::String& name, Work, std::function<void (const juce::Result&)> done);
    ~BackgroundTask() override;
    bool isRunning() const { return isThreadRunning(); }

private:
    void run() override;
    Work work;
    std::function<void (const juce::Result&)> done;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};

} // namespace spinus
