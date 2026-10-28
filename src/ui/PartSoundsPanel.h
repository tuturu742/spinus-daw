#pragma once

#include "MessageLabel.h"
#include "ai/AiService.h"
#include "session/AnalysisLibrary.h"
#include "session/PartSounds.h"
#include "session/Session.h"

#include <memory>
#include <optional>
#include <vector>

namespace spinus::ui
{

/** The Track creator's sounds per part (experimental, milestone 3), shown in its panel: the song's parts (the
    user's list: Add part, remove, a role each; the Write page's arrangement proposes the first list, and it
    starts again on a new description or summary), each with the closest sounds of the sound index and the
    reasons, to hear, swap and add to the project. The song writer writes exactly these parts with these sounds.

    Choose sounds:
    1. measures the reference songs' stems (kept in the analysis library) over their notes and drum hits, and
       picks the closest sounds of the index for each part (session/PartSounds); needs no AI;
    2. with "Listen closer" and AI on: the first five presets of each part go on to closest preset's CLAP
       embeddings (their previews played in the sound helper, ai.v1.presets.add and .match), which reorder them;
    3. with "Tune the synths": synth match of the chosen preset toward the part's stem, in the sound helper
       (a separate process: a synth that crashes there takes only the helper down; slow, can be stopped);
    4. with "Effects and mix" and AI on: each chosen sound plays the part's notes (in the helper), the AI
       service measures it and the stems (ai.v1.mix.analyze) and sets an EQ, a compressor and its level
       toward the songs, and a limiter on the master (ai.v1.mix.toward).

    Listen plays the chosen sound playing a short phrase (rendered in the helper), The songs' plays the part's
    stem there. Add these sounds to the project makes it one proposal through commands: one undo step.

    Without AI, or with the AI service missing, the sounds are still chosen and can be added; the panel says
    what the AI would add. Message thread only; what takes time runs on threads of its own. */
class PartSoundsPanel final : public juce::Component
{
public:
    /** What the Track creator knows: the summary, the analyzed songs, where things are. */
    struct Context
    {
        juce::var summary;
        struct Song
        {
            juce::String digest, name;
        };
        std::vector<Song> songs;
        AnalysisLibrary library { juce::File() };
        std::vector<SoundSource> sources; // every instrument the Browser lists (Other sound, From an instrument)
        juce::File indexFolder, workFolder;
        SoundIndexer::Options helper;
        ai::AiService* service = nullptr;
    };

    PartSoundsPanel (Session&, std::function<Context()> context);
    ~PartSoundsPanel() override;

    struct Row
    {
        parts::SongPart part; // the user's part: its name and role
        parts::Target target; // what it needs (parts::targetFor)
        std::vector<parts::Choice> choices;
        int selected = 0;
        std::optional<SynthMatchResult> tuned;
        juce::String note;              // what happened to it (tuning, CLAP...)
        juce::Array<juce::var> effects; // mixplan items for its insert
        double volumeDb = 0.0;
        // The reference: the song whose stem has the most of the part, and the part of it to play.
        juce::String songDigest, songName;
        juce::File stem;                     // the stem's FLAC file in the library
        Excerpt excerpt;                     // its busiest few seconds, with the notes
        int measured = 0;                    // notes (or hits) measured over all songs
        std::optional<parts::Choice> picked; // the user's own (Other sound...): kept first, and it wins
        bool tune = false;                   // tuned toward the references' part (shown only with references)
        const parts::Choice* chosen() const { return choices.empty() ? nullptr : &choices[(size_t)selected]; }
    };

    //==============================================================================
    // For the buttons and for tests. Results arrive asynchronously.
    bool chooseSounds();

