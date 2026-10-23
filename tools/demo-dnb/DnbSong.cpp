// "Sodium Lights": an original liquid drum & bass demo song (174 BPM, F minor), made only through the Command
// API (each change an undoable command, as a user's edits are) on Spinus's own instruments and effects and the
// free open-source plugins that exist for both Linux and Windows (Surge XT, Surge XT Effects, Dragonfly Reverb,
// PeakEater), then saved as a normal project. It ships with Spinus beside the showcase song: demo/Sodium Lights
// (demo/README.md says how to make it again). It reuses the showcase song's helpers
// (tools/showcase-song/ShowcaseComposition.h).
//
//   spinus_demo_dnb --plugins <folder> --probe
//   spinus_demo_dnb --plugins <folder> --make <project folder>
//   spinus_demo_dnb --plugins <folder> --render <project folder> --wav <file>
//   spinus_demo_dnb --plugins <folder> --shots <project folder> --out <folder>   (pictures of the main panels)
//
// <folder> holds the free plugins (Surge XT.clap, Surge XT Effects.clap, the Dragonfly reverbs,
// peakeater.clap). Run it with HOME set to a scratch folder: Tracktion keeps its settings under ~/.config.
// Environment: DNB_AUDIO=<folder> with the song's sounds, made by `python3 make_audio.py <folder>` (needed:
// the Beat Slicer's break is one of them); for --render, DNB_STEMS=<folder> also exports stems.

#include "commands/CommandRegistry.h"
#include "dsp/slicer/BeatSlicerProcessor.h"
#include "engine/BundledDevices.h"
#include "engine/DeviceFactory.h"
#include "engine/EngineAdapter.h"
#include "engine/EngineHost.h"
#include "model/ModelJson.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "project/ProjectFiles.h"
#include "session/ProjectDocument.h"
#include "session/Session.h"
#include "session/SlicesToPattern.h"
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
// The same helpers as tools/showcase-song/ShowcaseSong.cpp (ShowcaseComposition.h expects them).
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

// The showcase song's Song class (channels, effects, patterns, automation), its plugin-state clean-up and
// its level statistics. Its own song and measurements are not used here.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "ShowcaseComposition.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

//==============================================================================
void probe (Session& session)
{
    Commands c (session);
    const char* instruments[] = { "spinus.beat-slicer", "spinus.drum-machine", "spinus.3x-osc", "bundled.surge-xt" };
    const char* effects[] = { "spinus.filter",           "spinus.eq",
                              "spinus.compressor",       "spinus.limiter",
                              "spinus.loudness-meter",   "spinus.ducker",
                              "spinus.stereo-utility",   "bundled.dragonfly.hall",
                              "bundled.dragonfly.plate", "bundled.surge-xt-effects",
                              "bundled.peakeater" };
    std::vector<std::pair<juce::String, int>> devices;
    for (auto* type : instruments)
    {
        const auto r = c.run (
            "channel.add",
            object ({ { "name", type }, { "instrument", object ({ { "kind", "stock" }, { "stockType", type } }) } }));
        devices.emplace_back (type, (int)r["device"]);
    }
    const auto insert = (int)c.run ("insert.add", object ({ { "name", "Probe" } }))["insert"];
    for (auto* type : effects)
    {
        const auto r = c.run (
            "insert.addEffect",
            object ({ { "insert", insert }, { "device", object ({ { "kind", "stock" }, { "stockType", type } }) } }));
        devices.emplace_back (type, (int)r["device"]);
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

#include "DnbComposition.h"
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
    if (args.contains ("--help") || valueOf ("--plugins").isEmpty())
    {
        std::cout << "spinus_demo_dnb --plugins <folder> (--probe | --make <project folder> | --render <project "
                     "folder> --wav <file>)\n";
        return args.contains ("--help") ? 0 : 1;
    }
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    const juce::File plugins (cwd.getChildFile (valueOf ("--plugins")));

    EngineHost::setSettingsName ("Spinus Demo DnB Tool");
    int status = 0;
    {
        EngineHost host ({ .openAudioDevice = false });
        Session session (host.getEngine(), makeDevices (host.getEngine(), plugins));
        session.getDeviceList().setBundledFolders ({ plugins });
        const auto settings =
            juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("spinus-demo-dnb-settings");
        ProjectDocument document (session, settings);

        if (args.contains ("--probe"))
            probe (session);
        else if (const auto make = valueOf ("--make"); make.isNotEmpty())
            status = dnb::make (session, document, cwd.getChildFile (make), plugins);
        else if (const auto shots = valueOf ("--shots"); shots.isNotEmpty())
            status = dnb::shots (session, document, cwd.getChildFile (shots), cwd.getChildFile (valueOf ("--out")));
        else if (const auto render = valueOf ("--render"); render.isNotEmpty())
            status = dnb::render (session, document, cwd.getChildFile (render), cwd.getChildFile (valueOf ("--wav")));
        settle (200);
    }
    return status;
}
