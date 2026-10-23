#pragma once

#include "DeviceFactory.h"
#include "model/StockDevices.h"

namespace spinus::engine
{

using model::BundledDevice;
using model::findBundledDevice;
using model::getBundledDevices;

/** Registers Spinus's own devices (registerSpinusDevices) and every bundled device with the factory.
    A bundled device's plugin file is looked for in `folders`, best first (normally
    plugins::PluginDownloads::getSearchFolders: the downloaded packs, then the system's CLAP folders),
    each time one is created, so plugins downloaded while Spinus runs work at once. They are loaded
    with Spinus's CLAP host, in-process, skipping the out-of-process scan: they are pinned releases
    Spinus has tested. Creating one fails while its file is not there (the device shows as missing). */
void registerBundledDevices (DefaultDeviceFactory&, tracktion::Engine&, const std::vector<juce::File>& folders);

/** Registers Spinus's own devices (the Sampler, ...), created through the internal "Spinus" plugin
    format that EngineHost adds to the engine. */
void registerSpinusDevices (DefaultDeviceFactory&, tracktion::Engine&);

} // namespace spinus::engine
