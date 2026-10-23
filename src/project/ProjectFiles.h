#pragma once

#include "model/Model.h"

#include <map>

namespace spinus::project
{

/** Everything a project folder stores: the model plus each device's binary state. */
struct ProjectData
{
    model::Project project;
    std::map<model::DeviceId, juce::MemoryBlock> deviceStates;
};

/*  A project is a folder `SongName/`:

        SongName.spn            the model (docs/dev/project-format.md); opening it opens the project
        plugin-states/<id>.bin  one file per device state
        audio/                  audio files the project uses
        cache/                  AI results, waveform peaks: never required, safe to delete
        .autosave/<time>/       the last 10 autosaves (project.spn + plugin-states/)
        .session.lock           present while a Spinus session has the project open

    Saving is crash-safe: every file is written to a temporary file and renamed into place, plugin
    states before the .spn file, and stale state files are removed last. At any moment the folder
    holds a loadable project.
*/

inline constexpr const char* projectFileExtension = ".spn";

/** The project's name: its folder's name (without the ".spinus" of a project saved in the older format). */
juce::String projectName (const juce::File& folder);

/** The project file in a folder: `<folder name>.spn`, else another .spn (the folder was renamed), else
    the `project.json` of a project saved in the older format. A nonexistent file if there is none. */
juce::File findProjectFile (const juce::File& folder);

/** The project folder a chosen path means: the folder itself, or the folder holding a .spn (or
    project.json) file. */
juce::File projectFolderFor (const juce::File& chosen);

/** True if the folder holds a project file. */
inline bool isProjectFolder (const juce::File& folder) { return findProjectFile (folder).existsAsFile(); }

juce::Result saveProject (const juce::File& folder, const ProjectData&);
juce::Result loadProject (const juce::File& folder, ProjectData& result);

/** Copies an audio file into the project's audio/ folder (renaming on a name clash) and returns the
    path to store in AudioClip::file, relative to audio/. */
juce::Result importAudioFile (const juce::File& folder, const juce::File& source, juce::String& relativePath);

//==============================================================================
/** Autosaves: every 2 minutes into .autosave/, keeping the newest 10. */
class Autosaves
{
public:
    static constexpr int keep = 10;

    explicit Autosaves (juce::File projectFolder);

    juce::Result save (const ProjectData&);

    /** Autosave folders, newest first. */
    juce::Array<juce::File> list() const;

    /** The newest autosave, if it is newer than the saved project file (worth offering to recover). */
    std::optional<juce::File> findNewerThanSaved() const;

private:
    juce::File folder;
};

/** Marks a project as open in a session. The lock names the process that holds it. If the lock is
    still there when the project is opened again and that process is gone, the previous session crashed
    and Spinus offers the newest autosave. */
class SessionLock
{
public:
    explicit SessionLock (const juce::File& projectFolder);
    ~SessionLock(); // removes the lock: a clean close

    enum class State
    {
        none,         // closed cleanly (or never opened)
        openInSpinus, // a running Spinus has it open (this one or another)
        leftByCrash   // the session that opened it ended without closing it
    };

    static State getState (const juce::File& projectFolder);

    /** True if a previous session left its lock behind (call before creating a SessionLock). */
    static bool previousSessionCrashed (const juce::File& projectFolder)
    {
        return getState (projectFolder) == State::leftByCrash;
    }

    /** Removes a lock left by a crash, once the user has chosen whether to recover. */
    static void clearCrashedLock (const juce::File& projectFolder);

private:
    juce::File lockFile;
    int lockDescriptor = -1; // Linux: holds an flock while the project is open
};

} // namespace spinus::project
