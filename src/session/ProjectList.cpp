#include "ProjectList.h"

#include "core/HiddenFiles.h"
#include "project/ProjectFiles.h"

namespace spinus
{

namespace
{
/** Folders that hold no projects but can hold very many folders. */
bool isSkipped (const juce::File& folder)
{
    const auto name = folder.getFileName();
    return name == "node_modules" || name == "__pycache__" || name.equalsIgnoreCase ("$RECYCLE.BIN") ||
           name == "System Volume Information" || folder.isSymbolicLink();
}

void sortByPath (juce::Array<juce::File>& files)
{
    struct ByPath
    {
        static int compareElements (const juce::File& a, const juce::File& b)
        {
            return a.getFullPathName().compareNatural (b.getFullPathName());
        }
    } byPath;
    files.sort (byPath);
}

juce::String keyOf (const juce::File& f) { return f.getFullPathName(); }
} // namespace

//==============================================================================
/** Searches the folders and checks the opened projects, then tells the list (on the message thread). */
class ProjectList::ScanThread final : public juce::Thread
{
public:
    ScanThread (ProjectList& o, juce::Array<juce::File> f, juce::Array<juce::File> p)
        : juce::Thread ("Spinus project search"), owner (o), folders (std::move (f)), opened (std::move (p))
    {
    }

    ~ScanThread() override
    {
        cancel = true;
        stopThread (-1);
    }

    void run() override
    {
        for (auto& folder : folders)
        {
            if (cancel)
                return;
            Result r;
            r.folder = folder;
            r.missing = ! folder.isDirectory();
            r.projects = findProjects (folder, maxDepth, maxFoldersPerScan, &cancel, &r.truncated);
            results.push_back (std::move (r));
        }
        for (auto& project : opened)
        {
            if (cancel)
                return;
            openedMissing.push_back (! project::isProjectFolder (project));
        }
        done = true;
        owner.triggerAsyncUpdate();
    }

    struct Result
    {
        juce::File folder;
        juce::Array<juce::File> projects;
        bool missing = false, truncated = false;
    };

    ProjectList& owner;
    const juce::Array<juce::File> folders, opened;
    std::vector<Result> results;
    std::vector<bool> openedMissing;
    std::atomic<bool> cancel { false }, done { false };
};

//==============================================================================
ProjectList::ProjectList() = default;

ProjectList::~ProjectList()
{
    scan = nullptr;
    cancelPendingUpdate();
}

juce::Array<juce::File> ProjectList::findProjects (const juce::File& root, int depthLimit, int maxFolders,
                                                   const std::atomic<bool>* cancel, bool* truncated)
{
    juce::Array<juce::File> found;
    if (truncated != nullptr)
        *truncated = false;
    if (! root.isDirectory())
        return found;

    std::vector<std::pair<juce::File, int>> toVisit { { root, 0 } };
    int visited = 0;
    while (! toVisit.empty())
    {
        if (cancel != nullptr && cancel->load())
            break;
        if (++visited > maxFolders)
        {
            if (truncated != nullptr)
                *truncated = true;
            break;
        }
        const auto [folder, depth] = toVisit.back();
        toVisit.pop_back();

        bool isProject = false;
        juce::Array<juce::File> subfolders;
        for (const auto& entry : juce::RangedDirectoryIterator (
                 folder, false, "*", juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles))
        {
            const auto& f = entry.getFile();
            if (core::isHidden (f))
                continue;
            if (entry.isDirectory())
            {
                if (depth < depthLimit && ! isSkipped (f))
                    subfolders.add (f);
            }
            else if (f.hasFileExtension (project::projectFileExtension) ||
                     (f.getFileName() == "project.json" && folder.getFileName().endsWithIgnoreCase (".spinus")))
            {
                isProject = true;
            }
        }
        if (isProject)
        {
            found.add (folder); // its subfolders are its own (audio/, plugin-states/, cache/)
            continue;
        }
        for (auto& sub : subfolders)
            toVisit.emplace_back (sub, depth + 1);
    }
    sortByPath (found);
    return found;
}

//==============================================================================
void ProjectList::setFile (const juce::File& f)
{
    file = f;
    load();
    sendSynchronousChangeMessage();
    rescan();
}

void ProjectList::load()
{
    opened.clear();
    folders.clear();
    hidden.clear();
    if (! file.existsAsFile())
        return;

    const auto json = juce::JSON::parse (file);
    auto paths = [] (const juce::var& list)
    {
        juce::Array<juce::File> result;
        if (auto* a = list.getArray())
            for (auto& v : *a)
                if (juce::File::isAbsolutePath (v.toString()))
                    result.addIfNotAlreadyThere (juce::File (v.toString()));
        return result;
    };
    for (auto& f : paths (json.getProperty ("opened", {})))
        opened.push_back ({ f });
    for (auto& f : paths (json.getProperty ("hidden", {})))
        hidden.insert (keyOf (f));
    if (auto* list = json.getProperty ("folders", {}).getArray())
        for (auto& v : *list)
        {
            const auto path = v.getProperty ("path", {}).toString();
            if (! juce::File::isAbsolutePath (path))
                continue;
            Folder folder { juce::File (path) };
            folder.truncated = (bool)v.getProperty ("truncated", false);
            for (auto& p : paths (v.getProperty ("projects", {})))
                folder.projects.push_back ({ p, project::projectName (p) });
            folders.push_back (std::move (folder));
        }
}

void ProjectList::save() const
{
    if (file == juce::File())
        return;
    auto list = [] (auto& items, auto toPath)
    {
        juce::Array<juce::var> a;
        for (auto& item : items)
            a.add (toPath (item));
        return juce::var (a);
    };
    auto* o = new juce::DynamicObject();
    o->setProperty ("opened", list (opened, [] (const Opened& p) { return keyOf (p.folder); }));
    juce::Array<juce::var> folderList;
    for (auto& folder : folders)
    {
        auto* f = new juce::DynamicObject();
        f->setProperty ("path", keyOf (folder.folder));
        f->setProperty ("projects", list (folder.projects, [] (const Project& p) { return keyOf (p.folder); }));
        if (folder.truncated)
            f->setProperty ("truncated", true);
        folderList.add (juce::var (f));
    }
    o->setProperty ("folders", folderList);
    o->setProperty ("hidden", list (hidden, [] (const juce::String& p) { return p; }));
    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::JSON::toString (juce::var (o)));
}

