#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "dsp/drums/DrumMachineEditor.h"
#include "dsp/drums/DrumMachineProcessor.h"
#include "dsp/drums/DrumSynth.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "ui/ChannelRack.h"

using namespace spinus;
using namespace spinus::dsp;

namespace
{
constexpr double rate = 48000.0;
constexpr int block = 256;

using Pads = DrumMachineProcessor;

struct Player
{
    explicit Player (DrumMachineProcessor& p) : drums (p) { drums.prepareToPlay (rate, block); }

    /** Renders `seconds`, sending `events` first; returns both channels. */
    std::array<std::vector<float>, 2> play (double seconds, const std::vector<juce::MidiMessage>& events = {})
    {
        std::array<std::vector<float>, 2> out;
        juce::AudioBuffer<float> buffer (2, block);
        juce::MidiBuffer midi;
        for (auto& e : events)
            midi.addEvent (e, 0);

        for (int done = 0; done < (int)(seconds * rate); done += block)
        {
            drums.processBlock (buffer, midi);
            midi.clear();
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < block; ++i)
                    out[(size_t)c].push_back (buffer.getSample (c, i));
        }
        return out;
    }

    DrumMachineProcessor& drums;
};

juce::MidiMessage hit (int note) { return juce::MidiMessage::noteOn (1, note, (juce::uint8)127); }

float peak (const std::vector<float>& x, double from = 0.0, double to = 1.0e9)
{
    float p = 0.0f;
    for (auto i = (size_t)(from * rate); i < juce::jmin (x.size(), (size_t)(to * rate)); ++i)
        p = juce::jmax (p, std::abs (x[i]));
    return p;
}

void setParameter (DrumMachineProcessor& d, const juce::String& id, float value)
{
    auto* parameter = d.getParameterState().getParameter (id);
    REQUIRE (parameter != nullptr);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

struct TempFolder
{
    TempFolder() { root.createDirectory(); }
    ~TempFolder() { root.deleteRecursively(); }
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile (juce::String ("spinus-drums-test") + "-" + juce::Uuid().toString());
};
} // namespace

TEST_CASE ("The Synth Kit has 16 sounds and is the same every time", "[dsp][drums]")
{
    const auto kit = makeSynthKit (rate), again = makeSynthKit (rate);
    REQUIRE (kit.size() == 16);
    for (size_t i = 0; i < kit.size(); ++i)
    {
        INFO (kit[i].name);
        CHECK (kit[i].audio.getMagnitude (0, kit[i].audio.getNumSamples()) > 0.3f);
        CHECK (kit[i].audio.getMagnitude (0, kit[i].audio.getNumSamples()) <= 1.0f);
        CHECK (std::abs (kit[i].audio.getSample (0, kit[i].audio.getNumSamples() - 1)) < 1.0e-9f); // no click
        CHECK (juce::approximatelyEqual (kit[i].audio.getRMSLevel (0, 0, kit[i].audio.getNumSamples()),
                                         again[i].audio.getRMSLevel (0, 0, again[i].audio.getNumSamples())));
    }
    CHECK (kit[3].name == "Closed Hat");
    CHECK ((kit[3].chokeGroup == 1 && kit[4].chokeGroup == 1));
}

TEST_CASE ("Pads play from C5 up, each with its own settings", "[dsp][drums]")
{
    DrumMachineProcessor drums;
    Player player (drums);
    CHECK (drums.getPads()[0].name == "Kick");
    CHECK (drums.getPads()[0].note == 60);
    CHECK (drums.getPads()[15].note == 75);

    auto out = player.play (0.3, { hit (60) });
    const auto kick = peak (out[0]);
    CHECK (kick > 0.5f);
    CHECK (peak (out[1]) == Catch::Approx (kick)); // centred

    // Pad 1 to the left, quieter.
    setParameter (drums, Pads::parameterId ("pan", 0), -1.0f);
    setParameter (drums, Pads::parameterId ("volume", 0), -12.0f);
    out = player.play (0.3, { hit (60) });
    CHECK (peak (out[1]) < 1.0e-6f);
    CHECK (peak (out[0]) == Catch::Approx (kick * juce::Decibels::decibelsToGain (-12.0f)).epsilon (0.05));

    // Another note for pad 1: C5 is now silent.
    drums.setPadNote (0, 36);
    player.play (1.0); // let the kick end
    out = player.play (0.2, { hit (60) });
    CHECK (peak (out[0]) < 1.0e-6f);
    out = player.play (0.2, { hit (36) });
    CHECK (peak (out[0]) > 0.1f);
}

TEST_CASE ("A closed hi-hat chokes the open one", "[dsp][drums]")
{
    DrumMachineProcessor drums;
    Player player (drums);
    const int closed = 63, open = 64; // pads 4 and 5

    auto out = player.play (0.3, { hit (open) });
    CHECK (peak (out[0], 0.2, 0.3) > 0.01f); // still ringing

    out = player.play (0.5, { hit (closed) });
    CHECK (peak (out[0], 0.2) < 1.0e-6f); // both done: the closed hat is short, the open one was cut
    CHECK (drums.getNumActiveVoices() == 0);

    // Without the choke group, the open hat keeps ringing.
    drums.setPadChoke (4, 0);
    player.play (0.3, { hit (open) });
    out = player.play (0.2, { hit (closed) });
    CHECK (peak (out[0], 0.15) > 0.01f);
}

