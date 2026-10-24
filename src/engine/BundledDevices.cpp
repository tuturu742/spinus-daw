#include "BundledDevices.h"

#include "dsp/SpinusPluginFormat.h"

#include "plugins/PluginDownloads.h"
#include "plugins/clap/ClapPluginFormat.h"

#include <algorithm>
#include <mutex>

namespace spinus::engine
{

namespace te = tracktion;

namespace
{
/** The bundled devices created so far, while they last (Tracktion clears the weak references). */
struct OpenDevice
{
    juce::File file;
    te::Selectable::WeakRef plugin;
};

struct OpenDevices
{
    std::mutex mutex;
    std::vector<OpenDevice> devices;
};

OpenDevices& getOpenDevices()
{
    static OpenDevices open;
    return open;
}

void noteOpen (const juce::File& file, te::Plugin& plugin)
{
    auto& open = getOpenDevices();
    const std::scoped_lock lock (open.mutex);
    // Forget the ones gone (the Analyzer makes and drops many), then note this one.
    open.devices.erase (std::remove_if (open.devices.begin(), open.devices.end(),
                                        [] (const OpenDevice& d) { return d.plugin == nullptr; }),
                        open.devices.end());
    open.devices.push_back ({ file, &plugin });
}
} // namespace

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

                // Describe the plugin file the first time one of its devices is used: CLAP, or VST3 for the
                // projects that publish no CLAP build.
                if (! find())
                {
                    juce::OwnedArray<juce::PluginDescription> found;
                    if (file.endsWithIgnoreCase (".vst3"))
                        juce::VST3PluginFormat().findAllTypesForFile (found, path);
                    else
                        plugins::ClapPluginFormat().findAllTypesForFile (found, path);
                    for (auto* d : found)
                        known.addType (*d);
                }

                const auto description = find();
                if (! description)
                    return nullptr;

                auto plugin = edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, *description);
                auto* external = dynamic_cast<te::ExternalPlugin*> (plugin.get());
                if (external == nullptr || external->getAudioPluginInstance() == nullptr)
                    return nullptr;
                noteOpen (located, *external);
                return plugin;
            });
    }
}

bool isBundledDeviceOpenFrom (const juce::File& folder)
{
    auto& open = getOpenDevices();
    const std::scoped_lock lock (open.mutex);
    return std::any_of (open.devices.begin(), open.devices.end(),
                        [&folder] (const OpenDevice& d)
                        {
                            auto* external = dynamic_cast<te::ExternalPlugin*> (d.plugin.get());
                            return external != nullptr && external->getAudioPluginInstance() != nullptr &&
                                   d.file.isAChildOf (folder);
                        });
}

} // namespace spinus::engine
