#include <catch2/catch_test_macros.hpp>

#include "commands/CommandRegistry.h"
#include "dsp/drums/DrumMachineProcessor.h"
#include "dsp/sampler/SamplerProcessor.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "session/StemSamples.h"

using namespace spinus;
namespace te = tracktion;

namespace
{
struct StemSession
{
    StemSession() : session (host.getEngine(), makeDevices (host.getEngine())) { folder.createDirectory(); }
    ~StemSession() { folder.deleteRecursively(); }

    static std::unique_ptr<engine::DeviceFactory> makeDevices (te::Engine& engine)
    {
        auto devices = std::make_unique<engine::DefaultDeviceFactory>();
        engine::registerBundledDevices (*devices, engine, {});
        return devices;
    }

    model::ChannelId addChannel (const juce::String& stockType)
    {
        juce::String error;
        auto command = commands::CommandRegistry::get().create (
            "channel.add",
            juce::JSON::parse (R"({"name": "Stem", "instrument": {"kind": "stock", "stockType": ")" + stockType +
                               R"("}})"),
            error);
        REQUIRE (command != nullptr);
        auto* raw = command.get();
        REQUIRE (session.getHistory().perform (std::move (command)).wasOk());
        return model::ChannelId { (uint32_t)(int)raw->getResult()["channel"] };
    }

    template <typename Processor> Processor* instrument (model::ChannelId channel)
    {
        const auto* c = session.getProject().findChannel (channel);
        if (c == nullptr || ! c->instrument)
            return nullptr;
        auto* plugin = dynamic_cast<te::ExternalPlugin*> (
            session.getAdapter().getDevicePlugin (c->instrument.value_or (model::Device {}).id));
        return plugin != nullptr ? dynamic_cast<Processor*> (plugin->getAudioPluginInstance()) : nullptr;
    }

    /** A 2-second mono stem at 44.1 kHz: full-scale DC from 0.5 s to 1.0 s, quiet otherwise. */
    juce::File writeStem()
    {
        const auto file = folder.getChildFile ("stem.wav");
        juce::AudioBuffer<float> audio (1, 88200);
        audio.clear();
        for (int i = 22050; i < 44100; ++i)
            audio.setSample (0, i, 0.5f);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = wav.createWriterFor (
            stream,
            juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (24));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
        return file;
    }

    EngineHost host { { .openAudioDevice = false } };
    Session session;
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("spinus-stem-samples-" + juce::Uuid().toString());
};
} // namespace

TEST_CASE ("A piece of a stem is cut with short fades", "[session][samples]")
{
    StemSession s;
    const auto stem = s.writeStem();
    std::unique_ptr<juce::AudioFormatReader> reader (
        juce::WavAudioFormat().createReaderFor (stem.createInputStream().release(), true));
    REQUIRE (reader != nullptr);
    const auto piece = cutPiece (*reader, 0.5, 1.0);
    REQUIRE (piece.getNumSamples() == 22050);
    CHECK (std::abs (piece.getSample (0, 0)) < 1e-6f);            // fades in
    CHECK (std::abs (piece.getSample (0, 11000) - 0.5f) < 1e-3f); // the sound itself
    CHECK (std::abs (piece.getSample (0, 22049)) < 2e-3f);        // fades out
    CHECK (cutPiece (*reader, 3.0, 4.0).getNumSamples() == 0);    // past the end
    CHECK (cutPiece (*reader, 1.0, 0.5).getNumSamples() == 0);
}

TEST_CASE ("The song's drum hits go on the pads that play them; undo and redo keep them", "[session][samples]")
{
    StemSession s;
    const auto channel = s.addChannel ("spinus.drum-machine");
    auto* drums = s.instrument<dsp::DrumMachineProcessor> (channel);
    REQUIRE (drums != nullptr);
    const auto snare = drums->getPads()[1];

    REQUIRE (loadDrumPieces (s.session, channel, s.writeStem(), { { 60, 0.5, 0.75, "Song - kick" } }).wasOk());
    const auto& pads = drums->getPads();
    CHECK (pads[0].name == "Song - kick");
    REQUIRE (pads[0].audio != nullptr);
    CHECK (pads[0].audio->getNumSamples() == 11025);
    CHECK (pads[1] == snare); // the snare pad keeps the Synth Kit's sound

    // Undo takes the channel away; redo brings it back with the song's kick.
    s.session.getHistory().undo();
    CHECK (s.session.getProject().channels.empty());
    s.session.getHistory().redo();
    auto* again = s.instrument<dsp::DrumMachineProcessor> (channel);
    REQUIRE (again != nullptr);
    CHECK (again->getPads()[0].name == "Song - kick");

    CHECK (! loadDrumPieces (s.session, model::ChannelId { 999 }, s.writeStem(), {}).wasOk());
}

TEST_CASE ("A stem's phrases go into a Sampler, one key each", "[session][samples]")
{
    StemSession s;
    const auto channel = s.addChannel ("spinus.sampler");
    REQUIRE (loadSamplerPieces (s.session, channel, s.writeStem(),
                                { { 60, 0.0, 0.5, "Song - phrase 1" }, { 61, 0.5, 1.5, "Song - phrase 2" } })
                 .wasOk());
    auto* sampler = s.instrument<dsp::SamplerProcessor> (channel);
    REQUIRE (sampler != nullptr);
    const auto& zones = sampler->getZones();
    REQUIRE (zones.size() == 2);
    CHECK ((zones[1].rootNote == 61 && zones[1].lowKey == 61 && zones[1].highKey == 61));
    CHECK (zones[1].getLength() == 44100);
    CHECK (zones[1].name == "Song - phrase 2");

    // Not a Sampler: it says so.
    const auto drumChannel = s.addChannel ("spinus.drum-machine");
    CHECK (! loadSamplerPieces (s.session, drumChannel, s.writeStem(), {}).wasOk());
}

TEST_CASE ("A drum sample file goes on the pad that plays its note; a pad's sound can be copied", "[session][samples]")
{
    StemSession s;
    const auto channel = s.addChannel ("spinus.drum-machine");
    auto* drums = s.instrument<dsp::DrumMachineProcessor> (channel);
    REQUIRE (drums != nullptr);
    REQUIRE (loadDrumPadFile (s.session, channel, 61, s.writeStem()).wasOk());
    CHECK (drums->getPads()[1].name == "stem");
    CHECK (! loadDrumPadFile (s.session, channel, 20, s.writeStem()).wasOk()); // no pad plays C0

    // An AIFF file named .wav (as some sample packs have): read by its contents.
    {
        const auto aiff = s.folder.getChildFile ("Kick Drum.wav");
        juce::AudioBuffer<float> audio (1, 4410);
        for (int i = 0; i < audio.getNumSamples(); ++i)
            audio.setSample (0, i, 0.5f * std::sin (0.05f * (float)i));
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (aiff);
        auto writer = juce::AiffAudioFormat().createWriterFor (
            stream,
            juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }
    REQUIRE (loadDrumPadFile (s.session, channel, 60, s.folder.getChildFile ("Kick Drum.wav")).wasOk());
    CHECK (drums->getPads()[0].name == "Kick Drum");

    REQUIRE (copyDrumPad (s.session, channel, 61, 69, "snare 2").wasOk());
    CHECK (drums->getPads()[9].name == "snare 2");
    REQUIRE (drums->getPads()[9].audio != nullptr);
    CHECK (drums->getPads()[9].audio->getNumSamples() == drums->getPads()[1].audio->getNumSamples());
    CHECK (! copyDrumPad (s.session, model::ChannelId { 999 }, 61, 69, "x").wasOk());
}
