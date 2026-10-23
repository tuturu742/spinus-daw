#include "BuiltInPresets.h"

#include "commands/CommandRegistry.h"
#include "dsp/threeosc/ThreeOscProcessor.h"

namespace spinus
{

namespace te = tracktion;

namespace
{
dsp::ThreeOscProcessor* threeOscOf (te::Plugin* plugin)
{
    auto* external = dynamic_cast<te::ExternalPlugin*> (plugin);
    return external != nullptr ? dynamic_cast<dsp::ThreeOscProcessor*> (external->getAudioPluginInstance()) : nullptr;
}
} // namespace

juce::Result loadBuiltInPreset (Session& session, model::DeviceId device, const juce::String& preset)
{
    const auto* existing = session.getProject().findDevice (device);
    if (existing == nullptr)
        return juce::Result::fail ("There is no such instrument.");

    // The project already has these settings (the same preset again): no command, so no empty undo step.
    const auto* found = dsp::threeOscPresets::find (preset);
    const auto values = dsp::threeOscPresets::values (found);
    const bool same =
        existing->kind == model::Device::Kind::stock && existing->stockType == "spinus.3x-osc" &&
        (found != nullptr || preset.trim().equalsIgnoreCase (dsp::threeOscPresets::defaultName)) &&
        std::all_of (values.begin(), values.end(),
                     [existing] (const auto& v)
                     {
                         const auto it = existing->parameters.find (v.first);
                         return it != existing->parameters.end() && juce::approximatelyEqual (it->second, v.second);
                     });
    if (! same)
    {
        auto* parameters = new juce::DynamicObject();
        parameters->setProperty ("device", (int)device.value);
        parameters->setProperty ("preset", preset);
        juce::String error;
        auto command = commands::CommandRegistry::get().create ("device.loadPreset", juce::var (parameters), error);
        if (command == nullptr)
            return juce::Result::fail (error);
        if (auto r = session.getHistory().perform (std::move (command)); r.failed())
            return r;
    }

    // Controls turned in the window since: the preset's values too.
    if (auto* synth = threeOscOf (session.getAdapter().getDevicePlugin (device)))
        for (auto& [id, value] : values)
            if (auto* parameter = synth->getParameterState().getParameter (id);
                parameter != nullptr && std::abs (parameter->getValue() - value) > 0.0005f)
                parameter->setValueNotifyingHost (value);
    return juce::Result::ok();
}

void connectBuiltInPresets (Session& session, te::Plugin& plugin)
{
    auto* synth = threeOscOf (&plugin);
    const auto device = session.getAdapter().findDevice (plugin);
    if (synth == nullptr || ! device)
        return;
    synth->onChoosePreset = [&session, device = *device] (const juce::String& preset)
    {
        [[maybe_unused]] const auto result = loadBuiltInPreset (session, device, preset);
        jassert (result.wasOk());
    };
}

} // namespace spinus