TEST_CASE ("Kits save, load and travel in the state", "[dsp][drums]")
{
    TempFolder temp;
    DrumMachineProcessor drums;
    juce::AudioBuffer<float> blip (1, 4800);
    for (int i = 0; i < blip.getNumSamples(); ++i)
        blip.setSample (0, i, 0.5f * std::sin ((float)i * 0.3f));
    drums.setPadAudio (2, blip, rate, "Blip");
    drums.setPadChoke (2, 3);
    drums.clearPad (15);
    setParameter (drums, Pads::parameterId ("tune", 2), 5.0f);

    const auto kit = temp.root.getChildFile ("Mine.spinuskit");
    REQUIRE (drums.saveKit (kit).wasOk());

    DrumMachineProcessor other;
    REQUIRE (other.loadKit (kit).wasOk());
    CHECK (other.getPads()[2].name == "Blip");
    CHECK (other.getPads()[2].chokeGroup == 3);
    CHECK (other.getPads()[15].audio == nullptr);
    CHECK (other.getPadSettings (2).coarse == Catch::Approx (5.0f));
    REQUIRE (other.getPads()[2].audio != nullptr);
    CHECK (other.getPads()[2].audio->getNumSamples() == 4800);

    // The Synth Kit puts everything back.
    other.loadSynthKit();
    CHECK (other.getPads()[2].name == "Clap");
    CHECK (other.getPads()[15].audio != nullptr);
    CHECK (other.getPadSettings (2).coarse == Catch::Approx (0.0f));

    // Not a kit.
    temp.root.getChildFile ("other.spinuskit").replaceWithText ("hello");
    CHECK (other.loadKit (temp.root.getChildFile ("other.spinuskit")).failed());
    CHECK (other.loadKit (temp.root.getChildFile ("missing.spinuskit")).failed());
}

TEST_CASE ("Clicking a pad plays it", "[dsp][drums]")
{
    DrumMachineProcessor drums;
    Player player (drums);
    drums.audition (0, true);
    auto out = player.play (0.1);
    CHECK (peak (out[0]) > 0.3f);
    drums.audition (0, false);
    drums.audition (99, true); // no such pad: ignored
    player.play (0.1);
}

TEST_CASE ("The Drum Machine window", "[dsp][drums][ui]")
{
    DrumMachineProcessor drums;
    std::unique_ptr<juce::AudioProcessorEditor> editor (drums.createEditorAndMakeActive());
    auto* window = dynamic_cast<DrumMachineEditor*> (editor.get());
    REQUIRE (window != nullptr);

    window->selectPad (4);
    CHECK (window->getSelectedPad() == 4);
    window->selectPad (40);
    CHECK (window->getSelectedPad() == 4);

    for (auto size : { juce::Point<int> (640, 400), juce::Point<int> (1400, 900) })
    {
        editor->setSize (size.x, size.y);
        CHECK (editor->createComponentSnapshot (editor->getLocalBounds()).isValid());
    }
    drums.editorBeingDeleted (editor.get());
}

TEST_CASE ("A Drum Machine channel plays its kick on the Channel Rack's steps", "[dsp][drums][audio]")
{
    EngineHost host ({ .openAudioDevice = false });
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerSpinusDevices (*devices, host.getEngine());
    Session session (host.getEngine(), std::move (devices));
    ui::ChannelRack rack (session);
    test::RealtimeRunner runner (host.getEngine(), { .outputChannels = 2, .maxSeconds = 4.0, .paceInRealTime = false });

    REQUIRE (rack.addChannel ("spinus.drum-machine", "Drums"));
    CHECK (session.getAdapter().getMissingDevices().empty());
    const auto channel = session.getProject().channels.back().id;
    for (int step : { 0, 4, 8, 12 })
        REQUIRE (rack.toggleStep (channel, step));

    session.play();
    runner.start();
    runner.runUntil (2.0);
    runner.stop();
    session.stop();
    const auto sr = runner.getParams().sampleRate;
    CHECK (test::checks::rms (runner.getChannel (0), (int64_t)(0.1 * sr), (int64_t)(1.9 * sr)) > 0.02f);
}

TEST_CASE ("Drum Machine window screenshot for the manual", "[.screenshot][drums]")
{
    DrumMachineProcessor drums;
    std::unique_ptr<juce::AudioProcessorEditor> editor (drums.createEditorAndMakeActive());
    auto* window = dynamic_cast<DrumMachineEditor*> (editor.get());
    REQUIRE (window != nullptr);
    window->selectPad (4);
    editor->setSize (800, 520);
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/drum-machine.png");
    file.deleteFile();
    juce::FileOutputStream out (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, out));
    drums.editorBeingDeleted (editor.get());
}

TEST_CASE ("Write the Synth Kit's sounds (for checking the drum recogniser)", "[.drumkit]")
{
    const juce::File folder (juce::SystemStats::getEnvironmentVariable ("SPINUS_DRUM_KIT_FOLDER", {}));
    if (folder == juce::File())
        SKIP ("set SPINUS_DRUM_KIT_FOLDER");
    folder.createDirectory();
    for (auto& drum : spinus::dsp::makeSynthKit (44100.0))
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream =
            std::make_unique<juce::FileOutputStream> (folder.getChildFile (drum.name + ".wav"));
        auto writer = wav.createWriterFor (
            stream,
            juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (drum.audio, 0, drum.audio.getNumSamples());
    }
}
