#pragma once

#include "core/FileDialog.h"
#include "Workspace.h"
#include "ai/AiService.h"
#include "session/AnalysisLibrary.h"
#include "session/Session.h"
#include "session/SoundIndex.h"
#include "MessageLabel.h"
#include "DescribeSongPanel.h"
#include "PartSoundsPanel.h"
#include "SongWriterPanel.h"

#include <memory>
#include <vector>

namespace spinus::ui
{

/** The Track creator (experimental), its first step: reference songs in, a summary of their style out.

    The user adds songs they like (files, or a folder such as an album: its songs and those one folder
    down). Each is analyzed in turn with the Analyzer's steps: prepared (decoded to WAV), its beat
    (Beat This!) and key, its loudness (as the automixer measures it), split into 4 stems (Demucs), the
    drums' hits and the other stems' notes (Basic Pitch); then the AI service measures its style
    (ai.v1.style.summarize: chords per bar, drum steps, parts, sections).

    Everything found goes into the analysis library (AnalysisLibrary, in the settings folder), shared with
    the Analyzer: a song added again, or analyzed in the Analyzer before, reuses what is there (its tempo,
    key, stems, notes), and one whose style was measured is ready at once. The decoded song and the stems'
    WAV files are made in a temporary folder and deleted after each song. Once all are analyzed, the
    summary of what the songs share is shown in plain words and can be saved as a JSON file (the input of
    the Track creator's later steps). The list of songs, the last summary, the description and the parts
    list (with their sounds) are remembered per project (track-creator.json in the project's folder; an untitled
    project's only while it is open, until Save As). A new project starts empty and an opened one brings back its
    own; the songs whose style is known are ready, and the others wait for Analyze again, so opening a project never
    starts the AI service. The current project's state is copied to track-creator.json beside the library, where
    the AI service's reference_style reads it.

    Nothing here changes the project. It needs AI turned on, the analysis engine installed and the model
    files downloaded (unless the library already has what they would find); it says what is missing and
    offers to fix it, like the Analyzer.

    The sound index (Session::getSoundIndex(), shown in full in Options › Sound index) is updated from here too:
    it measures how the instruments and samples on this computer sound, in the background and in a separate
    program, so later steps can choose the closest sound the user has for each part. It needs neither AI nor
    the AI service.

    Message thread only (each song is read and decoded on a thread of its own). */
class TrackCreator final : public Panel,
                           public juce::FileDragAndDropTarget,
                           private juce::AsyncUpdater,
                           private juce::ChangeListener
{
public:
    TrackCreator (Session&, ai::AiService*);
    ~TrackCreator() override;

    /** The AI service; the first time, the remembered songs come back. */
    void setService (ai::AiService*);

    /** Opens Options > AI on the analysis engine page. */
    std::function<void()> onSetUpEngine;

    /** The version of the measured style kept in the library (its `features`); change it with
        spinus_ai.style.FEATURES_VERSION (a test checks they agree), so songs measured by an older Spinus are
        measured again. */
    static constexpr int analysisVersion = 5;

    enum class Status
    {
        waiting,
        preparing, // read and decoded
        analyzing, // the AI service's steps
        done,
        failed
    };

    struct Reference
    {
        juce::File file;
        Status status = Status::waiting;
        juce::String step;                  // what is being done, or the problem
        double progress = -1.0;             // of the step, 0 to 1 (-1: unknown)
        bool keptBefore = false;            // its style was in the library already
        juce::String digest;                // its contents' SHA-256
        juce::int64 size = 0, modified = 0; // the file when its digest was read (remembered with it)
        juce::var features;                 // what spinus_ai.style measured (perSong)
    };

    //==============================================================================
    // For the buttons and for tests. Results arrive asynchronously.

    /** Adds songs: audio files, and folders (their audio files and those of their folders, sorted by name).
        Songs already in the list are left out. Analyzing starts at once. Returns how many were added. */
    int addReferences (const juce::Array<juce::File>&);
    /** Removes one (stopping it if it is being analyzed). */
    bool removeReference (int index);
    void clearReferences();
    /** Stops the song being analyzed (it can be tried again with Analyze). */
    void cancel();
    /** Analyzes the songs that are waiting or failed (after a problem was solved). */
    void analyzeAgain();
    /** Downloads the model files the analysis needs (the user agreed to their size). */
    void downloadModels();
    /** Writes the summary as JSON. False if there is none or it cannot be written. */
    bool saveSummary (const juce::File&) const;
    /** Opens the window of the analysis library (its songs, the space they take, deleting). */
    void showLibrary();

    const std::vector<Reference>& getReferences() const noexcept { return references; }
    /** The text of a song's row: its name, then how it is going ("Done", "Finding the beat... 40 %"). */
    juce::String describe (const Reference&) const;
    bool isBusy() const noexcept { return current >= 0 || downloading || summarizing; }
    /** What the songs share (ai.v1.style.summarize's result), void until there is one. */
    const juce::var& getSummary() const noexcept { return summary; }
    juce::String getSummaryText() const { return summaryText.getText(); }
    juce::String getStatusText() const { return status.getText(); }
    juce::String getNeedsText() const { return needsText.getText(); }
    juce::int64 getDownloadSize() const noexcept { return downloadSize; }
    /** The AI steps run for the songs so far (tests: what the library saved). */
    const juce::StringArray& getMethodsRun() const noexcept { return methodsRun; }

    /** The analysis library (none without the AI service). */
    AnalysisLibrary getLibrary() const;
    /** The copy of the current project's songs, summary and description that the AI service reads. */
    juce::File getListFile() const;
    /** Where the project's songs, summary, description and parts are remembered (none while untitled). */
    juce::File getProjectFile() const;
    /** Where a song's large files are made while it is analyzed (a temporary folder, deleted after). */
    juce::File getWorkFolder (const juce::String& digest) const;

    //==============================================================================
    // The sound index.

    /** Where the index is kept and how the helper runs: the session's sound index (tests set them there). */
    juce::File getSoundIndexFolder() const { return session.getSoundIndex().getFolder(); }

    /** Brings the index up to date in the background (what is new or changed); progress shows in the panel. */
    void updateSoundIndex() { session.getSoundIndex().update(); }
    void cancelSoundIndex() { session.getSoundIndex().cancel(); }
    bool isIndexingSounds() const noexcept { return session.getSoundIndex().isUpdating(); }
    juce::String getSoundIndexText() const { return soundsStatus.getText(); }
    /** Opens Options › Sound index: the list of sounds and instruments, with search, problems and settings. */
    void showSounds();

    //==============================================================================
    // Sounds for each part (milestone 3).

    /** Shows the songs and their style (false) or the sounds for each part (true). */
    void showPartSounds (bool sounds) { showPage (sounds ? Page::sounds : Page::style); }
    bool isShowingPartSounds() const noexcept { return page == Page::sounds; }
    PartSoundsPanel& getPartSounds() noexcept { return *partSounds; }

    //==============================================================================
    // The new song (milestone 4).

    enum class Page
    {
        style,    // the songs and their style
        describe, // a song described in words, instead of songs
        sounds,   // sounds for each part
        song      // write a song
    };
    void showPage (Page);
    Page getPage() const noexcept { return page; }
    SongWriterPanel& getSongWriter() noexcept { return *songWriter; }

    //==============================================================================
    // A song described in words (the second way in).
    DescribeSongPanel& getDescriber() noexcept { return *describer; }
    /** Whether the Sounds and Write pages work from the description (read last) or from the songs. */
    bool isFromDescription() const noexcept { return described.isObject(); }
    /** The summary those pages work from: the description's or the songs'. */
    const juce::var& getActiveSummary() const noexcept { return described.isObject() ? described : summary; }
    /** Opens the automixer with this reference song (the main window sets it). */
    std::function<void (const juce::File& reference)> onOpenAutomixer;

    juce::Rectangle<int> getDefaultBounds() const override { return { 100, 70, 720, 720 }; }

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void visibilityChanged() override;

private:
    class Preparer;
    class Rows;
    struct Work;

    void handleAsyncUpdate() override; // the preparer finished
    void startNext();
    void checkReady (const std::function<void()>& then);
    void runStep (const juce::String& method, const juce::var& params, const juce::String& words,
                  const std::function<void (const juce::var&)>& done);
    void runTask (const juce::String& words, BackgroundTask::Work, const std::function<void()>& then);
    void findTempo();
    void findKey();
    void measureLoudness();
    void separate();
    void keepStems();
    void transcribeNext();
    void measureStyle();
    void finishSong (const juce::var& features, bool keptBefore);
    void failSong (const juce::String& why);
    /** Puts the current song back to waiting and stops: what is missing (AI, engine, models) is said. */
    void holdQueue (const juce::String& why);
    void stopCurrent();
    void summarize();
    void showSummary();
    void updateButtons();
    juce::StringArray neededModels() const;
    Reference* currentReference();
    void remember() const;
    void forget();
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void recall();
    void showSoundSummary();

    Session& session;
    ai::AiService* service = nullptr;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    std::shared_ptr<bool> run = std::make_shared<bool> (true); // the song being analyzed (false: stopped)

    std::vector<Reference> references;
    int current = -1; // the song being analyzed
    std::unique_ptr<Preparer> preparer;
    std::unique_ptr<BackgroundTask> task; // keeping or restoring the current song's stems
    std::unique_ptr<Work> work;
    juce::String job; // the AI service's job for the current step
    juce::String beatModel;
    struct Missing
    {
        juce::String id, name;
        juce::int64 size = 0;
    };
    std::vector<Missing> missing;
    juce::int64 downloadSize = 0;
    bool downloading = false, summarizing = false, recalled = false, recalling = false;
    juce::String summarized; // the songs (digests) the summary is of
    juce::var summary;
    juce::File workRoot;
    juce::StringArray methodsRun;

    juce::Label intro;
    MessageLabel status, notice, needsText;
    juce::TextButton addButton { "Add songs..." }, folderButton { "Add a folder..." }, removeButton { "Remove" },
        clearButton { "Clear" }, cancelButton { "Cancel" }, againButton { "Analyze again" },
        downloadButton { "Download" }, engineButton { "Set up the analysis engine..." },
        saveButton { "Save summary..." }, libraryButton { "Saved analyses..." };
    std::unique_ptr<Rows> rows;
    juce::ListBox list;
    juce::Label summaryTitle;
    juce::TextEditor summaryText;
    std::unique_ptr<core::FileDialog> chooser;

    bool soundSummaryShown = false;
    juce::Label soundsTitle;
    MessageLabel soundsStatus;
    juce::TextButton updateSoundsButton { "Update sound index" }, stopSoundsButton { "Stop" },
        showSoundsButton { "Sound index..." };
    juce::TextButton styleTab { "1. References" }, describeTab { "2. Description" }, soundsTab { "3. Parts" },
        songTab { "4. Write a song" };
    std::unique_ptr<DescribeSongPanel> describer;
    juce::var described;       // the last description's summary
    int projectLoads = 0;      // the session's, when this project came in
    juce::File projectFolder;  // where its state is kept (none: untitled)
    void describeReferences(); // the songs' summary into the description, read
    Page page = Page::style;
    std::unique_ptr<PartSoundsPanel> partSounds;
    std::unique_ptr<SongWriterPanel> songWriter;
};

} // namespace spinus::ui
