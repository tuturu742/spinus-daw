#pragma once

#include "core/FileDialog.h"
#include "session/EffectProfiles.h"
#include "session/Session.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ai
{
class AiService;
}

namespace spinus::ui
{

/** Learn this effect (an effect slot's menu in the Mixer): every setting of the effect as the plugin reports it
    (its name and the values at the ends of its range, read without AI), and what the user knows of each: what it
    does, a safe range and whether it is one of the few that matter. With a line for what the effect is good for,
    a search over the settings (plugins can have hundreds), and a profile exported or imported as a file. Read the
    manual has the AI read the effect's manuals (PDF files, web pages; ai.v1.effect.readManual) and propose what
    they say, shown for the user to Apply or Discard; what the user wrote stays. The assistant and outside agents
    read what is saved. Nothing is kept until Save. */
class LearnEffectPanel final : public juce::Component, private juce::TableListBoxModel
{
public:
    LearnEffectPanel (const juce::String& effectName, std::vector<Session::ParameterText> settings,
                      std::optional<EffectProfiles::Profile> profile);
    ~LearnEffectPanel() override;

    std::function<void (std::optional<EffectProfiles::Profile>)> onSave; // nullopt: Forget this effect
    std::function<void()> onCancel, onExport, onImport, onRead, onChooseManual;

    /** What is set now, as a profile (settings with nothing learned left out). */
    EffectProfiles::Profile getProfile() const;
    juce::var getShared() const { return EffectProfiles::toShared (getProfile(), name); }
    /** A shared profile read in: what it knows of settings the user has not set fills them; its "good for" fills
        an empty one. Returns what it says to the user. */
    juce::String importShared (const juce::var&, const juce::String& fileName);

    /** Manuals (Read the manual): a file's full path or a web page's address; kept with the profile. */
    void addManual (const juce::String& source);
    void removeManual (int index);
    const std::vector<EffectProfiles::Manual>& getManuals() const noexcept { return manuals; }
    /** Whether Read the manual can run: empty, or why not, shown on the greyed button. */
    void setCanRead (const juce::String& whyNot);
    /** While the AI reads: what it is doing (empty: done). */
    void setReadingProgress (const juce::String& progress);
    /** What the AI read in the manuals (none: `problem` says why), shown as a proposal for Apply or Discard. */
    void setReading (std::optional<EffectProfiles::Reading>, const juce::String& problem = {});
    bool hasReading() const noexcept { return reading.has_value(); }
    /** Takes the reading into the settings the user has not set (and an empty "good for"); returns how many
        settings it filled. */
    int applyReading();
    void discardReading();

    /** For tests and the window: the settings as rows, the search, and one row's fields. */
    int getNumShown() const { return (int)shown.size(); }
    void search (const juce::String& text);
    void setWhat (int row, const juce::String& what);
    void setRange (int row, const juce::String& low, const juce::String& high);
    void setMatters (int row, bool matters);
    void setGoodFor (const juce::String& text) { goodFor.setText (text, false); }
    juce::String getRowText (int row) const;
    juce::String getMessage() const { return message.getText(); }

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    enum Column
    {
        settingColumn = 1,
        rangeColumn,
        whatColumn,
        lowColumn,
        highColumn,
        mattersColumn
    };
    struct Row
    {
        Session::ParameterText plugin;
        EffectProfiles::Setting learned;
    };
    class TextCell;

    int getNumRows() override { return (int)shown.size(); }
    void paintRowBackground (juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    juce::Component* refreshComponentForCell (int row, int column, bool selected, juce::Component* existing) override;
    juce::String getCellTooltip (int row, int column) override;
    void cellClicked (int row, int column, const juce::MouseEvent&) override;
    Row* rowAt (int shownRow);
    void edited (int shownRow, int column, const juce::String& text);
    /** Fills the settings the user has not set, and an empty "good for"; returns how many settings it filled. */
    int fill (const std::vector<EffectProfiles::Setting>& settings, const juce::String& goodForText);
    int countFills (const std::vector<EffectProfiles::Setting>& settings) const;
    void updateManuals();

    juce::String name;
    std::vector<Row> rows;
    std::vector<int> shown; // indices into rows
    std::vector<EffectProfiles::Manual> manuals;
    std::optional<EffectProfiles::Reading> reading;
    juce::String readWhyNot;
    bool readingNow = false;
    juce::Label intro, goodForLabel, manualsLabel, message;
    juce::TextEditor goodFor, searchBox, manualEditor;
    juce::ComboBox manualsBox;
    juce::TextButton removeManualButton { "Remove" }, chooseManualButton { "Choose file..." },
        addManualButton { "Add" }, readButton { "Read the manual" }, applyButton { "Apply" },
        discardButton { "Discard" };
    juce::TableListBox table;
    juce::TextButton saveButton { "Save" }, cancelButton { "Cancel" }, forgetButton { "Forget this effect" },
        exportButton { "Export..." }, importButton { "Import..." };
};

/** The panel in a window of its own, one per effect: Save keeps the profile beside the sound index. */
class LearnEffectWindow final : public juce::DocumentWindow
{
public:
    /** Opens (or brings to the front) the window for `effect` (its key, InstrumentPresets::libraryOf), with the
        effect's settings as the plugin reports them now. */
    static void open (const juce::File& indexFolder, const juce::String& effect, const juce::String& name,
                      std::vector<Session::ParameterText> settings, juce::Component* near,
                      ai::AiService* service = nullptr);
    static int getNumOpen();
    static void closeAll();
    /** For tests: the panel of the window opened last; null when none is open. */
    static LearnEffectPanel* lastOpened();

    ~LearnEffectWindow() override;
    void closeButtonPressed() override;

private:
    LearnEffectWindow (const juce::File& indexFolder, const juce::String& effect, const juce::String& name,
                       std::vector<Session::ParameterText> settings, ai::AiService* service);
    void finish();
    void readManuals();
    void chooseManual();
    void exportProfile();
    void importProfile();

    juce::File indexFolder;
    juce::String effect, name;
    std::vector<Session::ParameterText> settings; // as the plugin reported them, for the AI
    ai::AiService* service = nullptr;
    std::unique_ptr<core::FileDialog> chooser;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
};

} // namespace spinus::ui
