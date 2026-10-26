#pragma once

#include "session/AnalysisLibrary.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** The analysis library's window ("Saved analyses...", from the Analyzer and the Track creator): every song
    Spinus has analyzed, the space it takes and how much of that is stems, the total, and deleting a song's
    stems, a song, all stems or everything. Only the library's own files are deleted, never the user's
    songs; a song is analyzed again when it is needed. */
class AnalysisLibraryView final : public juce::Component, private juce::ListBoxModel
{
public:
    explicit AnalysisLibraryView (AnalysisLibrary);
    ~AnalysisLibraryView() override;

    /** Opens it in a window of its own beside `near`. */
    static void show (const AnalysisLibrary&, juce::Component* near);

    /** Reads the library again (sizes included). */
    void refresh();
    const std::vector<AnalysisLibrary::Entry>& getEntries() const noexcept { return entries; }
    juce::String getTotalText() const { return total.getText(); }
    /** The text of a song's row: its name and the space it takes. */
    static juce::String describe (const AnalysisLibrary::Entry&);
    static juce::String sizeText (juce::int64 bytes);

    // The buttons (and tests): each deletes, then refreshes.
    bool deleteStems (int row);
    bool deleteSong (int row);
    bool deleteAllStems();
    bool deleteEverything();
    void selectRow (int row) { list.selectRow (row); }

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    int getNumRows() override { return (int)entries.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int) override { updateButtons(); }
    void updateButtons();

    AnalysisLibrary library;
    std::vector<AnalysisLibrary::Entry> entries;
    juce::Label intro, total;
    juce::ListBox list;
    juce::TextButton stemsButton { "Delete its stems" }, songButton { "Delete" }, allStemsButton { "Delete all stems" },
        everythingButton { "Delete everything" };
};

} // namespace spinus::ui
