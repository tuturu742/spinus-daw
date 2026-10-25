#pragma once

#include "Model.h"

namespace spinus::model
{

/** Converts a project to the JSON stored in `project.json`. The format is documented
    in docs/dev/project-format.md. Output is deterministic: the same project always gives the same text. */
juce::var toJson (const Project&);
juce::String toJsonText (const Project&);

/** Reads `project.json` content. Upgrades older schema versions (see Migrations.h), rejects newer ones
    and anything that fails validate(). Missing optional fields take their defaults. */
juce::Result fromJson (const juce::var&, Project& result);
juce::Result fromJsonText (const juce::String&, Project& result);

/** Pieces of the format, shared with the Command API's JSON parameters. The readers return an error
    message (empty on success). */
juce::var noteToJson (const Note&);
juce::String noteFromJson (const juce::var&, Note& result);
juce::var deviceToJson (const Device&);
juce::String deviceFromJson (const juce::var&, Device& result);
juce::var targetToJson (const ParameterTarget&);
juce::String targetFromJson (const juce::var&, ParameterTarget& result);
juce::var pointToJson (const AutomationPoint&);
juce::var clipToJson (const Clip&);
juce::String clipFromJson (const juce::var&, Clip& result);
juce::var tuneNoteToJson (const TuneNote&);
juce::String tuneNoteFromJson (const juce::var&, TuneNote& result);
juce::String pointFromJson (const juce::var&, AutomationPoint& result);

} // namespace spinus::model
