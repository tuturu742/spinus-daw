#pragma once

#include "Session.h"

namespace spinus
{

/** The open project as a file: which folder it lives in, whether it has unsaved
    changes, New / Open / Save / Save As, autosave every 2 minutes, the session lock, crash recovery and
    the recent projects list.

    A project that was never saved is "Untitled". Its autosaves go to a folder of its own under
    `<settings>/Unsaved/`, which is deleted when the project is saved elsewhere or closed without saving.

    Message thread only. Sends a change message when the folder, the name or the unsaved state changes.
*/
class ProjectDocument : public juce::ChangeBroadcaster, private juce::Timer
{
public:
    static constexpr int autosaveIntervalMs = 2 * 60 * 1000;
    static constexpr int maxRecentProjects = 10;

    /** settingsFolder holds the recent projects list and the Unsaved/ folder. */
    ProjectDocument (Session&, juce::File settingsFolder);
    ~ProjectDocument() override; // a clean close: removes the lock and an untitled project's autosaves

    Session& getSession() noexcept { return session; }

    bool isUntitled() const noexcept { return folder == juce::File(); }
    juce::File getFolder() const noexcept { return folder; }

    /** "Untitled", or the folder name (without the ".spinus" of a project saved in the older format). */
    juce::String getName() const;

    /** Undo steps since the last save (cheap: used for the window title). */
    bool hasUnsavedEdits() const;

    /** Unsaved edits, or a device whose settings changed since the last save (asks every plugin for
        its settings: use before closing, not on every repaint). */
    bool hasUnsavedChanges() const;

    //==============================================================================
    /** An empty project, untitled. Discards unsaved changes: ask first. */
    void newProject();

    /** Opens a project folder, or the .spn file in it. Discards unsaved changes: ask first. On failure
        nothing changes. */
    juce::Result open (const juce::File&);

    /** Saves to the current folder. Fails for an untitled project (use saveAs). */
    juce::Result save();

    /** Saves to a new folder (a chosen "Song.spn" means the folder "Song"). The project's audio files
        come along. Refuses a folder that has other things in it and is not a project. */
    juce::Result saveAs (juce::File newFolder);

    /** Saves an autosave now if anything changed since the last save or autosave. */
    juce::Result autosave();

    //==============================================================================
    /** A project whose last session crashed and whose newest autosave has work the saved project lacks. */
    struct Recovery
    {
        juce::File projectFolder; // the project, or its folder under Unsaved/ when it was untitled
        juce::File autosave;
        bool untitled = false;
        juce::String name;
        juce::Time time;
    };

    /** Looks for crashed sessions among the recent projects and the untitled projects. Newest first. */
    std::vector<Recovery> findRecoveries() const;

    /** The recovery for one project folder, if its last session crashed with unsaved work. */
    static std::optional<Recovery> findRecovery (const juce::File& projectFolder, bool untitled = false);

    /** Opens the autosave as the project (with unsaved changes, so the user decides whether to keep it). */
    juce::Result recover (const Recovery&);

    /** The user chose not to recover: forget the crash (an untitled project's folder is deleted). */
    static void discard (const Recovery&);

    //==============================================================================
    juce::Array<juce::File> getRecentProjects() const;
    void clearRecentProjects();

private:
    void timerCallback() override;
    void historyChanged();
    void addToRecent (const juce::File&);
    void closeCurrent(); // releases the lock and drops an untitled project's autosaves
    void markSaved();
    juce::Result openFolder (const juce::File&);
    juce::Result collectAudioFiles (const juce::File& projectFolder);
    juce::File getUnsavedRoot() const { return settings.getChildFile ("Unsaved"); }
    juce::File getRecentListFile() const { return settings.getChildFile ("recent-projects.txt"); }

    Session& session;
    juce::File settings;
    juce::File folder;         // invalid while untitled
    juce::File untitledFolder; // where an untitled project autosaves; created on its first autosave
    std::unique_ptr<project::SessionLock> lock;
    int historyListener = 0;

    uint64_t savedStateId = 0, autosavedStateId = 0;
    std::map<model::DeviceId, juce::MemoryBlock> savedStates, autosavedStates;
    bool lastNotifiedUnsaved = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProjectDocument)
};

} // namespace spinus
