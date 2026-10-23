#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "model/ModelJson.h"
#include "plugins/SurgePresets.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "session/SynthMatch.h"

using namespace spinus;

namespace
{
juce::File bundledPlugins()
{
#if defined(SPINUS_PLUGINS_DIR)
    return juce::File (SPINUS_PLUGINS_DIR);
#else
    return {};
#endif
}

struct MatchSession
{
    static std::unique_ptr<engine::DeviceFactory> makeDevices (tracktion::Engine& engine)
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerBundledDevices (*devices, engine, { bundledPlugins() });
        return devices;
    }

    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine(), makeDevices (host.getEngine()) };
};

/** AddressSanitizer makes every try many times slower: fewer tries there, and a looser bar. */
#if defined(__SANITIZE_ADDRESS__)
constexpr int fewer = 4;
constexpr double looser = 0.9;
#else
constexpr int fewer = 1;
constexpr double looser = 1.0;
#endif

juce::AudioBuffer<float> tone (double frequency, bool saw, double seconds = 1.0)
{
    juce::AudioBuffer<float> b (1, (int)(seconds * 48000.0));
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const auto phase = std::fmod (frequency * i / 48000.0, 1.0);
        b.setSample (
            0, i,
            (float)(saw ? 0.3 * (2.0 * phase - 1.0) : 0.3 * std::sin (juce::MathConstants<double>::twoPi * phase)));
    }
    return b;
}

/** Synth profiles from a folder of the test's own, as if in ~/.local/share/spinus/synth-profiles. */
struct UserProfiles
{
    UserProfiles()
    {
        ::setenv ("XDG_DATA_HOME", data.getFullPathName().toRawUTF8(), 1);
        folder.createDirectory();
    }
    ~UserProfiles()
    {
        if (old.isEmpty())
            ::unsetenv ("XDG_DATA_HOME");
        else
            ::setenv ("XDG_DATA_HOME", old.toRawUTF8(), 1);
        data.deleteRecursively();
    }
    UserProfiles (const UserProfiles&) = delete;
    UserProfiles& operator= (const UserProfiles&) = delete;
    void add (const juce::String& file, const juce::String& json) const
    {
        folder.getChildFile (file).replaceWithText (json);
    }

    juce::File data = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-profiles-" + juce::Uuid().toString());
    juce::File folder = data.getChildFile ("spinus/synth-profiles");
    juce::String old = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});
};

/** One of the test instruments in test-synth.clap ("Spinus Test Synth", "Spinus Test Organ"), installed. */
model::Device installTestPlugin (tracktion::Engine& engine, const juce::String& name)
{
    plugins::ClapPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile (
        found, juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("test-synth.clap").getFullPathName());
    model::Device device;
    for (auto* d : found)
        if (d->name == name)
        {
            engine.getPluginManager().knownPluginList.addType (*d);
            device.kind = model::Device::Kind::plugin;
            device.plugin = { d->pluginFormatName, d->name, d->manufacturerName, d->fileOrIdentifier, d->uniqueId };
        }
    return device;
}

void setByName (juce::AudioPluginInstance& instance, const juce::String& name, float value)
{
    for (auto* p : instance.getParameters())
        if (p->getName (200) == name)
            p->setValueNotifyingHost (value);
}

model::ChannelId addChannel (Session& session, const model::Device& instrument)
{
    juce::String error;
    auto parameters = juce::JSON::parse (R"({"name": "Synth"})");
    parameters.getDynamicObject()->setProperty ("instrument", model::deviceToJson (instrument));
    auto command = commands::CommandRegistry::get().create ("channel.add", parameters, error);
    REQUIRE (command != nullptr);
    auto* raw = command.get();
    REQUIRE (session.getHistory().perform (std::move (command)).wasOk());
    return model::ChannelId { (uint32_t)(int)raw->getResult()["channel"] };
}

bool tune (SynthMatcher& matcher, const juce::AudioBuffer<float>& target,
           const std::vector<HiddenInstrument::Note>& notes, int tries)
{
    bool done = false;
    matcher.start (target, notes, tries, {}, [&] { done = true; });
    for (int i = 0; i < 12000 && ! done; ++i) // up to 600 s (much slower with AddressSanitizer)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    return done;
}

void setParameter (juce::AudioPluginInstance& instance, const juce::String& id, float value)
{
    for (auto* p : instance.getParameters())
    {
        auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (p);
        auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p);
        if ((hosted != nullptr && hosted->getParameterID() == id) || (withId != nullptr && withId->paramID == id))
            p->setValueNotifyingHost (value);
    }
}
} // namespace

