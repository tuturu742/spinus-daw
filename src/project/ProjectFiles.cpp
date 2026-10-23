#include "ProjectFiles.h"

#if JUCE_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "model/ModelJson.h"

namespace spinus::project
{

namespace
{

const char* const legacyProjectJson = "project.json"; // projects saved in the older project format
const char* const autosaveProjectFile = "project.spn";
const char* const statesFolder = "plugin-states";
const char* const audioFolder = "audio";
const char* const cacheFolder = "cache";
const char* const autosaveFolder = ".autosave";
const char* const lockName = ".session.lock";

/** Writes via a temporary file and an atomic rename, so the target is never half-written. */
juce::Result writeAtomically (const juce::File& target, const void* data, size_t size)
{
    if (! target.getParentDirectory().createDirectory())
        return juce::Result::fail ("cannot create " + target.getParentDirectory().getFullPathName());

    juce::TemporaryFile temp (target, juce::TemporaryFile::useHiddenFile);
    {
        juce::FileOutputStream out (temp.getFile());
        if (out.failedToOpen() || ! out.write (data, size))
            return juce::Result::fail ("cannot write " + target.getFullPathName());
        out.flush();
        if (out.getStatus().failed())
            return out.getStatus();
    }

    return temp.overwriteTargetFileWithTemporary() ? juce::Result::ok()
                                                   : juce::Result::fail ("cannot replace " + target.getFullPathName());
}

juce::String stateFileName (model::DeviceId id) { return juce::String (id.value) + ".bin"; }

juce::Result writeModelAndStates (const juce::File& folder, const juce::File& modelFile, const ProjectData& data,
                                  bool removeStaleStates)
{
    const auto states = folder.getChildFile (statesFolder);

    for (auto& [id, block] : data.deviceStates)
        if (auto r = writeAtomically (states.getChildFile (stateFileName (id)), block.getData(), block.getSize());
            r.failed())
            return r;

    const auto json = model::toJsonText (data.project) + "\n";
    if (auto r = writeAtomically (modelFile, json.toRawUTF8(), json.getNumBytesAsUTF8()); r.failed())
        return r;

    if (removeStaleStates)
        for (auto& file : states.findChildFiles (juce::File::findFiles, false, "*.bin"))
            if (! data.deviceStates.contains (
                    model::DeviceId { (uint32_t)file.getFileNameWithoutExtension().getIntValue() }))
                file.deleteFile();

    return juce::Result::ok();
}

juce::Result readModelAndStates (const juce::File& folder, ProjectData& result)
{
    const auto file = findProjectFile (folder);
    if (! file.existsAsFile())
        return juce::Result::fail ("no " + juce::String (projectFileExtension) + " project file in " +
                                   folder.getFullPathName());

    ProjectData data;
    if (auto r = model::fromJsonText (file.loadFileAsString(), data.project); r.failed())
        return juce::Result::fail (file.getFullPathName() + ": " + r.getErrorMessage());

    for (auto& stateFile : folder.getChildFile (statesFolder).findChildFiles (juce::File::findFiles, false, "*.bin"))
    {
        const auto id = model::DeviceId { (uint32_t)stateFile.getFileNameWithoutExtension().getIntValue() };
        juce::MemoryBlock block;
        if (id.isValid() && stateFile.loadFileAsData (block))
            data.deviceStates[id] = std::move (block);
    }

    result = std::move (data);
    return juce::Result::ok();
}

} // namespace

//==============================================================================
juce::String projectName (const juce::File& folder)
{
    const auto name = folder.getFileName();
    return name.endsWithIgnoreCase (".spinus") ? name.dropLastCharacters (7) : name;
}

juce::File findProjectFile (const juce::File& folder)
{
    auto named = folder.getChildFile (projectName (folder) + projectFileExtension);
    if (named.existsAsFile())
        return named;

    auto others = folder.findChildFiles (juce::File::findFiles, false, juce::String ("*") + projectFileExtension);
    others.sort();
    if (! others.isEmpty())
        return others.getFirst();

    return folder.getChildFile (legacyProjectJson).existsAsFile() ? folder.getChildFile (legacyProjectJson) : named;
}

juce::File projectFolderFor (const juce::File& chosen)
{
    if (chosen.existsAsFile() &&
        (chosen.hasFileExtension (projectFileExtension) || chosen.getFileName() == legacyProjectJson))
        return chosen.getParentDirectory();
    return chosen;
}

juce::Result saveProject (const juce::File& folder, const ProjectData& data)
{
    if (! folder.createDirectory())
        return juce::Result::fail ("cannot create " + folder.getFullPathName());

    folder.getChildFile (audioFolder).createDirectory();

    // Keep the .spn file's name if the folder was renamed; an older project gets a .spn in place of
    // its project.json, removed once the .spn is written.
    auto modelFile = findProjectFile (folder);
    if (! modelFile.hasFileExtension (projectFileExtension))
        modelFile = folder.getChildFile (projectName (folder) + projectFileExtension);

    if (auto r = writeModelAndStates (folder, modelFile, data, true); r.failed())
        return r;

    folder.getChildFile (legacyProjectJson).deleteFile();
    return juce::Result::ok();
}

juce::Result loadProject (const juce::File& folder, ProjectData& result)
{
    if (! folder.isDirectory())
        return juce::Result::fail (folder.getFullPathName() + " is not a project folder");

    return readModelAndStates (folder, result);
}

juce::Result importAudioFile (const juce::File& folder, const juce::File& source, juce::String& relativePath)
{
    if (! source.existsAsFile())
        return juce::Result::fail (source.getFullPathName() + " does not exist");

    const auto audio = folder.getChildFile (audioFolder);
    if (! audio.createDirectory())
        return juce::Result::fail ("cannot create " + audio.getFullPathName());

    auto target = audio.getChildFile (source.getFileName());

    // Same name, different content: keep both ("kick(2).wav").
    if (target.existsAsFile() && target.getSize() == source.getSize() && target.hasIdenticalContentTo (source))
    {
        relativePath = target.getFileName();
        return juce::Result::ok();
    }

    if (target.exists())
        target = target.getNonexistentSibling (true);

    if (! source.copyFileTo (target))
        return juce::Result::fail ("cannot copy " + source.getFullPathName() + " into the project");

    relativePath = target.getFileName();
    return juce::Result::ok();
}

//==============================================================================
Autosaves::Autosaves (juce::File projectFolder) : folder (std::move (projectFolder)) {}

juce::Result Autosaves::save (const ProjectData& data)
{
    // Folder names sort by time: 20261012-143005-123.
    const auto now = juce::Time::getCurrentTime();
    auto target =
        folder.getChildFile (autosaveFolder)
            .getChildFile (now.formatted ("%Y%m%d-%H%M%S-") + juce::String (now.getMilliseconds()).paddedLeft ('0', 3));
    target = target.getNonexistentSibling (false);

    if (auto r = writeModelAndStates (target, target.getChildFile (autosaveProjectFile), data, false); r.failed())
        return r;

    auto all = list();
    for (int i = keep; i < all.size(); ++i)
        all[i].deleteRecursively();

    return juce::Result::ok();
}

juce::Array<juce::File> Autosaves::list() const
{
    auto dirs = folder.getChildFile (autosaveFolder).findChildFiles (juce::File::findDirectories, false);
    dirs.removeIf ([] (const juce::File& d) { return ! isProjectFolder (d); });

    struct NewestFirst
    {
        static int compareElements (const juce::File& a, const juce::File& b)
        {
            return b.getFileName().compareNatural (a.getFileName());
        }
    } order;

    dirs.sort (order);
    return dirs;
}

std::optional<juce::File> Autosaves::findNewerThanSaved() const
{
    const auto all = list();
    if (all.isEmpty())
        return std::nullopt;

    const auto saved = findProjectFile (folder);
    auto newest = all.getFirst();

    if (! saved.existsAsFile())
        return newest;

    // File times may have one-second resolution: an autosave from the same second as the save counts
    // if its contents differ.
    const auto autosaved = findProjectFile (newest);
    if (autosaved.getLastModificationTime() < saved.getLastModificationTime())
        return std::nullopt;

    auto sameContents = [] (const juce::File& a, const juce::File& b)
    { return a.existsAsFile() == b.existsAsFile() && (! a.existsAsFile() || a.hasIdenticalContentTo (b)); };

    if (! sameContents (autosaved, saved))
        return newest;

    const auto savedStates = folder.getChildFile (statesFolder), autosavedStates = newest.getChildFile (statesFolder);
    for (auto& state : autosavedStates.findChildFiles (juce::File::findFiles, false, "*.bin"))
        if (! sameContents (state, savedStates.getChildFile (state.getFileName())))
            return newest;

    return std::nullopt;
}

//==============================================================================
namespace
{
int currentProcessId()
{
#if JUCE_WINDOWS
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

bool isProcessRunning (int pid)
{
#if JUCE_WINDOWS
    const auto process = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (process == nullptr)
        return false;
    // Windows soon gives a finished process's number to another one: only a running Spinus counts.
    DWORD code = 0;
    wchar_t image[MAX_PATH] {};
    DWORD length = MAX_PATH;
    const bool running = GetExitCodeProcess (process, &code) != 0 && code == STILL_ACTIVE &&
                         QueryFullProcessImageNameW (process, 0, image, &length) != 0 &&
                         juce::File (juce::String (image, length)).getFileName().startsWithIgnoreCase ("spinus");
    CloseHandle (process);
    return running;
#else
    // A running process has a /proc entry (Linux).
    return juce::File ("/proc/" + juce::String (pid)).isDirectory();
#endif
}
} // namespace

SessionLock::SessionLock (const juce::File& projectFolder) : lockFile (projectFolder.getChildFile (lockName))
{
    // "flock": the lock is held as long as this Spinus runs, and the system lets it go when it ends, however
    // it ends. Process numbers cannot tell: a Flatpak has its own (its Spinus is often number 2 there).
    lockFile.replaceWithText ("pid " + juce::String (currentProcessId()) + "\nopened " +
                                  juce::Time::getCurrentTime().toISO8601 (true) + "\n" +
#if ! JUCE_WINDOWS
                                  "flock\n",
#else
                                  "",
#endif
                              false, false, "\n");
#if ! JUCE_WINDOWS
    lockDescriptor = open (lockFile.getFullPathName().toRawUTF8(), O_RDONLY | O_CLOEXEC);
    if (lockDescriptor >= 0 && flock (lockDescriptor, LOCK_EX | LOCK_NB) != 0)
    {
        close (lockDescriptor);
        lockDescriptor = -1;
    }
#endif
}

SessionLock::~SessionLock()
{
    lockFile.deleteFile();
#if ! JUCE_WINDOWS
    if (lockDescriptor >= 0)
        close (lockDescriptor);
#endif
}

SessionLock::State SessionLock::getState (const juce::File& projectFolder)
{
    const auto file = projectFolder.getChildFile (lockName);
    if (! file.existsAsFile())
        return State::none;

    juce::StringArray lines;
    file.readLines (lines);
#if ! JUCE_WINDOWS
    // A lock with an flock: open exactly while some Spinus (in a Flatpak or not) holds it.
    if (lines.contains ("flock"))
    {
        const auto descriptor = open (file.getFullPathName().toRawUTF8(), O_RDONLY | O_CLOEXEC);
        if (descriptor < 0)
            return State::leftByCrash;
        const bool held = flock (descriptor, LOCK_SH | LOCK_NB) != 0;
        close (descriptor);
        return held ? State::openInSpinus : State::leftByCrash;
    }
#endif
    for (auto& line : lines)
        if (line.startsWith ("pid "))
        {
            const auto pid = line.fromFirstOccurrenceOf ("pid ", false, false).trim().getIntValue();
            if (pid > 0 && isProcessRunning (pid))
                return State::openInSpinus;
        }

    return State::leftByCrash;
}

void SessionLock::clearCrashedLock (const juce::File& projectFolder)
{
    if (getState (projectFolder) == State::leftByCrash)
        projectFolder.getChildFile (lockName).deleteFile();
}

} // namespace spinus::project
