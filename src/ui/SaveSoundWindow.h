#pragma once

#include "engine/LiveInstrument.h"
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
    Save and keep open keeps the sound as it is and leaves the window open for the next one (another preset, another
    kit): several sounds from one window. The plugin's states go to `onSave`, which keeps them as the user's saved
    sounds (SavedSounds) for the
    sound index to measure. Generic for every plugin: the plugin's own editor, or JUCE's generic one (a slider
    per setting) for a plugin without one. The instrument is loaded as Spinus loads it in a project (Windows
    plugins through Wine among them), into a hidden edit of its own: nothing in the project changes.

    The instrument plays live through the sound card while the window is open (LiveInstrument, through the
    sample preview), so what the user does in its window is heard, and Play plays the part's phrase on it. Keys...
    opens Learn this instrument (LearnInstrumentWindow): many sampled instruments switch what they play with keys
    outside their range, so Play (and the rest of the Track creator) keeps to the keys that play, moving the phrase
    by octaves.

    Message thread. The window deletes itself when closed (Save, or Close/Cancel, which keeps what was saved with Save
    and keep open and nothing else). `onSave` is called once it has gone, when anything was saved,
    with its instrument: a plugin may take seconds to close, and is not measured while it does. */
class SaveSoundWindow final : public juce::DocumentWindow
{
public:
    struct Saved
    {
        juce::String name;
        juce::MemoryBlock state;
    };
    /** Every sound saved in the window, in order. */
    using OnSave = std::function<void (std::vector<Saved>)>;

    /** The keys the instrument plays (InstrumentProfiles): Play keeps to them (moved by octaves), and Keys... next
        to it opens Learn this instrument (`learn`, which calls back with the keys once it is saved). No `learn`: no
        Keys... button (the drums). */
    struct Notes
    {
        std::optional<PlayableKeys> playable;
        std::function<void (std::function<void (std::optional<PlayableKeys>)>)> learn;
    };

    /** Opens the window for `start`'s instrument with its sound (a preset, a saved sound, or as it opens), the
        name field holding `name`; Play plays `phrase`, within `notes`. False (and `problem` says why) when the
       instrument cannot be loaded. */
    static bool open (Session&, const parts::Choice& start, const juce::String& name,
                      std::vector<OfflineInstrument::Note> phrase, Notes notes, juce::Component* near, OnSave,
                      juce::String& problem);

    /** The open windows (tests). */
    static int getNumOpen();
    /** Closes every one at once, without saving (before the session goes). */
    static void closeAll();
    /** For tests: saves (as the button does) in the window opened last, under `name`. */
    static bool saveLastOpened (const juce::String& name);
    /** For tests: Save and keep open in the window opened last. */
    static bool keepLastOpened (const juce::String& name);
    /** For tests: closes the window opened last as its Close button does (keeping what was saved). */
    static void closeLastOpened();
    /** For tests: Play in the window opened last. False when there is nothing to play. */
    static bool playLastOpened();
    /** For tests: the notes Play plays in the window opened last, as it is now. */
    static std::vector<int> playedNotesLastOpened();
    /** For tests: the keys the window opened last plays, as Learn this instrument says once saved. */
    static void setNotesLastOpened (std::optional<PlayableKeys>);
    /** For tests (no sound card): what the window opened last plays in the next `seconds`; its loudest sample. */
    static float mixLastOpened (double seconds);

    ~SaveSoundWindow() override;
    void closeButtonPressed() override;

private:
    class Content;
    SaveSoundWindow (Session&, std::unique_ptr<HiddenInstrument>, const juce::String& title, const juce::String& name,
                     std::vector<OfflineInstrument::Note> phrase, Notes, OnSave, juce::Component* owner);
    void save (const juce::String& name);
    /** Keeps the sound as it is now under `name`; false when the plugin gave nothing. */
    bool keep (const juce::String& name);
    /** Closes, then hands over what was kept. */
    void finish();
    bool play();
    std::vector<OfflineInstrument::Note> playedPhrase() const;
    void setNotes (std::optional<PlayableKeys>);
    void close (std::function<void()> afterwards = {});

    Session& session;
    std::unique_ptr<HiddenInstrument> hidden;
    std::unique_ptr<Content> content;
    std::vector<OfflineInstrument::Note> phrase;
    Notes notes;
    OnSave onSave;
    std::vector<Saved> kept;
    std::unique_ptr<engine::LiveInstrument> live;
};

} // namespace spinus::ui