TEST_CASE ("Synth profiles: the built-in ones, a user's own, and broken ones", "[session][synthmatch]")
{
    const auto osc = SynthProfile::find ("spinus.3x-osc");
    REQUIRE (osc.has_value());
    CHECK (osc.value_or (SynthProfile {}).settings.size() >= 10);
    const auto surge = SynthProfile::find ("bundled.surge-xt");
    REQUIRE (surge.has_value());
    CHECK (surge.value_or (SynthProfile {}).start == "closestPreset");

    CHECK (! SynthProfile::fromJson (juce::JSON::parse (R"({"stockType": "x", "settings": []})")));
    CHECK (! SynthProfile::fromJson (
        juce::JSON::parse (R"({"stockType": "x", "start": "?", "settings": [{"id": "a"}]})")));
    CHECK (! SynthProfile::fromJson (juce::JSON::parse (R"({"stockType": "x", "settings": [{"min": 0.5}]})")));

    // A profile in the user's folder replaces Spinus's own for that synth.
    const auto data = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-profiles-" + juce::Uuid().toString());
    const auto old = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});
    ::setenv ("XDG_DATA_HOME", data.getFullPathName().toRawUTF8(), 1);
    data.getChildFile ("spinus/synth-profiles").createDirectory();
    data.getChildFile ("spinus/synth-profiles/mine.json")
        .replaceWithText (
            R"({"name": "Mine", "stockType": "spinus.3x-osc", "settings": [{"id": "cutoff", "max": 0.5}]})");
    const auto mine = SynthProfile::find ("spinus.3x-osc").value_or (SynthProfile {});
    CHECK (mine.name == "Mine");
    REQUIRE (mine.settings.size() == 1);
    CHECK ((mine.settings[0].id == "cutoff" && juce::approximatelyEqual (mine.settings[0].max, 0.5f)));
    if (old.isEmpty())
        ::unsetenv ("XDG_DATA_HOME");
    else
        ::setenv ("XDG_DATA_HOME", old.toRawUTF8(), 1);
    data.deleteRecursively();
}

TEST_CASE ("Synth profiles of the user's own for an installed plugin and a downloaded synth", "[session][synthmatch]")
{
    model::Device some;
    some.kind = model::Device::Kind::plugin;
    some.plugin = { "VST3", "Some Synth", "Someone", "/usr/lib/vst3/Some Synth.vst3", 1 };
    model::Device odin;
    odin.stockType = "bundled.odin2";
    CHECK (! SynthProfile::find (some));
    CHECK (SynthProfile::find (odin).value_or (SynthProfile {}).name == "Odin 2");

    const UserProfiles user;
    user.add ("some.json", R"({"plugin": "some synth", "manufacturer": "Someone", "settings": [{"name": "Cutoff"}]})");
    user.add ("odin.json", R"({"name": "My Odin", "plugin": "Odin2", "settings": [{"name": "Filter1 Frequency"}]})");
    const auto mine = SynthProfile::find (some);
    REQUIRE (mine.has_value());
    CHECK (mine.value_or (SynthProfile {}).settings.size() == 1);
    // The user's own replaces Spinus's for the same synth, found by the plugin's name.
    CHECK (SynthProfile::find (odin).value_or (SynthProfile {}).name == "My Odin");
    CHECK (SynthProfile::isTested (odin)); // Spinus's own still decides how it is offered
    some.plugin.manufacturer = "Someone else";
    CHECK (! SynthProfile::find (some));
}

TEST_CASE ("Spectral distance: the same sound, louder or not, is close; another shape is far", "[session][synthmatch]")
{
    const auto sine = tone (220.0, false), saw = tone (220.0, true);
    auto louder = sine;
    louder.applyGain (4.0f);
    CHECK (spectralDistance (sine, sine) < 1.0e-9);
    CHECK (spectralDistance (sine, louder) < 1.0e-3);
    CHECK (spectralDistance (sine, saw) > 0.5);
    CHECK (spectralDistance (sine, tone (233.0, false)) < spectralDistance (sine, saw));
    CHECK (spectralDistance (sine, juce::AudioBuffer<float> (1, 100)) > 1000.0); // too short to compare
}

