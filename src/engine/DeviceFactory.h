#pragma once

#include "model/Model.h"

#include <tracktion_engine/tracktion_engine.h>

#include <functional>
#include <set>

namespace spinus::engine
{

/** Creates the Tracktion plugin that plays a model Device (instrument or effect). */
class DeviceFactory
{
public:
    virtual ~DeviceFactory() = default;

    /** Returns nullptr when the device cannot be created (unknown stock type, plugin not installed);
        the Engine Adapter then leaves the slot empty and the model keeps the device. */
    virtual tracktion::Plugin::Ptr create (tracktion::Edit&, const model::Device&) = 0;
};

/** Stock devices come from a registry of creators (filled by src/dsp/ and the bundled-plugin setup);
    third-party plugins are found in the engine's KnownPluginList by format, ID, file and name. */
class DefaultDeviceFactory : public DeviceFactory
{
public:
    using Creator = std::function<tracktion::Plugin::Ptr (tracktion::Edit&, const model::Device&)>;

    void registerStockDevice (const juce::String& stockType, Creator);

    tracktion::Plugin::Ptr create (tracktion::Edit&, const model::Device&) override;

    /** Third-party plugins this says yes to run in a separate process: through the engine's
        "Spinus Sandbox" plugin format, with the real plugin in spinus-plugin-host. */
    void setRunSeparately (std::function<bool (const juce::PluginDescription&)> f) { runSeparately = std::move (f); }

private:
    /** Logs that a device could not be made (once per device: the adapter asks again on every change). */
    void noteFailure (const model::Device&, const juce::String& why);

    std::map<juce::String, Creator> stockCreators;
    std::function<bool (const juce::PluginDescription&)> runSeparately;
    std::set<juce::String> reported; // devices whose failure is in the log already
};

} // namespace spinus::engine
