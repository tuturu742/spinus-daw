#pragma once

#include "MessageLabel.h"
#include "ai/AiService.h"
#include "session/AnalysisLibrary.h"
#include "session/PartSounds.h"
#include "session/Session.h"

#include <memory>
#include <vector>

namespace spinus::ui
{

/** The Track creator's sounds per part (experimental, milestone 3), shown in its panel: for each part of the
    style summary, the closest sounds of the sound index, with the reasons, to hear, swap and add to the
    project.

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
        juce::File indexFolder, workFolder;
        SoundIndexer::Options helper;
        ai::AiService* service = nullptr;
    };

    PartSoundsPanel (Session&, std::function<Context()> context);
    ~PartSoundsPanel() override;

    struct Row
    {
        parts::Target target;
        std::vector<parts::Choice> choices;
        int selected = 0;
        std::optional<SynthMatchResult> tuned;
        juce::String note;              // what happened to it (tuning, CLAP...)
        juce::Array<juce::var> effects; // mixplan items for its insert
        double volumeDb = 0.0;
        // The reference: the song whose stem has the most of the part, and the part of it to play.
        juce::String songDigest, songName;
        juce::File stem;  // the stem's FLAC file in the library
        Excerpt excerpt;  // its busiest few seconds, with the notes
        int measured = 0; // notes (or hits) measured over all songs
        const parts::Choice* chosen() const { return choices.empty() ? nullptr : &choices[(size_t)selected]; }
    };

    //==============================================================================
    // For the buttons and for tests. Results arrive asynchronously.
    bool chooseSounds();
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
    void setTune (bool on) { tuneButton.setToggleState (on, juce::dontSendNotification); }
    void setEffects (bool on) { effectsButton.setToggleState (on, juce::dontSendNotification); }
    void setTuneTries (int tries) { tuneTries = tries; }

    const std::vector<Row>& getRows() const noexcept { return rows; }
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
    juce::ToggleButton closerButton { "Listen closer (AI)" }, tuneButton { "Tune the synths (slow)" },
        effectsButton { "Effects and mix (AI)" };
    juce::TextButton chooseButton { "Choose sounds" }, stopButton { "Stop" },
        effectsAgainButton { "Set effects again" }, addButton { "Add these sounds to the project" };
    juce::Viewport viewport;
    juce::Component list;
    std::vector<std::unique_ptr<RowView>> views;
};

} // namespace spinus::ui
