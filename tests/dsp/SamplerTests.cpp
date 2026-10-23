#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "dsp/NoteExpression.h"
#include "dsp/SpinusPluginFormat.h"
#include "dsp/sampler/SamplerEditor.h"
#include "dsp/sampler/SamplerProcessor.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "ui/ChannelRack.h"

using namespace spinus;
using namespace spinus::dsp;

namespace
{
constexpr double rate = 48000.0;
constexpr int block = 256;

juce::AudioBuffer<float> sine (double frequency, double seconds, float level = 0.5f, int channels = 1)
{
    juce::AudioBuffer<float> b (channels, (int)(seconds * rate));
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (c, i, level * (float)std::sin (2.0 * M_PI * frequency * i / rate));
    return b;
}

juce::AudioBuffer<float> constant (float value, double seconds)
{
    juce::AudioBuffer<float> b (1, (int)(seconds * rate));
    b.clear();
    for (int i = 0; i < b.getNumSamples(); ++i)
        b.setSample (0, i, value);
    return b;
}

/** Runs a prepared Sampler block by block; notes are sent at the start of a block. */
struct Player
{
    explicit Player (SamplerProcessor& p) : sampler (p) { sampler.prepareToPlay (rate, block); }

    /** Renders `seconds`, sending `events` first; returns the left channel. */
    std::vector<float> play (double seconds, const std::vector<juce::MidiMessage>& events = {})
    {
        std::vector<float> out;
        juce::AudioBuffer<float> buffer (2, block);
        juce::MidiBuffer midi;
        for (auto& e : events)
            midi.addEvent (e, 0);

        for (int done = 0; done < (int)(seconds * rate); done += block)
        {
            sampler.processBlock (buffer, midi);
            midi.clear();
            for (int i = 0; i < block; ++i)
                out.push_back (buffer.getSample (0, i));
        }
        return out;
    }

