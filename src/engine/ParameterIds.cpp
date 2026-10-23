#include "ParameterIds.h"

namespace spinus::engine
{

juce::String getParameterId (const juce::AudioProcessorParameter& parameter)
{
    // As tracktion::ExternalPlugin::buildParameterList names its automatable parameters (a test checks they
    // agree): a parameter with a string ID of its own keeps it, any other is known by its position.
    if (auto* withId = dynamic_cast<const juce::AudioProcessorParameterWithID*> (&parameter))
        return withId->paramID;
    return juce::String (parameter.getParameterIndex());
}

juce::String getPluginOwnParameterId (const juce::AudioProcessorParameter& parameter)
{
    if (auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*> (&parameter))
        return hosted->getParameterID();
    return {};
}

ParameterFinder::ParameterFinder (const juce::AudioProcessor& processor)
{
    for (auto* parameter : processor.getParameters())
    {
        bySpinusId.try_emplace (getParameterId (*parameter), parameter);
        if (auto own = getPluginOwnParameterId (*parameter); own.isNotEmpty())
            byPluginId.try_emplace (own, parameter);
    }
}

juce::AudioProcessorParameter* ParameterFinder::find (const juce::String& id) const
{
    auto it = bySpinusId.find (id);
    return it != bySpinusId.end() ? it->second : nullptr;
}

juce::AudioProcessorParameter* ParameterFinder::findSaved (const juce::String& key, bool* legacy) const
{
    if (legacy != nullptr)
        *legacy = false;
    if (auto* parameter = find (key))
        return parameter;
    auto it = byPluginId.find (key);
    if (it == byPluginId.end())
        return nullptr;
    if (legacy != nullptr)
        *legacy = true;
    return it->second;
}

std::map<juce::String, float> readParameterValues (const juce::AudioProcessor& processor)
{
    std::map<juce::String, float> values;
    for (auto* parameter : processor.getParameters())
        values[getParameterId (*parameter)] = juce::jlimit (0.0f, 1.0f, parameter->getValue());
    return values;
}

} // namespace spinus::engine
