/*  The curated open-source effects Spinus offers.

    Checks that the curated LSP Plugins, Dragonfly Reverb and Surge XT Effects plugins, built from
    pinned sources inside the Spinus tree, load through Spinus's own CLAP host (and JUCE's VST3 host
    for comparison), process audio without NaNs, survive random parameter changes, restore their
    state, and fit the mixer budget (64 channels x 4 effects at <= 50% DSP).

    Measurements are printed as "Bundled | name | value" lines.
    Run: ./build/release/tests/checks/spinus_bundled_plugin_checks --order decl
*/

#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "engine/EngineHost.h"
#include "plugins/clap/ClapPluginFormat.h"

#include <chrono>
#include <iostream>
#include <random>

using namespace spinus;
using namespace spinus::test;
namespace te = tracktion;

namespace
{

void metric (const std::string& name, const juce::String& value)
{
    std::cout << "Bundled | " << name << " | " << value << '\n';
}

/** One curated plugin: the file it lives in and its name as the plugin reports it. */
struct Curated
{
    const char* role;
    const char* file;
    const char* name;
};

const juce::File pluginsDir { SPINUS_PLUGINS_DIR };

// The stock effects that the open-source plugins cover (stereo variants).
const std::vector<Curated> curated {
    { "Parametric EQ", "lsp-plugins", "Parametric Equalizer x16 Stereo" },
    { "Compressor", "lsp-plugins", "Compressor Stereo" },
    { "Limiter", "lsp-plugins", "Limiter Stereo" },
    { "Gate", "lsp-plugins", "Gate Stereo" },
    { "Multiband Compressor", "lsp-plugins", "Multiband Compressor Stereo x8" },
    { "Spectrum Analyzer", "lsp-plugins", "Spectrum Analyzer x2" },
    { "Reverb (hall)", "DragonflyHallReverb", "Dragonfly Hall Reverb" },
    { "Reverb (room)", "DragonflyRoomReverb", "Dragonfly Room Reverb" },
    { "Reverb (plate)", "DragonflyPlateReverb", "Dragonfly Plate Reverb" },
    { "Early reflections", "DragonflyEarlyReflections", "Dragonfly Early Reflections" },
    { "Character effects", "Surge XT Effects", "Surge XT Effects" },
};

struct Host
{
    Host() : host ({ .openAudioDevice = false })
    {
        auto& formats = host.getEngine().getPluginManager().pluginFormatManager;

        for (auto* format : formats.getFormats())
        {
            if (format->getName() == plugins::ClapPluginFormat::formatName)
                clap = format;
            else if (format->getName() == "VST3")
                vst3 = format;
        }

        REQUIRE ((clap != nullptr && vst3 != nullptr));
    }

    juce::AudioPluginFormat& format (bool useClap) { return useClap ? *clap : *vst3; }

    static juce::String pathFor (const Curated& c, bool useClap)
    {
        return pluginsDir.getChildFile (juce::String (c.file) + (useClap ? ".clap" : ".vst3")).getFullPathName();
    }

    juce::OwnedArray<juce::PluginDescription> scan (const juce::String& path, bool useClap)
    {
        juce::OwnedArray<juce::PluginDescription> found;
        format (useClap).findAllTypesForFile (found, path);
        return found;
    }

    std::unique_ptr<juce::PluginDescription> describe (const Curated& c, bool useClap)
    {
        for (auto* d : scan (pathFor (c, useClap), useClap))
            if (d->name == c.name)
                return std::make_unique<juce::PluginDescription> (*d);

        return {};
    }

    std::unique_ptr<juce::AudioPluginInstance> create (const juce::PluginDescription& desc, bool useClap,
                                                       double sampleRate, int blockSize)
    {
        juce::String error;
        auto instance = format (useClap).createInstanceFromDescription (desc, sampleRate, blockSize, error);
        INFO ("Load error: " << error);
        REQUIRE (instance != nullptr);
        return instance;
    }

