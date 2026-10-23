#include "Command.h"

namespace spinus::commands
{

void ChangeSet::merge (const ChangeSet& other)
{
    everything |= other.everything;
    settings |= other.settings;
    channelOrder |= other.channelOrder;
    insertOrder |= other.insertOrder;
    laneOrder |= other.laneOrder;
    launcher |= other.launcher;
    markers |= other.markers;
    channels.insert (other.channels.begin(), other.channels.end());
    patterns.insert (other.patterns.begin(), other.patterns.end());
    inserts.insert (other.inserts.begin(), other.inserts.end());
    lanes.insert (other.lanes.begin(), other.lanes.end());
    clips.insert (other.clips.begin(), other.clips.end());
}

bool ChangeSet::isEmpty() const
{
    return ! (everything || settings || channelOrder || insertOrder || laneOrder || launcher || markers) &&
           channels.empty() && patterns.empty() && inserts.empty() && lanes.empty() && clips.empty();
}

//==============================================================================
Transaction::Transaction (juce::String d) : description (std::move (d)) {}

void Transaction::add (std::unique_ptr<Command> c) { commands.push_back (std::move (c)); }

juce::var Transaction::getParameters() const
{
    juce::Array<juce::var> list;

    for (auto& c : commands)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("command", c->getName());
        o->setProperty ("parameters", c->getParameters());
        list.add (juce::var (o));
    }

    auto* o = new juce::DynamicObject();
    o->setProperty ("description", description);
    o->setProperty ("commands", list);
    return juce::var (o);
}

juce::Result Transaction::perform (Project& project)
{
    for (size_t i = 0; i < commands.size(); ++i)
    {
        if (auto r = commands[i]->perform (project); r.failed())
        {
            for (auto j = i; j > 0; --j)
                commands[j - 1]->undo (project);

            return juce::Result::fail ("step " + juce::String ((int)i + 1) + " (" + commands[i]->getName() +
                                       ") failed: " + r.getErrorMessage());
        }
    }

    return juce::Result::ok();
}

void Transaction::undo (Project& project)
{
    for (auto it = commands.rbegin(); it != commands.rend(); ++it)
        (*it)->undo (project);
}

ChangeSet Transaction::getChanges() const
{
    ChangeSet all;
    for (auto& c : commands)
        all.merge (c->getChanges());
    return all;
}

juce::var Transaction::getResult() const
{
    juce::Array<juce::var> results;
    for (auto& c : commands)
        results.add (c->getResult());

    auto* o = new juce::DynamicObject();
    o->setProperty ("steps", results);
    return juce::var (o);
}

} // namespace spinus::commands
