#include "ProjectDocument.h"

#include "commands/CommandRegistry.h"

namespace spinus
{

ProjectDocument::ProjectDocument (Session& s, juce::File settingsFolder)
    : session (s), settings (std::move (settingsFolder))
{
    historyListener = session.getHistory().addListener ([this] (const commands::ChangeSet&) { historyChanged(); });
    markSaved();
    startTimer (autosaveIntervalMs);
}

ProjectDocument::~ProjectDocument()
{
    session.getHistory().removeListener (historyListener);
    closeCurrent();
}

juce::String ProjectDocument::getName() const
{
    return isUntitled() ? juce::String ("Untitled") : project::projectName (folder);
}

bool ProjectDocument::hasUnsavedEdits() const { return session.getHistory().getStateId() != savedStateId; }

bool ProjectDocument::hasUnsavedChanges() const
{
    return hasUnsavedEdits() || session.getAdapter().captureDeviceStates() != savedStates;
}

void ProjectDocument::markSaved()
{
    savedStateId = autosavedStateId = session.getHistory().getStateId();
    savedStates = autosavedStates = session.getAdapter().captureDeviceStates();
    lastNotifiedUnsaved = false;
    sendChangeMessage();
}

void ProjectDocument::historyChanged()
{
    if (hasUnsavedEdits() != lastNotifiedUnsaved)
    {
        lastNotifiedUnsaved = ! lastNotifiedUnsaved;
        sendChangeMessage();
    }
}

void ProjectDocument::closeCurrent()
{
    lock = nullptr;
    if (untitledFolder != juce::File())
        untitledFolder.deleteRecursively();
    untitledFolder = juce::File();
}

//==============================================================================
void ProjectDocument::newProject()
{
    closeCurrent();
    folder = juce::File();
    session.setProjectFolder ({});
    session.loadProject ({});
    markSaved();
}

juce::Result ProjectDocument::open (const juce::File& file) { return openFolder (project::projectFolderFor (file)); }

juce::Result ProjectDocument::openFolder (const juce::File& projectFolder)
{
    if (! project::isProjectFolder (projectFolder))
        return juce::Result::fail (projectFolder.getFullPathName() + " is not a Spinus project (no " +
                                   project::projectFileExtension + " file)");

    if (projectFolder != folder &&
        project::SessionLock::getState (projectFolder) == project::SessionLock::State::openInSpinus)
        return juce::Result::fail (projectFolder.getFileName() + " is already open in another Spinus window");

    project::ProjectData data;
    if (auto r = project::loadProject (projectFolder, data); r.failed())
        return r;

    // A crash left the lock: drop it, the caller has already offered the autosave (findRecoveries).
    project::SessionLock::clearCrashedLock (projectFolder);

    closeCurrent();
    folder = projectFolder;
    lock = std::make_unique<project::SessionLock> (folder);
    session.setProjectFolder (folder);
    session.loadProject (std::move (data));
    markSaved();
    addToRecent (folder);
    return juce::Result::ok();
}

juce::Result ProjectDocument::collectAudioFiles (const juce::File& projectFolder)
{
    // Audio clips that play files from elsewhere on disk: copy the files into audio/ so the project
    // keeps working when the originals move. One undo step for all of them.
    std::map<juce::String, juce::String> copied; // absolute path -> path relative to audio/
    auto transaction = std::make_unique<commands::Transaction> ("Copy audio files into the project");

    for (auto& clip : session.getProject().playlist.clips)
    {
        auto* audio = std::get_if<model::AudioClip> (&clip.content);
        if (audio == nullptr || ! juce::File::isAbsolutePath (audio->file) || ! juce::File (audio->file).existsAsFile())
            continue;

        if (! copied.contains (audio->file))
        {
            juce::String relative;
            if (auto r = project::importAudioFile (projectFolder, juce::File (audio->file), relative); r.failed())
                return r;
            copied[audio->file] = relative;
        }

        auto* params = new juce::DynamicObject();
        params->setProperty ("clip", (int)clip.id.value);
        params->setProperty ("file", copied[audio->file]);
        juce::String error;
        if (auto command = commands::CommandRegistry::get().create ("clip.setAudio", juce::var (params), error))
            transaction->add (std::move (command));
    }

    if (transaction->size() == 0)
        return juce::Result::ok();

    // The adapter must find the copies where the clips now point.
    session.setProjectFolder (projectFolder);
    return session.getHistory().perform (std::move (transaction));
}

juce::Result ProjectDocument::save()
{
    if (isUntitled())
        return juce::Result::fail ("The project has no folder yet: use Save As");

    if (auto r = collectAudioFiles (folder); r.failed())
        return r;

    if (auto r = project::saveProject (folder, session.captureProjectData()); r.failed())
        return r;

    if (lock == nullptr)
        lock = std::make_unique<project::SessionLock> (folder);
    markSaved();
    addToRecent (folder);
    return juce::Result::ok();
}

juce::Result ProjectDocument::saveAs (juce::File newFolder)
{
    if (newFolder.hasFileExtension (project::projectFileExtension))
        newFolder = newFolder.withFileExtension ({});

    if (newFolder == folder)
        return save();

    if (newFolder.existsAsFile())
        return juce::Result::fail (newFolder.getFullPathName() + " is a file, not a project folder");

    // A project is a whole folder: never mix one into a folder of other things.
    if (newFolder.isDirectory() && ! project::isProjectFolder (newFolder) &&
        newFolder.getNumberOfChildFiles (juce::File::findFilesAndDirectories) > 0)
        return juce::Result::fail (newFolder.getFullPathName() +
                                   " already has other files in it. Choose a new name for the project.");

    if (project::SessionLock::getState (newFolder) == project::SessionLock::State::openInSpinus)
        return juce::Result::fail (newFolder.getFileName() + " is open in another Spinus window");

    // The audio files the project uses come along (the cache does not: it is rebuilt when needed).
    if (! isUntitled())
    {
        const auto oldAudio = folder.getChildFile ("audio");
        for (auto& file : oldAudio.findChildFiles (juce::File::findFiles, true))
        {
            const auto target = newFolder.getChildFile ("audio").getChildFile (file.getRelativePathFrom (oldAudio));
            if (! target.exists() && (! target.getParentDirectory().createDirectory() || ! file.copyFileTo (target)))
                return juce::Result::fail ("cannot copy " + file.getFullPathName() + " into the new project");
        }
    }

    if (auto r = collectAudioFiles (newFolder); r.failed())
        return r;

    if (auto r = project::saveProject (newFolder, session.captureProjectData()); r.failed())
        return r;

    closeCurrent();
    folder = std::move (newFolder);
    session.setProjectFolder (folder);
    lock = std::make_unique<project::SessionLock> (folder);
    markSaved();
    addToRecent (folder);
    return juce::Result::ok();
}

//==============================================================================
juce::Result ProjectDocument::autosave()
{
    const auto stateId = session.getHistory().getStateId();
    auto data = session.captureProjectData();
    if (stateId == autosavedStateId && data.deviceStates == autosavedStates)
        return juce::Result::ok();

    if (isUntitled() && untitledFolder == juce::File())
    {
        const auto now = juce::Time::getCurrentTime();
        untitledFolder =
            getUnsavedRoot().getChildFile (now.formatted ("Untitled %Y%m%d-%H%M%S")).getNonexistentSibling (true);
        if (! untitledFolder.createDirectory())
            return juce::Result::fail ("cannot create " + untitledFolder.getFullPathName());
        lock = std::make_unique<project::SessionLock> (untitledFolder);
    }

    auto states = data.deviceStates;
    if (auto r = project::Autosaves (isUntitled() ? untitledFolder : folder).save (data); r.failed())
        return r;

    autosavedStateId = stateId;
    autosavedStates = std::move (states);
    return juce::Result::ok();
}

void ProjectDocument::timerCallback()
{
    // A failed autosave is retried next time; the user's own saves report their problems.
    [[maybe_unused]] const auto result = autosave();
    jassert (result.wasOk());
}

//==============================================================================
std::optional<ProjectDocument::Recovery> ProjectDocument::findRecovery (const juce::File& projectFolder, bool untitled)
{
    if (! project::SessionLock::previousSessionCrashed (projectFolder))
        return std::nullopt;

    const project::Autosaves autosaves (projectFolder);
    const auto all = autosaves.list();
    const auto newest =
        untitled ? (all.isEmpty() ? std::optional<juce::File>() : all.getFirst()) : autosaves.findNewerThanSaved();
    if (! newest.has_value())
        return std::nullopt;

    return Recovery { projectFolder, *newest, untitled,
                      untitled ? juce::String ("Untitled") : project::projectName (projectFolder),
                      project::findProjectFile (*newest).getLastModificationTime() };
}

std::vector<ProjectDocument::Recovery> ProjectDocument::findRecoveries() const
{
    std::vector<Recovery> result;

    auto consider = [&] (const juce::File& projectFolder, bool untitled)
    {
        if (projectFolder != folder && projectFolder != untitledFolder)
            if (auto r = findRecovery (projectFolder, untitled))
                result.push_back (std::move (*r));
    };

    for (auto& recent : getRecentProjects())
        consider (recent, false);
    for (auto& unsaved : getUnsavedRoot().findChildFiles (juce::File::findDirectories, false))
        consider (unsaved, true);

    std::sort (result.begin(), result.end(), [] (auto& a, auto& b) { return a.time > b.time; });
    return result;
}

juce::Result ProjectDocument::recover (const Recovery& recovery)
{
    project::ProjectData data;
    if (auto r = project::loadProject (recovery.autosave, data); r.failed())
        return r;

    closeCurrent();
    project::SessionLock::clearCrashedLock (recovery.projectFolder);

    if (recovery.untitled)
    {
        // Carry on in the crashed project's folder: its autosaves stay until the project is saved.
        folder = juce::File();
        untitledFolder = recovery.projectFolder;
        lock = std::make_unique<project::SessionLock> (untitledFolder);
    }
    else
    {
        folder = recovery.projectFolder;
        lock = std::make_unique<project::SessionLock> (folder);
        addToRecent (folder);
    }

    session.setProjectFolder (folder);
    session.loadProject (std::move (data));
    markSaved();

    // The recovered work is not in the saved project yet.
    savedStateId = 0;
    savedStates.clear();
    lastNotifiedUnsaved = true;
    sendChangeMessage();
    return juce::Result::ok();
}

void ProjectDocument::discard (const Recovery& recovery)
{
    if (recovery.untitled)
        recovery.projectFolder.deleteRecursively();
    else
        project::SessionLock::clearCrashedLock (recovery.projectFolder);
}

//==============================================================================
juce::Array<juce::File> ProjectDocument::getRecentProjects() const
{
    juce::StringArray lines;
    getRecentListFile().readLines (lines);

    juce::Array<juce::File> result;
    for (auto& line : lines)
        if (juce::File::isAbsolutePath (line) && project::isProjectFolder (juce::File (line)))
            result.addIfNotAlreadyThere (juce::File (line));
    return result;
}

void ProjectDocument::addToRecent (const juce::File& projectFolder)
{
    auto recent = getRecentProjects();
    recent.removeAllInstancesOf (projectFolder);
    recent.insert (0, projectFolder);
    recent.resize (juce::jmin (recent.size(), maxRecentProjects));

    juce::StringArray lines;
    for (auto& f : recent)
        lines.add (f.getFullPathName());
    if (settings.createDirectory())
        getRecentListFile().replaceWithText (lines.joinIntoString ("\n") + "\n");
}

void ProjectDocument::clearRecentProjects() { getRecentListFile().deleteFile(); }

} // namespace spinus
