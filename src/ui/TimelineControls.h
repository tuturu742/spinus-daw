#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

namespace spinus::ui
{

/** Zooming and following the play position in the timeline editors (Playlist, Piano Roll, Automation editor):
    the same steps, keys and page turns in each. */
namespace timeline
{
/** One zoom step (a button, a key, a wheel notch). */
inline constexpr double zoomStep = 1.25;

/** Zooms a time axis by `factor` around `x` (pixels from where the axis starts), so the time under the mouse
    stays there. `pixelsPerBeat` is kept within `limits`; returns the new scroll position in ticks (the time
    at the axis' start), never before `earliest`. */
double zoomAround (double& pixelsPerBeat, double scrollTicks, double x, double factor, juce::Range<double> limits,
                   double ticksPerBeat, double earliest = 0.0);

/** Following the play position: the view's new start when `playhead` has left the view (`scroll` to
    `scroll + visible`, all in ticks), turning a page so it shows from just before the playhead; none while it
    is in view. */
std::optional<double> followPlayhead (double playhead, double scroll, double visible);

/** The zoom keys: Ctrl+= (or Ctrl++) zooms in, Ctrl+- out, Ctrl+0 fits. */
enum class ZoomKey
{
    none,
    in,
    out,
    fit
};
ZoomKey zoomKeyOf (const juce::KeyPress&);

/** The wheel zooms with Ctrl; with Ctrl+Shift the editor's height (rows or keys). */
inline double wheelZoomFactor (const juce::MouseWheelDetails& wheel)
{
    const auto delta = std::abs (wheel.deltaY) > std::abs (wheel.deltaX) ? wheel.deltaY : wheel.deltaX;
    return delta > 0.0f ? zoomStep : (delta < 0.0f ? 1.0 / zoomStep : 1.0);
}
} // namespace timeline

/** The zoom buttons of a timeline editor's toolbar ("−", "+", "Fit") and, where the editor follows the
    play position, its "Follow" switch. The switch is remembered in the settings under `followSetting`. */
class TimelineControls final : public juce::Component
{
public:
    /** `settings` may be null (tests); `followSetting` empty: no Follow switch. */
    TimelineControls (juce::PropertiesFile* settings, juce::String followSetting);

    std::function<void (double factor)> onZoom;
    std::function<void()> onFit;

    bool isFollowing() const { return followButton.isVisible() && followButton.getToggleState(); }
    void setFollowing (bool);

    /** The width the controls need at this height. */
    int getPreferredWidth() const;
    void resized() override;

private:
    juce::PropertiesFile* settings;
    juce::String followSetting;
    juce::TextButton zoomOutButton { juce::String::fromUTF8 ("\xe2\x88\x92") }, zoomInButton { "+" },
        fitButton { "Fit" }, followButton { "Follow" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineControls)
};

} // namespace spinus::ui
