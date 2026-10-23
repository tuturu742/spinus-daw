#include "Migrations.h"

#include "Model.h"

#include <functional>

namespace spinus::model
{

namespace
{
/** One step per schema version: steps[n] upgrades version n + 1 to n + 2. None exist yet. */
const std::vector<std::function<void (juce::var&)>> steps;
} // namespace

juce::Result migrateToCurrent (juce::var& projectJson)
{
    const auto version = projectJson.getProperty ("schemaVersion", {});

    if (! (version.isInt() || version.isInt64()))
        return juce::Result::fail ("project.json has no schemaVersion");

    auto v = (int)version;

    if (v > Project::currentSchemaVersion)
        return juce::Result::fail ("this project was saved by a newer version of Spinus (schema " + juce::String (v) +
                                   "); this version reads up to schema " +
                                   juce::String (Project::currentSchemaVersion));

    if (v < 1)
        return juce::Result::fail ("unknown schemaVersion " + juce::String (v));

    if (v < Project::currentSchemaVersion)
        projectJson = projectJson.clone(); // never modify the caller's object

    for (; v < Project::currentSchemaVersion; ++v)
    {
        steps[(size_t)v - 1](projectJson);
        projectJson.getDynamicObject()->setProperty ("schemaVersion", v + 1);
    }

    return juce::Result::ok();
}

} // namespace spinus::model
