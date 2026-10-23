#pragma once

#include "Command.h"

#include <functional>

namespace spinus::commands
{

/** Owns a project and its undo/redo history. The only way to change the project.

    Message thread only. Listeners are told about every change (perform, undo and redo) with the
    command's ChangeSet.
*/
class CommandHistory
{
public:
    explicit CommandHistory (Project initial = {});

    const Project& getProject() const noexcept { return project; }

    /** Performs the command; on success it becomes the newest undo step and the redo list is cleared.
        On failure the project is unchanged and the command is discarded. */
    juce::Result perform (std::unique_ptr<Command>);

    /** Performs the command and joins it to the newest undo step if that step is a command of the
        same name (or such commands already joined): every move of one fader drag is applied, so it is
        heard, but the whole drag is one undo step. Otherwise behaves like perform(). */
    juce::Result performJoined (std::unique_ptr<Command>);

    /** Makes the newest `count` undo steps one step with this description (several recorded takes placed
        one by one, undone together). False if there are fewer steps. */
    bool joinLastSteps (int count, const juce::String& description);

    bool canUndo() const noexcept { return ! undoStack.empty(); }
    bool canRedo() const noexcept { return ! redoStack.empty(); }
    bool undo();
    bool redo();

    /** Descriptions of the undo steps, oldest first (for the History panel). */
    juce::StringArray getUndoDescriptions() const;
    juce::StringArray getRedoDescriptions() const;

    /** Drops all history, e.g. after loading another project. */
    void reset (Project newProject);

    /** Identifies the project's current state: it changes with every perform, undo and redo, and
        returns to an earlier value when undo or redo returns to that state. A saved project compares
        it with the value at save time to know whether there are unsaved changes. */
    uint64_t getStateId() const noexcept { return undoIds.empty() ? baseStateId : undoIds.back(); }

    using Listener = std::function<void (const ChangeSet&)>;

    /** Listeners are called in the order they were added. Returns an ID for removeListener(). */
    int addListener (Listener);
    void removeListener (int id);

private:
    juce::Result performChecked (Command&);
    void notify (const ChangeSet&);

    Project project;
    std::vector<std::unique_ptr<Command>> undoStack, redoStack;
    std::vector<uint64_t> undoIds, redoIds; // the state each step leads to, parallel to the stacks
    uint64_t nextStateId = 1, baseStateId = nextStateId++;
    std::vector<std::pair<int, Listener>> listeners;
    int nextListenerId = 1;
};

} // namespace spinus::commands
