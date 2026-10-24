#include "DeviceFactory.h"

#include "core/Log.h"
#include "plugins/sandbox/SandboxPluginFormat.h"
#include "session/ProgramFolder.h"

namespace spinus::engine
{

namespace te = tracktion;

void DefaultDeviceFactory::registerStockDevice (const juce::String& stockType, Creator creator)
{
    stockCreators[stockType] = std::move (creator);
}

te::Plugin::Ptr DefaultDeviceFactory::create (te::Edit& edit, const model::Device& device)
{
    if (device.kind == model::Device::Kind::stock)
    {
        auto it = stockCreators.find (device.stockType);
        auto plugin = it != stockCreators.end() ? it->second (edit, device) : nullptr;
        if (plugin == nullptr)
            noteFailure (device, it == stockCreators.end() ? "not part of this Spinus" : "could not be made");
        return plugin;
    }

    const auto& ref = device.plugin;
    const auto types = edit.engine.getPluginManager().knownPluginList.getTypes();

    // Best match first: same format and file (the exact plugin the project used), then same format and
    // unique ID (moved or reinstalled elsewhere), then same format and name.
    auto find = [&] (auto&& matches) -> const juce::PluginDescription*
    {
        for (auto& type : types)
            if (type.pluginFormatName == ref.format && matches (type))
                return &type;
        return nullptr;
    };

    const juce::PluginDescription* match = nullptr;
    if (ref.fileOrIdentifier.isNotEmpty())
        // Compared as the computer's paths: a project saved in the Flatpak (/run/host/usr/...) opens natively
        // (/usr/...), and the other way round.
        match = find (
            [&] (auto& t)
            {
                return toPortablePluginPath (t.fileOrIdentifier) == toPortablePluginPath (ref.fileOrIdentifier) &&
                       t.name == ref.name;
            });
    if (match == nullptr && ref.uniqueId != 0)
        match = find ([&] (auto& t) { return t.uniqueId == ref.uniqueId && t.name == ref.name; });
    if (match == nullptr)
        match = find ([&] (auto& t) { return t.name == ref.name && t.manufacturerName == ref.manufacturer; });

    if (match == nullptr)
    {
        noteFailure (device, "not installed here (or not scanned): its slot shows it as Missing");
        return nullptr;
    }

    // In its own process: Tracktion finds plugins in its known list, so the sandboxed twin goes there too.
    auto description = *match;
    const auto formats = edit.engine.getPluginManager().pluginFormatManager.getFormats();
    if (runSeparately && runSeparately (*match) &&
        std::any_of (formats.begin(), formats.end(),
                     [] (auto* f) { return f->getName() == plugins::sandbox::SandboxPluginFormat::formatName; }))
    {
        description = plugins::sandbox::SandboxPluginFormat::describe (*match);
        edit.engine.getPluginManager().knownPluginList.addType (description);
    }

    auto plugin = edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, description);
    auto* external = dynamic_cast<te::ExternalPlugin*> (plugin.get());
    if (external == nullptr || external->getAudioPluginInstance() == nullptr)
    {
        const auto why = external != nullptr ? external->getLoadError() : juce::String();
        noteFailure (device, "did not load (" + description.fileOrIdentifier + ")" +
                                 (why.isNotEmpty() ? ": " + why : juce::String()));
        return nullptr;
    }
    return plugin;
}

void DefaultDeviceFactory::noteFailure (const model::Device& device, const juce::String& why)
{
    const auto what = device.kind == model::Device::Kind::stock
                          ? device.stockType
                          : device.plugin.name + " (" + device.plugin.format + ")";
    if (reported.insert (what + why).second)
        core::Logs::warning ("plugin", what + " " + why);
}

} // namespace spinus::engine
