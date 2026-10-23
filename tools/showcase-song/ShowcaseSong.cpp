// The showcase song "Siskin": an original multi-pattern, multi-track demo with many automation clips, made
// only through the Command API on Spinus's own instruments and effects and the free open-source plugins (Surge
// XT, Surge XT Effects, Dragonfly Reverb: all made for Linux and Windows), then saved as a normal project. It
// doubles as a stress test: it measures loading, opening patterns, saving, rendering and real-time playback.
// The song made by --make (without its rendered .wav) is the demo song shipped with Spinus: demo/Siskin
// (demo/README.md says how to make it again).
//
//   spinus_showcase_song --plugins <folder> --probe
//   spinus_showcase_song --plugins <folder> --make <project folder>
//   spinus_showcase_song --plugins <folder> --measure <project folder> --wav <file> [--save-copy <folder>]
//
// <folder> holds the free plugins (Surge XT.clap, Surge XT Effects.clap, the Dragonfly reverbs ...), e.g. a
// development build's plugins/ folder or ~/.local/share/Spinus DAW/plugins/<pack>. Run it with HOME set to
// a scratch folder: Tracktion keeps its settings under ~/.config.
//
// Settings are given as text in the units the devices show ("60%", "180 ms", "-18 dB", "2500 Hz"), read by
// Spinus's one rule for typed settings (dsp/ParameterText.h).
//
// Environment: SHOWCASE_AUDIO=<folder> with the song's two sound effects (made by `python3 make_audio.py
// <folder>`; without it the song has no audio clips); for --make, SHOWCASE_SHOW_SETTINGS=1 prints what each
// setting shows once set; for --measure, SHOWCASE_STEMS=<folder> also exports stems,
// SHOWCASE_COMMANDS=<file.json> runs commands first ([{"command": ..., "parameters": {...}}]), and
// SHOWCASE_DESCRIBE=1 prints the engine's view of the mixer.

#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/DeviceFactory.h"
#include "engine/EngineAdapter.h"
#include "engine/EngineHost.h"
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

/** The free plugins and Spinus's own devices, found in `folder` as Spinus finds downloaded ones. */
std::unique_ptr<engine::DeviceFactory> makeDevices (tracktion::Engine& engine, const juce::File& folder)
{
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerBundledDevices (*devices, engine, { folder });
    return devices;
}

/** Performs commands one at a time through the session's history (each its own undo step, as when a user
    makes the song), stopping on the first failure. */
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

//==============================================================================
// Probe: every instrument and effect used, with its parameters (to choose automation targets).

void probe (Session& session)
{
    Commands c (session);
    const char* instruments[] = { "spinus.3x-osc", "spinus.drum-machine", "bundled.surge-xt" };
    const char* effects[] = { "spinus.filter",
                              "spinus.eq",
                              "spinus.compressor",
                              "spinus.limiter",
                              "spinus.ducker",
                              "spinus.stereo-utility",
                              "spinus.gate",
                              "spinus.loudness-meter",
                              "bundled.dragonfly.hall",
                              "bundled.dragonfly.plate",
                              "bundled.dragonfly.room",
                              "bundled.surge-xt-effects",
                              "bundled.peakeater",
                              "bundled.odin2",
                              "bundled.ob-xf" };
    std::vector<std::pair<juce::String, int>> devices;
    for (auto* type : instruments)
    {
        const auto r = c.run (
            "channel.add",
            object ({ { "name", type }, { "instrument", object ({ { "kind", "stock" }, { "stockType", type } }) } }));
        devices.emplace_back (type, (int)r["device"]);
    }
    const auto insert = (int)c.run ("insert.add", object ({ { "name", "FX" } }))["insert"];
    for (auto* type : effects)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (
            "insert.addEffect",
            object ({ { "insert", insert }, { "device", object ({ { "kind", "stock" }, { "stockType", type } }) } }),
            error);
        auto* raw = command.get();
        if (session.getHistory().perform (std::move (command)).wasOk())
            devices.emplace_back (type, (int)raw->getResult()["device"]);
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

#include "ShowcaseComposition.h"
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
        std::cout << "spinus_showcase_song --plugins <folder> (--probe | --make <project folder> | --measure "
                     "<project folder> --wav <file> [--save-copy <folder>])\n";
        return args.contains ("--help") ? 0 : 1;
    }
    const juce::File plugins (juce::File::getCurrentWorkingDirectory().getChildFile (valueOf ("--plugins")));

    EngineHost::setSettingsName ("Siskin Tool");
    int status = 0;
    {
        EngineHost host ({ .openAudioDevice = false });
        Session session (host.getEngine(), makeDevices (host.getEngine(), plugins));
        session.getDeviceList().setBundledFolders ({ plugins });
        const auto settings =
            juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("spinus-showcase-settings");
        ProjectDocument document (session, settings);

        if (args.contains ("--probe"))
            probe (session);
        else if (const auto make = valueOf ("--make"); make.isNotEmpty())
            status = showcase::make (session, document, juce::File::getCurrentWorkingDirectory().getChildFile (make),
                                     plugins);
        else if (const auto measure = valueOf ("--measure"); measure.isNotEmpty())
            status = showcase::measure (
                session, document, host, juce::File::getCurrentWorkingDirectory().getChildFile (measure),
                juce::File::getCurrentWorkingDirectory().getChildFile (valueOf ("--wav")),
                valueOf ("--save-copy").isEmpty()
                    ? juce::File()
                    : juce::File::getCurrentWorkingDirectory().getChildFile (valueOf ("--save-copy")));
        settle (200);
    }
    return status;
}
