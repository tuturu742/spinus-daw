#include "HiddenInstrument.h"

#include "engine/PluginCrashGuard.h"
#include "plugins/SandboxList.h"

namespace spinus
{

namespace te = tracktion;

namespace
{
model::Device stockDevice (const juce::String& stockType)
{
    model::Device device;
    device.kind = model::Device::Kind::stock;
    device.stockType = stockType;
    return device;
}
} // namespace

HiddenInstrument::HiddenInstrument (Session& session, const juce::String& stockType)
    : HiddenInstrument (session, stockDevice (stockType))
{
}

HiddenInstrument::HiddenInstrument (Session& session, const model::Device& device)
{
    edit = te::Edit::createSingleTrackEdit (session.getEdit().engine);
    // A plugin's libraries are noted, as for the song's own devices, so a crash in them names it.
    const engine::PluginCrashGuard::ModuleWatch watch;
    plugin = session.getDeviceFactory().create (*edit, device);
    if (plugin != nullptr && device.kind == model::Device::Kind::plugin)
        watch.addNewModules (plugins::SandboxList::keyOf (device.plugin.format, device.plugin.fileOrIdentifier),
                             device.plugin.name, device.plugin.fileOrIdentifier);
    if (auto* external = dynamic_cast<te::ExternalPlugin*> (plugin.get()))
        instance = external->getAudioPluginInstance();
    if (instance == nullptr)
    {
        problem = "The instrument (" +
                  (device.kind == model::Device::Kind::stock ? device.stockType : device.plugin.name) +
                  ") is not installed.";
        return;
    }
    player = std::make_unique<OfflineInstrument> (*instance);
}

HiddenInstrument::~HiddenInstrument()
{
    player = nullptr; // releases the plugin's resources
    plugin = nullptr;
    edit = nullptr;
}

juce::File HiddenInstrument::getPluginFile() const
{
    return instance != nullptr ? juce::File (instance->getPluginDescription().fileOrIdentifier) : juce::File();
}

juce::Result HiddenInstrument::loadPreset (const juce::File& file)
{
    InstrumentPreset preset;
    preset.kind = InstrumentPreset::Kind::file;
    preset.name = file.getFileNameWithoutExtension();
    preset.file = file;
    return loadPreset (preset);
}

juce::Result HiddenInstrument::loadPreset (const InstrumentPreset& preset)
{
    if (player == nullptr)
        return juce::Result::fail (problem);
    return player->loadPreset (preset);
}

juce::AudioBuffer<float> HiddenInstrument::render (const std::vector<Note>& notes, double tail)
{
    if (player == nullptr)
        return {};
    return player->render (notes, tail);
}

} // namespace spinus
