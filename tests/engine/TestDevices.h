#pragma once

#include "engine/DeviceFactory.h"

namespace spinus::test
{

namespace te = tracktion;

/** Stand-ins for stock devices until the real ones exist: Tracktion's built-in plugins. Third-party
    plugins (the example's Dragonfly reverb) are not installed in tests, so they stay missing. */
struct TestDevices : engine::DefaultDeviceFactory
{
    explicit TestDevices (te::Engine& engine)
    {
        engine.getPluginManager().createBuiltInType<te::ToneGeneratorPlugin>();

        auto builtIn = [] (const char* type)
        {
            return [type] (te::Edit& edit, const model::Device&)
            { return edit.getPluginCache().createNewPlugin (type, {}); };
        };

        registerStockDevice ("test.tone",
                             [] (te::Edit& edit, const model::Device&)
                             {
                                 auto p =
                                     edit.getPluginCache().createNewPlugin (te::ToneGeneratorPlugin::xmlTypeName, {});
                                 auto& tone = dynamic_cast<te::ToneGeneratorPlugin&> (*p);
                                 tone.frequency = 440.0f;
                                 tone.level = 0.25f;
                                 return p;
                             });
        registerStockDevice ("spinus.sampler", builtIn (te::FourOscPlugin::xmlTypeName));
        registerStockDevice ("bundled.surge-xt", builtIn (te::FourOscPlugin::xmlTypeName));
        registerStockDevice ("bundled.lsp.compressor-stereo", builtIn (te::CompressorPlugin::xmlTypeName));
        registerStockDevice ("spinus.reverb", builtIn (te::ReverbPlugin::xmlTypeName));
    }
};

} // namespace spinus::test
