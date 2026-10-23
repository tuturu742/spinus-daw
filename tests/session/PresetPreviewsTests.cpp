#include <catch2/catch_test_macros.hpp>

#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "plugins/SurgePresets.h"
#include "session/PresetPreviews.h"

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

struct SurgeSession
{
    static std::unique_ptr<engine::DeviceFactory> makeDevices (tracktion::Engine& engine)
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerBundledDevices (*devices, engine, { bundledPlugins() });
        return devices;
    }

    ~SurgeSession() { folder.deleteRecursively(); }

    EngineHost host { { .openAudioDevice = false } };
    Session session { host.getEngine(), makeDevices (host.getEngine()) };
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("spinus-previews-" + juce::Uuid().toString());
};

float peakOf (const juce::File& wav)
{
    std::unique_ptr<juce::AudioFormatReader> reader (
        juce::WavAudioFormat().createReaderFor (wav.createInputStream().release(), true));
    if (reader == nullptr)
        return -1.0f;
    juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
    return audio.getMagnitude (0, audio.getNumSamples());
}
} // namespace

TEST_CASE ("Preset previews: Surge XT plays the phrase with each preset", "[session][presets]")
{
    if (! bundledPlugins().getChildFile ("Surge XT.clap").existsAsFile())
        SKIP ("Surge XT not built");
    SurgeSession s;
    const auto presets = plugins::SurgePresets::scan (
        plugins::SurgePresets::findDataFolder (bundledPlugins().getChildFile ("Surge XT.clap")), {});
    REQUIRE (presets.size() > 100);

    PresetPreviews previews (s.session, "bundled.surge-xt");
    REQUIRE (previews.getProblem().isEmpty());
    const auto first = s.folder.getChildFile ("first.wav");
    REQUIRE (previews.renderOne (presets[0].file, first).wasOk());

    // One after another (the instrument loads each on a thread of its own once it has played).
    const auto second = s.folder.getChildFile ("second.wav");
    REQUIRE (previews.renderOne (presets[7].file, second).wasOk());
    CHECK (peakOf (second) > 0.01f);

    // A batch on the timer, with progress, measured.
    std::vector<PresetPreviews::Item> items;
    items.reserve (40);
    for (size_t i = 0; i < 40; ++i)
    {
        InstrumentPreset preset;
        preset.file = presets[i * 7 % presets.size()].file;
        items.push_back ({ preset, s.folder.getChildFile (juce::String ((int)i) + ".wav") });
    }
    int lastDone = 0;
    std::optional<juce::Result> result;
    const auto started = juce::Time::getMillisecondCounterHiRes();
    previews.render (items, [&] (int done, int) { lastDone = done; }, [&] (const juce::Result& r) { result = r; });
    for (int i = 0; i < 600 && ! result; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    REQUIRE (result.has_value());
    CHECK (result.value_or (juce::Result::fail ("none")).wasOk());
    CHECK (lastDone == 40);
    WARN ("40 previews in " << (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0 << " s");
    int sounding = 0;
    for (auto& item : items)
        sounding += peakOf (item.output) > 0.01f ? 1 : 0;
    CHECK (sounding >= 36); // nearly every preset makes a sound on these notes

    // Different presets sound different.
    CHECK (std::abs (peakOf (items[0].output) - peakOf (items[1].output)) > 1e-3f);
}

TEST_CASE ("Preset previews say when the instrument is missing", "[session][presets]")
{
    EngineHost host { { .openAudioDevice = false } };
    Session session (host.getEngine());
    PresetPreviews previews (session, "bundled.surge-xt");
    CHECK (previews.getProblem().isNotEmpty());
    CHECK (! previews.renderOne (juce::File ("/nowhere.fxp"), juce::File ("/nowhere.wav")).wasOk());
}

/** For checking closest preset by hand: SPINUS_PRESET_CHECK_FOLDER gets previews/<n>.wav of every
    factory and user preset (and index.txt: "<n>\t<name>"), and queries/<n>.wav of every tenth one playing
    a different melody, as a stand-in for a stem. */
TEST_CASE ("Write preset previews and queries (for checking closest preset)", "[.presetcheck]")
{
    const juce::File folder (juce::SystemStats::getEnvironmentVariable ("SPINUS_PRESET_CHECK_FOLDER", {}));
    if (folder == juce::File() || ! bundledPlugins().getChildFile ("Surge XT.clap").existsAsFile())
        SKIP ("set SPINUS_PRESET_CHECK_FOLDER (and build Surge XT)");
    SurgeSession s;
    const auto presets = plugins::SurgePresets::scan (
        plugins::SurgePresets::findDataFolder (bundledPlugins().getChildFile ("Surge XT.clap")), {});
    PresetPreviews previews (s.session, "bundled.surge-xt");
    REQUIRE (previews.getProblem().isEmpty());
    const std::vector<PresetPreviews::PhraseNote> melody { { 43, 0.0, 0.4 },  { 46, 0.5, 0.4 },   { 50, 1.0, 0.9 },
                                                           { 55, 2.0, 0.25 }, { 53, 2.25, 0.25 }, { 50, 2.5, 1.0 } };
    juce::String index;
    int n = 0;
    for (auto& preset : presets)
    {
        if (preset.source == plugins::SurgePreset::Source::thirdParty)
            continue;
        const auto name = juce::String (n);
        REQUIRE (previews.renderOne (preset.file, folder.getChildFile ("previews/" + name + ".wav")).wasOk());
        if (n % 10 == 0)
            REQUIRE (
                previews.renderOne (preset.file, folder.getChildFile ("queries/" + name + ".wav"), melody).wasOk());
        index << name << "\t" << plugins::SurgePresets::describe (preset.source) << ": " << preset.category << "/"
              << preset.name << "\n";
        ++n;
    }
    folder.getChildFile ("index.txt").replaceWithText (index);
}
