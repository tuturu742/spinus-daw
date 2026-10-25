#pragma once

#include "TimelineControls.h"
#include "Workspace.h"
#include "session/Session.h"

#include <set>

namespace spinus::ui
{

/** The Playlist (F5): the song, as patterns placed on tracks (lanes). Lanes are not tied
    to channels (FL behaviour). Song mode plays it.

    Mouse (as in the Piano Roll): click an empty spot to place the current pattern (keep the button down
    to drag it); drag a clip to move it (with every selected clip when it is selected), its right edge to
    resize it (the selection's too); right-click or right-drag over clips deletes them; Ctrl+drag
    selects, Shift+click adds to the selection; double-click makes the clip's pattern the current one (or
    opens an audio clip in Spinus Tune, an automation clip in the Automation editor). Tools: Draw, Paint
    (the current pattern again and again as far as you drag), Slice (cut clips along a line), Select.
    Delete, Ctrl+A, Escape. Click the ruler to move the song position. Zoom: the toolbar's buttons, Ctrl+= and
    Ctrl+- (Ctrl+0 fits the song), Ctrl+wheel around the mouse; Ctrl+Shift+wheel makes the tracks taller or
    shorter. Follow (on by default, remembered) turns the page while the song plays. Clips snap to the grid chosen
    next to the tools (Alt: free). Drop audio files from the file manager to add audio clips. Every
    gesture is one undo step.
*/
class Playlist final : public Panel,
                       public juce::FileDragAndDropTarget,
                       public juce::DragAndDropTarget,
                       private juce::ChangeListener,
                       private juce::Timer,
                       private juce::AsyncUpdater,
                       private juce::ScrollBar::Listener
{
public:
    static constexpr int headerWidth = 120, rulerHeight = 36, toolbarHeight = 30, minimumRows = 12;

    explicit Playlist (Session&);
    ~Playlist() override;

    //==============================================================================
    // Editing (used by the mouse handlers and by tests). Each call is one undo step (adding a clip on a
    // row that has no lane yet first adds the missing lanes, as one more step).
    bool placePattern (int row, model::Ticks start);
    bool moveClip (model::ClipId, int row, model::Ticks start);
    bool resizeClip (model::ClipId, model::Ticks length);
    bool deleteClip (model::ClipId);

    // Several clips at once (each call one undo step; moving onto rows without a track first adds
    // the tracks, as one more step).
    bool moveClips (const std::set<model::ClipId>&, model::Ticks deltaTicks, int deltaRows);
    /** Lengthens (or shortens) the clips by the same amount. */
    bool resizeClips (const std::set<model::ClipId>&, model::Ticks deltaTicks);
    bool deleteClips (const std::set<model::ClipId>&);
    bool deleteSelection() { return deleteClips (selection); }
    /** Cuts the clips on rows `firstRow` to `lastRow` that play across `at` into two there. */
    bool slice (model::Ticks at, int firstRow, int lastRow);
    /** Places the current pattern from `from` again and again (one pattern length apart) up to `to`,
        at least once. */
    bool paintPattern (int row, model::Ticks from, model::Ticks to);
    /** Where paintPattern places clips. */
    std::vector<model::Ticks> paintPositions (model::Ticks from, model::Ticks to) const;

    //==============================================================================
    // The selection: part of the view, it follows clips by their IDs.
    const std::set<model::ClipId>& getSelection() const noexcept { return selection; }
    void setSelection (std::set<model::ClipId>);
    void selectAll();
    void clearSelection() { setSelection ({}); }
    /** Selects the clips overlapping a time range on rows `firstRow` to `lastRow`. */
    void selectArea (model::Ticks from, model::Ticks to, int firstRow, int lastRow, bool add);

    enum class Tool
    {
        draw,
        paint,
        slice,
        select
    };
    void setTool (Tool);
    Tool getTool() const noexcept { return tool; }

    /** The grid, in ticks (0: none). */
    void setSnap (model::Ticks);
    model::Ticks getSnap() const noexcept { return snap; }
    /** The grid lines' spacing: the snap, or a beat without one. */
    model::Ticks getGridStep() const noexcept { return snap > 0 ? snap : model::ticksPerQuarterNote; }

    /** Called when an audio clip is double-clicked: Spinus Tune opens it. */
    std::function<void (model::ClipId)> onOpenAudioClip;
    /** Called when an automation clip is double-clicked: the Automation editor opens it. */
    std::function<void (model::ClipId)> onOpenAutomationClip;
    bool setLaneMuted (int row, bool muted);
    /** Markers on the ruler (each one undo step); jumpTo moves the song position. */
    bool addMarker (model::Ticks at, const juce::String& name = {});
    bool renameMarker (model::MarkerId, const juce::String&);
    bool moveMarker (model::MarkerId, model::Ticks at);
    bool removeMarker (model::MarkerId);
    const model::Marker* findMarker (model::MarkerId) const;
    void jumpTo (model::Ticks);
    /** A copy of the clip's pattern for it alone, made the current pattern (FL's Make unique). */
    bool makeClipUnique (model::ClipId);
    /** Changes an audio clip's playback (clip.setAudio): pitch, speed, stretch, reversed, fit. One undo step. */
    bool setAudioPlayback (model::ClipId, const juce::NamedValueSet& changes);
    /** Alt+drag of the end: audio clips play faster or slower to fill the new length (pattern clips resize). */
    bool stretchClips (const std::set<model::ClipId>&, model::Ticks deltaTicks);
    bool renameLane (int row, const juce::String&);
    bool setLaneColour (int row, juce::uint32 argb);
    bool removeLane (int row);

    /** Adds an audio clip for the file at this row and start, playing through a new channel named after
        the file (FL: dropping a sample on the playlist). A saved project gets a copy of the file in its
        audio/ folder; an unsaved one refers to the file where it is until it is saved. MP3, Opus, M4A and
        other compressed files arrive as FLAC (project/AudioImport.h). */
    bool addAudioFile (int row, model::Ticks start, const juce::File&, model::Ticks offset = 0);
    /** A recorded take (AudioRecorder) where it was played in the song, its file's first `-startSeconds` left
        out when it began before the song's start: on a new track, or `into` an audio track; there, over
        audio already on it, as another take (clip.addTake). */
    bool addRecordedTake (const juce::File&, double startSeconds, std::optional<model::ChannelId> into = {});
    /** Comping: this audio clip plays, the same track's other takes are muted where it plays (one undo step). */
    bool useTake (model::ClipId);
    /** True if other clips of the clip's audio track play at the same time (takes to choose from). */
    bool hasOtherTakes (model::ClipId) const;
    /** Adds the files as audio clips on consecutive tracks from `row`, all at `start`. Returns how many
        were added. */
    int addAudioFiles (const juce::StringArray&, int row, model::Ticks start);
    /** The first row below every track that holds a clip (where new audio tracks' clips go). */
    int getFirstFreeRow() const;

    /** Track groups (FL): the project lanes shown, top to bottom, as indices; those in a folded group are left
        out. Rows below them are new tracks waiting to be made. */
    std::vector<int> getVisibleLanes() const;
    /** The lane index a row shows (a visible lane, or below them a lane to be made). */
    int laneIndexOfRow (int row) const;
    /** The row a lane is shown on, or -1 when its group is folded. */
    int rowOfLaneIndex (int laneIndex) const;
    /** Puts the track under the one above it (or out of its group), and folds or unfolds a group. */
    bool groupLaneWithAbove (int row);
    bool ungroupLane (int row);
    bool setLaneCollapsed (int row, bool collapsed);

    /** Called with a message when a dropped file cannot be used (e.g. M4A without ffmpeg). */
    std::function<void (const juce::String&)> onProblem;

    // Automation clip points (each call one undo step). Times are ticks from the clip's start, values 0..1.
    bool addAutomationPoint (model::ClipId, model::Ticks time, float value);
    bool moveAutomationPoint (model::ClipId, int index, model::Ticks time, float value);
    bool removeAutomationPoint (model::ClipId, int index);

    /** A waveform picture of an audio file (loaded in the background; repaints when ready). */
    juce::AudioThumbnail* getThumbnail (const juce::File&);

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    // Samples dragged from the Browser land where they are dropped, as files do.
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    /** Rows shown: every lane plus empty rows below (at least minimumRows). */
    int getNumRows() const;

    model::Ticks snapTicks (model::Ticks) const;
    float tickToX (model::Ticks) const;
    model::Ticks xToTick (float) const;
    int rowAtY (float y) const;
    void handleUpdateNowIfNeeded() { juce::AsyncUpdater::handleUpdateNowIfNeeded(); }

    //==============================================================================
    // The view: zoom (`aroundX`: pixels from the lanes' left, where the time stays put; none: the middle),
    // track height, and following the song position while it plays.
    void zoom (double factor, std::optional<float> aroundX = {});
    void zoomRows (double factor);
    /** Shows the whole song (every clip) across the view. */
    void fitSong();
    double getPixelsPerBeat() const noexcept { return pixelsPerBeat; }
    int getRowHeight() const noexcept { return rowHeight; }
    double getScrollTicks() const noexcept { return scrollTicks; }
    bool isFollowing() const { return timelineControls.isFollowing(); }
    void setFollowing (bool follow) { timelineControls.setFollowing (follow); }
    /** Where the view starts and how much it shows, in ticks. */
    double getVisibleTicks() const;
    /** Turns the page when `ticks` (the song position while it plays) has left the view and Follow is on. */
    void follow (double ticks);

    juce::Rectangle<int> getDefaultBounds() const override { return { 80, 60, 820, 400 }; }
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scale) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    class Lanes;
    class Headers;
    class Ruler;