TEST_CASE ("The part of a stem to match: the busiest few seconds, read at the synth's rate", "[session][synthmatch]")
{
    using Note = HiddenInstrument::Note;
    // A lone note, then three notes close together.
    const std::vector<Note> notes { { 40, 0.0, 0.5 }, { 45, 5.0, 1.0 }, { 47, 6.2, 1.0 }, { 48, 7.5, 1.0 } };
    const auto excerpt = chooseExcerpt (notes, 3.0);
    CHECK (juce::approximatelyEqual (excerpt.start, 5.0));
    REQUIRE (excerpt.notes.size() == 3);
    CHECK (juce::approximatelyEqual (excerpt.notes[1].start, 1.2));
    CHECK (std::abs (excerpt.notes[2].length - 0.5) < 1.0e-9); // cut at the window's end
    CHECK (std::abs (excerpt.length - 3.0) < 1.0e-9);
    CHECK (chooseExcerpt ({}).notes.empty());

    // 44.1 kHz stereo in, 48 kHz mono out.
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-excerpt-" + juce::Uuid().toString() + ".wav");
    {
        juce::AudioBuffer<float> audio (2, 44100 * 4);
        for (int i = 0; i < audio.getNumSamples(); ++i)
            for (int c = 0; c < 2; ++c)
                audio.setSample (c, i, i >= 44100 * 2 ? 0.5f : 0.0f);
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = juce::WavAudioFormat().createWriterFor (
            stream,
            juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (24));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }
    const auto part = readExcerpt (file, 1.5, 1.0, 0.2);
    file.deleteFile();
    REQUIRE (part.getNumChannels() == 1);
    CHECK (std::abs (part.getNumSamples() - (int)(1.2 * 48000.0)) < 4);
    CHECK (std::abs (part.getSample (0, 1000)) < 1.0e-3f);         // before 2 s: quiet
    CHECK (std::abs (part.getSample (0, 48000) - 0.5f) < 1.0e-2f); // after: the sound
    CHECK (readExcerpt (juce::File ("/nowhere.wav"), 0.0, 1.0).getNumSamples() == 0);
}

TEST_CASE ("CMA-ES finds the bottom of a bowl inside [0, 1], also at an edge", "[session][synthmatch]")
{
    CHECK (juce::approximatelyEqual (Cmaes::fold (0.25), 0.25));
    CHECK (juce::approximatelyEqual (Cmaes::fold (1.25), 0.75));
    CHECK (juce::approximatelyEqual (Cmaes::fold (-0.25), 0.25));
    CHECK (juce::approximatelyEqual (Cmaes::fold (2.5), 0.5));

    const std::vector<double> bottom { 0.2, 0.8, 0.5, 0.33, 0.9, 0.0 };
    auto loss = [&] (const std::vector<double>& x)
    {
        double sum = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
            sum += (double)(i + 1) * (x[i] - bottom[i]) * (x[i] - bottom[i]);
        return sum;
    };
    Cmaes search ({ 0.5, 0.5, 0.5, 0.5, 1.0, 0.0 }, 0.3); // two start at an edge
    CHECK (search.getPopulation() >= 8);
    for (int generation = 0; generation < 200; ++generation)
    {
        const auto points = search.ask();
        std::vector<double> losses;
        for (auto& p : points)
        {
            auto inside = p;
            for (auto& v : inside)
                v = Cmaes::fold (v);
            losses.push_back (loss (inside));
        }
        search.tell (points, losses);
    }
    CHECK (search.getBestLoss() < 1.0e-6);
    for (size_t i = 0; i < bottom.size(); ++i)
        CHECK (std::abs (search.getBest()[i] - bottom[i]) < 1.0e-3);
}

