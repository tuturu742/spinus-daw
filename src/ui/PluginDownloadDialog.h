#pragma once

#include "model/Model.h"
#include "plugins/PluginDownloads.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace spinus::ui
{

/** Options → Get free plugins (also offered on first launch): the open-source plugins
    Spinus works with that are published for this system (Surge XT, Odin 2, ...), each with what it is, its
    size, license and website, and a tick box. Download fetches the ticked ones from their projects' own release pages
    in the background, with progress and Cancel; Spinus keeps working meanwhile.

    Packs already downloaded, or installed on the system (e.g. with pacman), are shown as such and not
    ticked. A pack Spinus downloaded has a Remove button: the row asks first ("Remove Surge XT? ...
    [Remove] [Keep]"), then deletes the pack's folder. A pack still in use (by the open project, see
    `checkRemove`, or still loaded at all, e.g. by a hidden copy the Analyzer plays) is not removed: the row
    says why. Spinus refuses rather than deleting a running plugin's files, the same on Linux and on
    Windows (where a loaded plugin's file cannot be deleted at all). */
class PluginDownloadDialog final : public juce::Component, private juce::Timer
{
public:
    /** `searchFolders`: where the bundled devices look for their files (to tell what is installed).
        `onlyPack`: tick just this pack (when opened for one plugin), or these packs ("surge-xt,lsp", for a
        project that needs them); empty: tick everything missing. */
    PluginDownloadDialog (const plugins::PluginDownloads&, std::vector<juce::File> searchFolders,
                          const juce::String& onlyPack = {},
                          std::vector<plugins::DownloadPack> packs = plugins::PluginDownloads::getStandardPacks());
    ~PluginDownloadDialog() override; // cancels a running download

    /** Opens the dialog in its own window (not modal; closing it cancels downloads), or brings it to the
        front. `onChanged` runs after each pack is installed or removed; `checkRemove` is the dialog's
        `checkRemove`. */
    static void show (const plugins::PluginDownloads&, std::vector<juce::File> searchFolders,
                      juce::Component* centreAround, const juce::String& onlyPack, std::function<void()> onChanged,
                      std::function<juce::String (const plugins::DownloadPack&)> checkRemove = {});

    /** Closes the window if it is open (on quit). */
    static void closeWindow();

    /** Called on the message thread after each pack is installed. */
    std::function<void()> onInstalled;
    /** Called on the message thread after a pack is removed (also when only some of its files stayed). */
    std::function<void()> onRemoved;
    /** Why a pack cannot be removed now, e.g. "Surge XT is used by ... in this project ..."; empty: it can.
        Asked when Remove is pressed and again just before deleting. Not set: only the loaded check applies. */
    std::function<juce::String (const plugins::DownloadPack&)> checkRemove;

    /** Why the pack cannot be removed while `project` is open, e.g. "Surge XT is used by 2 channels (Lead,
        Pad) and 1 mixer insert (Insert 3) in this project. ...": something in it uses one of the pack's
        plugins (a bundled device of the pack, or a plugin whose file is inside `packFolder`). Empty if
        nothing does. MainView's `checkRemove`. */
    static juce::String describeUseInProject (const model::Project&, const plugins::DownloadPack&,
                                              const juce::File& packFolder);

    /** True if a plugin file inside `folder` is loaded in Spinus now (by any instance, hidden ones too). */
    static bool isLoadedFrom (const juce::File& folder);

    //==============================================================================
    // For the buttons and for tests.
    void setSelected (const juce::String& pack, bool);
    bool isSelected (const juce::String& pack) const;
    juce::String getPackStatus (const juce::String& pack) const;
    juce::String getStatusText() const { return status.getText(); }

    /** Remove (a pack Spinus downloaded, while nothing downloads): the row asks to confirm, or says why
        it cannot be removed now. */
    bool canRemove (const juce::String& pack) const;
    void askToRemove (const juce::String& pack);
    bool isAskingToRemove (const juce::String& pack) const;
    /** The row's question or refusal; empty when it shows its description. */
    juce::String getRowMessage (const juce::String& pack) const;
    /** The question's Remove: checks again, then deletes the pack. */
    void confirmRemove (const juce::String& pack);
    /** The question's Keep (or the refusal's OK). */
    void keepPack (const juce::String& pack);

    void startDownloads();
    void cancelDownloads();
    bool isDownloading() const;
    /** Waits for running downloads and applies their results (tests). */
    void finishForTests();

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class DownloadThread;
    struct PackRow;

    void timerCallback() override;
    void finished();
    void refreshRows();
    PackRow* findRow (const juce::String& pack) const;
    juce::String whyNotRemovable (const plugins::DownloadPack&) const;
    void layOutRow (PackRow&, juce::Rectangle<int>);

    const plugins::PluginDownloads& downloads;
    std::vector<juce::File> searchFolders;
    const std::vector<plugins::DownloadPack> packs;
    std::vector<std::unique_ptr<PackRow>> packRows;
    struct RowsArea;
    std::unique_ptr<RowsArea> rowsArea; // the rows, scrolled in rowsView when the screen is too short for all
    juce::Viewport rowsView;
    juce::Label intro, status;
    juce::TextButton downloadButton { "Download" }, closeButton { "Close" };
    double progress = 0.0;
    juce::ProgressBar progressBar { progress };
    std::unique_ptr<DownloadThread> thread;
    size_t reportedInstalls = 0;
};

} // namespace spinus::ui
