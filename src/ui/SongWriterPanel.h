#pragma once

#include "MessageLabel.h"
#include "ai/AiService.h"
#include "session/AnalysisLibrary.h"
#include "session/PartSounds.h"
#include "session/Session.h"
#include "session/SongWriter.h"

#include <map>
#include <memory>
#include <optional>

namespace spinus::ui
{

/** The Track creator's new song (experimental, milestone 4), shown in its panel: a song written from the style
    summary, to read about and apply.

    Write asks the AI service (ai.v1.song.write, spinus_ai/songwriter.py) for a song with the Sounds page's parts
    (the user's list, each with a role): rules and seeded chance on this computer, no AI connection needed. Each
    part plays the sound chosen for it on the Sounds page; a part without one gets the closest sound of its role
    from the sound index (varied between parts and songs), else Spinus's own. With "Let the AI vary the plan" and
    an AI connection, the AI may vary the sections, chord loops and melody shapes (checked against the style; the
    rules write every note). Another version writes again with a new seed; Redo part writes one part or one
    section again, keeping the rest. Apply puts it into the project through commands as one undo step
    (session/SongWriter), replacing the song written before when the user says so, or, once a song is in, only the
    parts or sections written again; that is how it is heard. Mix it opens the automixer toward the reference
    songs.

    Who writes the notes: Spinus (offline, the default) or, opt-in, the user's AI connection. The AI composes
    through the music harness (ai.v1.song.compose, spinus_ai/harness.py): tools that read and edit a working copy of
    the song and check it, then finish. While it works, the page shows each step in plain words and the running total
    of tokens; after, the tokens each round used as the provider reported them (no estimate before: nobody knows
    how long a model will think). "Stop now" keeps the best checked version;
    Cancel keeps the song shown before. After it finishes, each part is played with its sound (balance) and, as many
    times as "Fix what Spinus hears" says, what was heard goes back to the AI for a revision round. The AI may pick
    each part's sound from a few candidates of the sound index; the Sounds page's choices always win.

    Nothing changes the project before Apply (AGENTS rule 3). Without the AI service the panel says so; the rest
    of Spinus works as ever. Message thread only. */
class SongWriterPanel final : public juce::Component
{
public:
    /** What the Track creator knows. */
    struct Context
    {
        juce::var summary;
        ai::AiService* service = nullptr;
        juce::File workFolder;              // for the preview's rendering
        juce::Array<juce::File> songs;      // the reference songs (the automixer's reference)
        parts::Plan sounds;                 // the sounds chosen on the Sounds page (none: Spinus's own)
        juce::File indexFolder;             // the sound index, for the parts the Sounds page has no sound for
        std::vector<parts::SongPart> parts; // the Sounds page's list: the parts to write
        bool partsFromPage = false;         // the list is the Sounds page's: none means none yet (else a sketch)
        SoundIndexer::Options helper;       // the sound helper, which plays each part to measure how loud it is
    };

    SongWriterPanel (Session&, std::function<Context()> context);
    ~SongWriterPanel() override;

    //==============================================================================
    // For the buttons and for tests. Results arrive asynchronously.
    /** Writes a song (a new seed each time, unless setSeed gave one). */
    bool write();
    /** Writes the song again with a new seed: another song in the same style. */
    bool anotherVersion() { return write(); }
    /** Writes one part ("drums", "bass", "chords", "lead") or one section (its id, "verse") again. */
    bool redo (const juce::String& part, const juce::String& section = {});
    /** Renders the project as it would be after Apply, and plays it (Stop stops). */
    void stop();
    /** Puts the song (or what was written again) into the project: one undo step. When the project has a song
        the Track creator wrote, it first asks whether to replace it (in its place; its channels, patterns and
        tracks go) or keep both. */
    bool apply();
    /** Opens the automixer with the first reference song as its reference (onMix). */
    bool mix();

    std::function<void (const juce::File& reference)> onMix;

    void setRefine (bool on) { refineButton.setToggleState (on, juce::dontSendNotification); }
    /** Who writes: false, Spinus (offline); true, the AI connection (when there is one). */
    void setAiWriter (bool on);
    bool isAiWriter() const;
    /** The AI's reasoning level: "" (the model's own), "low" (the default), "medium", "high", "xhigh" (where
        offered). */
    void setEffort (const juce::String& effort);
    juce::String getEffort() const;
    /** How many times what Spinus hears goes back to the AI (0 to 3; 1 by default). */
    void setRounds (int rounds);
    int getRounds() const { return roundsBox.getSelectedId() - 1; }
    /** Asks the AI service which connection would write (asynchronously; the page names it). */
    void refreshConnection();
    juce::String getInfoText() const { return infoLabel.getText(); }
    /** Stops the AI after its current step, keeping its best checked version. */
    bool stopNow();
    bool isComposing() const noexcept { return composing; }
    /** "sketch" (four parts) or "full" (about a dozen layers): what Write writes next. Remembered. */
    /** The sound each layer of the full song gets that the Sounds page did not choose (layer id -> what was
        found in the sound index), worked out once per song. */
    const std::map<juce::String, parts::Choice>& getLayerChoices();
    void setSeed (int s) { fixedSeed = s; }