TEST_CASE ("Synth match: 3x Osc learns to sound like another 3x Osc, and the result goes on a channel",
           "[session][synthmatch]")
{
    MatchSession s;
    const auto profile = SynthProfile::find ("spinus.3x-osc").value_or (SynthProfile {});
    const std::vector<HiddenInstrument::Note> notes { { 45, 0.0, 0.5 }, { 52, 0.6, 0.5 }, { 57, 1.2, 0.7 } };

    // The target: a dark, slow square sound.
    HiddenInstrument other (s.session, "spinus.3x-osc");
    REQUIRE (other.getProblem().isEmpty());
    auto& instance = *other.getInstance();
    setParameter (instance, "osc1_shape", 0.75f); // square
    setParameter (instance, "osc2_level", 0.0f);
    setParameter (instance, "filter", 1.0f / 3.0f); // low pass
    setParameter (instance, "cutoff", 0.35f);
    setParameter (instance, "attack", 0.03f);
    const auto target = other.render (notes, 0.2);

    SynthMatcher matcher (s.session, profile);
    REQUIRE (matcher.getProblem().isEmpty());
    CHECK (matcher.getNumSettings() == (int)profile.settings.size());
    int lastTries = 0;
    bool done = false;
    matcher.start (target, notes, 250 / fewer, [&] (int tries, double) { lastTries = tries; }, [&] { done = true; });
    for (int i = 0; i < 8000 && ! done; ++i) // up to 400 s (much slower with AddressSanitizer)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    REQUIRE (done);
    const auto& result = matcher.getResult();
    WARN ("3x Osc match: " << result.startDistance << " -> " << result.distance << " in " << result.tries << " tries");
    CHECK (result.tries >= 240 / fewer);
    CHECK (lastTries == result.tries);
    CHECK (result.distance < 0.6 / looser * result.startDistance);
    CHECK (result.settings.size() == profile.settings.size());

    // On the user's channel.
    juce::String error;
    auto command = commands::CommandRegistry::get().create (
        "channel.add",
        juce::JSON::parse (R"({"name": "Bass", "instrument": {"kind": "stock", "stockType": "spinus.3x-osc"}})"),
        error);
    REQUIRE (command != nullptr);
    auto* raw = command.get();
    REQUIRE (s.session.getHistory().perform (std::move (command)).wasOk());
    const model::ChannelId channel { (uint32_t)(int)raw->getResult()["channel"] };
    CHECK (SynthMatcher::apply (s.session, channel, result) == (int)result.settings.size());
    CHECK (SynthMatcher::apply (s.session, model::ChannelId { 999 }, result) == 0);
}

