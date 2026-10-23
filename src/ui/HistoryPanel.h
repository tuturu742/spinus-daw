#pragma once

#include "Workspace.h"
#include "session/Session.h"

namespace spinus::ui
{

/** The History panel (Ctrl+H): every undo step by description, oldest first, then the steps
    that can be redone (dimmed). Clicking a step undoes or redoes everything up to it. The first row is
    the project as it was opened.
*/
class HistoryPanel final : public Panel, private juce::ListBoxModel, private juce::AsyncUpdater
{
public:
    explicit HistoryPanel (Session&);
    ~HistoryPanel() override;

    /** Rows: "Open project", then undo steps, then redo steps. */
    int getNumRows() override;
    juce::String getRowText (int row) const;

    /** The row of the current state (0 = nothing done). */
    int getCurrentRow() const;

    /** Undoes or redoes until `row` is the current state. */
    void goTo (int row);

    void updateNow() { handleUpdateNowIfNeeded(); }

    juce::Rectangle<int> getDefaultBounds() const override { return { 700, 60, 280, 360 }; }
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override { goTo (row); }
    void handleAsyncUpdate() override;

    Session& session;
    int historyListener = 0;
    juce::StringArray undoSteps, redoSteps;
    juce::ListBox list;
};

} // namespace spinus::ui