    bool isBusy() const noexcept { return busy; }
    const juce::var& getSong() const noexcept { return song; }
    juce::String getStatusText() const { return status.getText(); }
    juce::String getText() const { return text.getText(); }
    /** What the last Apply made (none before it). */
    const song::Applied& getApplied() const noexcept { return applied; }
    /** What Apply would do now: the whole song, or only what was written again; void when nothing. */
    juce::var makeProposal (juce::String* error = nullptr);
    /** The same, replacing the project's song `replace` ("Song 1"), as Apply does when the user says so. */
    juce::var makeProposal (juce::String* error, const juce::String& replace);
    /** Asks "Replace Song 1?" (Replace, Keep both, Cancel: 0, 1, 2); tests answer it themselves. */
    using MessageBoxHandler = std::function<void (const juce::MessageBoxOptions&, std::function<void (int)>)>;
    void setMessageBoxHandler (MessageBoxHandler h) { messageBox = std::move (h); }

    void resized() override;
    void paint (juce::Graphics&) override;
    void visibilityChanged() override;

private:
    void run (const juce::var& params, const juce::String& words, std::function<void (const juce::var&)> then,
              const juce::String& method = "ai.v1.song.write");
    /** The AI writer's request: the summary, the parts, the candidate sounds, the reasoning level. */
    juce::var composeParams (int seed);
    /** For each part without a sound from the Sounds page, a few sounds of the index it may pick from (id ->
        choice, kept for when the AI picks one). */
    juce::var candidatesFor (juce::var& current);
    void composeRound (const juce::var& params, const juce::String& words);
    /** After the song is written and measured: another round with what was heard, or done. */
    void afterMeasured (const std::map<juce::String, double>& levels,
                        const std::map<juce::String, juce::String>& problems);
    void finishComposing();
    void layoutAiRow (juce::Rectangle<int> row);
    void show();
    void updateButtons();
    void showRedoMenu();
    bool aiReady() const;
    song::Options optionsFor (const model::Project&, const juce::String& replace = {});
    bool applyNow (const juce::String& replace);
    const parts::Plan::Part* pagePartFor (const song::Layer&) const;
    /** The chosen sound of a full arrangement's layer: the Sounds page's for its part, or the index's. */
    std::optional<parts::Plan::Part> chosenForLayer (const song::Layer&);
    /** After writing: each layer's busiest bars played with its sound in the sound helper, measured, and its
        starting fader set from that and its role (song::startingFaders), for Apply. */
    void
    balance (std::function<void (const std::map<juce::String, double>&, const std::map<juce::String, juce::String>&)>
                 then = {});

    Session& session;
    std::function<Context()> getContext;
    Context context;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    bool busy = false;
    int fixedSeed = -1;
    juce::String job;
    juce::var song;
    // Against the song in the project: write the whole song (true) or only the parts and sections written again.
    bool whole = true;
    juce::StringArray redoneParts, redoneSections;
    song::Applied applied;

    juce::Label intro;
    MessageLabel status;
    MessageBoxHandler messageBox;
    juce::ToggleButton refineButton { "Let the AI vary the plan (uses your AI connection)" };
    juce::Label writerLabel { {}, "Who writes the notes:" };
    juce::ComboBox writerBox, effortBox, roundsBox;
    juce::Label infoLabel;
    juce::TextButton stopNowButton { "Stop now, keep the best" };
    bool aiAvailable = false, composing = false, stopRequested = false;
    int roundsLeft = 0;
    juce::String connectionName;
    juce::var composeRequest; // the AI writer's request, for its revision rounds
    std::map<juce::String, std::map<juce::String, parts::Choice>> candidateChoices; // part -> sound id -> choice
    std::map<juce::String, parts::Choice> layerChoices;                             // for `song`
    std::map<juce::String, double> faders; // for `song`: its layers' starting faders (balance)
    std::unique_ptr<BackgroundTask> task;
    bool layerChoicesKnown = false;
    juce::TextButton writeButton { "Write" }, againButton { "Another version" }, redoButton { "Redo part..." },
        stopButton { "Stop" }, applyButton { "Apply" }, mixButton { "Mix it..." };
    juce::TextEditor text;
};

} // namespace spinus::ui
