#include "ProjectContext.h"

#include "model/ModelJson.h"
#include "model/StockDevices.h"
#include "session/Session.h"

namespace spinus::ai
{
namespace
{
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> properties)
{
    auto* o = new juce::DynamicObject();
    for (auto& [name, value] : properties)
        o->setProperty (name, value);
    return juce::var (o);
}

// The most settings sent for one device: plugins can have thousands, and the model reads them all.
constexpr int mostParameters = 96;
} // namespace

juce::var projectContext (Session& session, const ContextOptions& options)
{
    const auto& project = session.getProject();
    auto* context = new juce::DynamicObject();
    juce::Array<juce::var> chips;
    for (auto& chip : options.chips)
        chips.add (chip);
    context->setProperty ("chips", chips);
    if (options.project)
        context->setProperty ("project", model::toJson (project));

    auto* selection = new juce::DynamicObject();
    if (options.channel.isValid())
        selection->setProperty ("channel", (int)options.channel.value);
    if (options.pattern.isValid())
        selection->setProperty ("pattern", (int)options.pattern.value);
    if (options.insert.isValid())
        selection->setProperty ("insert", (int)options.insert.value);
    context->setProperty ("selection", juce::var (selection));

    // Device names as Spinus shows them (the project file only has types), and each device's settings as they
    // show now.
    auto* names = new juce::DynamicObject();
    auto* parameters = new juce::DynamicObject();
    const auto add = [&] (const model::Device& device)
    {
        const auto key = juce::String ((int)device.id.value);
        names->setProperty (key, model::getDeviceDisplayName (device));
        if (! options.project)
            return;
        juce::Array<juce::var> list;
        for (auto& [id, name] : session.getDeviceParameters (device.id))
        {
            if (list.size() >= mostParameters)
                break;
            list.add (object (
                { { "id", id }, { "name", name }, { "text", session.getDeviceParameterValueText (device.id, id) } }));
        }
        if (! list.isEmpty())
            parameters->setProperty (key, list);
    };
    for (auto& channel : project.channels)
        if (channel.instrument)
            add (*channel.instrument);
    const auto addEffects = [&] (const model::Insert& insert)
    {
        for (auto& slot : insert.effects)
            add (slot.device);
    };
    addEffects (project.mixer.master);
    for (auto& insert : project.mixer.inserts)
        addEffects (insert);
    context->setProperty ("deviceNames", juce::var (names));
    context->setProperty ("deviceParameters", juce::var (parameters));

    // What can be added: Spinus's devices, the free plugins and the installed plugins, each with the device
    // JSON the commands take (so a model never guesses an ID).
    juce::Array<juce::var> devices;
    for (auto& entry : session.getDeviceList().getEntries())
    {
        auto device = model::deviceToJson (entry.device);
        if (auto* o = device.getDynamicObject())
        {
            o->removeProperty ("id");
            o->removeProperty ("bypassed");
        }
        devices.add (object ({ { "name", entry.name },
                               { "maker", entry.maker },
                               { "type", entry.instrument ? "instrument" : "effect" },
                               { "format", entry.badge.isNotEmpty() ? entry.badge : juce::String ("Spinus") },
                               { "available", entry.available },
                               { "device", device } }));
    }
    context->setProperty ("devices", devices);
    return juce::var (context);
}

} // namespace spinus::ai
