#pragma once

#include "Model.h"

namespace spinus::model
{

/** One device Spinus offers from an open-source plugin it downloads on request.
    Plain data, so commands and the UI can name devices; engine/BundledDevices loads them. */
struct BundledDevice
{
    const char* stockType;   // stored in projects, e.g. "bundled.surge-xt"
    const char* displayName; // shown in the browser and the channel rack
    const char* file;        // in the bundled plugins folder
    const char* pluginName;  // the plugin's own name inside that file
    bool isInstrument;
    const char* project; // who made it: shown in the browser, e.g. "Dragonfly Reverb"
    const char* license;
    const char* pack; // the download that provides it (plugins::PluginDownloads), e.g. "dragonfly-reverb"
};

/** A device Spinus makes itself, hosted through its internal plugin format. */
struct SpinusDevice
{
    const char* stockType;   // stored in projects, e.g. "spinus.sampler"
    const char* displayName; // e.g. "Sampler"
    const char* identifier;  // the internal format's identifier, e.g. "spinus:sampler"
    bool isInstrument;
};

const std::vector<SpinusDevice>& getSpinusDevices();
const SpinusDevice* findSpinusDevice (const juce::String& stockType);

/** A device of Tracktion Engine's own standing in for a bundled one on a system its project publishes no
    build for (LSP Plugins has no Windows build). Projects keep the bundled device's stock type, so the same
    project opens on every system; only what plays it differs. */
struct StandIn
{
    const char* stockType; // the bundled device it stands in for, e.g. "bundled.lsp.eq"
    const char* builtIn;   // a Tracktion built-in plugin's type name, e.g. "4bandEq"
};

/** The stand-ins on this system (none: no Tracktion effects are chosen to stand in for LSP's on Windows,
    so those devices are listed in the manual's Known issues). */
const std::vector<StandIn>& getStandIns();
const StandIn* findStandIn (const juce::String& stockType);

/** The bundled devices Spinus offers, in browser order. */
const std::vector<BundledDevice>& getBundledDevices();
const BundledDevice* findBundledDevice (const juce::String& stockType);

/** What the UI calls a device: a bundled device's display name, a plugin's name, or its stock type. */
juce::String getDeviceDisplayName (const Device&);

} // namespace spinus::model
