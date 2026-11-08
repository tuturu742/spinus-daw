#pragma once

#include "session/AnalysisLibrary.h"
#include "session/InstrumentProfiles.h"
#include "session/ManualReading.h"
#include "session/SoundIndex.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace spinus::ai
{
class AiService;
}

namespace spinus::ui
{

/** Learn this instrument: what each key of an instrument does, mode by mode (InstrumentProfiles), on a keyboard
    coloured by job (plays, switch, special, unused, not known). Drag across keys to select them, then mark them;
    hovering a key says what it does and where Spinus learned it. Each mode has a name, how to enter it (in words,
    or the key that switches to it), what it suits and whether it plays chords. Add mode / Remove mode for an
    instrument with several (a sampled guitar's normal and strummer modes). Listen to every key hears the mode's
    keys (KeyProbe, in the sound index's helper), and Ask the plugin reads the names the plugin gives its keys
    (pluginkeys, there too); either shows what it found as a proposal: the keys that would change are marked, and
    nothing changes until Apply (the keys you set stay yours); Discard drops it. Read the manual has the AI read
    the instrument's manuals (PDF files, web pages; ai.v1.instrument.readManual) and proposes the modes they name,
    how to enter each, what each suits and what their keys do (manualreading), applied the same way. Nothing is kept
    until Save; Forget this instrument removes its profile (Save then keeps that). Message thread. */
class LearnInstrumentPanel final : public juce::Component
{
public:
    using Profile = InstrumentProfiles::Profile;
    using Job = InstrumentProfiles::Job;

    LearnInstrumentPanel (const juce::String& instrumentName, const std::optional<Profile>& start);
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
    /** Listen to every key, for the mode shown (its entry key); none: the button says it can't. */
    std::function<void (int enterKey)> onListen;
    std::function<void()> onStopListening;
    /** Ask the plugin: its own names for its keys, for the mode shown. */
    std::function<void()> onAsk;
    /** While listening or asking: "Listening: 40 of 128 keys"; empty when done. */
    void setListening (const juce::String& progress);
    /** Where a proposal comes from: Listen to every key, or the plugin's own report. */
    enum class From
    {
        listening,
        plugin
    };
    /** What was heard (or what the plugin reported) for the mode shown when it started (none: `problem` says why). */
    void setHeard (std::optional<std::array<InstrumentProfiles::Key, 128>>, const juce::String& problem = {},
                   From = From::listening);
    bool hasHeard() const noexcept { return heard.has_value(); }
    /** Apply: the keys heard into the mode (the ones you set stay yours). Discard: dropped. */
    void applyHeard();
    void discardHeard();
    /** Manuals (Read the manual): a file's full path or a web page's address; kept with the profile. */
    void addManual (const juce::String& source);
    void removeManual (int index);
    const std::vector<InstrumentProfiles::Manual>& getManuals() const noexcept { return profile.manuals; }
    /** The instrument's version now (its plugin's): a manual read for another version may be out of date. */
    void setInstrumentVersion (const juce::String&);
    /** Whether Read the manual can run: empty, or why not ("needs the AI service"), shown on the greyed button. */
    void setCanRead (const juce::String& whyNot);
    std::function<void()> onRead, onChooseManual;
    /** While the AI reads: what it is doing; empty when done. */
    void setReadingProgress (const juce::String&);
    /** What the AI read in the manuals (none: `problem` says why), as a proposal like a listening's. */
    void setReading (std::optional<manualreading::Reading>, const juce::String& problem = {});
    bool hasReading() const noexcept { return reading.has_value(); }
    /** "2 manuals: Guitar_Strummer.pdf, example.org/manual (read for 1.0.2; the instrument is 1.0.3 now)". */
    juce::String getManualsText() const;
    /** What the probing found, or why not ("Heard: 49 keys play, 3 switch keys..."). */
    juce::String getHeardText() const { return heardLabel.getText(); }
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
    void updateManuals();

    juce::String name;
    Profile profile;
    std::optional<std::array<InstrumentProfiles::Key, 128>> heard;
    std::optional<manualreading::Reading> reading;
    juce::String instrumentVersion, readWhyNot;
    bool readingNow = false;
    int heardMode = 0;
    From heardFrom = From::listening;
    bool listening = false;
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
    juce::TextButton listenButton { "Listen to every key" }, askButton { "Ask the plugin" }, applyButton { "Apply" },
        discardButton { "Discard" };
    juce::Label heardLabel, manualsLabel;
    juce::ComboBox manualsBox;
    juce::TextEditor manualEditor;
    juce::TextButton removeManualButton { "Remove" }, chooseManualButton { "Choose file..." },
        addManualButton { "Add" }, readButton { "Read the manual" };
};

/** The panel in a window of its own, one per instrument: Save keeps the profile beside the sound index and calls
    `saved` with the keys the instrument now plays as it opens (nullopt: not known). */
class LearnInstrumentWindow final : public juce::DocumentWindow
{
public:
    using Saved = std::function<void (std::optional<PlayableKeys>)>;
    /** What Listen to every key loads: the instrument (with its sound: a preset id, or a saved sound's state file)
        and the helper that runs it. */
    struct Listening
    {
        SoundIndexer::Options helper;
        SoundSource source;
        juce::String preset;
        juce::File state;
    };

    /** Opens (or brings to the front) the window for `instrument` (the sound index's key). Without `listening`,
        Listen to every key says it needs the instrument chosen for a part. */
    static void open (const juce::File& indexFolder, const juce::String& instrument, const juce::String& name,
                      juce::Component* near, Saved saved, std::optional<Listening> listening = {},
                      ai::AiService* service = nullptr);
    static int getNumOpen();
    static void closeAll();
    /** For tests: the panel of the window opened last; null when none is open. */
    static LearnInstrumentPanel* lastOpened();

    ~LearnInstrumentWindow() override;
    void closeButtonPressed() override;

private:
    LearnInstrumentWindow (const juce::File& indexFolder, const juce::String& instrument, const juce::String& name,
                           Saved saved, std::optional<Listening> listening, ai::AiService* service);
    void finish();
    void listen (int enterKey);
    void ask();
    void readManuals();
    void chooseManual();

    juce::File indexFolder;
    juce::String instrument;
    Saved saved;
    std::optional<Listening> listening;
    ai::AiService* service = nullptr;
    juce::String name;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<BackgroundTask> task;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    std::shared_ptr<std::atomic<bool>> stopping = std::make_shared<std::atomic<bool>> (false);
};

} // namespace spinus::ui
