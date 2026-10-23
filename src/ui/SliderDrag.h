#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace spinus::ui
{

/** Calls `apply (value, joined)` for every change, so the sound follows a knob or fader while it is
    dragged: the first change of a mouse drag is a new undo step, the rest join it. Changes made from the
    model (setValue with dontSendNotification) are not sent. */
inline void bindDrag (juce::Slider& s, std::function<void (double, bool)> apply)
{
    auto dragging = std::make_shared<int> (0); // 0 idle, 1 drag started, 2 drag has moved
    s.onDragStart = [dragging] { *dragging = 1; };
    s.onDragEnd = [dragging] { *dragging = 0; };
    s.onValueChange = [&s, dragging, apply = std::move (apply)]
    {
        apply (s.getValue(), *dragging == 2);
        if (*dragging == 1)
            *dragging = 2;
    };
}

/** Shows an automated volume or pan on its control while the automation plays (FL): the slider follows
    the parameter's value; when the automation stops (no clip, pattern mode), it goes back to `modelValue`.
    `wasFollowing` remembers which of the two it showed. */
template <typename Parameter, typename ToSlider>
void followAutomation (juce::Slider& slider, Parameter* parameter, ToSlider toSlider, double modelValue,
                       bool& wasFollowing)
{
    if (slider.isMouseButtonDown())
        return;
    const bool active = parameter != nullptr && parameter->isAutomationActive();
    if (active)
        slider.setValue (toSlider (parameter->getCurrentValue()), juce::dontSendNotification);
    else if (wasFollowing)
        slider.setValue (modelValue, juce::dontSendNotification);
    wasFollowing = active;
}

} // namespace spinus::ui
