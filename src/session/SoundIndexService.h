#pragma once

#include "session/SoundIndex.h"

#include <deque>

namespace spinus
{

/** The sound index for all of Spinus (Session::getSoundIndex()): where it is kept, which plugins it measures,
    and its updates, one at a time in the background. Options › Sound index shows and edits it, the Browser's
    Refresh sound index measures one instrument or some samples again, and the Track creator reads it.

    Spinus's own instruments and the downloaded ones are always measured; the user's other plugins only when
    ticked (`plugins.json` beside the index: `{include: [keys]}`), so an update loads none of them unasked.

    A request made while an update runs waits for it, then runs. Listeners hear of every change (progress,
    the end, a request queued). Message thread. */
class SoundIndexService final : public juce::ChangeBroadcaster
{
public:
    explicit SoundIndexService (const plugins::DeviceList&);
    ~SoundIndexService() override;

    /** Where the index is kept: SoundIndex::getDefaultFolder(), or another (tests). */
    juce::File getFolder() const { return folder; }
    void setFolder (const juce::File&);
    /** How the helper runs: Spinus's spinus-plugin-scanner, with the computer's libraries in the Flatpak. */
    const SoundIndexer::Options& getOptions() const noexcept { return options; }
    void setOptions (SoundIndexer::Options o) { options = std::move (o); }

    /** The Browser's sample folders, which an update measures (set by the main window). */
    std::function<juce::Array<juce::File>()> getSampleFolders;
    /** Every instrument Spinus could measure: the Browser's (findSoundSources) unless set (tests). */
    std::function<std::vector<SoundSource>()> getSources;

    /** Every instrument it could measure, and those it does: Spinus's own, the downloaded ones and the ticked. */
    std::vector<SoundSource> getAllSources() const;
    std::vector<SoundSource> getIndexedSources() const;
    juce::StringArray getIncludedPlugins() const;
    void setIncludedPlugins (const juce::StringArray& keys);
    /** Ticks or unticks one of the user's plugins. */
    void setIncluded (const juce::String& key, bool include);
    static bool isAlwaysMeasured (const SoundSource& s) { return s.stockType.isNotEmpty(); }

    /** Brings the whole index up to date: only what is new or changed is measured. */
    void update();
    /** One instrument measured again from the start (ticked first when it is one of the user's plugins). False
        when it is not an instrument Spinus could measure. */
    bool refreshInstrument (const juce::String& key);
    /** These sample files, and those in these folders, measured again. */
    void refreshSamples (const juce::Array<juce::File>& filesOrFolders);
    /** Measured again at the next update: one instrument, or (empty key) every silent or failed one. Returns how
        many, and starts the update. */
    int measureAgain (const juce::String& key);
    /** One instrument's sounds leave the index (and it is unticked, so it is not measured again). */
    bool forget (const juce::String& key);
    /** Deletes the whole index (not the saved sounds, roles or preset folders kept beside it). False while it
        updates. */
    bool deleteIndex();

    /** Stops the update that runs, and drops the waiting requests. */
    void cancel();
    bool isUpdating() const noexcept { return indexer != nullptr; }
    int getWaiting() const noexcept { return (int)waiting.size(); }
    SoundIndexer::Progress getProgress() const;
    const std::optional<SoundIndexer::Result>& getLastResult() const noexcept { return lastResult; }

    /** One line on how it is: its size and problems, or how far an update is. */
    juce::String getStatusText() const;

private:
    struct Request
    {
        juce::String what; // for the status line
        SoundIndexer::Job job;
    };
    void enqueue (Request);
    void startNext();

    const plugins::DeviceList& devices;
    juce::File folder = SoundIndex::getDefaultFolder();
    SoundIndexer::Options options;
    std::unique_ptr<SoundIndexer> indexer;
    juce::String running; // what the running one does
    std::deque<Request> waiting;
    SoundIndexer::Progress progress;
    std::optional<SoundIndexer::Result> lastResult;
    juce::String lastText; // what the last one did, said before the summary
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE (SoundIndexService)
};

} // namespace spinus
