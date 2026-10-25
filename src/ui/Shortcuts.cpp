#include "Shortcuts.h"

namespace spinus::ui::shortcuts
{

namespace
{
juce::ApplicationCommandManager* commands = nullptr;

/** Asks the main window's commands, whichever exist at the time of the key. */
struct Forwarder final : juce::KeyListener
{
    bool keyPressed (const juce::KeyPress& key, juce::Component* origin) override
    {
        return commands != nullptr && commands->getKeyMappings()->keyPressed (key, origin);
    }

    bool keyStateChanged (bool isKeyDown, juce::Component* origin) override
    {
        return commands != nullptr && commands->getKeyMappings()->keyStateChanged (isKeyDown, origin);
    }
};

Forwarder& forwarder()
{
    static Forwarder instance;
    return instance;
}

/** A text field types any key with a character, Ctrl held or not: Ctrl+Alt+1 or Ctrl+= in the Browser's
    search box typed "1" or "=" instead of choosing a layout or zooming. While a text field has the focus,
    Ctrl with a digit or a sign goes to the shortcuts first; Ctrl with a letter stays with the field (Ctrl+A,
    Ctrl+C, Ctrl+V...). */
struct TextFieldKeys final : juce::KeyListener, juce::FocusChangeListener
{
    bool keyPressed (const juce::KeyPress& key, juce::Component* origin) override
    {
        const auto character = key.getTextCharacter();
        return key.getModifiers().isCommandDown() && character > ' ' &&
               ! juce::CharacterFunctions::isLetter (character) && forwarder().keyPressed (key, origin);
    }

    void globalFocusChanged (juce::Component* focused) override
    {
        if (auto* field = dynamic_cast<juce::TextEditor*> (focused))
        {
            field->removeKeyListener (this); // once
            field->addKeyListener (this);
        }
    }
};

TextFieldKeys& textFieldKeys()
{
    static TextFieldKeys instance;
    return instance;
}
} // namespace

void setCommandManager (juce::ApplicationCommandManager& manager)
{
    commands = &manager;
    auto& desktop = juce::Desktop::getInstance();
    desktop.removeFocusChangeListener (&textFieldKeys()); // once
    desktop.addFocusChangeListener (&textFieldKeys());
}

void forget (juce::ApplicationCommandManager& manager)
{
    if (commands == &manager)
    {
        commands = nullptr;
        juce::Desktop::getInstance().removeFocusChangeListener (&textFieldKeys());
    }
}

juce::KeyListener& getWindowKeys() { return forwarder(); }

juce::KeyListener& getTextFieldKeys() { return textFieldKeys(); }

void addTo (juce::Component& window) { window.addKeyListener (&forwarder()); }

void removeFrom (juce::Component& window) { window.removeKeyListener (&forwarder()); }

} // namespace spinus::ui::shortcuts
