#include "StockDevices.h"

namespace spinus::model
{

const std::vector<BundledDevice>& getBundledDevices()
{
    static const std::vector<BundledDevice> devices {
        { "bundled.surge-xt", "Surge XT", "Surge XT.clap", "Surge XT", true, "Surge XT", "GPL-3.0", "surge-xt" },
        { "bundled.surge-xt-effects", "Surge XT Effects", "Surge XT Effects.clap", "Surge XT Effects", false,
          "Surge XT", "GPL-3.0", "surge-xt" },
        { "bundled.odin2", "Odin 2", "Odin2.clap", "Odin2", true, "Odin 2", "GPL-3.0", "odin2" },
        { "bundled.ob-xf", "OB-Xf", "OB-Xf.clap", "OB-Xf", true, "OB-Xf", "GPL-3.0", "ob-xf" },
        { "bundled.six-sines", "Six Sines", "Six Sines.clap", "Six Sines", true, "Six Sines", "GPL-3.0", "six-sines" },
        { "bundled.lsp.eq", "Parametric EQ", "lsp-plugins.clap", "Parametric Equalizer x16 Stereo", false,
          "LSP Plugins", "LGPL-3.0", "lsp-plugins" },
        { "bundled.lsp.compressor", "Compressor", "lsp-plugins.clap", "Compressor Stereo", false, "LSP Plugins",
          "LGPL-3.0", "lsp-plugins" },
        { "bundled.lsp.limiter", "Limiter", "lsp-plugins.clap", "Limiter Stereo", false, "LSP Plugins", "LGPL-3.0",
          "lsp-plugins" },
        { "bundled.lsp.gate", "Gate", "lsp-plugins.clap", "Gate Stereo", false, "LSP Plugins", "LGPL-3.0",
          "lsp-plugins" },
        { "bundled.lsp.multiband", "Multiband Compressor", "lsp-plugins.clap", "Multiband Compressor Stereo x8", false,
          "LSP Plugins", "LGPL-3.0", "lsp-plugins" },
        { "bundled.lsp.analyzer", "Spectrum Analyzer", "lsp-plugins.clap", "Spectrum Analyzer x2", false, "LSP Plugins",
          "LGPL-3.0", "lsp-plugins" },
        { "bundled.dragonfly.hall", "Hall Reverb", "DragonflyHallReverb.clap", "Dragonfly Hall Reverb", false,
          "Dragonfly Reverb", "GPL-3.0", "dragonfly-reverb" },
        { "bundled.dragonfly.room", "Room Reverb", "DragonflyRoomReverb.clap", "Dragonfly Room Reverb", false,
          "Dragonfly Reverb", "GPL-3.0", "dragonfly-reverb" },
        { "bundled.dragonfly.plate", "Plate Reverb", "DragonflyPlateReverb.clap", "Dragonfly Plate Reverb", false,
          "Dragonfly Reverb", "GPL-3.0", "dragonfly-reverb" },
        { "bundled.dragonfly.early", "Early Reflections", "DragonflyEarlyReflections.clap",
          "Dragonfly Early Reflections", false, "Dragonfly Reverb", "GPL-3.0", "dragonfly-reverb" },
        { "bundled.peakeater", "PeakEater", "peakeater.clap", "peakeater", false, "PeakEater", "GPL-3.0", "peakeater" },
        { "bundled.time12", "TIME-12", "TIME-12.vst3", "TIME-12", false, "TIME-12", "GPL-3.0", "time12" },
        { "bundled.gate12", "GATE-12", "GATE-12.vst3", "GATE-12", false, "GATE-12", "GPL-3.0", "gate12" },
    };
    return devices;
}

const std::vector<SpinusDevice>& getSpinusDevices()
{
    static const std::vector<SpinusDevice> devices {
        { "spinus.sampler", "Sampler", "spinus:sampler", true },
        { "spinus.drum-machine", "Drum Machine", "spinus:drum-machine", true },
        { "spinus.3x-osc", "3x Osc", "spinus:3x-osc", true },
        { "spinus.beat-slicer", "Beat Slicer", "spinus:beat-slicer", true },
        { "spinus.filter", "Filter", "spinus:filter", false },
        { "spinus.ducker", "Sidechain Ducker", "spinus:ducker", false },
        { "spinus.stereo-utility", "Stereo Utility", "spinus:stereo-utility", false },
        { "spinus.tune", "Spinus Tune", "spinus:tune", false },
        { "spinus.loudness-meter", "Loudness Meter", "spinus:loudness-meter", false },
        { "spinus.eq", "Spinus EQ", "spinus:eq", false },
        { "spinus.compressor", "Spinus Compressor", "spinus:compressor", false },
        { "spinus.limiter", "Spinus Limiter", "spinus:limiter", false },
        { "spinus.gate", "Spinus Gate", "spinus:gate", false },
        { "spinus.delay", "Spinus Delay", "spinus:delay", false },
    };
    return devices;
}

const SpinusDevice* findSpinusDevice (const juce::String& stockType)
{
    for (auto& d : getSpinusDevices())
        if (stockType == d.stockType)
            return &d;
    return nullptr;
}

const std::vector<StandIn>& getStandIns()
{
    static const std::vector<StandIn> standIns {};
    return standIns;
}

const StandIn* findStandIn (const juce::String& stockType)
{
    for (auto& s : getStandIns())
        if (stockType == s.stockType)
            return &s;
    return nullptr;
}

const BundledDevice* findBundledDevice (const juce::String& stockType)
{
    for (auto& d : getBundledDevices())
        if (stockType == d.stockType)
            return &d;
    return nullptr;
}

juce::String getDeviceDisplayName (const Device& d)
{
    if (d.kind == Device::Kind::plugin)
        return d.plugin.name;
    if (auto* bundled = findBundledDevice (d.stockType))
        return bundled->displayName;
    if (auto* own = findSpinusDevice (d.stockType))
        return own->displayName;
    return d.stockType;
}

} // namespace spinus::model
