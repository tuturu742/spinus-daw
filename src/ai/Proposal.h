#pragma once

#include "commands/CommandHistory.h"

namespace spinus::ai
{

/** A change the assistant proposes: Spinus commands to apply or reject.

    prepare() checks it without touching the project: each step is created through the CommandRegistry
    and performed on a copy, with references to earlier steps' results ("$1.channel") filled in, and
    described for the user. apply() performs the steps on the real project as one undo step. Nothing is
    applied without the user (AGENTS rule 3). */
struct Proposal
{
    struct Step
    {
        juce::String command;
        juce::var parameters; // with references filled in
        juce::String description;
    };

    juce::String summary;
    std::vector<Step> steps;
    juce::String error;    // why it cannot be applied (empty: it can)
    juce::String undoName; // the undo step's name; "Assistant: <summary>" when empty
    uint64_t checkedAgainst = 0;
    /** The project as it would be with the steps done (the copy prepare() checked them on), to listen to before
        applying; the project as it was when the proposal cannot be applied. */
    model::Project preview;

    bool canApply() const noexcept { return error.isEmpty() && ! steps.empty(); }

    /** From the service's {summary, steps: [{command, parameters}]}. */
    static Proposal prepare (const juce::var& json, const commands::CommandHistory&);

    /** Performs the steps as one undo step ("Assistant: <summary>"). If the project changed since prepare(),
        checks the proposal again first. */
    juce::Result apply (commands::CommandHistory&);

private:
    juce::var source;
};

} // namespace spinus::ai
