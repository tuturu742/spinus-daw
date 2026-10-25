#pragma once

#include "TimelineControls.h"
#include "Workspace.h"
#include "session/Session.h"

#include <set>

namespace spinus::ui
{

/** The Piano Roll (F7): edits the selected channel's notes in the current pattern.

    Mouse (FL's pencil): click empty space to add a note and drag it into place; drag a note to move it
    (with every selected note when it is selected); drag its right edge to change its length (the
    selection's too); right-click or right-drag over notes to delete them; Ctrl+drag selects, Shift+click
    adds to the selection. Tools: Draw, Paint, Slice, Select. Delete removes the selection, Ctrl+A
    selects everything, Escape clears the selection; Ctrl+C, Ctrl+X, Ctrl+V and Ctrl+B copy, cut, paste
    and duplicate. Notes snap to the grid
    (the pattern's step by default, or the snap chosen beside the tools; hold Alt to place freely). The lane at the
   bottom edits one value of each note, chosen in its corner as in FL: velocity, release velocity, pan, fine pitch or
   volume; right-click a bar to reset it. The wheel scrolls (Shift: sideways), Ctrl+wheel zooms around the mouse
   (Ctrl+Shift+wheel: the keys' height); the toolbar's zoom buttons, Ctrl+= and Ctrl+- zoom too, Ctrl+0 fits the
   pattern. Follow (on by default, remembered) turns the page while the pattern plays.

    Edits work on a copy while the mouse is down and are committed as one Command when it is released,
    so every gesture is one undo step.

    Ghost notes (FL's): the other channels' notes in the same pattern are drawn faintly in their
    channel's colour behind the edited notes, to write harmonies and bass lines against them. They are
    not editable and clicks pass through them. The "Ghosts" corner chooses whose are shown.

    Suggestions (Session::getSuggestions, e.g. from the pattern generator): the suggested notes of the
    edited channel are drawn over its notes, which are dimmed, with Accept, Discard, Regenerate and the
    variations in a bar at the top of the grid.
*/
class PianoRoll final : public Panel,
                        private juce::ChangeListener,
                        private juce::Timer,
                        private juce::AsyncUpdater,
                        private juce::ScrollBar::Listener
{
public:
    static constexpr int keyboardWidth = 56, rulerHeight = 22, velocityHeight = 64;

    /** What the lane at the bottom edits. */
    enum class LaneMode
    {
        velocity,
        release,
        pan,
        finePitch,
        volume
    };
    static constexpr int numLaneModes = 5;

    explicit PianoRoll (Session&);
    ~PianoRoll() override;

    //==============================================================================
    // Editing (used by the mouse handlers and by tests). Each call is one undo step.
    bool addNote (int pitch, model::Ticks start, model::Ticks length, float velocity);
    bool moveNote (int index, model::Ticks deltaTicks, int deltaPitch);
    bool resizeNote (int index, model::Ticks newLength);
    bool deleteNote (int index);
    bool setVelocity (int index, float velocity);
    /** Sets one lane value of a note (clamped to its range). */
    bool setNoteValue (int index, LaneMode, float value);

    //==============================================================================
    // The lane.
    void setLaneMode (LaneMode);
    LaneMode getLaneMode() const noexcept { return laneMode; }
    static juce::String getLaneName (LaneMode);
    /** The value's range and default: velocity, release and volume 0..1; pan -1..1; fine pitch -100..100 cents. */
    static juce::Range<float> getLaneRange (LaneMode);
    static float getLaneDefault (LaneMode);
    static float getNoteValue (const model::Note&, LaneMode);
    static void setNoteValue (model::Note&, LaneMode, float value);
    /** Why the selected channel's instrument may not respond to the lane (empty when it does). */
    juce::String getLaneNote() const;

    /** The notes being shown (the selected channel's notes in the current pattern, sorted by start). */
    const std::vector<model::Note>& getNotes() const noexcept { return notes; }

    //==============================================================================
    // Selection and editing several notes at once. The selection is part of the
    // view and follows the notes through edits and undo.
    const std::set<int>& getSelection() const noexcept { return selection; }
    bool isSelected (int index) const { return selection.contains (index); }
    void setSelection (std::set<int>);
    void selectAll();
    void clearSelection() { setSelection ({}); }
    /** Selects the notes overlapping a time range and a pitch range (both inclusive); `add` keeps the
        notes already selected. */
    void selectArea (model::Ticks from, model::Ticks to, int lowPitch, int highPitch, bool add);

    /** Moves every selected note (one undo step). */
    bool moveSelection (model::Ticks deltaTicks, int deltaPitch);
    /** Lengthens (or shortens) every selected note by the same amount, each at least `minimum` long. */
    bool resizeSelection (model::Ticks deltaTicks, model::Ticks minimum = 1);
    bool deleteSelection();

    /** Copy and paste (FL's Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+B). Copy takes the selected notes, or all of them
        when none is selected. Paste puts the notes at the same times into the channel shown now (another
        channel, another pattern), or right after themselves where they would land on the very same notes,
        and selects them. Duplicate puts the selection again right after itself. Each is one undo step. */
    bool copySelection();
    bool cutSelection();
    bool paste();
    bool duplicateSelection();
    /** The copied notes, kept for the whole program (shared by every Piano Roll). */
    static const std::vector<model::Note>& getCopiedNotes();
    /** Deletes some notes (right-dragging over them deletes every note it touches). */
    bool deleteNotes (const std::set<int>& indices);
    /** Cuts the notes that play across `at` (and whose pitch is between the two) into two notes there. */
    bool slice (model::Ticks at, int lowPitch, int highPitch);
    /** Paint: notes of the last length, one after another, at every grid step from `from` to `to` (as
        dragged), each at its pitch (`pitches` follows the mouse, one per step; the last is repeated). */
    bool paint (model::Ticks from, model::Ticks to, const std::vector<int>& pitches);

    /** The mouse tools (FL's): Draw adds and moves notes, Paint lays notes as you drag, Slice cuts notes
        along a line, Select draws a selection box. In every tool Ctrl+drag selects, Shift+click adds to
        the selection and right-drag deletes what it touches. */
    enum class Tool
    {
        draw,
        paint,
        slice,
        select
    };
    void setTool (Tool);
    Tool getTool() const noexcept { return tool; }

    /** Sounds these pitches on the channel being edited, releasing the ones sounding before (empty: silence).
        Notes placed or dragged sound while the mouse is down, as in FL. */
    void previewPitches (const std::vector<int>& pitches, float velocity = 0.8f);
    const std::vector<int>& getPreviewedPitches() const noexcept { return previewing; }
    /** Whether placing and dragging notes sounds them (on by default). */
    void setPreviewOn (bool on)
    {
        previewOn = on;
        if (! on)
            previewPitches ({});
    }

    //==============================================================================
    // Ghost notes. What is shown is part of the view, not the project.
    struct Ghost
    {
        model::ChannelId channel;
        juce::String name;
        juce::Colour colour;
        std::vector<model::Note> notes;
    };

    /** The ghost notes shown now, one entry per channel, in channel rack order. */
    const std::vector<Ghost>& getGhosts() const noexcept { return ghosts; }

    /** Turns all ghost notes off or on (on by default). */
    void setGhostsShown (bool);
    bool areGhostsShown() const noexcept { return ghostsShown; }

    /** Shows or hides one channel's ghost notes (all are shown by default). */
    void setGhostChannelShown (model::ChannelId, bool);
    bool isGhostChannelShown (model::ChannelId channel) const { return ! hiddenGhosts.contains (channel); }

    //==============================================================================
    // Scale highlighting: rows outside the scale are darker, the key's root note
    // tinted. A view setting: the project does not change.
    /** Follows the song's key (Key button), the default; nothing is highlighted when it has none. */
    void highlightSongKey();
    /** Highlights another key, or nothing. */
    void highlightKey (std::optional<model::Key>);
    bool isFollowingSongKey() const noexcept { return followSongKey; }
    /** The key highlighted now, if any. */
    std::optional<model::Key> getHighlightedKey() const;
    /** "Scale: A minor", "Scale: off". */
    juce::String getScaleButtonText() const;
    void showScaleMenu();

    //==============================================================================
    // Suggestions.
    /** The shown suggestion's notes for the edited channel in the current pattern, or nullptr. */
    const std::vector<model::Note>* getSuggestedNotes() const;

    /** The bar with Accept and Discard (shown while a suggestion for this pattern is, or one is made). */
    juce::Component& getSuggestionBar() noexcept;
    /** The suggestion bar's Listen: plays the suggested notes instead of the pattern's (and starts the
        pattern playing when stopped), without changing the project. */
    void listenToSuggestion (bool);

    /** Shows the "Record idea" button, which calls this with itself (to place its menu). */
    void setRecordIdea (std::function<void (juce::Component& button)>);

    /** Shows the "Generate" button (the AI pattern generator), which calls this with itself. */
    void setGenerate (std::function<void (juce::Component& button)>);
    bool hasAiButtons() const noexcept { return recordIdeaButton.isVisible() || generateButton.isVisible(); }

    model::Ticks getSnap() const noexcept { return snap; }

    /** What notes snap to: the pattern's step (a sixteenth, or a sixteenth-note triplet for a pattern with
        triplet steps), a bar, a beat, an eighth, a sixteenth, a thirty-second, eighth or sixteenth
        triplets, or nothing. */
    enum class Snap
    {
        step = 1,
        bar,
        beat,
        eighth,
        sixteenth,
        thirtySecond,
        eighthTriplet,
        sixteenthTriplet,
        none
    };
    void setSnapChoice (Snap);

    /** The chord stamp: 1 a single note, 2 or 3 the key's chord of 3 or 4 notes, 10 + n the nth of
        model::getChordTypes(). A click then draws the chord on the clicked note. */
    void setChordStamp (int);
    int getChordStamp() const noexcept { return chordStamp; }
    /** What a click on `root` draws now. */
    std::vector<int> stampPitches (int root) const;

    /** The Tools menu, on the selected notes (or all): one undo step each; the result is selected. */
    enum class NoteTool
    {
        quantize,
        strumUp,
        strumDown,
        arpeggiate,
        chop,
        legato,
        humanize
    };
    bool applyNoteTool (NoteTool);
    static constexpr model::Ticks strumSpacing = 40; // a 24th of a beat between the notes of a strum
    Snap getSnapChoice() const noexcept { return snapChoice; }
    /** The current pattern's time signature (its own, or the song's), and its bar in ticks. */
    model::TimeSignature getTimeSignature() const;
    model::Ticks getBarLength() const;
    model::Ticks snapTicks (model::Ticks) const;

    //==============================================================================
    // View: where notes are drawn.
    double getPixelsPerBeat() const noexcept { return pixelsPerBeat; }
    int getKeyHeight() const noexcept { return keyHeight; }
    float tickToX (model::Ticks) const;
    model::Ticks xToTick (float x) const;
    float pitchToY (int pitch) const;
    int yToPitch (float y) const;
    void scrollToPitch (int pitch);
    /** Zooms the time (`aroundX`: pixels from the grid's left, where the time stays put; none: the middle). */
    void zoom (double factor, std::optional<float> aroundX = {});
    /** Makes the keys taller or shorter, keeping the middle pitch in place. */
    void zoomKeys (double factor);
    /** Shows the whole pattern across the grid. */
    void fitPattern();
    double getScrollTicks() const noexcept { return scrollTicks; }
    double getVisibleTicks() const;
    bool isFollowing() const { return timelineControls.isFollowing(); }
    void setFollowing (bool follow) { timelineControls.setFollowing (follow); }
    /** Turns the page when `ticks` (the pattern position while it plays) has left the view and Follow is on. */
    void follow (double ticks);

    /** Applies pending updates now (tests). */
    using juce::AsyncUpdater::handleUpdateNowIfNeeded;

    juce::Rectangle<int> getDefaultBounds() const override { return { 140, 100, 760, 420 }; }
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scale) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    class Grid;
    class Keyboard;
    class Ruler;
    class Lane;
    class LaneChoice;
    class GhostChoice;
    class SuggestionBar;

