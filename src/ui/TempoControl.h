#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>

namespace spinus::ui
{

/** The tempo display in the top bar, FL-style: drag up/down or use the mouse wheel to change it,
    double-click to type a value. A change is committed once, when the gesture ends (one undo step); mouse
    wheel notches close together are one gesture (see onCommit). */
class TempoControl : public juce::Component
{
public:
    static constexpr double minBpm = 20.0, maxBpm = 999.0;

    TempoControl();

    void setTempo (double bpm); // display only (e.g. after undo)
    double getDisplayedTempo() const noexcept { return shown; }

    /** Changes the tempo by a step and commits it as a change of its own. */
    void nudge (double deltaBpm) { commit (shown + deltaBpm, false); }

    /** One mouse wheel notch at `nowMs` (a millisecond counter): commits the change, continuing the previous
        notch's gesture if that came less than wheelGapMs before. */
    void wheelStep (double deltaBpm, juce::uint32 nowMs);
    static constexpr juce::uint32 wheelGapMs = 800;

    /** What typed text means: the tempo it names, kept within minBpm to maxBpm; nothing for text that is not
        a number (the edit is then ignored). */
    static std::optional<double> parseTyped (const juce::String& text);

    /** The box for typing a value (double-click opens it). */
    juce::Label& getEditor() noexcept { return editor; }

    /** Called with the new tempo when a drag, wheel notch or typed value is finished. `continuing` is true for
        a wheel notch that closely follows another: the caller joins it to the previous undo step. */
    std::function<void (double bpm, bool continuing)> onCommit;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void commit (double bpm, bool continuing);

    double shown = 120.0, dragStartValue = 120.0;
    bool dragging = false;
    std::optional<juce::uint32> lastWheelMs;
    juce::Label editor;
};

} // namespace spinus::ui
