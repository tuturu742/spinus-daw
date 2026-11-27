#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "commands/CommandRegistry.h"
#include "engine/DeviceFactory.h"
#include "engine/EngineHost.h"
#include "engine/OutputMix.h"
#include "plugins/SandboxList.h"
#include "session/Session.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "plugins/sandbox/SandboxPluginFormat.h"
#include "session/OfflineInstrument.h"

using namespace spinus;
using namespace spinus::engine;
using Catch::Approx;
using outputmix::Side;

namespace
{
juce::PluginDescription manyOutputs()
{
    plugins::ClapPluginFormat clap;
    juce::OwnedArray<juce::PluginDescription> found;
    clap.findAllTypesForFile (
        found, juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile ("many-outputs.clap").getFullPathName());
    REQUIRE (found.size() == 1);
    return *found[0];
}

/** The test instrument plays (k + 1) / 100 on its channel k: all sixteen in the middle, at -3 dB. */
float allInTheMiddle()
{
    float sum = 0.0f;
    for (int k = 0; k < 16; ++k)
        sum += (float)(k + 1) / 100.0f;
    return sum * outputmix::centreGain;
}
} // namespace

TEST_CASE ("Where each of an instrument's many outputs goes in stereo", "[engine][outputs]")
{
    SECTION ("two channels or fewer play as they are")
    {
        CHECK (outputmix::sidesOf (juce::AudioChannelSet::stereo()).empty());
        CHECK (outputmix::sidesOf (juce::AudioChannelSet::mono()).empty());
        CHECK (outputmix::sidesOf (juce::AudioChannelSet::discreteChannels (2)).empty());
    }
    SECTION ("numbered outputs (a microphone on each) go to the middle")
    {
        CHECK (outputmix::sidesOf (juce::AudioChannelSet::discreteChannels (4)) ==
               std::vector<Side> { Side::centre, Side::centre, Side::centre, Side::centre });
    }
    SECTION ("a left output goes left and a right one right")
    {
        const auto sides = outputmix::sidesOf (juce::AudioChannelSet::create5point1());
        REQUIRE (sides.size() == 6);
        for (int c = 0; c < 6; ++c)
        {
            const auto type = juce::AudioChannelSet::create5point1().getTypeOfChannel (c);
            using T = juce::AudioChannelSet::ChannelType;
            const auto expected = type == T::left || type == T::leftSurround     ? Side::left
                                  : type == T::right || type == T::rightSurround ? Side::right
                                                                                 : Side::centre;
            CHECK (sides[(size_t)c] == expected);
        }
    }
}

TEST_CASE ("An instrument's many outputs are mixed into two", "[engine][outputs]")
{
    juce::AudioBuffer<float> buffer (4, 8);
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 8; ++i)
            buffer.setSample (c, i, (float)(c + 1));
    const std::vector<Side> sides { Side::left, Side::right, Side::centre, Side::centre };

    // Only the range asked for: samples 2 to 5.
    outputmix::mixToStereo (buffer, 2, 4, sides.data(), (int)sides.size());
    const auto middle = (3.0f + 4.0f) * outputmix::centreGain;
    CHECK (buffer.getSample (0, 3) == Approx (1.0f + middle));
    CHECK (buffer.getSample (1, 3) == Approx (2.0f + middle));
    CHECK (buffer.getSample (2, 3) == Approx (0.0f));
    CHECK (buffer.getSample (3, 5) == Approx (0.0f));
    CHECK (buffer.getSample (0, 1) == Approx (1.0f)); // outside the range: untouched
    CHECK (buffer.getSample (3, 6) == Approx (4.0f));

    // No sides: nothing changes.
    juce::AudioBuffer<float> stereo (2, 4);
    stereo.clear();
    stereo.setSample (0, 0, 0.5f);
    outputmix::mixToStereo (stereo, 0, 4, nullptr, 0);
    CHECK (stereo.getSample (0, 0) == Approx (0.5f));
}

TEST_CASE ("An instrument with sixteen outputs is heard whole when it plays on its own", "[engine][outputs]")
{
    plugins::ClapPluginFormat clap;
    juce::String error;
    auto instance = clap.createInstanceFromDescription (manyOutputs(), 48000.0, 512, error);
    INFO (error);
    REQUIRE (instance != nullptr);
    REQUIRE (instance->getTotalNumOutputChannels() == 16);

    OfflineInstrument player (*instance);
    const auto sound = player.renderStereo ({ { 60, 0.0, 0.1, 0.8f } }, 0.0);
    REQUIRE (sound.getNumChannels() == 2);
    const auto at = sound.getNumSamples() / 2;
    CHECK (sound.getSample (0, at) == Approx (allInTheMiddle()).margin (1.0e-4));
    CHECK (sound.getSample (1, at) == Approx (allInTheMiddle()).margin (1.0e-4));
}

