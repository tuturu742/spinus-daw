// "Rainy Window": a lo-fi hip hop demo song (82 BPM, D minor), made only through the Command API, as a user makes one,
// on Spinus's own instruments and effects, Surge XT and Dragonfly Reverb, with sounds synthesised for it by
// make_audio.py. It reuses the showcase song's helpers (tools/showcase-song: the Song builder and the
// cleaning of plugin states) and is shipped as demo/Rainy Window (demo/README.md says how to make it again).
//
//   spinus_demo_lofi --plugins <folder> --probe
//   spinus_demo_lofi --plugins <folder> --audio <folder from make_audio.py> --make <project folder>
//   spinus_demo_lofi --plugins <folder> --render <project folder> --wav <file> [--stems <folder>]
//
// --render exports the song (48 kHz, 24-bit) and, with --stems, one file per mixer insert; with LOFI_PARAMETERS=1
// it first prints every Spinus device's and effect's settings as the devices show them.
//
// <folder> holds the free plugins (Surge XT.clap, the Dragonfly reverbs ...), e.g. a development build's
// plugins/ folder. Run it with HOME set to a scratch folder: Tracktion keeps its settings under ~/.config.

#include "commands/CommandRegistry.h"
#include "dsp/drums/DrumMachineProcessor.h"
#include "dsp/sampler/SamplerProcessor.h"
#include "engine/BundledDevices.h"
#include "engine/DeviceFactory.h"
#include "engine/EngineAdapter.h"
#include "engine/EngineHost.h"
#include "model/Chords.h"
#include "model/NoteTools.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "project/ProjectFiles.h"
#include "session/ProjectDocument.h"
#include "session/Session.h"
#include "ui/MainView.h"
#include "ui/PianoRoll.h"

#include "RealtimeRunner.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <random>

using namespace spinus;

namespace
{
// The helpers ShowcaseComposition.h expects (the same as in tools/showcase-song/ShowcaseSong.cpp).
using Clock = std::chrono::steady_clock;
double msSince (Clock::time_point start)
{
    return std::chrono::duration<double, std::milli> (Clock::now() - start).count();
}

constexpr juce::int64 beat = 960, bar = 3840, step = 240;

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> properties)
{
    auto* o = new juce::DynamicObject();
    for (auto& [name, value] : properties)
        o->setProperty (name, value);
    return juce::var (o);
}

void settle (int ms = 5) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

std::unique_ptr<engine::DeviceFactory> makeDevices (tracktion::Engine& engine, const juce::File& folder)
{
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerBundledDevices (*devices, engine, { folder });
    return devices;
}

/** Performs commands one at a time through the session's history (each its own undo step), stopping on
    the first failure. */
class Commands
{
public:
    explicit Commands (Session& s) : session (s) {}

    juce::var run (const juce::String& name, const juce::var& params)
    {
        auto& registry = commands::CommandRegistry::get();
        juce::String error;
        auto command = registry.create (name, params, error);
        if (command == nullptr)
            fail (name + ": " + error + "\n  " + juce::JSON::toString (params, true));
        auto* raw = command.get();
        const auto result = session.getHistory().perform (std::move (command));
        if (result.failed())
            fail (name + ": " + result.getErrorMessage() + "\n  " + juce::JSON::toString (params, true));
        ++count;
        if (count % 50 == 0)
            settle (1);
        return raw->getResult();
    }

    int count = 0;

private:
    [[noreturn]] static void fail (const juce::String& message)
    {
        std::cerr << "Command failed: " << message << std::endl;
        std::exit (2);
    }
    Session& session;
};

// The showcase song's builder (Song), plugin-state cleaning (makeStatesPortable) and level statistics; its
// own song and measurements are not used here.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "ShowcaseComposition.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

//==============================================================================
// Probe: every instrument and effect the song uses, with its parameters (to choose automation targets).

