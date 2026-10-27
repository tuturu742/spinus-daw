#pragma once

#include "MessageLabel.h"
#include "ai/AiService.h"
#include "engine/Exporter.h"
#include "session/PartSounds.h"
#include "session/Session.h"
#include "session/SongWriter.h"

#include <memory>

namespace spinus::ui
{

/** The Track creator's new song (experimental, milestone 4), shown in its panel: a song written from the style
    summary, to read about, listen to and apply.

    Write asks the AI service (ai.v1.song.write, spinus_ai/songwriter.py) for a song: rules and seeded chance on
    this computer, no AI connection needed; with "Let the AI vary the plan" and an AI connection, the AI may vary
    the sections, chord loops and melody shapes (checked against the style; the rules write every note).
    Another version writes again with a new seed; Redo part writes one part or one section again, keeping the
    rest. Listen renders the project as it would be with the song (the proposal tried on a copy) and plays it;
    Apply puts it into the project through commands as one undo step (session/SongWriter), or, once a song is
    in, only the parts or sections written again. Mix it opens the automixer toward the reference songs.

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
        juce::File workFolder;         // for the preview's rendering
        juce::Array<juce::File> songs; // the reference songs (the automixer's reference)
        parts::Plan sounds;            // the sounds chosen on the Sounds page (none: Spinus's own)
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
    bool listen();
    void stop();
    /** Puts the song (or what was written again) into the project: one undo step. */
    bool apply();
    /** Opens the automixer with the first reference song as its reference (onMix). */
    bool mix();

    std::function<void (const juce::File& reference)> onMix;

    void setRefine (bool on) { refineButton.setToggleState (on, juce::dontSendNotification); }
    void setSeed (int s) { fixedSeed = s; }

    bool isBusy() const noexcept { return busy; }
    const juce::var& getSong() const noexcept { return song; }
    juce::String getStatusText() const { return status.getText(); }
    juce::String getText() const { return text.getText(); }
    /** What the last Apply made (none before it). */
    const song::Applied& getApplied() const noexcept { return applied; }
    /** What Apply would do now: the whole song, or only what was written again; void when nothing. */
    juce::var makeProposal (juce::String* error = nullptr) const;
    juce::File getLastPreview() const { return lastPreview; }

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void run (const juce::var& params, const juce::String& words, std::function<void (const juce::var&)> then);
    void show();
    void updateButtons();
    void showRedoMenu();
    bool aiReady() const;
    song::Options optionsFor (const model::Project&) const;

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
    std::unique_ptr<engine::ExportJob> rendering;
    juce::File lastPreview;

    juce::Label intro;
    MessageLabel status;
    juce::ToggleButton refineButton { "Let the AI vary the plan (uses your AI connection)" };
    juce::TextButton writeButton { "Write" }, againButton { "Another version" }, redoButton { "Redo part..." },
        listenButton { "Listen" }, stopButton { "Stop" }, applyButton { "Apply" }, mixButton { "Mix it..." };
    juce::TextEditor text;
};

} // namespace spinus::ui
