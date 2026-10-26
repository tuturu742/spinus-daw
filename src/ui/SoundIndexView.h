#pragma once

#include "session/SoundIndex.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** The sound index's sounds, to check what Spinus made of them ("Show sounds...", in the Track creator): every
    measured preset and sample with where it comes from, its role guess (or a sample's class) and a few of
    its measurements, with a search over all of that ("bass", "Surge", "loop"...). Read only. */
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
    juce::String getCountText() const { return count.getText(); }
    /** The text of a row. */
    static juce::String describe (const SoundIndex::Row&);

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    int getNumRows() override { return (int)shown.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    juce::String getTooltipForRow (int row) override;

    std::vector<SoundIndex::Row> all, shown;
    juce::Label intro, count;
    juce::TextEditor searchBox;
    juce::ListBox list;
};

} // namespace spinus::ui
