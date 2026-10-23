#pragma once

#include <juce_events/juce_events.h>

#include <atomic>
#include <set>

namespace spinus
{

/** The projects the Browser lists (docs/user/browser.md#projects), kept in Spinus's settings
    (`projects.json`), not in a project: changing this list is not a project change, so it is not a Command.

    - **Opened:** every project opened or saved in Spinus (File › Open, Open recent, the command line, the
      Browser itself; MainView adds them), newest first. One that is no longer on disk stays listed as
      missing (it may be on a drive that is not plugged in) until the user removes it.
    - **Folders** the user adds: searched for projects (a folder with a .spn file, or an older ".spinus"
      folder with its project.json), into subfolders up to `maxDepth` deep, leaving out hidden folders,
      links to folders and the inside of projects, and stopping after `maxFoldersPerScan` folders. A project
      that is gone drops out at the next search; the result of the last search is kept, so the list shows at
      once on the next start while it is searched again.

    A project is listed once: in the first added folder it is in, else under Opened. "Remove from the list"
    on a project found in a folder hides it there (until it is opened again). Searching runs on a thread of
    its own; the message thread only gets the result (a change message). Message thread only otherwise.
*/
class ProjectList final : public juce::ChangeBroadcaster, private juce::AsyncUpdater
{
public:
    static constexpr int maxDepth = 6, maxFoldersPerScan = 20000, maxOpened = 500;

    ProjectList();
    ~ProjectList() override;

    /** Where the list is kept; loads it and searches the folders again (in the background). Without a file
        the list is not kept. */
    void setFile (const juce::File&);

    struct Project
    {
        juce::File folder;
        juce::String name;
        bool missing = false; // not a project folder any more (found by the last check)
    };

    struct Folder
    {
        juce::File folder;
        std::vector<Project> projects; // by path, so subfolders stay together
        bool missing = false;          // the folder is not there
        bool truncated = false;        // the search stopped at maxFoldersPerScan
    };

    /** Opened or saved in Spinus, newest first; without the ones listed under a folder. */
    std::vector<Project> getOpened() const;
    const std::vector<Folder>& getFolders() const noexcept { return folders; }
    /** Every project listed, once (tests). */
    juce::Array<juce::File> getAllProjects() const;

    /** A project was opened or saved: listed first under Opened (a .spn file means its folder). */
    void addOpened (const juce::File& projectFolderOrFile);
    /** Takes a project off the list (the files stay). */
    void remove (const juce::File& projectFolder);

    /** Adds a folder to search; false if it is not a folder or is already searched (inside one that is). An
        added folder that holds folders added before takes their place. */
    bool addFolder (const juce::File&);
    void removeFolder (const juce::File&);

    /** Searches every folder again and checks the opened projects are still there, in the background. */
    void rescan();
    bool isScanning() const noexcept { return scan != nullptr; }
    /** Waits for a search and applies its result (tests). */
    void waitForScan();

    /** The projects under `root`, by path (the search a folder gets; runs on any thread). `truncated` is set
        if it stopped at `maxFolders`. */
    static juce::Array<juce::File> findProjects (const juce::File& root, int depthLimit = maxDepth,
                                                 int maxFolders = maxFoldersPerScan,
                                                 const std::atomic<bool>* cancel = nullptr, bool* truncated = nullptr);

private:
    class ScanThread;

    void handleAsyncUpdate() override;
    void load();
    void save() const;
    void changed();

    juce::File file;
    struct Opened
    {
        juce::File folder;
        bool missing = false;
    };
    std::vector<Opened> opened;
    std::vector<Folder> folders;
    std::set<juce::String> hidden; // found in a folder, removed from the list by the user
    std::unique_ptr<ScanThread> scan;
    bool rescanPending = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProjectList)
};

} // namespace spinus
