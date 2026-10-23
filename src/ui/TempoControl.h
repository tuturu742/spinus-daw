#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace spinus::ui
{

/** The tempo display in the top bar, FL-style: drag up/down or use the mouse wheel to change it,
    double-click to type a value. A change is committed once, when the gesture ends (one undo step). */
class TempoControl : public juce::Component
{
public:
    static constexpr double minBpm = 20.0, maxBpm = 999.0;

    TempoControl();

    void setTempo (double bpm); // display only (e.g. after undo)
    double getDisplayedTempo() const noexcept { return shown; }

    /** Changes the tempo by a step and commits it (what the mouse wheel does). */
    void nudge (double deltaBpm) { commit (shown + deltaBpm); }

    /** Called with the new tempo when a drag, wheel step or typed value is finished. */
    std::function<void (double)> onCommit;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void commit (double bpm);

    double shown = 120.0, dragStartValue = 120.0;
    bool dragging = false;
    juce::Label editor;
};

} // namespace spinus::ui