    SamplerProcessor& sampler;
};

double frequencyOf (const std::vector<float>& x, size_t from, size_t to)
{
    int crossings = 0;
    for (auto i = from + 1; i < to; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
            ++crossings;
    return crossings * rate / (double)(to - from);
}

float peak (const std::vector<float>& x, size_t from, size_t to)
{
    float p = 0.0f;
    for (auto i = from; i < juce::jmin (to, x.size()); ++i)
        p = juce::jmax (p, std::abs (x[i]));
    return p;
}

void setParameter (SamplerProcessor& s, const juce::String& id, float value)
{
    auto* parameter = s.getParameterState().getParameter (id);
    REQUIRE (parameter != nullptr);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

juce::File writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                   .withSampleRate (rate)
                                                   .withNumChannels (audio.getNumChannels())
                                                   .withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

struct TempFolder
{
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-sampler-test") + "-" + juce::Uuid().toString());
};
} // namespace

TEST_CASE ("Note names in file names, FL style", "[dsp][sampler]")
{
    CHECK (noteFromFileName ("Piano_C5") == 60);
    CHECK (noteFromFileName ("Piano C#4 soft") == 49);
    CHECK (noteFromFileName ("bass-Bb2") == 34);
    CHECK (noteFromFileName ("Strings_A-1") == std::nullopt); // below the keyboard
    CHECK (noteFromFileName ("Kick2") == std::nullopt);
    CHECK (noteFromFileName ("Bass") == std::nullopt);
    CHECK (noteFromFileName ("Snare 07") == std::nullopt);
    CHECK (noteName (60) == "C5");
    CHECK (noteName (61) == "C#5");
    CHECK (noteName (0) == "C0");

    std::vector<SampleZone> zones (3);
    zones[0].rootNote = 48;
    zones[1].rootNote = 60;
    zones[2].rootNote = 72;
    mapZonesByRoot (zones);
    CHECK ((zones[0].lowKey == 0 && zones[0].highKey == 54));
    CHECK ((zones[1].lowKey == 55 && zones[1].highKey == 66));
    CHECK ((zones[2].lowKey == 67 && zones[2].highKey == 127));
}

TEST_CASE ("The Sampler plays a sample at the note's pitch", "[dsp][sampler]")
{
    SamplerProcessor sampler;
    sampler.addZone (sine (440.0, 2.0), rate, "A", 60);
    Player player (sampler);

    auto out = player.play (0.5, { juce::MidiMessage::noteOn (1, 60, (juce::uint8)127) });
    CHECK (frequencyOf (out, 2000, 20000) == Catch::Approx (440.0).epsilon (0.01));

    player.play (0.5, { juce::MidiMessage::noteOff (1, 60) });
    out = player.play (0.5, { juce::MidiMessage::noteOn (1, 72, (juce::uint8)127) });
    CHECK (frequencyOf (out, 2000, 20000) == Catch::Approx (880.0).epsilon (0.01));

    // Pitch knob: +7 semitones.
    player.play (0.5, { juce::MidiMessage::noteOff (1, 72) });
    setParameter (sampler, "coarse", 7.0f);
    out = player.play (0.5, { juce::MidiMessage::noteOn (1, 60, (juce::uint8)127) });
    CHECK (frequencyOf (out, 2000, 20000) == Catch::Approx (440.0 * std::pow (2.0, 7.0 / 12.0)).epsilon (0.01));
}

TEST_CASE ("The Sampler plays per-note fine pitch, volume and pan", "[dsp][sampler]")
{
    SamplerProcessor sampler;
    sampler.addZone (sine (440.0, 2.0), rate, "A", 60);
    sampler.prepareToPlay (rate, block);

    // A note on a member channel with its values first: a semitone up, half volume, hard left.
    juce::MidiBuffer midi;
    for (auto& m : dsp::noteExpression::encode (3, { 100.0f, -1.0f, 0.5f }))
        midi.addEvent (m, 0);
    midi.addEvent (juce::MidiMessage::noteOn (3, 60, (juce::uint8)127), 0);

    std::vector<float> left, right;
    juce::AudioBuffer<float> buffer (2, block);
    for (int done = 0; done < (int)(0.5 * rate); done += block)
    {
        sampler.processBlock (buffer, midi);
        midi.clear();
        for (int i = 0; i < block; ++i)
        {
            left.push_back (buffer.getSample (0, i));
            right.push_back (buffer.getSample (1, i));
        }
    }

    CHECK (frequencyOf (left, 2000, 20000) == Catch::Approx (440.0 * std::pow (2.0, 1.0 / 12.0)).epsilon (0.01));
    CHECK (peak (left, 2000, 20000) == Catch::Approx (0.25).epsilon (0.03)); // the sine's 0.5, at half volume
    CHECK (peak (right, 2000, 20000) < 0.001f);

    // A plain note on channel 1 is not affected by the member channel's values.
    Player player (sampler);
    player.play (0.5, { juce::MidiMessage::noteOff (3, 60) });
    const auto plain = player.play (0.5, { juce::MidiMessage::noteOn (1, 60, (juce::uint8)127) });
    CHECK (frequencyOf (plain, 2000, 20000) == Catch::Approx (440.0).epsilon (0.01));
}

TEST_CASE ("Envelope, one-shot, loop and velocity", "[dsp][sampler]")
{
    SamplerProcessor sampler;
    sampler.addZone (sine (440.0, 0.5), rate, "A", 60);
    setParameter (sampler, "release", 0.01f);
    Player player (sampler);

    SECTION ("A note stops after its release")
    {
        auto out = player.play (0.1, { juce::MidiMessage::noteOn (1, 60, (juce::uint8)127) });
        CHECK (peak (out, 0, out.size()) > 0.4f);
        out = player.play (0.2, { juce::MidiMessage::noteOff (1, 60) });
        CHECK (peak (out, (size_t)(0.05 * rate), out.size()) < 1.0e-4f);
        CHECK (sampler.getNumActiveVoices() == 0);
    }

    SECTION ("One-shot ignores the note-off and plays to the end of the sample")
    {
        setParameter (sampler, "oneShot", 1.0f);
        player.play (0.1, { juce::MidiMessage::noteOn (1, 60, (juce::uint8)127) });
        auto out = player.play (0.2, { juce::MidiMessage::noteOff (1, 60) });
        CHECK (peak (out, 0, (size_t)(0.2 * rate)) > 0.4f);
        out = player.play (0.5);
        CHECK (peak (out, (size_t)(0.3 * rate), out.size()) < 1.0e-6f); // the sample is over
    }

    SECTION ("Without a loop the sample ends; with one it keeps playing while held")
    {
        auto out = player.play (1.0, { juce::MidiMessage::noteOn (1, 60, (juce::uint8)127) });
        CHECK (peak (out, (size_t)(0.6 * rate), out.size()) < 1.0e-6f);
        player.play (0.1, { juce::MidiMessage::noteOff (1, 60) });

        sampler.updateZone (0,
                            [] (SampleZone& z)
                            {
                                z.loop = true;
                                z.loopStart = (juce::int64)(0.1 * rate);
                                z.loopEnd = (juce::int64)(0.2 * rate);
                            });
        out = player.play (1.5, { juce::MidiMessage::noteOn (1, 60, (juce::uint8)127) });
        CHECK (peak (out, (size_t)(1.2 * rate), out.size()) > 0.4f);
        // A whole number of cycles in the loop: the loop point does not click.
        CHECK (test::checks::findSineDiscontinuities (out, (int64_t)(0.15 * rate), (int64_t)(1.4 * rate), 440.0, rate,
                                                      0.02f)
                   .empty());
    }

    SECTION ("Velocity sets the level, unless the Velocity knob is at 0")
    {
        auto loud = player.play (0.1, { juce::MidiMessage::noteOn (1, 60, (juce::uint8)127) });
        player.play (0.2, { juce::MidiMessage::noteOff (1, 60) });
        auto soft = player.play (0.1, { juce::MidiMessage::noteOn (1, 60, (juce::uint8)32) });
        player.play (0.2, { juce::MidiMessage::noteOff (1, 60) });
        CHECK (peak (soft, 0, soft.size()) < peak (loud, 0, loud.size()) * 0.1f);

        setParameter (sampler, "velocity", 0.0f);
        soft = player.play (0.1, { juce::MidiMessage::noteOn (1, 60, (juce::uint8)32) });
        CHECK (peak (soft, 0, soft.size()) == Catch::Approx (peak (loud, 0, loud.size())).epsilon (0.02));
    }
}

TEST_CASE ("Zones: key ranges, velocity layers and round-robin", "[dsp][sampler]")
{
    SamplerProcessor sampler;
    sampler.addZone (constant (0.25f, 0.2), rate, "soft");
    sampler.addZone (constant (0.75f, 0.2), rate, "loud");
    sampler.addZone (constant (-0.5f, 0.2), rate, "other");
    sampler.updateZone (0, [] (SampleZone& z) { z.lowKey = z.highKey = 60, z.highVelocity = 64; });
    sampler.updateZone (1, [] (SampleZone& z) { z.lowKey = z.highKey = 60, z.lowVelocity = 65; });
    sampler.updateZone (2, [] (SampleZone& z) { z.lowKey = z.highKey = 62; });
    setParameter (sampler, "velocity", 0.0f);
    Player player (sampler);

    auto levelOf = [&] (int note, int velocity)
    {
        auto out = player.play (0.05, { juce::MidiMessage::noteOn (1, note, (juce::uint8)velocity) });
        player.play (0.3, { juce::MidiMessage::noteOff (1, note) });
        return out[(size_t)(0.02 * rate)];
    };

    CHECK (levelOf (60, 40) == Catch::Approx (0.25f).margin (0.01));
    CHECK (levelOf (60, 100) == Catch::Approx (0.75f).margin (0.01));
    CHECK (levelOf (62, 100) == Catch::Approx (-0.5f).margin (0.01));
    CHECK (levelOf (61, 100) == Catch::Approx (0.0f).margin (1.0e-6)); // no zone

    // Two zones with the same range take turns.
    sampler.updateZone (1, [] (SampleZone& z) { z.lowVelocity = 1; });
    sampler.updateZone (0, [] (SampleZone& z) { z.highVelocity = 127; });
    const auto first = levelOf (60, 100), second = levelOf (60, 100), third = levelOf (60, 100);
    CHECK (first != Catch::Approx (second));
    CHECK (first == Catch::Approx (third));
}

TEST_CASE ("Changing the samples while notes play is safe", "[dsp][sampler]")
{
    SamplerProcessor sampler;
    sampler.addZone (sine (440.0, 2.0), rate, "A");
    Player player (sampler);
    player.play (0.1, { juce::MidiMessage::noteOn (1, 60, (juce::uint8)127) });
    CHECK (sampler.getNumActiveVoices() == 1);

    // A new set: the playing voice stops within a block (its zone belonged to the old set).
    sampler.addZone (sine (220.0, 2.0), rate, "B");
    player.play (0.05);
    CHECK (sampler.getNumActiveVoices() == 0);

    // Rapid edits (e.g. dragging a loop marker) while playing.
    player.play (0.01, { juce::MidiMessage::noteOn (1, 60, (juce::uint8)127) });
    for (int i = 0; i < 50; ++i)
    {
        sampler.updateZone (0, [i] (SampleZone& z) { z.start = (juce::int64)i * 100; });
        player.play (0.01, { juce::MidiMessage::noteOn (1, 60, (juce::uint8)127) });
    }
    CHECK (sampler.getZones()[0].start == 4900);
}

TEST_CASE ("The Sampler's state holds its samples", "[dsp][sampler]")
{
    SamplerProcessor sampler;
    auto loud = sine (440.0, 0.25, 1.5f, 2); // louder than full scale: kept as it is
    sampler.addZone (loud, rate, "Loud", 57);
    sampler.updateZone (0,
                        [] (SampleZone& z)
                        {
                            z.loop = true;
                            z.loopStart = 100;
                            z.loopEnd = 5000;
                            z.lowKey = 40;
                        });
    setParameter (sampler, "release", 2.5f);

    juce::MemoryBlock state;
    sampler.getStateInformation (state);
    CHECK (state.getSize() < (size_t)loud.getNumSamples() * 2 * 3); // compressed (FLAC, 24 bits)

    SamplerProcessor restored;
    restored.setStateInformation (state.getData(), (int)state.getSize());
    const auto zones = restored.getZones();
    REQUIRE (zones.size() == 1);
    CHECK (zones[0].name == "Loud");
    CHECK (zones[0].rootNote == 57);
    CHECK (zones[0].lowKey == 40);
    CHECK (zones[0].loop);
    CHECK (zones[0].loopEnd == 5000);
    CHECK (restored.getSettings().release == Catch::Approx (2.5f));
    REQUIRE (zones[0].audio != nullptr);
    REQUIRE (zones[0].audio->getNumChannels() == 2);
    float worst = 0.0f;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < loud.getNumSamples(); ++i)
            worst = juce::jmax (worst, std::abs (zones[0].audio->getSample (c, i) - loud.getSample (c, i)));
    CHECK (worst < 1.0e-6f * 1.5f * 8.0f);

    // Garbage is ignored.
    const char junk[] = "not a sampler";
    restored.setStateInformation (junk, (int)sizeof (junk));
    CHECK (restored.getZones().size() == 1);
}

TEST_CASE ("Loading files: one sample, a multi-sample, round-robin", "[dsp][sampler]")
{
    TempFolder temp;
    REQUIRE (temp.root.createDirectory());
    SamplerProcessor sampler;

    const auto kick = writeWav (temp.root.getChildFile ("Kick.wav"), sine (60.0, 0.2));
    REQUIRE (sampler.loadFiles ({ kick }).wasOk());
    REQUIRE (sampler.getZones().size() == 1);
    CHECK (sampler.getZones()[0].name == "Kick");
    CHECK ((sampler.getZones()[0].lowKey == 0 && sampler.getZones()[0].highKey == 127));

    // Note names: mapped around their notes.
    const auto low = writeWav (temp.root.getChildFile ("Piano_C4.wav"), sine (261.6, 0.2));
    const auto high = writeWav (temp.root.getChildFile ("Piano_C6.wav"), sine (1046.5, 0.2));
    REQUIRE (sampler.loadFiles ({ low, high }).wasOk());
    auto zones = sampler.getZones();
    REQUIRE (zones.size() == 2);
    CHECK ((zones[0].rootNote == 48 && zones[0].highKey == 60));
    CHECK ((zones[1].rootNote == 72 && zones[1].lowKey == 61));

    // No note names: round-robin over the whole keyboard; "add" keeps what is there.
    const auto a = writeWav (temp.root.getChildFile ("Snare 1.wav"), sine (200.0, 0.2));
    const auto b = writeWav (temp.root.getChildFile ("Snare 2.wav"), sine (210.0, 0.2));
    REQUIRE (sampler.loadFiles ({ a, b }, false).wasOk());
    zones = sampler.getZones();
    REQUIRE (zones.size() == 4);
    CHECK ((zones[2].lowKey == 0 && zones[3].lowKey == 0 && zones[3].highKey == 127));

    // Not audio: refused, nothing changes.
    const auto text = temp.root.getChildFile ("notes.txt");
    text.replaceWithText ("hello");
    const auto r = sampler.loadFiles ({ text });
    CHECK (r.failed());
    CHECK (r.getErrorMessage().contains ("notes.txt"));
    CHECK (sampler.getZones().size() == 4);
}

TEST_CASE ("The Sampler window shows the samples and takes dropped files", "[dsp][sampler][ui]")
{
    TempFolder temp;
    REQUIRE (temp.root.createDirectory());
    SamplerProcessor sampler;
    std::unique_ptr<juce::AudioProcessorEditor> editor (sampler.createEditorAndMakeActive());
    REQUIRE (editor != nullptr);
    auto* samplerEditor = dynamic_cast<SamplerEditor*> (editor.get());
    REQUIRE (samplerEditor != nullptr);

    const auto file = writeWav (temp.root.getChildFile ("Clap.wav"), sine (300.0, 0.1));
    CHECK (samplerEditor->isInterestedInFileDrag ({ file.getFullPathName() }));
    CHECK (! samplerEditor->isInterestedInFileDrag ({ "/tmp/readme.txt" }));
    samplerEditor->filesDropped ({ file.getFullPathName() }, 10, 10);
    CHECK (sampler.getZones().size() == 1);
    CHECK (samplerEditor->getSelectedZone() == 0);

    // Painting with a sample (waveform and markers) works at any size.
    for (auto size : { juce::Point<int> (560, 360), juce::Point<int> (1200, 700) })
    {
        editor->setSize (size.x, size.y);
        CHECK (editor->createComponentSnapshot (editor->getLocalBounds()).isValid());
    }

    sampler.editorBeingDeleted (editor.get());
}

TEST_CASE ("A Sampler channel plays in a Spinus project and keeps its sample", "[dsp][sampler][audio]")
{
    EngineHost host ({ .openAudioDevice = false });
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerSpinusDevices (*devices, host.getEngine());
    Session session (host.getEngine(), std::move (devices));
    ui::ChannelRack rack (session);
    test::RealtimeRunner runner (host.getEngine(), { .outputChannels = 2, .maxSeconds = 4.0, .paceInRealTime = false });

    REQUIRE (rack.addChannel ("spinus.sampler", "Sampler"));
    const auto& channel = session.getProject().channels.back();
    REQUIRE (channel.instrument.has_value());
    const auto device = channel.instrument.value_or (model::Device {}).id;
    CHECK (session.getAdapter().getMissingDevices().empty());

    auto* plugin = dynamic_cast<tracktion::ExternalPlugin*> (session.getAdapter().getDevicePlugin (device));
    REQUIRE (plugin != nullptr);
    auto* sampler = dynamic_cast<SamplerProcessor*> (plugin->getAudioPluginInstance());
    REQUIRE (sampler != nullptr);
    sampler->addZone (sine (440.0, 1.0), rate, "A");

    for (int step : { 0, 4, 8, 12 })
        REQUIRE (rack.toggleStep (channel.id, step));
    session.play();
    runner.start();
    runner.runUntil (2.0);
    runner.stop();
    session.stop();
    const auto sr = runner.getParams().sampleRate;
    CHECK (test::checks::rms (runner.getChannel (0), (int64_t)(0.2 * sr), (int64_t)(1.8 * sr)) > 0.01f);

    // Saving and loading the project brings the sample back.
    auto data = session.captureProjectData();
    session.loadProject ({});
    session.loadProject (data);
    plugin = dynamic_cast<tracktion::ExternalPlugin*> (session.getAdapter().getDevicePlugin (device));
    REQUIRE (plugin != nullptr);
    sampler = dynamic_cast<SamplerProcessor*> (plugin->getAudioPluginInstance());
    REQUIRE (sampler != nullptr);
    REQUIRE (sampler->getZones().size() == 1);
    CHECK (sampler->getZones()[0].name == "A");
}

TEST_CASE ("Dropping audio files on the Channel Rack adds Sampler channels", "[dsp][sampler][ui]")
{
    TempFolder temp;
    REQUIRE (temp.root.createDirectory());
    EngineHost host ({ .openAudioDevice = false });
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerSpinusDevices (*devices, host.getEngine());
    Session session (host.getEngine(), std::move (devices));
    ui::ChannelRack rack (session);

    const auto kick = writeWav (temp.root.getChildFile ("Kick.wav"), sine (60.0, 0.2));
    const auto hat = writeWav (temp.root.getChildFile ("Hat.wav"), sine (6000.0, 0.05));
    temp.root.getChildFile ("readme.txt").replaceWithText ("hi");

    CHECK (rack.isInterestedInFileDrag ({ kick.getFullPathName() }));
    CHECK (! rack.isInterestedInFileDrag ({ temp.root.getChildFile ("readme.txt").getFullPathName() }));
    rack.filesDropped (
        { kick.getFullPathName(), temp.root.getChildFile ("readme.txt").getFullPathName(), hat.getFullPathName() }, 0,
        0);

    const auto& channels = session.getProject().channels;
    REQUIRE (channels.size() == 2);
    CHECK (channels[0].name == "Kick");
    CHECK (channels[1].name == "Hat");

    auto samplerOf = [&] (size_t i)
    {
        auto* plugin = dynamic_cast<tracktion::ExternalPlugin*> (
            session.getAdapter().getDevicePlugin (channels[i].instrument.value_or (model::Device {}).id));
        return plugin != nullptr ? dynamic_cast<SamplerProcessor*> (plugin->getAudioPluginInstance()) : nullptr;
    };
    REQUIRE (samplerOf (0) != nullptr);
    REQUIRE (samplerOf (0)->getZones().size() == 1);
    CHECK (samplerOf (0)->getZones()[0].name == "Kick");

    // Undo removes the channel; redo brings it back with its sample.
    REQUIRE (session.getHistory().undo());
    REQUIRE (channels.size() == 1);
    REQUIRE (session.getHistory().redo());
    REQUIRE (channels.size() == 2);
    REQUIRE (samplerOf (1) != nullptr);
    REQUIRE (samplerOf (1)->getZones().size() == 1);
    CHECK (samplerOf (1)->getZones()[0].name == "Hat");
}

TEST_CASE ("Sampler CPU cost: 32 voices with the filter, 48 kHz, 128 samples", "[.bench][sampler]")
{
    SamplerProcessor sampler;
    sampler.addZone (sine (440.0, 10.0, 0.5f, 2), rate, "A");
    setParameter (sampler, "filter", 1.0f);
    setParameter (sampler, "cutoff", 2000.0f);
    sampler.prepareToPlay (rate, 128);

    juce::AudioBuffer<float> buffer (2, 128);
    juce::MidiBuffer midi;
    for (int n = 0; n < 32; ++n)
        midi.addEvent (juce::MidiMessage::noteOn (1, 40 + n, (juce::uint8)100), 0);
    sampler.processBlock (buffer, midi);
    midi.clear();
    REQUIRE (sampler.getNumActiveVoices() == 32);

    const int blocks = (int)(5.0 * rate / 128);
    const auto start = juce::Time::getHighResolutionTicks();
    for (int i = 0; i < blocks; ++i)
        sampler.processBlock (buffer, midi);
    const auto seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
    std::cout << "32 voices: " << seconds / 5.0 * 100.0 << " % of one core (5 s of audio in " << seconds << " s)\n";
}

TEST_CASE ("Sampler window screenshot for the manual", "[.screenshot][sampler]")
{
    // A kick-like sample: a falling sine with a decay, and a loop on its tail.
    juce::AudioBuffer<float> kick (1, (int)(0.6 * rate));
    double phase = 0.0;
    for (int i = 0; i < kick.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        phase += 2.0 * M_PI * (50.0 + 120.0 * std::exp (-t * 30.0)) / rate;
        kick.setSample (0, i, (float)(0.9 * std::exp (-t * 6.0) * std::sin (phase)));
    }

    SamplerProcessor sampler;
    sampler.addZone (kick, rate, "Kick 808");
    sampler.updateZone (0,
                        [] (SampleZone& z)
                        {
                            z.loop = true;
                            z.loopStart = (juce::int64)(0.3 * rate);
                            z.loopEnd = (juce::int64)(0.5 * rate);
                        });
    setParameter (sampler, "release", 0.3f);
    setParameter (sampler, "filter", 1.0f);
    setParameter (sampler, "cutoff", 4000.0f);

    std::unique_ptr<juce::AudioProcessorEditor> editor (sampler.createEditorAndMakeActive());
    REQUIRE (editor != nullptr);
    editor->setSize (720, 420);
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/sampler.png");
    file.deleteFile();
    juce::FileOutputStream out (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, out));
    sampler.editorBeingDeleted (editor.get());
}
