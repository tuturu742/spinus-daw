#pragma once

#include "Workspace.h"
#include "session/Session.h"

namespace spinus::ui
{

/** The clip launcher (View › Launcher), Ableton-style: scenes are rows,
    launcher tracks are columns, and each slot holds a pattern or an audio clip.

    Mouse: click a clip to launch it (on the next bar, beat or at once, as chosen at the top); click an
    empty slot to put the current pattern there; drag a clip to another slot to move it; drop audio files
    on a slot; right-click a clip for looping, length and removing it. The triangle beside a scene
    launches the whole row; the square under a column stops it. Rows and columns beyond the last ones are
    empty, waiting to be used: putting a clip there adds them (as one more undo step, like the Playlist's
    empty tracks).

    "Record to Playlist" records what is launched (in song mode) and, when playback stops, writes it as
    Playlist clips, one undo step.

    Keyboard (when the launcher has focus): the arrow keys choose a slot, Enter launches it (or puts the
    current pattern in an empty one), Ctrl+Enter launches its scene, Shift+Enter stops its column, Delete
    empties it.

    A channel playing a launched clip stops playing the Playlist until "Back to the Playlist". Every edit
    is a Command; launching is not part of the project. */
class Launcher final : public Panel,
                       public juce::FileDragAndDropTarget,
                       private juce::ChangeListener,
                       private juce::Timer,
                       private juce::AsyncUpdater
{
public:
    static constexpr int sceneWidth = 140, headerHeight = 26, footerHeight = 28, cellWidth = 124, cellHeight = 36,
                         minimumColumns = 4, minimumRows = 4;

    explicit Launcher (Session&);
    ~Launcher() override;

    //==============================================================================
    // Editing (the mouse, the keyboard and tests). Each call is one undo step.
    bool placePattern (int column, int row, model::PatternId);
    /** The file plays through a new channel named after it; a saved project gets a copy in its audio/
        folder, as on the Playlist. One pass is the file's length, to the nearest beat. */
    bool addAudioFile (int column, int row, const juce::File&);
    bool moveClip (model::ClipId, int column, int row);
    bool removeClip (model::ClipId);
    bool setLoop (model::ClipId, bool);
    /** One pass in bars; 0 for the pattern's own length (audio: the file's). */
    bool setLengthInBars (model::ClipId, int bars);
    bool setQuantize (model::LaunchQuantize);
    bool renameTrack (int column, const juce::String&);
    bool renameScene (int row, const juce::String&);
    bool removeTrack (int column);
    bool removeScene (int row);

    // Playing (not undoable: transport state).
    bool launch (int column, int row);
    void launchScene (int row);
    void stopColumn (int column);

    //==============================================================================
    /** The clip in a slot, or nullptr (also for rows and columns not made yet). */
    const model::LauncherClip* getClip (int column, int row) const;
    int getNumColumns() const; // every launcher track, plus empty ones (at least minimumColumns)
    int getNumRows() const;    // every scene, plus empty ones (at least minimumRows)
    juce::String getStatusText() const { return status.getText(); }

    /** The slot the keyboard acts on. */
    void selectSlot (int column, int row);
    std::pair<int, int> getSelectedSlot() const noexcept { return selected; }

    /** Where a slot is, in the grid's coordinates (tests). */
    juce::Rectangle<int> getSlotBounds (int column, int row) const;
    juce::Component& getGrid() noexcept;
    void handleUpdateNowIfNeeded() { juce::AsyncUpdater::handleUpdateNowIfNeeded(); }

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;
    bool keyPressed (const juce::KeyPress&) override;

    juce::Rectangle<int> getDefaultBounds() const override { return { 100, 70, 760, 330 }; }
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Grid;

    bool perform (const juce::String& command, const juce::var& parameters);
    model::LauncherTrackId ensureTrack (int column);
    model::SceneId ensureScene (int row);
    void updateControls();
    void say (const juce::String&);
    juce::String nameOf (const model::LauncherClip&) const;

    void changeListenerCallback (juce::ChangeBroadcaster*) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override;
    void timerCallback() override;

    Session& session;
    int historyListener = 0;
    std::pair<int, int> selected { 0, 0 };

    std::unique_ptr<Grid> grid;
    juce::Viewport viewport;
    juce::Label launchLabel, status;
    juce::ComboBox quantize;
    juce::TextButton stopAllButton { "Stop all" }, backButton { "Back to the Playlist" },
        recordButton { "Record to Playlist" };
    int shownRecorded = 0; // the session's last launcher recording, as last reported

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Launcher)
};

} // namespace spinus::ui
