#include "BundledDevices.h"

#include "dsp/SpinusPluginFormat.h"

#include "plugins/PluginDownloads.h"
#include "plugins/clap/ClapPluginFormat.h"

namespace spinus::engine
{

namespace te = tracktion;

void registerSpinusDevices (DefaultDeviceFactory& factory, te::Engine& engine)
{
    for (auto& device : model::getSpinusDevices())
        factory.registerStockDevice (
            device.stockType,
            [&engine, identifier = juce::String (device.identifier)] (te::Edit& edit,
                                                                      const model::Device&) -> te::Plugin::Ptr
            {
                const auto description = dsp::SpinusPluginFormat::findDescription (identifier);
                if (! description)
                    return nullptr;

                // Tracktion finds plugins in the engine's list.
                auto& known = engine.getPluginManager().knownPluginList;
                if (known.getTypeForIdentifierString (description->createIdentifierString()) == nullptr)
                    known.addType (*description);

                auto plugin = edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, *description);
                auto* external = dynamic_cast<te::ExternalPlugin*> (plugin.get());
                return external != nullptr && external->getAudioPluginInstance() != nullptr ? plugin : nullptr;
            });
}

void registerBundledDevices (DefaultDeviceFactory& factory, te::Engine& engine, const std::vector<juce::File>& folders)
{
    registerSpinusDevices (factory, engine);

    for (auto& device : getBundledDevices())
    {
        // A pack not published for this system (LSP Plugins on Windows): a stand-in plays its devices, if
        // there is one; otherwise they stay missing, as when not downloaded.
        if (plugins::PluginDownloads::findStandardPack (device.pack) == nullptr)
        {
            if (const auto* standIn = model::findStandIn (device.stockType))
                factory.registerStockDevice (
                    device.stockType,
                    [type = juce::String (standIn->builtIn)] (te::Edit& edit, const model::Device&)
                    {
                        auto v = te::createValueTree (te::IDs::PLUGIN, te::IDs::type, type);
                        return edit.getPluginCache().createNewPlugin (v);
                    });
            continue;
        }

        factory.registerStockDevice (
            device.stockType,
            [&engine, folders, file = juce::String (device.file),
             name = juce::String (device.pluginName)] (te::Edit& edit, const model::Device&) -> te::Plugin::Ptr
            {
                const auto located = plugins::PluginDownloads::findFile (folders, file);
                if (located == juce::File())
                    return nullptr; // not downloaded (yet)

                auto& known = engine.getPluginManager().knownPluginList;
                const auto& path = located.getFullPathName();

                auto find = [&]() -> std::optional<juce::PluginDescription>
                {
                    for (auto& type : known.getTypes())
                        if (type.fileOrIdentifier == path && type.name == name)
                            return type;
                    return std::nullopt;
                };

                // Describe the plugin file the first time one of its devices is used.
                if (! find())
                {
                    plugins::ClapPluginFormat clap;
                    juce::OwnedArray<juce::PluginDescription> found;
                    clap.findAllTypesForFile (found, path);
                    for (auto* d : found)
                        known.addType (*d);
                }

                const auto description = find();
                if (! description)
                    return nullptr;

                auto plugin = edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, *description);
                auto* external = dynamic_cast<te::ExternalPlugin*> (plugin.get());
                return external != nullptr && external->getAudioPluginInstance() != nullptr ? plugin : nullptr;
            });
    }
}

} // namespace spinus::engine
