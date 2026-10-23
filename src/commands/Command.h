#pragma once

#include "model/Model.h"

#include <memory>
#include <set>

namespace spinus::commands
{

using namespace spinus::model;

/** What a command touched, so the Engine Adapter and the UI can update only that. */
struct ChangeSet
{
    bool everything = false; // e.g. a different project was loaded
    bool settings = false;
    bool channelOrder = false;
    bool insertOrder = false;
    bool laneOrder = false;
    bool launcher = false; // anything in the clip launcher
    bool markers = false;  // the Playlist's markers
    std::set<ChannelId> channels;
    std::set<PatternId> patterns;
    std::set<InsertId> inserts;
    std::set<LaneId> lanes;
    std::set<ClipId> clips;

    void merge (const ChangeSet& other);
    bool isEmpty() const;
};

/** One change to a project. Every mutation of the model goes through a Command.

    perform() checks its parameters against the project and either applies the whole change or
    nothing. undo() restores exactly the state before perform(); perform() may be called again after
    undo() (redo) and then reuses the IDs it allocated the first time, so later commands that refer to
    them stay valid.
*/
class Command
{
public:
    virtual ~Command() = default;

    /** The command's name in the schema, e.g. "channel.add". */
    virtual juce::String getName() const = 0;

    /** For the History panel, e.g. "Add channel 'Kick'". */
    virtual juce::String getDescription() const = 0;

    /** The parameters as JSON, as accepted by CommandRegistry::create(). */
    virtual juce::var getParameters() const = 0;

    virtual juce::Result perform (Project&) = 0;
    virtual void undo (Project&) = 0;

    /** What perform() and undo() touch. Valid after the first successful perform(). */
    virtual ChangeSet getChanges() const = 0;

    /** IDs of objects the command created, for callers such as the AI service (empty object if none). */
    virtual juce::var getResult() const { return juce::var (new juce::DynamicObject()); }
};

/** Several commands applied as one undo step (AGENTS rule 3: one AI proposal = one step). */
class Transaction final : public Command
{
public:
    explicit Transaction (juce::String description);

    void add (std::unique_ptr<Command>);
    int size() const noexcept { return (int)commands.size(); }

    juce::String getName() const override { return "transaction"; }
    juce::String getDescription() const override { return description; }
    juce::var getParameters() const override;
    juce::Result perform (Project&) override;
    void undo (Project&) override;
    ChangeSet getChanges() const override;
    juce::var getResult() const override;

private:
    juce::String description;
    std::vector<std::unique_ptr<Command>> commands;
};

} // namespace spinus::commands
