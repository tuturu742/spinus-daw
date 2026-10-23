#include "CommandHistory.h"

#include "model/Validation.h"

namespace spinus::commands
{

namespace
{
/** Commands of one gesture (see performJoined), undone and redone together as one step. */
class JoinedSteps final : public Command
{
public:
    explicit JoinedSteps (std::unique_ptr<Command> first) { steps.push_back (std::move (first)); }

    void add (std::unique_ptr<Command> c) { steps.push_back (std::move (c)); }

    juce::String getName() const override { return steps.front()->getName(); }
    juce::String getDescription() const override { return steps.front()->getDescription(); }
    juce::var getParameters() const override { return steps.back()->getParameters(); }

    juce::Result perform (Project& p) override
    {
        for (auto& s : steps)
            if (auto r = s->perform (p); r.failed())
                return r; // cannot happen: redo replays steps that succeeded on this state
        return juce::Result::ok();
    }

    void undo (Project& p) override
    {
        for (auto it = steps.rbegin(); it != steps.rend(); ++it)
            (*it)->undo (p);
    }

    ChangeSet getChanges() const override
    {
        ChangeSet c;
        for (auto& s : steps)
            c.merge (s->getChanges());
        return c;
    }

private:
    std::vector<std::unique_ptr<Command>> steps;
};
} // namespace

CommandHistory::CommandHistory (Project initial) : project (std::move (initial)) {}

juce::Result CommandHistory::performChecked (Command& command)
{
    if (auto r = command.perform (project); r.failed())
        return r;

    // Safety net: a command must never leave the project invalid. If one does, that is a bug in the
    // command; roll it back rather than keep a broken project.
    if (auto problems = model::validate (project); ! problems.isEmpty())
    {
        jassertfalse;
        command.undo (project);
        return juce::Result::fail ("internal error in " + command.getName() + ": " + problems.joinIntoString ("; "));
    }

    return juce::Result::ok();
}

juce::Result CommandHistory::perform (std::unique_ptr<Command> command)
{
    jassert (command != nullptr);

    if (auto r = performChecked (*command); r.failed())
        return r;

    const auto changes = command->getChanges();
    undoStack.push_back (std::move (command));
    undoIds.push_back (nextStateId++);
    redoStack.clear();
    redoIds.clear();
    notify (changes);
    return juce::Result::ok();
}

bool CommandHistory::joinLastSteps (int count, const juce::String& description)
{
    if (count < 1 || (size_t)count > undoStack.size())
        return false;
    // Already performed: the transaction only holds them, to undo them together and redo them in order.
    auto joined = std::make_unique<Transaction> (description);
    const auto first = undoStack.end() - count;
    for (auto it = first; it != undoStack.end(); ++it)
        joined->add (std::move (*it));
    undoStack.erase (first, undoStack.end());
    const auto state = undoIds.back();
    undoIds.erase (undoIds.end() - count, undoIds.end());
    undoStack.push_back (std::move (joined));
    undoIds.push_back (state);
    return true;
}

juce::Result CommandHistory::performJoined (std::unique_ptr<Command> command)
{
    jassert (command != nullptr);

    if (undoStack.empty() || undoStack.back()->getName() != command->getName())
        return perform (std::move (command));

    if (auto r = performChecked (*command); r.failed())
        return r;

    auto& top = undoStack.back();
    auto* joined = dynamic_cast<JoinedSteps*> (top.get());
    if (joined == nullptr)
    {
        auto steps = std::make_unique<JoinedSteps> (std::move (top));
        joined = steps.get();
        top = std::move (steps);
    }

    const auto changes = command->getChanges();
    joined->add (std::move (command));
    undoIds.back() = nextStateId++;
    redoStack.clear();
    redoIds.clear();
    notify (changes);
    return juce::Result::ok();
}

bool CommandHistory::undo()
{
    if (undoStack.empty())
        return false;

    auto command = std::move (undoStack.back());
    undoStack.pop_back();
    redoIds.push_back (undoIds.back());
    undoIds.pop_back();
    command->undo (project);
    const auto changes = command->getChanges();
    redoStack.push_back (std::move (command));
    notify (changes);
    return true;
}

bool CommandHistory::redo()
{
    if (redoStack.empty())
        return false;

    auto command = std::move (redoStack.back());
    redoStack.pop_back();
    undoIds.push_back (redoIds.back());
    redoIds.pop_back();

    // Redo replays a command that already succeeded on this exact state, so it cannot fail.
    [[maybe_unused]] const auto result = command->perform (project);
    jassert (result.wasOk());

    const auto changes = command->getChanges();
    undoStack.push_back (std::move (command));
    notify (changes);
    return true;
}

juce::StringArray CommandHistory::getUndoDescriptions() const
{
    juce::StringArray result;
    for (auto& c : undoStack)
        result.add (c->getDescription());
    return result;
}

juce::StringArray CommandHistory::getRedoDescriptions() const
{
    juce::StringArray result;
    for (auto it = redoStack.rbegin(); it != redoStack.rend(); ++it)
        result.add ((*it)->getDescription());
    return result;
}

void CommandHistory::reset (Project newProject)
{
    project = std::move (newProject);
    undoStack.clear();
    redoStack.clear();
    undoIds.clear();
    redoIds.clear();
    baseStateId = nextStateId++;

    ChangeSet everything;
    everything.everything = true;
    notify (everything);
}

int CommandHistory::addListener (Listener l)
{
    listeners.emplace_back (nextListenerId, std::move (l));
    return nextListenerId++;
}

void CommandHistory::removeListener (int id)
{
    std::erase_if (listeners, [id] (auto& entry) { return entry.first == id; });
}

void CommandHistory::notify (const ChangeSet& changes)
{
    // Copy: a listener may add or remove listeners.
    const auto current = listeners;
    for (auto& [id, l] : current)
        l (changes);
}

} // namespace spinus::commands