void ProjectList::changed()
{
    save();
    sendSynchronousChangeMessage(); // the Browser shows it at once
}

//==============================================================================
std::vector<ProjectList::Project> ProjectList::getOpened() const
{
    std::set<juce::String> inFolders;
    for (auto& folder : folders)
        for (auto& p : folder.projects)
            inFolders.insert (keyOf (p.folder));

    std::vector<Project> result;
    for (auto& p : opened)
        if (! inFolders.contains (keyOf (p.folder)))
            result.push_back ({ p.folder, project::projectName (p.folder), p.missing });
    return result;
}

juce::Array<juce::File> ProjectList::getAllProjects() const
{
    juce::Array<juce::File> all;
    for (auto& folder : folders)
        for (auto& p : folder.projects)
            all.add (p.folder);
    for (auto& p : getOpened())
        all.add (p.folder);
    return all;
}

void ProjectList::addOpened (const juce::File& projectFolderOrFile)
{
    const auto folder = project::projectFolderFor (projectFolderOrFile);
    if (folder == juce::File())
        return;
    std::erase_if (opened, [&] (const Opened& p) { return p.folder == folder; });
    opened.insert (opened.begin(), { folder });
    if ((int)opened.size() > maxOpened)
        opened.resize ((size_t)maxOpened);

    // Inside an added folder (saved there, or hidden there before and used again): listed there at once.
    hidden.erase (keyOf (folder));
    for (auto& f : folders)
        if (folder == f.folder || folder.isAChildOf (f.folder))
        {
            if (std::none_of (f.projects.begin(), f.projects.end(),
                              [&] (const Project& p) { return p.folder == folder; }))
            {
                f.projects.push_back ({ folder, project::projectName (folder) });
                std::sort (f.projects.begin(), f.projects.end(), [] (const Project& a, const Project& b)
                           { return a.folder.getFullPathName().compareNatural (b.folder.getFullPathName()) < 0; });
            }
            break;
        }
    changed();
}

void ProjectList::remove (const juce::File& projectFolder)
{
    std::erase_if (opened, [&] (const Opened& p) { return p.folder == projectFolder; });
    for (auto& folder : folders)
        if (std::erase_if (folder.projects, [&] (const Project& p) { return p.folder == projectFolder; }) > 0)
            hidden.insert (keyOf (projectFolder));
    changed();
}

bool ProjectList::addFolder (const juce::File& folder)
{
    if (! folder.isDirectory())
        return false;
    for (auto& f : folders)
        if (f.folder == folder || folder.isAChildOf (f.folder))
            return false;

    std::erase_if (folders, [&] (const Folder& f) { return f.folder.isAChildOf (folder); });
    folders.push_back ({ folder });
    changed();
    rescan();
    return true;
}

void ProjectList::removeFolder (const juce::File& folder)
{
    std::erase_if (folders, [&] (const Folder& f) { return f.folder == folder; });
    changed();
}

void ProjectList::rescan()
{
    if (scan != nullptr)
    {
        rescanPending = true; // the running search may have read the folders before the change
        return;
    }
    rescanPending = false;
    juce::Array<juce::File> folderFiles, openedFiles;
    for (auto& f : folders)
        folderFiles.add (f.folder);
    for (auto& p : opened)
        openedFiles.add (p.folder);
    scan = std::make_unique<ScanThread> (*this, folderFiles, openedFiles);
    scan->startThread (juce::Thread::Priority::low);
}

void ProjectList::waitForScan()
{
    while (scan != nullptr)
    {
        scan->waitForThreadToExit (-1);
        handleAsyncUpdate();
    }
}

void ProjectList::handleAsyncUpdate()
{
    if (scan == nullptr || ! scan->done)
    {
        if (scan != nullptr && ! scan->isThreadRunning()) // cancelled
            scan = nullptr;
        return;
    }
    scan->waitForThreadToExit (-1);
    const auto finished = std::move (scan);

    for (auto& result : finished->results)
        for (auto& folder : folders)
            if (folder.folder == result.folder)
            {
                folder.missing = result.missing;
                folder.truncated = result.truncated;
                folder.projects.clear();
                for (auto& p : result.projects)
                    if (! hidden.contains (keyOf (p)))
                        folder.projects.push_back ({ p, project::projectName (p) });
            }
    for (size_t i = 0; i < finished->openedMissing.size(); ++i)
        for (auto& p : opened)
            if (p.folder == finished->opened[(int)i])
                p.missing = finished->openedMissing[i];

    changed();
    if (rescanPending)
        rescan();
}

} // namespace spinus
