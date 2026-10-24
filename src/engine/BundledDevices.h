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
    plugins::PluginDownloads::getSearchFolders: the downloaded packs, then the system's CLAP and VST3
    folders), each time one is created, so plugins downloaded while Spinus runs work at once. They are
    loaded with Spinus's CLAP host (or JUCE's VST3 host, for a project that publishes no CLAP build),
    in-process, skipping the out-of-process scan: they are pinned releases Spinus has tested. Creating
    one fails while its file is not there (the device shows as missing). */
void registerBundledDevices (DefaultDeviceFactory&, tracktion::Engine&, const std::vector<juce::File>& folders);

/** Whether a bundled device created by registerBundledDevices' factory, from a plugin file inside `folder`,
    still has its plugin open (in any edit: the song's, an export's, the Analyzer's hidden ones). Spinus's
    CLAP host knows its own open files; this covers the VST3 plugins too, which JUCE may keep mapped after
    the last copy closes. Message thread. */
bool isBundledDeviceOpenFrom (const juce::File& folder);

/** Registers Spinus's own devices (the Sampler, ...), created through the internal "Spinus" plugin
    format that EngineHost adds to the engine. */
void registerSpinusDevices (DefaultDeviceFactory&, tracktion::Engine&);

} // namespace spinus::engine