    EngineHost host;
    juce::AudioPluginFormat* clap = nullptr;
    juce::AudioPluginFormat* vst3 = nullptr;
};

struct ProcessStats
{
    int blocks = 0;
    int blocksWithNaN = 0;
    float outputRms = 0.0f;
    double meanBlockMicros = 0.0;
};

/** Feeds a sine-plus-noise signal through the plugin. With fuzz, every block first sets every
    parameter to a random value, like pluginval's automation test. */
ProcessStats process (juce::AudioPluginInstance& plugin, double sampleRate, int blockSize, int numBlocks, bool fuzz,
                      uint32_t seed = 1)
{
    const auto channels = std::max ({ 2, plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels() });
    plugin.setRateAndBufferSizeDetails (sampleRate, blockSize);
    plugin.prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> buffer (channels, blockSize);
    juce::MidiBuffer midi;
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> unit (0.0f, 1.0f);
    ProcessStats stats;
    double sumSquares = 0.0;
    int64_t samples = 0;
    double totalMicros = 0.0;
    double phase = 0.0;

    for (int b = 0; b < numBlocks; ++b)
    {
        if (fuzz)
        {
            // SPINUS_CHECK_PARAM=<index> fuzzes one parameter only (to isolate crashes).
            const auto onlyParam = juce::SystemStats::getEnvironmentVariable ("SPINUS_CHECK_PARAM", "-1").getIntValue();

            for (int i = 0; i < plugin.getParameters().size(); ++i)
                if (onlyParam < 0 || i == onlyParam)
                    plugin.getParameters()[i]->setValue (unit (rng));
        }

        for (int i = 0; i < blockSize; ++i)
        {
            const auto s = 0.3f * (float)std::sin (phase) + 0.05f * (unit (rng) - 0.5f);
            phase += juce::MathConstants<double>::twoPi * 220.0 / sampleRate;

            for (int ch = 0; ch < channels; ++ch)
                buffer.setSample (ch, i, s);
        }

        const auto start = std::chrono::steady_clock::now();
        plugin.processBlock (buffer, midi);
        totalMicros += std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - start).count();

        bool nan = false;
        for (int ch = 0; ch < std::min (2, channels); ++ch)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const auto v = buffer.getSample (ch, i);
                if (! std::isfinite (v))
                    nan = true;
                else
                    sumSquares += (double)v * v;
            }
        }

        samples += 2 * (int64_t)blockSize;
        stats.blocksWithNaN += nan ? 1 : 0;
        ++stats.blocks;
    }

    plugin.releaseResources();
    stats.outputRms = (float)std::sqrt (sumSquares / (double)std::max<int64_t> (1, samples));
    stats.meanBlockMicros = totalMicros / numBlocks;
    return stats;
}

std::string label (const Curated& c, bool useClap)
{
    return std::string (c.role) + " [" + c.name + (useClap ? ", CLAP]" : ", VST3]");
}

} // namespace

//==============================================================================
TEST_CASE ("Bundled plugins: Bundled plugin files scan in both formats", "[bundled]")
{
    Host h;
    juce::StringArray files;
    for (auto& c : curated)
        files.addIfNotAlreadyThere (c.file);

    for (auto& file : files)
    {
        for (bool useClap : { true, false })
        {
            const auto path = Host::pathFor ({ "", file.toRawUTF8(), "" }, useClap);
            const auto found = h.scan (path, useClap);
            metric (file.toStdString() + (useClap ? " (CLAP)" : " (VST3)") + ": plugins", juce::String (found.size()));
            CHECK (! found.isEmpty());
        }
    }

    for (auto& c : curated)
    {
        for (bool useClap : { true, false })
        {
            juce::StringArray names;
            for (auto* d : h.scan (Host::pathFor (c, useClap), useClap))
                names.add (d->name);

            INFO (label (c, useClap) << ", found: " << names.joinIntoString (", "));
            CHECK (names.contains (c.name));
        }
    }
}

TEST_CASE ("Bundled plugins: Each curated plugin processes audio cleanly at 48 kHz / 128", "[bundled]")
{
    Host h;

    for (auto& c : curated)
    {
        for (bool useClap : { true, false })
        {
            auto desc = h.describe (c, useClap);
            INFO (label (c, useClap));
            REQUIRE (desc != nullptr);

            auto plugin = h.create (*desc, useClap, 48000.0, 128);
            const auto stats = process (*plugin, 48000.0, 128, 750, false);

            metric (label (c, useClap) + ": params / latency / rms / us per 128-sample block",
                    juce::String (plugin->getParameters().size()) + " / " + juce::String (plugin->getLatencySamples()) +
                        " / " + juce::String (stats.outputRms, 3) + " / " + juce::String (stats.meanBlockMicros, 1));
            CHECK (stats.blocksWithNaN == 0);

            // Every curated plugin is an effect that passes or colours the input at default settings.
            CHECK (stats.outputRms > 0.001f);
        }
    }
}

