#pragma once

#include <juce_core/juce_core.h>

namespace spinus::model
{

/** Upgrades project.json content to Project::currentSchemaVersion, one version step at a time.
    Fails for files without a schemaVersion or from a newer Spinus.

    Adding a schema version: bump Project::currentSchemaVersion, add a step here that turns version
    N - 1 into N, and add a test file for the old version under tests/model/files/. */
juce::Result migrateToCurrent (juce::var& projectJson);

} // namespace spinus::model
