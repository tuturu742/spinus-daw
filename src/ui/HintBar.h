#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** The FL-style hint bar at the bottom of the main window: shows what the control under
    the mouse does. Controls describe themselves with Component::setHelpText(); the nearest parent
    with help text is used. */
class HintBar : public juce::Component
{
public:
    HintBar();
    ~HintBar() override;

    /** The hint for a component: its own help text, or the nearest parent's. */
    static juce::String hintFor (const juce::Component*);

    /** Shows a message for a few seconds (e.g. "Saved"), replacing the hover hint. */
    void showMessage (const juce::String&);

    juce::String getText() const { return text; }
    void paint (juce::Graphics&) override;

private:
    struct GlobalMouse : juce::MouseListener
    {
        explicit GlobalMouse (HintBar& b) : bar (b) {}
        void mouseMove (const juce::MouseEvent& e) override { bar.mouseMovedAnywhere (e); }
        HintBar& bar;
    };

    void mouseMovedAnywhere (const juce::MouseEvent&);
    void setText (const juce::String&);

    GlobalMouse globalMouse { *this };
    juce::String text;
    juce::uint32 messageUntil = 0;
};

} // namespace spinus::ui
