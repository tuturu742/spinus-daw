#pragma once

#include "core/FileDialog.h"
#include "plugins/DeviceList.h"
#include "session/ProjectList.h"
#include "session/Session.h"

namespace spinus::ui
{

/** The Browser (left dock, F8): Spinus's instruments and effects and the installed
    plugins, each with its format (VST3, CLAP, LV2, VST2, WIN), the user's sample folders, and the
    plugins that failed to scan.

    Double-click an instrument to add a channel playing it, or an effect to add it to the selected mixer
    insert (MainView decides, through onUse). Or drag it onto the Channel Rack or a mixer strip or slot.
    "Scan plugins" looks for installed plugins, each file in a separate process.

    Samples: folders the user adds ("+ Add a folder of samples"), remembered in the settings. Folders
    open to their subfolders and audio files; the search finds files in them too. Click a sample to
    hear it (Session::getSamplePreview), double-click to add it to the Channel Rack (onUseSample), or
    drag it onto the Channel Rack, the Playlist or Audio tracks (sampleFromDrag reads the drag).

    Projects (the ProjectList, kept in the settings): the demo songs (when they are installed) and the projects
    in folders the user adds ("+ Add a folder of projects"), searched in the background. Double-click one to
    open it (onOpenProject, onOpenDemo: MainView asks to save changes first).
*/
class Browser final : public juce::Component,
                      private juce::ListBoxModel,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    explicit Browser (Session&);
    ~Browser() override;

    /** Double-click or "Add" in the right-click menu. */
    std::function<void (const plugins::DeviceList::Entry&)> onUse;

    /** The same on a free open-source plugin that is not downloaded yet: the pack to offer (e.g. "surge-xt"). */
    std::function<void (const juce::String& pack)> onDownload;

    //==============================================================================
    // Projects.
    /** Where the project list is kept (Spinus's settings); loads it and searches its folders again. */
    void setProjectsFile (const juce::File&);
    ProjectList& getProjects() noexcept { return projects; }
    /** The demo songs to list first, each as "<Name> (demo)" (the shipped ones: demo::findBundledDemos). */
    void setDemos (juce::Array<juce::File> songs);
    bool addProjectFolder (const juce::File&);

    /** Double-click or "Open" on a project: its folder. */
    std::function<void (const juce::File&)> onOpenProject;
    /** Double-click or "Open" on a demo song: its shipped folder. */
    std::function<void (const juce::File&)> onOpenDemo;

    //==============================================================================
    // Samples.
    /** Where the sample folders are remembered (one path a line); loads it. Without it they are not kept. */
    void setSampleFoldersFile (const juce::File&);
    const juce::Array<juce::File>& getSampleFolders() const noexcept { return sampleFolders; }
    bool addSampleFolder (const juce::File&);
    void removeSampleFolder (const juce::File&);
    /** Opens or closes a folder in the list. */
    void toggleFolder (const juce::File&);

    /** Double-click or "Add to the Channel Rack" on a sample. */
    std::function<void (const juce::File&)> onUseSample;
    /** "Add as an audio track" on a sample. */
    std::function<void (const juce::File&)> onAddSampleAsTrack;
    /** "Open in Beat Slicer" on a sample. */
    std::function<void (const juce::File&)> onSliceSample;

    /** The sample a Browser drag carries, if it carries one. */
    static std::optional<juce::File> sampleFromDrag (const juce::var& description);
    /** Most samples a search lists; the folders are searched up to `maxIndexedFiles` files. */
    static constexpr int maxSearchResults = 300, maxIndexedFiles = 50000;

    void setSearchText (const juce::String&);

    /** Starts scanning for plugins in the background (no-op without a catalog). Asked while a scan runs, it
        scans again when that one ends: plugins installed meanwhile (e.g. a Windows plugin) are not missed. */
    void startScan();
    bool isRescanPending() const noexcept { return rescanPending; }
    void cancelScan();
    bool isScanning() const;

    /** Waits for a running scan and applies its result (tests). */
    void finishScanForTests();

    //==============================================================================
    // What the list shows (tests).
    enum class RowKind
    {
        section, // Instruments, Effects, Samples, Problem plugins
        group,   // Installed plugins, Spinus, Free open source
        device,
        problem,
        message,
        folder, // a sample folder (`path`), indented by `depth`
        sample, // an audio file (`path`)
        action, // "+ Add a folder of samples" (key "addSamples"), "+ Add a folder of projects" ("addProjects")
        project // a project folder (`path`); a demo song (its shipped folder) has key "demo"
    };

    struct Row
    {
        RowKind kind;
        juce::String text, badge, key; // key: the section or group a header opens and closes
        int entry = -1;                // device rows: index into the device list
        int problem = -1;              // problem rows: index into the problem list
        juce::String note;             // device rows: shown dimmed, e.g. the open-source project
        bool available = true;         // device rows: false for a free plugin not downloaded yet; project
                                       // rows: false for one that is missing
        juce::String path;             // folder, sample and project rows; a project folder's group
        int depth = 0;                 // folder and sample rows: how far inside a sample folder
    };

    const std::vector<Row>& getRows() const noexcept { return rows; }
    void toggleOpen (const juce::String& key);
    void useRow (int row);
    juce::String getStatusText() const { return status.getText(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;

private:
    class ScanThread;

    void rebuildRows();
    void addSampleRows (const juce::String& query);
    void addProjectRows (const juce::String& query);
    void chooseProjectFolder();
    void addFolderRows (const juce::File& folder, int depth);
    void saveSampleFolders() const;
    void chooseSampleFolder();
    const std::vector<juce::File>& getSampleIndex();
    int countInstalled() const; // installed plugins, as the list shows them
    void scanFinished();
    void showRowMenu (int row);

    // ListBoxModel
    int getNumRows() override { return (int)rows.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override { rebuildRows(); } // devices, projects
    void timerCallback() override;

    Session& session;
    plugins::DeviceList& devices;
    juce::TextEditor search;
    juce::ListBox list { "Browser", this };
    juce::Label status;
    juce::TextButton scanButton { "Scan plugins" };
    std::vector<Row> rows;
    std::set<juce::String> closed { "problems" };
    juce::Array<juce::File> sampleFolders;
    juce::File sampleFoldersFile;
    std::set<juce::String> openFolders;
    std::vector<juce::File> sampleIndex;
    bool sampleIndexValid = false;
    std::unique_ptr<core::FileDialog> folderChooser;
    ProjectList projects;
    juce::Array<juce::File> demos;
    std::unique_ptr<ScanThread> scan;
    bool rescanPending = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Browser)
};

} // namespace spinus::ui