TEST_CASE ("Bundled plugins: State save and restore", "[bundled]")
{
    Host h;

    for (auto& c : curated)
    {
        for (bool useClap : { true, false })
        {
            auto desc = h.describe (c, useClap);
            INFO (label (c, useClap));
            REQUIRE (desc != nullptr);

            auto a = h.create (*desc, useClap, 48000.0, 512);
            auto b = h.create (*desc, useClap, 48000.0, 512);

            // Change a spread of parameters (skip the first, often a bypass switch).
            const auto& params = a->getParameters();
            std::vector<float> wanted;
            for (int i = 1; i < params.size(); i += std::max (1, params.size() / 8))
            {
                params[i]->setValueNotifyingHost (0.37f);
                wanted.push_back (params[i]->getValue());
            }

            // Let CLAP plugins apply queued parameter changes.
            process (*a, 48000.0, 512, 2, false);

            juce::MemoryBlock state;
            a->getStateInformation (state);
            REQUIRE (state.getSize() > 0);
            b->setStateInformation (state.getData(), (int)state.getSize());
            process (*b, 48000.0, 512, 2, false);

            juce::StringArray differing;
            for (int i = 0; i < params.size(); ++i)
                if (std::abs (params[i]->getValue() - b->getParameters()[i]->getValue()) > 1.0e-3f)
                    differing.add (params[i]->getName (32));

            metric (label (c, useClap) + ": state bytes / parameters differing after restore",
                    juce::String ((int)state.getSize()) + " / " + juce::String (differing.size()) +
                        (differing.isEmpty() ? "" : " (" + differing.joinIntoString (", ") + ")"));
            // CLAP is the format Spinus loads bundled plugins in. Through JUCE's VST3 host a few LSP and
            // Dragonfly parameters read back differently after a restore (reported, not required).
            if (useClap)
                CHECK (differing.isEmpty());
        }
    }
}

TEST_CASE ("Bundled plugins: Random parameter changes every block (like pluginval's automation test)",
           "[bundled][fuzz]")
{
    Host h;
    int pluginsWithNaN = 0;

    // SPINUS_CHECK_FORMAT=clap or =vst3 limits the run to one format (to isolate crashes).
    const auto only = juce::SystemStats::getEnvironmentVariable ("SPINUS_CHECK_FORMAT", {});

    for (auto& c : curated)
    {
        for (bool useClap : { true, false })
        {
            if (only.isNotEmpty() && only != (useClap ? "clap" : "vst3"))
                continue;

            auto desc = h.describe (c, useClap);
            INFO (label (c, useClap));
            REQUIRE (desc != nullptr);

            int nanBlocks = 0;
            int blocks = 0;

            for (auto [rate, size] : { std::pair { 48000.0, 128 }, std::pair { 96000.0, 1024 } })
            {
                auto plugin = h.create (*desc, useClap, rate, size);
                const auto stats = process (*plugin, rate, size, 200, true, 7);
                nanBlocks += stats.blocksWithNaN;
                blocks += stats.blocks;
            }

            metric (label (c, useClap) + ": blocks with NaN under random parameters",
                    juce::String (nanBlocks) + " of " + juce::String (blocks));
            pluginsWithNaN += nanBlocks > 0 ? 1 : 0;
        }
    }

    // Informational: a NaN here means Spinus must guard plugin output (the invalid-audio guard).
    metric ("plugin/format combinations producing NaN under fuzzing", juce::String (pluginsWithNaN));
}

