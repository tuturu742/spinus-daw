#pragma once

#include "core/FileDialog.h"
#include "session/EffectProfiles.h"
#include "session/Session.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** Learn this effect (an effect slot's menu in the Mixer): every setting of the effect as the plugin reports it
    (its name and the values at the ends of its range, read without AI), and what the user knows of each: what it
    does, a safe range and whether it is one of the few that matter. With a line for what the effect is good for,
    a search over the settings (plugins can have hundreds), and a profile exported or imported as a file. The
    assistant and outside agents read what is saved. Nothing is kept until Save. */
class LearnEffectPanel final : public juce::Component, private juce::TableListBoxModel
{
public:
    LearnEffectPanel (const juce::String& effectName, std::vector<Session::ParameterText> settings,
                      std::optional<EffectProfiles::Profile> profile);
    ~LearnEffectPanel() override;

    std::function<void (std::optional<EffectProfiles::Profile>)> onSave; // nullopt: Forget this effect
    std::function<void()> onCancel, onExport, onImport;

    /** What is set now, as a profile (settings with nothing learned left out). */
    EffectProfiles::Profile getProfile() const;
    juce::var getShared() const { return EffectProfiles::toShared (getProfile(), name); }
    /** A shared profile read in: what it knows of settings the user has not set fills them; its "good for" fills
        an empty one. Returns what it says to the user. */
    juce::String importShared (const juce::var&, const juce::String& fileName);

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

    juce::String name;
    std::vector<Row> rows;
    std::vector<int> shown; // indices into rows
    juce::Label intro, goodForLabel, message;
    juce::TextEditor goodFor, searchBox;
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
                      std::vector<Session::ParameterText> settings, juce::Component* near);
    static int getNumOpen();
    static void closeAll();
    /** For tests: the panel of the window opened last; null when none is open. */
    static LearnEffectPanel* lastOpened();

    ~LearnEffectWindow() override;
    void closeButtonPressed() override;

private:
    LearnEffectWindow (const juce::File& indexFolder, const juce::String& effect, const juce::String& name,
                       std::vector<Session::ParameterText> settings);
    void finish();
    void exportProfile();
    void importProfile();

    juce::File indexFolder;
    juce::String effect, name;
    std::unique_ptr<core::FileDialog> chooser;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
};

} // namespace spinus::ui
