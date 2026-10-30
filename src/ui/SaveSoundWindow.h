#pragma once

#include "session/PartSounds.h"
#include "session/Session.h"

#include <memory>

namespace spinus
{
class HiddenInstrument;
}

namespace spinus::ui
{

/** The Track creator's Save this sound: an instrument's own window, outside the project, where the user sets
    the sound up (picks a kit in a drum player that is silent until then, or a patch), names it and clicks Save.
    The plugin's state goes to `onSave`, which keeps it as one of the user's saved sounds (SavedSounds) for the
    sound index to measure. Generic for every plugin: the plugin's own editor, or JUCE's generic one (a slider
    per setting) for a plugin without one. The instrument is loaded as Spinus loads it in a project (Windows
    plugins through Wine among them), into a hidden edit of its own: nothing in the project changes.

    Message thread. The window deletes itself when closed (Save or Cancel). */
class SaveSoundWindow final : public juce::DocumentWindow
{
public:
    using OnSave = std::function<void (const juce::String& name, const juce::MemoryBlock& state)>;

    /** Opens the window for `start`'s instrument with its sound (a preset, a saved sound, or as it opens), the
        name field holding `name`. False (and `problem` says why) when the instrument cannot be loaded. */
    static bool open (Session&, const parts::Choice& start, const juce::String& name, juce::Component* near, OnSave,
                      juce::String& problem);

    /** The open windows (tests). */
    static int getNumOpen();
    /** Closes every one at once, without saving (before the session goes). */
    static void closeAll();
    /** For tests: saves (as the button does) in the window opened last, under `name`. */
    static bool saveLastOpened (const juce::String& name);

    ~SaveSoundWindow() override;
    void closeButtonPressed() override;

private:
    class Content;
    SaveSoundWindow (std::unique_ptr<HiddenInstrument>, const juce::String& title, const juce::String& name, OnSave,
                     juce::Component* owner);
    void save (const juce::String& name);
    void close();

    std::unique_ptr<HiddenInstrument> hidden;
    std::unique_ptr<Content> content;
    OnSave onSave;
};

} // namespace spinus::ui