TEST_CASE ("Bundled plugins: 64 channels x 4 bundled effects (target: <= 50% DSP)", "[bundled][realtime]")
{
    Host h;
    auto& engine = h.host.getEngine();
    engine.getPluginManager().createBuiltInType<te::ToneGeneratorPlugin>();

    RealtimeRunner runner (engine, { .outputChannels = 2, .maxSeconds = 8.0, .paceInRealTime = true });
    auto edit = te::Edit::createSingleTrackEdit (engine);
    edit->getMasterVolumePlugin()->setVolumeDb (-24.0f);

    // EQ, compressor, gate and limiter: a typical mixer channel strip.
    std::vector<std::unique_ptr<juce::PluginDescription>> chain;
    for (int i : { 0, 1, 3, 2 })
    {
        chain.push_back (h.describe (curated[(size_t)i], true));
        REQUIRE (chain.back() != nullptr);
        engine.getPluginManager().knownPluginList.addType (*chain.back());
    }

    for (int t = 0; t < 64; ++t)
    {
        auto tracks = te::getAudioTracks (*edit);
        auto track =
            t == 0 ? tracks[0] : edit->insertNewAudioTrack (te::TrackInsertPoint (nullptr, tracks.getLast()), nullptr);

        auto tone = edit->getPluginCache().createNewPlugin (te::ToneGeneratorPlugin::xmlTypeName, {});
        dynamic_cast<te::ToneGeneratorPlugin&> (*tone).frequency = 110.0f + 5.0f * (float)t;
        track->pluginList.insertPlugin (tone, 0, nullptr);

        for (size_t i = 0; i < chain.size(); ++i)
        {
            auto plugin = edit->getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, *chain[i]);
            auto* external = dynamic_cast<te::ExternalPlugin*> (plugin.get());
            REQUIRE ((external != nullptr && external->getAudioPluginInstance() != nullptr));
            track->pluginList.insertPlugin (plugin, (int)i + 1, nullptr);
        }
    }

    edit->dispatchPendingUpdatesSynchronously();
    edit->getTransport().ensureContextAllocated();
    edit->getTransport().play (false);
    runner.start();
    runner.runUntil (7.0);
    runner.stop();
    edit->getTransport().stop (false, true);

    const auto& times = runner.getBlockTimesMs();
    std::vector<float> steady (times.begin() + (long)(times.size() / 7), times.end());
    std::sort (steady.begin(), steady.end());
    double sum = 0.0;
    for (auto t : steady)
        sum += t;

    const auto period = runner.getBlockPeriodMs();
    const auto meanLoad = 100.0 * sum / (double)steady.size() / period;
    int overruns = 0;
    for (auto t : steady)
        if (t > period)
            ++overruns;

    metric ("64 x (tone + EQ + compressor + gate + limiter), CLAP: mean DSP load % (48 kHz, 128)",
            juce::String (meanLoad, 1));
    metric ("99th percentile block ms", juce::String (steady[steady.size() * 99 / 100], 3));
    metric ("max block ms (budget " + juce::String (period, 2).toStdString() + ")", juce::String (steady.back(), 3));
    metric ("overrun blocks after start-up", juce::String (overruns));

    const auto sr = runner.getParams().sampleRate;
    CHECK (checks::rms (runner.getChannel (0), (int64_t)(2.0 * sr), (int64_t)(6.0 * sr)) > 0.001f);
    CHECK (meanLoad <= 50.0);
}

//==============================================================================
/*  The free plugins Spinus downloads rather than builds (Odin 2, OB-Xf, PeakEater, Six Sines, TIME-12,
    GATE-12). Point SPINUS_CHECK_DOWNLOADED at a folder holding their unpacked CLAP files and VST3 folders
    (searched recursively), e.g. the plugins folder after Options › Get free plugins. Skipped without it. */