    bool perform (const juce::String& command, const juce::var& parameters);
    /** Several commands as one undo step. */
    bool performAll (const juce::String& description, const juce::Array<juce::var>& steps);
    bool setPoints (model::ClipId, const std::vector<model::AutomationPoint>&);
    model::LaneId ensureLane (int row);
    /** Brings the file into the project (audio/ folder, FLAC when needed): its path there and its length. */
    bool importAudio (const juce::File&, juce::String& path, model::Ticks& ticks);
    void updateScrollBars();

    void changeListenerCallback (juce::ChangeBroadcaster*) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override;
    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar*, double) override;

    Session& session;
    int historyListener = 0;

    model::Ticks snap = model::ticksPerQuarterNote;
    double pixelsPerBeat = 24.0;
    int rowHeight = 44;
    double scrollTicks = 0.0, scrollY = 0.0;
    double playheadTicks = -1.0;

    juce::AudioThumbnailCache thumbnailCache { 64 };
    std::map<juce::String, std::unique_ptr<juce::AudioThumbnail>> thumbnails;

    /** The song's loop region as shown: the one being dragged on the ruler, or the session's. */
    std::optional<Session::LoopRegion> getShownLoop() const
    {
        return pendingLoop ? pendingLoop : session.getLoopRegion (Session::Mode::song);
    }
    std::optional<Session::LoopRegion> pendingLoop;

    std::set<model::ClipId> selection;
    Tool tool = Tool::draw;

    std::unique_ptr<Lanes> lanes;
    std::unique_ptr<Headers> headers;
    std::unique_ptr<Ruler> ruler;
    juce::ScrollBar horizontal { false }, vertical { true };
    juce::TextButton toolButtons[4];
    juce::ComboBox snapChoice;
    TimelineControls timelineControls;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Playlist)
};

} // namespace spinus::ui
