#include "HistoryPanel.h"

#include "Theme.h"

namespace spinus::ui
{

namespace
{
const Theme& themeOf (const juce::Component& c)
{
    static const Theme fallback = Theme::dark();
    auto* lf = dynamic_cast<SpinusLookAndFeel*> (&c.getLookAndFeel());
    return lf != nullptr ? lf->getTheme() : fallback;
}
} // namespace

HistoryPanel::HistoryPanel (Session& s) : Panel ("history", "History"), session (s)
{
    setHelpText ("History (Ctrl+H): click a step to go back to it; dimmed steps can be redone");
    list.setModel (this);
    list.setRowHeight (22);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (list);

    historyListener = session.getHistory().addListener ([this] (const commands::ChangeSet&) { triggerAsyncUpdate(); });
    handleAsyncUpdate();
}

HistoryPanel::~HistoryPanel()
{
    cancelPendingUpdate();
    session.getHistory().removeListener (historyListener);
}

void HistoryPanel::handleAsyncUpdate()
{
    undoSteps = session.getHistory().getUndoDescriptions();
    redoSteps = session.getHistory().getRedoDescriptions();
    list.updateContent();
    list.scrollToEnsureRowIsOnscreen (getCurrentRow());
    list.repaint();
}

int HistoryPanel::getNumRows() { return 1 + undoSteps.size() + redoSteps.size(); }

int HistoryPanel::getCurrentRow() const { return undoSteps.size(); }

juce::String HistoryPanel::getRowText (int row) const
{
    if (row == 0)
        return "Open project";
    if (row <= undoSteps.size())
        return undoSteps[row - 1];
    return redoSteps[row - 1 - undoSteps.size()];
}

void HistoryPanel::goTo (int row)
{
    auto& history = session.getHistory();
    row = juce::jlimit (0, getNumRows() - 1, row);

    // Count from the live history, not the cached rows: they update asynchronously.
    while (history.getUndoDescriptions().size() > row && history.undo())
    {
    }
    while (history.getUndoDescriptions().size() < row && history.redo())
    {
    }
    handleUpdateNowIfNeeded();
}

void HistoryPanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool)
{
    const auto& theme = themeOf (*this);
    const bool current = row == getCurrentRow();
    const bool redoable = row > getCurrentRow();

    if (current)
    {
        g.setColour (theme.get ("accentAlt"));
        g.fillRect (0, 0, width, height);
    }

    g.setColour (redoable ? theme.get ("textDim").withAlpha (0.6f) : theme.get ("text"));
    g.setFont (juce::FontOptions (13.0f, row == 0 ? juce::Font::italic : juce::Font::plain));
    g.drawText (getRowText (row), 10, 0, width - 20, height, juce::Justification::centredLeft, true);
}

void HistoryPanel::resized() { list.setBounds (getLocalBounds()); }

void HistoryPanel::paint (juce::Graphics& g) { g.fillAll (themeOf (*this).get ("panel")); }

} // namespace spinus::ui