TEST_CASE ("Synth match can stop early and keeps the best so far", "[session][synthmatch]")
{
    MatchSession s;
    SynthMatcher matcher (s.session, SynthProfile::find ("spinus.3x-osc").value_or (SynthProfile {}));
    bool done = false;
    matcher.start (tone (110.0, true, 1.5), { { 45, 0.0, 1.0 } }, 100000, {}, [&] { done = true; });
    juce::MessageManager::getInstance()->runDispatchLoopUntil (500);
    matcher.stop();
    for (int i = 0; i < 200 && ! done; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    REQUIRE (done);
    CHECK (matcher.getResult().tries < 100000);
    CHECK (matcher.getResult().distance <= matcher.getResult().startDistance);
}

TEST_CASE ("Synth match: Surge XT finds settings changed from its preset", "[session][synthmatch][surge]")
{
    if (! bundledPlugins().getChildFile ("Surge XT.clap").existsAsFile())
        SKIP ("Surge XT not built");
    MatchSession s;
    const auto presets = plugins::SurgePresets::scan (
        plugins::SurgePresets::findDataFolder (bundledPlugins().getChildFile ("Surge XT.clap")), {});
    auto find = [&] (const juce::String& name)
    {
        for (auto& p : presets)
            if (p.name == name)
                return p.file;
        return juce::File();
    };
    const std::vector<HiddenInstrument::Note> notes { { 40, 0.0, 0.5 }, { 47, 0.6, 0.5 }, { 52, 1.2, 0.7 } };
    // The target: Square Bass, darker and slower.
    HiddenInstrument other (s.session, "bundled.surge-xt");
    REQUIRE (other.loadPreset (find ("Square Bass")).wasOk());
    const auto before = other.render (notes, 0.2);
    const auto again = other.render (notes, 0.2);
    for (auto* p : other.getInstance()->getParameters())
    {
        if (p->getName (100) == "A Filter 1 Cutoff")
            p->setValueNotifyingHost (p->getValue() * 0.6f);
        if (p->getName (100) == "A Amp EG Attack")
            p->setValueNotifyingHost (0.35f);
    }
    const auto target = other.render (notes, 0.2);
    CHECK (spectralDistance (before, again) < 1.0e-6); // the same each time

    SynthMatcher matcher (s.session, SynthProfile::find ("bundled.surge-xt").value_or (SynthProfile {}));
    REQUIRE (matcher.getProblem().isEmpty());
    CHECK (matcher.getNumSettings() >= 15); // the profile's names are Surge XT's
    REQUIRE (matcher.loadPreset (find ("Square Bass")).wasOk());
    bool done = false;
    const auto started = juce::Time::getMillisecondCounterHiRes();
    // As many tries as the Analyzer gives Surge XT.
    matcher.start (target, notes, 800 / fewer, {}, [&] { done = true; });
    for (int i = 0; i < 8000 && ! done; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    REQUIRE (done);
    const auto& result = matcher.getResult();
    WARN ("Surge XT match: " << result.startDistance << " -> " << result.distance << " in " << result.tries
                             << " tries, " << (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0 << " s");
    // Clearly closer (about 0.38 -> 0.21 with the app's 800 tries).
    CHECK (result.distance < (fewer > 1 ? 0.95 : 0.7) * result.startDistance);
}

TEST_CASE ("Synth match: an installed plugin, with a profile made from its settings' names", "[session][synthmatch]")
{
    MatchSession s;
    const auto synth = installTestPlugin (s.host.getEngine(), "Spinus Test Synth");
    REQUIRE (synth.kind == model::Device::Kind::plugin);
    CHECK (! SynthProfile::find (synth));
    const std::vector<HiddenInstrument::Note> notes { { 45, 0.0, 0.5 }, { 52, 0.6, 0.5 }, { 57, 1.2, 0.7 } };

    // The target: square, darker, slower.
    HiddenInstrument other (s.session, synth);
    REQUIRE (other.getProblem().isEmpty());
    setByName (*other.getInstance(), "Wave", 0.5f);
    setByName (*other.getInstance(), "Cutoff", 0.45f);
    setByName (*other.getInstance(), "Attack", 0.3f);
    const auto target = other.render (notes, 0.2);

    SynthMatcher matcher (s.session, synth);
    REQUIRE (matcher.getProblem().isEmpty());
    const auto& profile = matcher.getProfile();
    CHECK (profile.automatic);
    CHECK (profile.name == "Spinus Test Synth");
    // Wave, Cutoff, Resonance and the four envelope stages; not Master Volume, Bypass, Voices, LFO Rate or
    // Delay Mix.
    CHECK (matcher.getNumSettings() == 7);
    REQUIRE (tune (matcher, target, notes, 300 / fewer));
    const auto& result = matcher.getResult();
    WARN ("Test synth, automatic profile: " << result.startDistance << " -> " << result.distance << " in "
                                            << result.tries << " tries");
    CHECK (result.distance < 0.6 / looser * result.startDistance);

    // On the user's channel with the same plugin: every setting found by the same IDs.
    const auto channel = addChannel (s.session, synth);
    CHECK (SynthMatcher::apply (s.session, channel, result) == (int)result.settings.size());
    const auto* made = s.session.getProject().findChannel (channel);
    REQUIRE (made != nullptr);
    auto* plugin = dynamic_cast<tracktion::ExternalPlugin*> (
        s.session.getAdapter().getDevicePlugin (made->instrument.value_or (model::Device {}).id));
    REQUIRE (plugin != nullptr);
    REQUIRE (plugin->getAudioPluginInstance() != nullptr);
    for (auto& [id, value] : result.settings)
    {
        auto* parameter = SynthMatcher::findParameter (*plugin->getAudioPluginInstance(), id);
        REQUIRE (parameter != nullptr);
        CHECK (std::abs (parameter->getValue() - value) < 1.0e-3f);
    }

    // A profile of the user's own replaces the automatic one.
    {
        const UserProfiles user;
        user.add ("test.json", R"({"plugin": "Spinus Test Synth", "settings": [{"name": "Cutoff"},
                                {"name": "Attack", "max": 0.4}, {"name": "Not There"}]})");
        SynthMatcher withMine (s.session, synth, SynthProfile::find (synth));
        CHECK (! withMine.getProfile().automatic);
        CHECK (withMine.getNumSettings() == 2); // the setting it does not have is left out
    }

    // An organ with only a volume, a switch and a transpose: nothing to tune.
    SynthMatcher organ (s.session, installTestPlugin (s.host.getEngine(), "Spinus Test Organ"));
    CHECK (organ.getProblem().isEmpty());
    CHECK (organ.getNumSettings() == 0);
    // A plugin that is not there.
    model::Device missing;
    missing.kind = model::Device::Kind::plugin;
    missing.plugin = { "VST3", "Nowhere Synth", "Nobody", "/nowhere.vst3", 5 };
    SynthMatcher gone (s.session, missing);
    CHECK (gone.getProblem().contains ("Nowhere Synth"));
    CHECK (gone.getNumSettings() == 0);
}

/*  The synths Spinus downloads (Odin 2, OB-Xf): their profiles name real settings, and each tunes to a
    changed sound of its own, with its profile and with an automatic one. Point SPINUS_CHECK_DOWNLOADED at a
    folder holding Odin2.clap and OB-Xf.clap (searched with its subfolders); skipped without it. */
TEST_CASE ("Synth match: Odin 2 and OB-Xf find settings changed from their default sound",
           "[session][synthmatch][downloaded]")
{
    const juce::File folder { juce::SystemStats::getEnvironmentVariable ("SPINUS_CHECK_DOWNLOADED", {}) };
    if (! folder.isDirectory())
        SKIP ("SPINUS_CHECK_DOWNLOADED is not set to a folder");

    struct Synth
    {
        const char* stockType;
        std::vector<std::pair<const char*, float>> changes;
    };
    const std::vector<Synth> synths {
        { "bundled.odin2",
          { { "Filter1 Frequency", 0.55f }, { "Filter1 Resonance", 0.5f }, { "AmpEnv Attack", 0.35f } } },
        { "bundled.ob-xf",
          { { "Filter Cutoff", 0.5f },
            { "Filter Resonance", 0.5f },
            { "Amp Env Attack", 0.3f },
            { "Osc 1 Pulse Wave", 1.0f } } },
    };
    EngineHost host { { .openAudioDevice = false } };
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    std::vector<juce::File> folders { folder };
    for (auto* file : { "Odin2.clap", "OB-Xf.clap" })
        for (const auto& found : folder.findChildFiles (juce::File::findFiles, true, file))
            folders.push_back (found.getParentDirectory());
    engine::registerBundledDevices (*devices, host.getEngine(), folders);
    Session session { host.getEngine(), std::move (devices) };
    const std::vector<HiddenInstrument::Note> notes { { 45, 0.0, 0.5 }, { 52, 0.6, 0.5 }, { 57, 1.2, 0.7 } };

    for (const auto& synth : synths)
    {
        INFO (synth.stockType);
        model::Device device;
        device.stockType = synth.stockType;
        HiddenInstrument other (session, device);
        if (other.getProblem().isNotEmpty())
        {
            WARN (synth.stockType << ": not in " << folder.getFullPathName());
            continue;
        }
        const auto profile = SynthProfile::find (device).value_or (SynthProfile {});
        REQUIRE (! profile.settings.empty());

        // Every setting the profile names is the synth's (how much each changes the sound is said).
        auto& instance = *other.getInstance();
        const auto before = other.render (notes, 0.2);
        for (const auto& setting : profile.settings)
        {
            juce::AudioProcessorParameter* parameter = nullptr;
            for (auto* p : instance.getParameters())
                if (p->getName (200) == setting.name)
                    parameter = p;
            INFO (setting.name);
            REQUIRE (parameter != nullptr);
            const auto was = parameter->getValue();
            double most = 0.0;
            for (auto value : { setting.min, setting.max })
            {
                parameter->setValueNotifyingHost (value);
                most = std::max (most, spectralDistance (before, other.render (notes, 0.2)));
            }
            parameter->setValueNotifyingHost (was);
            WARN (synth.stockType << " " << setting.name << ": " << most);
        }

        // The target: the same synth, changed.
        for (const auto& [name, value] : synth.changes)
            setByName (instance, name, value);
        const auto target = other.render (notes, 0.2);

        for (bool automatic : { false, true })
        {
            SynthMatcher matcher (session, device, automatic ? std::nullopt : std::optional { profile });
            REQUIRE (matcher.getProblem().isEmpty());
            if (! automatic)
                CHECK (matcher.getNumSettings() == (int)profile.settings.size());
            juce::StringArray names;
            for (const auto& s : matcher.getProfile().settings)
                names.add (s.name);
            WARN (synth.stockType << (automatic ? " automatic" : " built-in") << " profile ("
                                  << matcher.getNumSettings() << "): " << names.joinIntoString (", "));
            const auto started = juce::Time::getMillisecondCounterHiRes();
            REQUIRE (tune (matcher, target, notes, profile.getTries() / fewer));
            const auto& result = matcher.getResult();
            WARN (synth.stockType << (automatic ? " automatic" : " built-in") << ": " << result.startDistance << " -> "
                                  << result.distance << " in " << result.tries << " tries, "
                                  << (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0 << " s");
            CHECK (result.distance < (automatic ? 0.9 : 0.7) / looser * result.startDistance);
        }
    }
}
