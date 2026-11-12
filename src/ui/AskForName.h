#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** Asks for a new name in a small window (async); calls back only when one is given. */
inline void askForName (const juce::String& title, const juce::String& current, std::function<void (juce::String)> then)
{
    auto* window = new juce::AlertWindow (title, {}, juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("name", current);
    window->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (true,
                             juce::ModalCallbackFunction::create (
                                 // Deleted from outside (the app closing), the window is gone by now: no name then.
                                 [safe = juce::Component::SafePointer<juce::AlertWindow> (window),
                                  then = std::move (then)] (int result)
                                 {
                                     if (safe == nullptr)
                                         return;
                                     const auto name = safe->getTextEditorContents ("name").trim();
                                     if (result == 1 && name.isNotEmpty())
                                         then (name);
                                 }),
                             true);
    // Ready to type over the old name, once the window is on screen (a new window takes focus then).
    juce::MessageManager::callAsync (
        [safe = juce::Component::SafePointer<juce::AlertWindow> (window)]
        {
            if (safe == nullptr)
                return;
            safe->toFront (true);
            if (auto* editor = safe->getTextEditor ("name"))
            {
                editor->grabKeyboardFocus();
                editor->selectAll();
            }
        });
}

} // namespace spinus::ui
