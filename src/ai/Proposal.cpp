#include "Proposal.h"

#include "commands/CommandRegistry.h"

namespace spinus::ai
{

namespace
{
/** Replaces "$<step>.<field>" strings with that step's result field. */
juce::var resolve (const juce::var& value, const std::vector<juce::var>& results, juce::String& error)
{
    if (value.isString())
    {
        const auto text = value.toString();
        if (! text.startsWithChar ('$') || ! text.containsChar ('.'))
            return value;
        const auto step = text.substring (1).upToFirstOccurrenceOf (".", false, false).getIntValue();
        const auto field = text.fromFirstOccurrenceOf (".", false, false);
        if (step < 1 || step > (int)results.size() || ! results[(size_t)step - 1].hasProperty (field))
        {
            error = "the reference " + text + " does not name an earlier step's result";
            return value;
        }
        return results[(size_t)step - 1][juce::Identifier (field)];
    }
    if (auto* array = value.getArray())
    {
        juce::Array<juce::var> copy;
        for (auto& item : *array)
            copy.add (resolve (item, results, error));
        return copy;
    }
    if (auto* object = value.getDynamicObject())
    {
        auto* copy = new juce::DynamicObject();
        for (auto& property : object->getProperties())
            copy->setProperty (property.name, resolve (property.value, results, error));
        return juce::var (copy);
    }
    return value;
}
} // namespace

Proposal Proposal::prepare (const juce::var& json, const commands::CommandHistory& history)
{
    Proposal p;
    p.source = json;
    p.summary = json["summary"].toString();
    p.checkedAgainst = history.getStateId();

    // A scratch copy: the real project is not touched.
    commands::CommandHistory scratch (history.getProject());
    std::vector<juce::var> results;
    const auto* steps = json["steps"].getArray();
    if (steps == nullptr || steps->isEmpty())
    {
        p.error = "The proposal has no steps.";
        return p;
    }

    for (int i = 0; i < steps->size(); ++i)
    {
        const auto& step = steps->getReference (i);
        Step s;
        s.command = step["command"].toString();
        juce::String error;
        s.parameters = resolve (step["parameters"], results, error);
        std::unique_ptr<commands::Command> command;
        if (error.isEmpty())
            command = commands::CommandRegistry::get().create (s.command, s.parameters, error);
        if (command != nullptr)
        {
            auto* raw = command.get();
            if (auto r = scratch.perform (std::move (command)); r.failed())
                error = r.getErrorMessage();
            else
            {
                s.description = raw->getDescription();
                results.push_back (raw->getResult());
            }
        }
        if (error.isNotEmpty())
        {
            s.description = s.command;
            p.steps.push_back (s);
            p.error = "Step " + juce::String (i + 1) + " (" + s.command + ") cannot be done: " + error;
            return p;
        }
        p.steps.push_back (s);
    }
    return p;
}

juce::Result Proposal::apply (commands::CommandHistory& history)
{
    // The project changed since the user saw the steps: check them again (IDs may differ now).
    if (history.getStateId() != checkedAgainst)
    {
        const auto name = undoName;
        *this = prepare (source, history);
        undoName = name;
    }
    if (! canApply())
        return juce::Result::fail (error.isNotEmpty() ? error : "Nothing to apply.");

    auto transaction =
        std::make_unique<commands::Transaction> (undoName.isNotEmpty() ? undoName : "Assistant: " + summary);
    for (auto& step : steps)
    {
        juce::String createError;
        auto command = commands::CommandRegistry::get().create (step.command, step.parameters, createError);
        if (command == nullptr)
            return juce::Result::fail (createError);
        transaction->add (std::move (command));
    }
    return history.perform (std::move (transaction));
}

} // namespace spinus::ai
