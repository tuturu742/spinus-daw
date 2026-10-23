#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** A label for messages that may be long (errors): the whole text shows on hover, and right-click copies
    it, so it can be pasted into a bug report. */
class MessageLabel final : public juce::Label
{
public:
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() || getText().isEmpty())
            return juce::Label::mouseDown (e);
        juce::PopupMenu menu;
        menu.addItem ("Copy message", [text = getText()] { juce::SystemClipboard::copyTextToClipboard (text); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
    }

private:
    void textWasChanged() override { setTooltip (getText()); }
};

} // namespace spinus::ui
