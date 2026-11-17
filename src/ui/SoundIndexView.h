#pragma once

#include "session/LeftOutSounds.h"
#include "session/SoundIndex.h"
#include "session/UserRoles.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** The sound index's sounds, to check what Spinus made of them (the Sounds tab of Options › Sound index): every
    measured preset and sample with where it comes from, its role guess (or a sample's class) and a few of
    its measurements, with a search over all of that ("bass", "pad", "loop"...). Right-click a preset to give it a
    role of your own ("Use as rhythm guitar"), kept beside the index (UserRoles), which the Track creator uses
    before its own guesses. */
class SoundIndexView final : public juce::Component, private juce::ListBoxModel
{
public:
    explicit SoundIndexView (const juce::File& folder);
    ~SoundIndexView() override;

    /** Reads the index again (after an update), keeping the search. */
    void reload();
    void setSearchText (const juce::String& text) { searchBox.setText (text, true); }
    juce::String getSearchText() const { return searchBox.getText(); }

    /** Shows only the sounds whose name, source, role or details contain every word of `text`, and that the
        filter lets through. */
    void search (const juce::String& text);

    /** What the Show menu lets through. */
    enum class Show
    {
        everything,
        presets,
        samples,
        problems,
        leftOut // the sounds the user left out (LeftOutSounds), to put back
    };
    /** The Show and From menus: `from` is an instrument's name, a sample folder (its samples and the folders
        inside it), or empty for all of them. */
    void setFilter (Show, const juce::String& from = {});
    Show getShow() const noexcept { return show; }
    juce::String getFrom() const { return from; }
    /** The sample folders the From menu offers (the Browser's). */
    void setSampleFolders (const juce::Array<juce::File>& folders);
    /** The Role menu: only sounds of this role (or a sample's class); empty for any. */
    void setRole (const juce::String& role);
    juce::String getRole() const { return roleFilter; }

    /** The selected rows (several with Ctrl or Shift), in order. */
    std::vector<SoundIndex::Row> getSelected() const;
    void selectRows (const juce::SparseSet<int>& rows) { list.setSelectedRows (rows); }
    /** Leave out / put back (the selected rows, or every shown one), and listen: what the page does with them. */
    std::function<void (const std::vector<SoundIndex::Row>&)> onLeaveOut, onPutBack;
    std::function<void (const SoundIndex::Row&)> onListen;
    /** A line under the list (what Listen or Leave out did). */
    void setMessage (const juce::String& text) { message.setText (text, juce::dontSendNotification); }
    juce::String getMessage() const { return message.getText(); }
    /** Leaves out (or, in Left out, puts back) the selected rows, or all shown ones (asking first when several). */
    void actOnSelected();
    void actOnAllShown();
    const std::vector<SoundIndex::Row>& getShown() const noexcept { return shown; }
    /** Gives a shown preset the user's own role (empty: Spinus's guess again); false for a sample. */
    bool mark (int row, const juce::String& role);
    /** The user's role of a shown row, or empty. */
    juce::String markOf (int row) const;
    juce::String getCountText() const { return count.getText(); }
    /** The text of a row. */
    static juce::String describe (const SoundIndex::Row&);

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    int getNumRows() override { return (int)shown.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    juce::String getTooltipForRow (int row) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    /** A preset row's instrument key and preset id (from the index), or empty. */
    std::pair<juce::String, juce::String> idsOf (const SoundIndex::Row&) const;
    bool passes (const SoundIndex::Row&) const;
    void fillRoleMenu();
    void selectedRowsChanged (int) override { updateButtons(); }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void updateButtons();
    /** Asks first when there are several, then calls onLeaveOut or onPutBack. */
    void act (const std::vector<SoundIndex::Row>&);
    /** The From menu's items again: every instrument in the index, then the sample folders. */
    void fillFromMenu();

    juce::File folder;
    std::vector<SoundIndex::Row> all, shown;
    std::map<juce::String, std::pair<juce::String, juce::String>> ids; // source + name -> instrument, preset
    UserRoles marked;
    juce::Label intro, count;
    juce::TextEditor searchBox;
    juce::ComboBox showBox, fromBox, roleBox;
    juce::String roleFilter;
    std::vector<SoundIndex::Row> leftOutRows;
    juce::Label message;
    juce::TextButton listenButton { "Listen" }, actButton { "Leave out" }, allButton { "Leave out all shown" };
    Show show = Show::everything;
    juce::String from;
    juce::StringArray fromItems; // the From menu's values after "Everything", in order
    juce::Array<juce::File> sampleFolders;
    juce::ListBox list;
};

} // namespace spinus::ui