TEST_CASE ("A plugin in its own process gives all its outputs back, past the first eight", "[engine][outputs][sandbox]")
{
    const auto real = manyOutputs();
    plugins::sandbox::SandboxPluginFormat format (
        juce::File (SPINUS_PLUGIN_HOST),
        [&real] (const juce::String& f, const juce::String& file, int) -> std::optional<juce::PluginDescription>
        {
            if (f == real.pluginFormatName && file == real.fileOrIdentifier)
                return real;
            return std::nullopt;
        });
    juce::String error;
    constexpr int block = 256;
    auto instance = format.createInstanceFromDescription (plugins::sandbox::SandboxPluginFormat::describe (real),
                                                          48000.0, block, error);
    INFO (error);
    REQUIRE (instance != nullptr);
    REQUIRE (instance->getTotalNumOutputChannels() == 16);
    // The same main output as the plugin's own: sixteen numbered channels, so they are mixed to the middle.
    CHECK (outputmix::sidesOf (instance->getChannelLayoutOfBus (false, 0)).size() == 16);

    instance->setNonRealtime (true); // each block waits for its answer
    instance->prepareToPlay (48000.0, block);
    juce::AudioBuffer<float> buffer (16, block);
    juce::MidiBuffer midi;
    for (int b = 0; b < 8; ++b)
    {
        buffer.clear();
        instance->processBlock (buffer, midi);
    }
    for (int c = 0; c < 16; ++c)
    {
        INFO ("output " << c + 1);
        CHECK (buffer.getSample (c, block - 1) == Approx ((float)(c + 1) / 100.0f).margin (1.0e-6));
    }
    instance->releaseResources();
}

TEST_CASE ("A song with an instrument of sixteen outputs exports all of them, in its process or its own",
           "[engine][outputs][sandbox][export]")
{
    const bool separately = GENERATE (false, true);
    INFO ((separately ? "in a separate process" : "in Spinus's process"));
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("spinus-many-outputs-" + juce::Uuid().toString());
    root.createDirectory();
    EngineHost host ({ .openAudioDevice = false, .pluginHost = juce::File (SPINUS_PLUGIN_HOST) });
    const auto real = manyOutputs();
    host.getEngine().getPluginManager().knownPluginList.addType (real);
    plugins::SandboxList list { root.getChildFile ("run-separately.json") };
    if (separately)
        list.set (plugins::SandboxList::keyOf ("CLAP", real.fileOrIdentifier), true, real.name);
    auto devices = std::make_unique<DefaultDeviceFactory>();
    devices->setRunSeparately (
        [&list] (const juce::PluginDescription& d)
        { return list.contains (plugins::SandboxList::keyOf (d.pluginFormatName, d.fileOrIdentifier)); });
    auto session = std::make_unique<Session> (host.getEngine(), std::move (devices));
    session->setSandboxList (&list);
    auto run = [&] (const juce::String& name, const juce::String& params)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (name, juce::JSON::parse (params), error);
        INFO (name << ": " << error);
        REQUIRE (command != nullptr);
        auto* raw = command.get();
        REQUIRE (session->getHistory().perform (std::move (command)).wasOk());
        return raw->getResult();
    };
    const auto channel = run (
        "channel.add",
        R"({"name": "Kit", "instrument": {"kind": "plugin", "plugin": {"format": "CLAP", "name": "Many Outputs", "manufacturer": "Spinus tests", "fileOrIdentifier": )" +
            juce::JSON::toString (real.fileOrIdentifier) + "}}}");
    const auto pattern = run ("pattern.add", R"({"name": "P", "length": 3840})");
    run ("pattern.setNotes", R"({"pattern": )" + pattern["pattern"].toString() + R"(, "channel": )" +
                                 channel["channel"].toString() +
                                 R"(, "notes": [{"start": 0, "length": 3600, "pitch": 60, "velocity": 0.9}]})");
    const auto lane = run ("lane.add", "{}");
    run ("clip.addPattern", R"({"lane": )" + lane["lane"].toString() + R"(, "pattern": )" +
                                pattern["pattern"].toString() + R"(, "start": 0})");
    session->setMode (Session::Mode::song);
    session->getEdit().dispatchPendingUpdatesSynchronously();

    ExportSettings settings;
    settings.format = ExportSettings::Format::wav32Float;
    settings.sampleRate = 48000.0;
    settings.tailSeconds = 0.2;
    std::optional<ExportJob::Result> result;
    const auto out = root.getChildFile ("Song.wav");
    auto job = session->createExportJob (out, settings, [&] (const ExportJob::Result& r) { result = r; });
    job->start();
    for (int i = 0; i < 1200 && ! result; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    job = nullptr;
    REQUIRE (result.has_value());
    INFO (result.value_or (ExportJob::Result {}).error);
    REQUIRE (result.value_or (ExportJob::Result {}).ok);

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (new juce::FileInputStream (out), true));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio (2, (int)reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
    const auto at = (int)(0.5 * 48000.0);
    const auto left = audio.getSample (0, at), right = audio.getSample (1, at);
    // All sixteen, not only the first two (0.03 together), the same on both sides, and nothing broken.
    CHECK (left > 0.3f);
    CHECK (right == Approx (left).margin (1.0e-3));
    CHECK (audio.getMagnitude (0, audio.getNumSamples()) < 2.0f);
    session = nullptr;
    root.deleteRecursively();
}
