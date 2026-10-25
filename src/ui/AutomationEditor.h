#pragma once

#include "TimelineControls.h"
#include "Workspace.h"
#include "model/Automation.h"
#include "session/Session.h"

#include <set>

namespace spinus::ui
{

/** The Automation editor: one automation clip, large, for fine work. Opened by
    double-clicking an automation clip on the Playlist, and when a clip is made.

    Mouse (as in the Piano Roll): click empty space to add a point and drag it into place; drag a point
    to move it (with every selected point when it is selected); press Shift during a drag for fine
    movement; right-click or right-drag over points to delete them; Ctrl+drag selects, Shift+click adds
    to the selection; drag the small diamond between two points up or down to bend that part of the
    curve (double-click it to straighten it); double-click a point to type its value and position.
    Tools: Draw, Line (drag a straight ramp), Paint (draw freehand), Select. Keys: Delete, Ctrl+A,
    Escape, the arrows nudge the selection (Up/Down: 1%, with Shift 0.1%; Left/Right: one grid step,
    with Alt one tick). Points snap to the grid (Alt: free). The wheel scrolls, Ctrl+wheel zooms around the
    mouse; the toolbar's zoom buttons, Ctrl+= and Ctrl+- zoom too, Ctrl+0 fits the clip. Follow (on by default,
    remembered) turns the page while the song plays through the clip.

    Edits work on a copy while the mouse is down and are written as one clip.setPoints command when it
    is released, so every gesture is one undo step.
*/
class AutomationEditor final : public Panel,
                               private juce::ChangeListener,
                               private juce::Timer,
                               private juce::AsyncUpdater,
                               private juce::ScrollBar::Listener
{
public:
    static constexpr int toolbarHeight = 30, rulerHeight = 20, axisWidth = 76;

    explicit AutomationEditor (Session&);
    ~AutomationEditor() override;

    /** Shows this clip (an automation clip), fitted to the window. */
    void setClip (model::ClipId);
    model::ClipId getClip() const noexcept { return clipId; }

    /** The points shown: the clip's, or the edited copy while the mouse is down. */
    const std::vector<model::AutomationPoint>& getPoints() const noexcept { return points; }
    /** The clip's length in ticks (0 when there is no clip). */
    model::Ticks getClipLength() const;

    //==============================================================================
    // Editing (used by the mouse and keys, and by tests). Each call is one undo step. Times are ticks
    // from the clip's start (0 to its length), values 0..1.
    bool addPoint (model::Ticks time, float value);
    /** Moves the points by the same amounts, each kept inside the clip and 0..1. */
    bool movePoints (const std::set<int>& indices, model::Ticks deltaTicks, float deltaValue);
    bool setPoint (int index, model::Ticks time, float value);
    /** The bend of the part of the curve that starts at this point: -1..1, 0 straight. */
    bool setCurve (int index, float curve);
    /** Deletes points; at least one stays (a clip without points does nothing). */
    bool deletePoints (const std::set<int>& indices);
    bool deleteSelection() { return deletePoints (selection); }
    /** Replaces the points between two times with a straight line from one value to the other. */
    bool drawLine (model::Ticks from, float fromValue, model::Ticks to, float toValue);
    /** Writes a shape over the selection's span (between its lowest and highest values, when they
        differ), or over the whole clip from 0 to 100%. `period`: one cycle, in ticks. */
    bool applyShape (model::AutomationShape, model::Ticks period);

    //==============================================================================
    // The selection: part of the view, it follows the points through edits.
    const std::set<int>& getSelection() const noexcept { return selection; }
    void setSelection (std::set<int>);
    void selectAll();
    void clearSelection() { setSelection ({}); }
    /** Selects the points inside a time range and a value range (both inclusive). */
    void selectArea (model::Ticks from, model::Ticks to, float low, float high, bool add);

    enum class Tool
    {
        draw,
        line,
        paint,
        select
    };
    void setTool (Tool);
    Tool getTool() const noexcept { return tool; }

    /** The grid, in ticks (0: no grid). */
    void setSnap (model::Ticks);
    model::Ticks getSnap() const noexcept { return snap; }
    model::Ticks snapTicks (model::Ticks) const;

    /** "2.3.120": bar, beat and tick (960 a beat) of a time from the clip's start, counted from 1 as
        the song's ruler does. */
    juce::String formatTime (model::Ticks) const;
    /** Reads "2", "2.3" or "2.3.120" back (bars and beats from 1); nothing if it is not a time. */
    std::optional<model::Ticks> parseTime (const juce::String&) const;

    float tickToX (double ticks) const;
    model::Ticks xToTick (float) const;
    float valueToY (float) const;
    float yToValue (float) const;

    /** Zooms the time (`aroundX`: pixels from the canvas' left, where the time stays put; none: the middle). */
    void zoom (double factor, std::optional<float> aroundX = {});
    /** Shows the whole clip. */
    void fitToClip();
    double getPixelsPerBeat() const noexcept { return pixelsPerBeat; }
    double getScrollTicks() const noexcept { return scrollTicks; }
    bool isFollowing() const { return timelineControls.isFollowing(); }
    void setFollowing (bool follow) { timelineControls.setFollowing (follow); }

    juce::Rectangle<int> getDefaultBounds() const override { return { 120, 110, 780, 360 }; }
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scale) override;

private:
    class Canvas;
    class Ruler;
    class Axis;

    const model::Clip* findClip() const;
    const model::AutomationClip* findAutomation() const;
    model::TimeSignature getTimeSignature() const;
    /** Sorts the points by time, keeping the selection on the same points. */
    void sortPoints (std::vector<model::AutomationPoint>&, std::set<int>&) const;
    bool commit (std::vector<model::AutomationPoint>, std::set<int> newSelection, const juce::String& what);
    void reload();
    void updateScrollBars();
    void updateStatus();
    void showShapesMenu();
    void editPoint (int index);
    void nudge (float deltaValue, model::Ticks deltaTicks);
    bool keyPressed (const juce::KeyPress&) override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { reload(); }
    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar*, double) override;

public:
    Session& session;

private:
    int historyListener = 0;
    model::ClipId clipId;
    std::vector<model::AutomationPoint> points;
    std::set<int> selection;
    bool editing = false; // the mouse is down: `points` is the edited copy
    Tool tool = Tool::draw;
    model::Ticks snap = model::ticksPerQuarterNote / 4;
    double pixelsPerBeat = 48.0, scrollTicks = 0.0, playheadTicks = -1.0;

    std::unique_ptr<Canvas> canvas;
    std::unique_ptr<Ruler> ruler;
    std::unique_ptr<Axis> axis;
    juce::Label title, status, emptyMessage;
    juce::TextButton toolButtons[4], shapesButton { "Shapes" };
    juce::ComboBox snapChoice;
    TimelineControls timelineControls;
    juce::ScrollBar horizontal { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomationEditor)
};

} // namespace spinus::ui
