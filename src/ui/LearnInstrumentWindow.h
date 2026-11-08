#pragma once

#include "session/InstrumentProfiles.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace spinus::ui
{

/** Learn this instrument: what each key of an instrument does, mode by mode (InstrumentProfiles), on a keyboard
    coloured by job (plays, switch, special, unused, not known). Drag across keys to select them, then mark them;
    hovering a key says what it does and where Spinus learned it. Each mode has a name, how to enter it (in words,
    or the key that switches to it), what it suits and whether it plays chords. Add mode / Remove mode for an
    instrument with several (a sampled guitar's normal and strummer modes). Nothing is kept until Save; Forget this
    instrument removes its profile (Save then keeps that). Message thread. */
class LearnInstrumentPanel final : public juce::Component
{
public:
    using Profile = InstrumentProfiles::Profile;
    using Job = InstrumentProfiles::Job;

    LearnInstrumentPanel (const juce::String& instrumentName, std::optional<Profile> start);
    ~LearnInstrumentPanel() override;

    /** Save (with the profile as it is now; nullopt when forgotten) and Cancel. */
    std::function<void (std::optional<Profile>)> onSave;
    std::function<void()> onCancel;

    /** The profile as edited so far (nullopt when forgotten). */
    std::optional<Profile> getProfile() const;
    int getMode() const noexcept { return mode; }
    void showMode (int index);
    void addMode (const juce::String& name);
    void removeMode();
    /** Selects keys `from` to `to`, as a drag across them does. */
    void selectKeys (int from, int to);
    /** Marks the selected keys (Plays, Switch... buttons), with what they do for switch and special keys. */
    void markSelected (Job, const juce::String& what = {});
    void forget();
    void save();
    /** The line under the keyboard ("Plays E2 to G6; 2 switch keys"). */
    juce::String getSummaryText() const;
    /** What hovering a key says. */
    juce::String describeKey (int note) const;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class Keyboard;
    InstrumentProfiles::Mode& current();
    void updateFields();
    void fieldsChanged();

    juce::String name;
    Profile profile;
    bool forgotten = false;
    int mode = 0, selectedFrom = -1, selectedTo = -1;

    juce::Label intro, modeLabel, enterLabel, goodForLabel, selectionLabel, summary;
    juce::ComboBox modeBox, enterKeyBox, voicesBox;
    juce::TextButton addModeButton { "Add mode" }, removeModeButton { "Remove mode" };
    juce::TextEditor modeName, enterEditor, goodForEditor, whatEditor;
    std::unique_ptr<Keyboard> keyboard;
    juce::TextButton playsButton { "Plays" }, switchButton { "Switch" }, specialButton { "Special" },
        unusedButton { "Unused" }, unknownButton { "Not known" };
    juce::TextButton forgetButton { "Forget this instrument" }, cancelButton { "Cancel" }, saveButton { "Save" };
};

/** The panel in a window of its own, one per instrument: Save keeps the profile beside the sound index and calls
    `saved` with the keys the instrument now plays as it opens (nullopt: not known). */
class LearnInstrumentWindow final : public juce::DocumentWindow
{
public:
    using Saved = std::function<void (std::optional<PlayableKeys>)>;

    /** Opens (or brings to the front) the window for `instrument` (the sound index's key). */
    static void open (const juce::File& indexFolder, const juce::String& instrument, const juce::String& name,
                      juce::Component* near, Saved saved);
    static int getNumOpen();
    static void closeAll();
    /** For tests: the panel of the window opened last; null when none is open. */
    static LearnInstrumentPanel* lastOpened();

    ~LearnInstrumentWindow() override;
    void closeButtonPressed() override;

private:
    LearnInstrumentWindow (const juce::File& indexFolder, const juce::String& instrument, const juce::String& name,
                           Saved saved);
    void finish();

    juce::File indexFolder;
    juce::String instrument;
    Saved saved;
};

} // namespace spinus::ui
