#pragma once

#include "SoundIndexView.h"
#include "session/SoundIndexService.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ai
{
class AiService;
}

namespace spinus::ui
{

/** Options › Sound index: the whole sound index in one place. Its state and Update / Stop at the top, then:
    - **Instruments**: every instrument Spinus could measure, ticked when the index measures it (Spinus's own
      and the downloaded ones always are; the user's plugins when ticked), with what was measured and what went
      wrong; Measure again, Forget its sounds, Show its sounds, Learn this instrument. One of the user's plugins
      that is not learned says so: Spinus's test notes may land on keys that switch it or play effects (it also
      says when its sounds suggest such keys); a learned one is measured on the keys it plays.
    - **Sounds**: every measured preset and sample, with search, problems and the user's own roles
      (SoundIndexView).
    - **Samples**: the Browser's sample folders with how many of their files are measured; Measure again.
    Everything goes through the session's SoundIndexService, so the Track creator and the Browser see the same
    index and the same update. */
class SoundIndexPage final : public juce::Component, private juce::ChangeListener
{
public:
    explicit SoundIndexPage (SoundIndexService&);
    ~SoundIndexPage() override;

    struct InstrumentRow
    {
        SoundSource source;
        bool always = false, measured = false; // measured: ticked, or always
        bool installed = true;                 // false: in the index, but no longer installed
        juce::String state;                    // what was measured, or why not
        bool problem = false;
        bool learned = false; // it has a profile (Learn this instrument), the user's or one Spinus ships
        bool hint = false;    // not learned, and its sounds suggest keys that switch it or play effects
    };
    const std::vector<InstrumentRow>& getInstruments() const noexcept { return instruments; }
    /** Ticks or unticks one of the user's plugins (Spinus's own and the downloaded ones stay ticked). */
    void toggle (int row);
    void measureAgain (int row);
    void forget (int row);
    /** Shows the instrument's sounds in the Sounds tab. */
    void showSoundsOf (int row);
    /** Opens Learn this instrument for it (its keys, from the user, its manual or listening to them); once
        saved, the next update measures it again on the keys it plays. */
    void learn (int row);
    /** For Learn this instrument's Read the manual. */
    void setAiService (ai::AiService* service) noexcept { aiService = service; }

    struct FolderRow
    {
        juce::File folder;
        int samples = 0, problems = 0;
    };
    const std::vector<FolderRow>& getFolders() const noexcept { return folders; }
    void measureFolderAgain (int row);
    /** Shows the folder's samples (or only those that went wrong) in the Sounds tab. */
    void showSamplesOf (int row, bool problemsOnly);

    /** Deletes the index (after the user confirmed it). */
    bool deleteIndex();
    juce::String getStatusText() const { return status.getText(); }
    SoundIndexView& getSounds() noexcept { return *sounds; }
    juce::TabbedComponent& getTabs() noexcept { return tabs; }

    /** Reads the index and the lists again. */
    void reload();

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class InstrumentList;
    class FolderList;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void updateButtons();
    void confirmDelete();

    SoundIndexService& index;
    ai::AiService* aiService = nullptr;
    std::vector<InstrumentRow> instruments;
    std::vector<FolderRow> folders;
    juce::Label status;
    juce::TextButton updateButton { "Update sound index" }, stopButton { "Stop" },
        againButton { "Measure again every silent or failed one" }, deleteButton { "Delete the index..." };
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    std::unique_ptr<InstrumentList> instrumentList;
    std::unique_ptr<SoundIndexView> sounds;
    std::unique_ptr<FolderList> folderList;
};

/** The page in a window of its own (one at a time). */
class SoundIndexWindow final : public juce::DocumentWindow
{
public:
    /** Opens it (or brings it to the front) beside `near`. */
    static void show (SoundIndexService&, juce::Component* near, ai::AiService* = nullptr);
    /** The open one's page (tests); null when it is closed. */
    static SoundIndexPage* getOpenPage();
    static void close();

    ~SoundIndexWindow() override;
    void closeButtonPressed() override;

private:
    explicit SoundIndexWindow (SoundIndexService&);
};

} // namespace spinus::ui