    void reloadNotes();
    bool commit (std::vector<model::Note> newNotes, const juce::String& description);
    bool ensurePattern();
    model::Ticks getPatternLength() const;

    void updateScrollBars();

    void changeListenerCallback (juce::ChangeBroadcaster*) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { reloadNotes(); }
    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;

    Session& session;
    int historyListener = 0;
    std::vector<model::Note> notes; // what the components draw (a working copy during gestures)
    std::vector<Ghost> ghosts;
    bool ghostsShown = true;
    std::set<model::ChannelId> hiddenGhosts;
    std::set<int> selection;
    std::optional<std::vector<model::Note>> selectAfterCommit; // the notes to select once the edit is in
    Tool tool = Tool::draw;
    std::vector<int> previewing;
    bool previewOn = true;
    juce::TextButton toolButtons[4];
    std::vector<model::Note> getSelectedNotes() const;
    void selectMatching (const std::vector<model::Note>&);
    bool commitKeepingSelection (std::vector<model::Note> newNotes, std::vector<model::Note> selected,
                                 const juce::String& description);
    std::vector<model::Note> paintedNotes (model::Ticks from, model::Ticks to, const std::vector<int>& pitches) const;
    bool followSongKey = true;
    std::optional<model::Key> chosenKey;

    model::Ticks snap = 240;
    Snap snapChoice = Snap::step;
    juce::ComboBox snapBox, chordBox;
    juce::TextButton toolsButton { "Tools" };
    TimelineControls timelineControls;
    int chordStamp = 1;
    void updateSnap();
    double pixelsPerBeat = 96.0;
    int keyHeight = 14;
    double scrollTicks = 0.0; // left edge of the grid
    double scrollY = 0.0;     // top edge of the grid, in pixels
    model::Ticks lastLength = 240;
    float lastVelocity = 0.78f;
    LaneMode laneMode = LaneMode::velocity;
    double playheadTicks = -1.0;

    /** The pattern's loop region as shown: the one being dragged on the timeline, or the session's. */
    std::optional<Session::LoopRegion> getShownLoop() const
    {
        return pendingLoop ? pendingLoop : session.getLoopRegion (Session::Mode::pattern);
    }
    std::optional<Session::LoopRegion> pendingLoop;

    std::unique_ptr<Grid> grid;
    std::unique_ptr<Keyboard> keyboard;
    std::unique_ptr<Ruler> ruler;
    std::unique_ptr<Lane> lane;
    std::unique_ptr<juce::Component> laneChoice, ghostChoice;
    std::unique_ptr<SuggestionBar> suggestionBar;
    juce::ScrollBar horizontal { false }, vertical { true };
    juce::TextButton recordIdeaButton { "Record idea" }, generateButton { "Generate" }, scaleButton;
    std::function<void (juce::Component&)> onRecordIdea, onGenerate;
    juce::Label emptyMessage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRoll)
};

} // namespace spinus::ui