    /** The song's parts (the user's list, which the song writer writes exactly). */
    std::vector<parts::SongPart> getParts() const;
    /** Starts the list again (a new description or summary), saying `why`: the parts a description names, or
        none; the sounds chosen go with it. */
    void resetParts (const juce::String& why = {});
    /** Fills the list with suggested parts: "genre" (the described genre's), "sketch" or "full". */
    bool suggestParts (const juce::String& which);
    void setParts (const std::vector<parts::SongPart>&);
    bool addPart (const juce::String& role);
    /** A part from an instrument (its role from its name), playing it. */
    bool addPartFrom (const parts::Choice&);
    /** The user's own sound for a part, any instrument and preset (Other sound...): it wins over suggestions. */
    bool pickSound (int row, const parts::Choice&);
    /** Gives an instrument the user's own role (empty: forgets it), kept beside the sound index. */
    bool markInstrument (const juce::String& instrument, const juce::String& role);
    /** Every instrument with its presets (or "Default sound"), for picking, and Mark it as. */
    void fillInstrumentMenu (juce::PopupMenu&, const std::function<void (const parts::Choice&)>& picked);
    bool removePart (int row);
    bool setRole (int row, const juce::String& role);
    void stop();
    bool isBusy() const noexcept { return busy; }
    /** Picks another of a part's choices (its tuning and effects are then dropped: Set effects again). */
    bool select (int row, int choice);
    /** Effects and mix again, for the sounds chosen now (AI on). */
    bool setEffectsAgain();
    bool listen (int row);
    bool listenToSongs (int row);
    /** The chosen sounds into the project: one proposal, one undo step. */
    bool addToProject();

    void setListenCloser (bool on) { closerButton.setToggleState (on, juce::dontSendNotification); }
    /** Whether parts are tuned toward the references when sounds are chosen (each part's own toggle, "Tune
        toward the references", shown only with references; this sets it for every part, for tests). */
    void setTune (bool on) { tuneByDefault = on; }
    /** Tunes one part toward the references' part now (on), or not when sounds are chosen (off). */
    bool setTuneFor (int row, bool on);
    void setEffects (bool on) { effectsButton.setToggleState (on, juce::dontSendNotification); }
    void setTuneTries (int tries) { tuneTries = tries; }

    const std::vector<Row>& getRows() const noexcept { return rows; }
    /** Whether there are reference songs (their part's button, The songs', shows only then). */
    bool hasReferences() const noexcept { return ! context.songs.empty(); }
    juce::String getStatusText() const { return status.getText(); }
    /** A part's line as the panel shows it: its name, the chosen sound, the reasons. */
    juce::String describe (int row) const;
    /** The plan Add would add (tests). */
    parts::Plan makePlan() const;
    /** The channels the last Add made, one per part (none for a loop). */
    const std::vector<model::ChannelId>& getAddedChannels() const noexcept { return added; }
    /** The last file Listen or The songs' played. */
    juce::File getLastPlayed() const { return lastPlayed; }

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class RowView;
    struct Measuring;

    void measured (const std::shared_ptr<Measuring>&);
    void listenCloser (size_t part);
    void tuneNext (size_t part);
    void setEffectsFor();
    void finish (const juce::String& message);
    void fail (const juce::String& message);
    void runInBackground (const juce::String& words, std::function<juce::String (const std::function<bool()>&)> work,
                          std::function<void (const juce::String& problem)> then);
    bool aiReady() const;
    juce::File work (const juce::String& name) const;
    juce::File stemWav (const Row&) const;
    juce::var renderItem (const Row&, const parts::Choice&, const std::vector<OfflineInstrument::Note>& notes,
                          bool tuned, const juce::File& output) const;
    void updateRows();
    void updateButtons();
    void showAddMenu();
    void showSuggestMenu();
    void showOtherMenu (int row);
    /** The role of a sound: the user's mark, else what its name says. */
    juce::String roleOf (const parts::Choice&) const;

    Session& session;
    std::function<Context()> getContext;
    Context context;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    std::shared_ptr<std::atomic<bool>> stopping = std::make_shared<std::atomic<bool>> (false);
    std::unique_ptr<BackgroundTask> task;
    bool busy = false;
    int tuneTries = 300;
    juce::String job;
    std::vector<Row> rows;
    juce::Array<juce::var> master; // mixplan items for the master
    std::vector<model::ChannelId> added;
    juce::File lastPlayed;
    juce::StringArray notes; // said with the result

    juce::Label intro;
    MessageLabel status;
    juce::ToggleButton closerButton { "Listen closer (AI)" }, effectsButton { "Effects and mix (AI)" };
    bool tuneByDefault = false, tuningOne = false;
    int chooseCount = 0;
    juce::TextButton chooseButton { "Choose sounds" }, addPartButton { "Add part..." },
        suggestButton { "Suggest parts..." }, stopButton { "Stop" }, effectsAgainButton { "Set effects again" },
        addButton { "Add these sounds to the project" };
    juce::Viewport viewport;
    juce::Component list;
    std::vector<std::unique_ptr<RowView>> views;
};

} // namespace spinus::ui