TEST_CASE ("Downloaded plugins: load, play notes and survive random parameter changes every block",
           "[downloaded][fuzz]")
{
    const juce::File folder { juce::SystemStats::getEnvironmentVariable ("SPINUS_CHECK_DOWNLOADED", {}) };
    if (! folder.isDirectory())
        SKIP ("SPINUS_CHECK_DOWNLOADED is not set to a folder");

    struct Downloaded
    {
        const char* file;
        const char* name;
        bool instrument;
        bool clap = true; // or VST3, for the projects that publish no CLAP build
    };
    const std::vector<Downloaded> downloaded {
        { "Odin2.clap", "Odin2", true },
        { "OB-Xf.clap", "OB-Xf", true },
        { "peakeater.clap", "peakeater", false },
        { "Six Sines.clap", "Six Sines", true },
        { "TIME-12.vst3", "TIME-12", false, false },
        { "GATE-12.vst3", "GATE-12", false, false },
    };

    Host h;
    for (auto& d : downloaded)
    {
        INFO (d.file);
        const auto files = folder.findChildFiles (juce::File::findFilesAndDirectories, true, d.file);
        if (files.isEmpty())
        {
            metric (std::string (d.name) + ": not in " + folder.getFullPathName().toStdString(), "skipped");
            continue;
        }

        std::unique_ptr<juce::PluginDescription> desc;
        for (auto* found : h.scan (files[0].getFullPathName(), d.clap))
            if (found->name.equalsIgnoreCase (d.name))
                desc = std::make_unique<juce::PluginDescription> (*found);
        REQUIRE (desc != nullptr);

        for (auto [rate, size] : { std::pair { 48000.0, 128 }, std::pair { 96000.0, 1024 } })
        {
            auto plugin = h.create (*desc, d.clap, rate, size);
            const auto channels =
                std::max ({ 2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels() });
            plugin->setRateAndBufferSizeDetails (rate, size);
            plugin->prepareToPlay (rate, size);

            juce::AudioBuffer<float> buffer (channels, size);
            std::mt19937 rng (7); // NOLINT(bugprone-random-generator-seed): a fixed seed keeps failures reproducible
            std::uniform_real_distribution<float> unit (0.0f, 1.0f);
            int nanBlocks = 0;
            double peakMicros = 0.0;
            constexpr int blocks = 400;

            for (int b = 0; b < blocks; ++b)
            {
                for (auto* p : plugin->getParameters())
                    p->setValue (unit (rng));

                juce::MidiBuffer midi;
                if (d.instrument && b % 20 == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 48 + (int)(unit (rng) * 36.0f), 0.8f), 0);
                if (d.instrument && b % 20 == 10)
                    midi.addEvent (juce::MidiMessage::allNotesOff (1), 0);

                for (int ch = 0; ch < channels; ++ch)
                    for (int i = 0; i < size; ++i)
                        buffer.setSample (ch, i, d.instrument ? 0.0f : 0.3f * (unit (rng) - 0.5f));

                const auto start = std::chrono::steady_clock::now();
                plugin->processBlock (buffer, midi);
                peakMicros = std::max (
                    peakMicros,
                    std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - start).count());

                bool nan = false;
                for (int ch = 0; ch < std::min (2, channels); ++ch)
                    for (int i = 0; i < size; ++i)
                        nan = nan || ! std::isfinite (buffer.getSample (ch, i));
                nanBlocks += nan ? 1 : 0;
            }

            // Then playing with its settings left alone, as in a song: the time a block takes.
            double steadyPeak = 0.0, steadySum = 0.0;
            constexpr int steadyBlocks = 400;
            for (int b = 0; b < steadyBlocks; ++b)
            {
                juce::MidiBuffer midi;
                if (d.instrument && b % 20 == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
                for (int ch = 0; ch < channels; ++ch)
                    for (int i = 0; i < size; ++i)
                        buffer.setSample (ch, i, d.instrument ? 0.0f : 0.3f * (unit (rng) - 0.5f));
                const auto start = std::chrono::steady_clock::now();
                plugin->processBlock (buffer, midi);
                const auto micros =
                    std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - start).count();
                steadyPeak = std::max (steadyPeak, micros);
                steadySum += micros;
            }

            plugin->releaseResources();

            // State survives a save and restore.
            juce::MemoryBlock state;
            plugin->getStateInformation (state);
            auto restored = h.create (*desc, d.clap, rate, size);
            restored->setStateInformation (state.getData(), (int)state.getSize());

            const auto at = juce::String ((int)rate) + " Hz / " + juce::String (size);
            metric (std::string (d.name) + " " + at.toStdString() + ": blocks with NaN under random parameters",
                    juce::String (nanBlocks) + " of " + juce::String (blocks));
            metric (std::string (d.name) + " " + at.toStdString() + ": slowest block (us)",
                    juce::String (juce::roundToInt (peakMicros)));
            metric (std::string (d.name) + " " + at.toStdString() +
                        ": settings left alone, mean / slowest block (us, budget " +
                        std::to_string (juce::roundToInt (1.0e6 * size / rate)) + ")",
                    juce::String (juce::roundToInt (steadySum / steadyBlocks)) + " / " +
                        juce::String (juce::roundToInt (steadyPeak)));
            metric (std::string (d.name) + " " + at.toStdString() + ": state bytes",
                    juce::String ((int)state.getSize()));
        }
    }
}