void probe (Session& session)
{
    const char* types[] = { "spinus.sampler",          "spinus.drum-machine",
                            "spinus.3x-osc",           "bundled.surge-xt",
                            "spinus.filter",           "spinus.eq",
                            "spinus.compressor",       "spinus.limiter",
                            "spinus.stereo-utility",   "spinus.tune",
                            "spinus.ducker",           "spinus.loudness-meter",
                            "bundled.dragonfly.hall",  "bundled.dragonfly.room",
                            "bundled.dragonfly.plate", "bundled.surge-xt-effects" };
    std::vector<std::pair<juce::String, int>> devices;
    auto& registry = commands::CommandRegistry::get();
    const auto insert = [&]
    {
        juce::String error;
        auto command = registry.create ("insert.add", object ({ { "name", "Probe" } }), error);
        auto* raw = command.get();
        session.getHistory().perform (std::move (command));
        return (int)raw->getResult()["insert"];
    }();
    for (int i = 0; i < 3; ++i)
    {
        juce::String error;
        auto command =
            registry.create ("channel.add",
                             object ({ { "name", types[i] },
                                       { "instrument", object ({ { "kind", "stock" }, { "stockType", types[i] } }) } }),
                             error);
        auto* raw = command.get();
        if (session.getHistory().perform (std::move (command)).wasOk())
            devices.emplace_back (types[i], (int)raw->getResult()["device"]);
    }
    for (int i = 3; i < (int)std::size (types); ++i)
    {
        juce::String error;
        auto command =
            registry.create ("insert.addEffect",
                             object ({ { "insert", insert },
                                       { "device", object ({ { "kind", "stock" }, { "stockType", types[i] } }) } }),
                             error);
        if (command == nullptr)
        {
            std::cout << "== " << types[i] << ": " << error << "\n";
            continue;
        }
        auto* raw = command.get();
        if (session.getHistory().perform (std::move (command)).wasOk())
            devices.emplace_back (types[i], (int)raw->getResult()["device"]);
    }
    session.getEdit().dispatchPendingUpdatesSynchronously();
    settle (500);
    for (auto& [type, id] : devices)
    {
        const auto parameters = session.getDeviceParameters (model::DeviceId { (uint32_t)id });
        std::cout << "== " << type << " (device " << id << "): " << parameters.size() << " parameters"
                  << (parameters.empty() ? " -- NOT LOADED" : "") << "\n";
        for (auto& [pid, name] : parameters)
            std::cout << "  " << pid << " | " << name << " | "
                      << session.getDeviceParameterValueText (model::DeviceId { (uint32_t)id }, pid) << "\n";
    }
}

#include "LofiComposition.h"
} // namespace

//==============================================================================
int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::StringArray args (argv, argc);
    const auto valueOf = [&] (const char* name) -> juce::String
    {
        const auto i = args.indexOf (name);
        return i >= 0 && i + 1 < args.size() ? args[i + 1] : juce::String();
    };
    const auto fileOf = [&] (const char* name)
    {
        const auto v = valueOf (name);
        return v.isEmpty() ? juce::File() : juce::File::getCurrentWorkingDirectory().getChildFile (v);
    };
    if (args.contains ("--help") || valueOf ("--plugins").isEmpty())
    {
        std::cout << "spinus_demo_lofi --plugins <folder> (--probe | --audio <folder> --make <project folder> | "
                     "--render <project folder> --wav <file> [--stems <folder>])\n";
        return args.contains ("--help") ? 0 : 1;
    }
    const auto plugins = fileOf ("--plugins");

    EngineHost::setSettingsName ("Spinus Demo Tool");
    int status = 0;
    {
        EngineHost host ({ .openAudioDevice = false });
        Session session (host.getEngine(), makeDevices (host.getEngine(), plugins));
        session.getDeviceList().setBundledFolders ({ plugins });
        const auto settings =
            juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("spinus-demo-lofi-settings");
        ProjectDocument document (session, settings);

        if (args.contains ("--probe"))
            probe (session);
        else if (const auto make = fileOf ("--make"); make != juce::File())
            status = lofi::make (session, document, make, plugins, fileOf ("--audio"));
        else if (const auto render = fileOf ("--render"); render != juce::File())
            status = lofi::render (session, document, render, fileOf ("--wav"), fileOf ("--stems"));
        settle (200);
    }
    return status;
}
