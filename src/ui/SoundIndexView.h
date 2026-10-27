#pragma once

#include "session/SoundIndex.h"
#include "session/UserRoles.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** The sound index's sounds, to check what Spinus made of them ("Show sounds...", in the Track creator): every
    measured preset and sample with where it comes from, its role guess (or a sample's class) and a few of
    its measurements, with a search over all of that ("bass", "pad", "loop"...). Right-click a preset to give it a
    role of your own ("Use as rhythm guitar"), kept beside the index (UserRoles), which the Track creator uses
    before its own guesses. */
class SoundIndexView final : public juce::Component, private juce::ListBoxModel
{
public:
    explicit SoundIndexView (const juce::File& folder);
    ~SoundIndexView() override;

    /** Opens it in a window of its own beside `near`. */
    static void show (const juce::File& folder, juce::Component* near);

    /** Shows only the sounds whose name, source, role or details contain every word of `text`. */
    void search (const juce::String& text);
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

    std::vector<SoundIndex::Row> all, shown;
    std::map<juce::String, std::pair<juce::String, juce::String>> ids; // source + name -> instrument, preset
    UserRoles marked;
    juce::Label intro, count;
    juce::TextEditor searchBox;
    juce::ListBox list;
};

} // namespace spinus::ui
