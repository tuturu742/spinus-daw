#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "dsp/threeosc/ThreeOscEditor.h"
#include "dsp/threeosc/ThreeOscProcessor.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "ui/ChannelRack.h"

using namespace spinus;
using namespace spinus::dsp;

namespace
{
constexpr double rate = 48000.0;
constexpr int block = 256;

struct Player
{
    explicit Player (ThreeOscProcessor& p) : synth (p) { synth.prepareToPlay (rate, block); }

    std::array<std::vector<float>, 2> play (double seconds, const std::vector<juce::MidiMessage>& events = {})
    {
        std::array<std::vector<float>, 2> out;
        juce::AudioBuffer<float> buffer (2, block);
        juce::MidiBuffer midi;
        for (auto& e : events)
            midi.addEvent (e, 0);
        for (int done = 0; done < (int)(seconds * rate); done += block)
        {
            synth.processBlock (buffer, midi);
            midi.clear();
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < block; ++i)
                    out[(size_t)c].push_back (buffer.getSample (c, i));
        }
        return out;
    }

    ThreeOscProcessor& synth;
};

void set (ThreeOscProcessor& s, const juce::String& id, float value)
{
    auto* parameter = s.getParameterState().getParameter (id);
    REQUIRE (parameter != nullptr);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

juce::MidiMessage on (int note, int velocity = 127)
{
    return juce::MidiMessage::noteOn (1, note, (juce::uint8)velocity);
}
juce::MidiMessage off (int note) { return juce::MidiMessage::noteOff (1, note); }

double frequencyOf (const std::vector<float>& x, size_t from, size_t to)
{
    // Cycles between the first and the last upward zero crossing, each placed between its two samples.
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (auto i = from + 1; i < to; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const auto at = (double)(i - 1) + (double)(-x[i - 1] / (x[i] - x[i - 1]));
            if (first < 0.0)
                first = at;
            last = at;
            ++crossings;
        }
    return crossings > 1 ? (crossings - 1) * rate / (last - first) : 0.0;
}

float rms (const std::vector<float>& x, double from, double to)
{
    return test::checks::rms (x, (int64_t)(from * rate), (int64_t)(to * rate));
}

/** Energy of x at one frequency (a single DFT bin), relative to the signal's RMS. */
double energyAt (const std::vector<float>& x, size_t from, size_t count, double frequency)
{
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < count; ++i)
    {
        const auto w = 2.0 * M_PI * frequency * (double)i / rate;
        re += x[from + i] * std::cos (w);
        im += x[from + i] * std::sin (w);
    }
    return std::sqrt (re * re + im * im) / (double)count;
}
} // namespace

TEST_CASE ("3x Osc plays the note's pitch; coarse and fine tune it", "[dsp][3xosc]")
{
    ThreeOscProcessor synth;
    set (synth, ThreeOscProcessor::parameterId ("shape", 0), 0.0f); // sine
    Player player (synth);

    auto out = player.play (0.5, { on (69) }); // A at 440 Hz
    CHECK (frequencyOf (out[0], 4800, 24000) == Catch::Approx (440.0).epsilon (0.01));
    player.play (0.5, { off (69) });

    set (synth, ThreeOscProcessor::parameterId ("coarse", 0), 12.0f);
    out = player.play (0.5, { on (69) });
    CHECK (frequencyOf (out[0], 4800, 24000) == Catch::Approx (880.0).epsilon (0.01));
    player.play (0.5, { off (69) });

    set (synth, ThreeOscProcessor::parameterId ("coarse", 0), 0.0f);
    set (synth, ThreeOscProcessor::parameterId ("fine", 0), 100.0f); // one semitone up
    out = player.play (0.5, { on (69) });
    CHECK (frequencyOf (out[0], 4800, 24000) == Catch::Approx (440.0 * std::pow (2.0, 1.0 / 12.0)).epsilon (0.01));
}

TEST_CASE ("3x Osc's saw and square are band-limited", "[dsp][3xosc]")
{
    // A high note: a naive saw folds harmonics above half the sample rate back down (aliasing).
    // PolyBLEP keeps that far below the fundamental.
    for (int shape : { 2, 3 })
    {
        INFO ((shape == 2 ? "saw" : "square"));
        ThreeOscProcessor synth;
        set (synth, ThreeOscProcessor::parameterId ("shape", 0), (float)shape);
        Player player (synth);
        const auto f = 440.0 * std::pow (2.0, (100 - 69) / 12.0); // ~2.6 kHz
        const auto out = player.play (0.5, { on (100) })[0];
        const auto fundamental = energyAt (out, 4800, 9600, f);
        // The first harmonic above half the sample rate (odd harmonics only for a square) would fold
        // back to rate - k * f in a naive oscillator, at about 1/k of the fundamental.
        int k = 1;
        while (k * f <= rate / 2.0 || (shape == 3 && k % 2 == 0))
            ++k;
        const auto alias = energyAt (out, 4800, 9600, rate - k * f);
        CHECK (alias < fundamental * 0.05);
        CHECK (fundamental > 0.02);
    }
}

TEST_CASE ("3x Osc mixes its oscillators, with pan, phase and noise", "[dsp][3xosc]")
{
    ThreeOscProcessor synth;
    Player player (synth);

    // Osc 1 alone (the default); then osc 2 added an octave down.
    auto one = player.play (0.3, { on (60) });
    player.play (0.5, { off (60) });
    set (synth, ThreeOscProcessor::parameterId ("level", 1), 1.0f);
    auto two = player.play (0.3, { on (60) });
    player.play (0.5, { off (60) });
    CHECK (rms (two[0], 0.1, 0.3) > rms (one[0], 0.1, 0.3) * 1.2f);

    // Osc 1 fully left, osc 2 off: nothing on the right.
    set (synth, ThreeOscProcessor::parameterId ("level", 1), 0.0f);
    set (synth, ThreeOscProcessor::parameterId ("pan", 0), -1.0f);
    auto panned = player.play (0.3, { on (60) });
    player.play (0.5, { off (60) });
    CHECK (rms (panned[1], 0.1, 0.3) < 1.0e-6f);
    CHECK (rms (panned[0], 0.1, 0.3) > 0.05f);

    // Start phase: a sine starting at 90 degrees starts at its peak.
    set (synth, ThreeOscProcessor::parameterId ("pan", 0), 0.0f);
    set (synth, ThreeOscProcessor::parameterId ("shape", 0), 0.0f);
    set (synth, ThreeOscProcessor::parameterId ("phase", 0), 0.25f);
    set (synth, "attack", 0.0f);
    auto phased = player.play (0.05, { on (60) });
    player.play (0.5, { off (60) });
    CHECK (phased[0][1] > 0.1f);

    // Noise: not periodic, but present.
    set (synth, ThreeOscProcessor::parameterId ("shape", 0), 4.0f);
    auto noise = player.play (0.3, { on (60) });
    CHECK (rms (noise[0], 0.1, 0.3) > 0.05f);
}

TEST_CASE ("3x Osc's envelope and filter", "[dsp][3xosc]")
{
    ThreeOscProcessor synth;
    set (synth, "release", 0.05f);
    Player player (synth);

    auto out = player.play (0.3, { on (60) });
    CHECK (rms (out[0], 0.1, 0.3) > 0.05f);
    out = player.play (0.3, { off (60) });
    CHECK (rms (out[0], 0.15, 0.3) < 1.0e-6f);
    CHECK (synth.getNumActiveVoices() == 0);

    // A low-pass at 300 Hz takes most of a bright saw away; envelope amount opens it again.
    const auto open = rms (player.play (0.3, { on (72) })[0], 0.1, 0.3);
    player.play (0.3, { off (72) });
    set (synth, "filter", 1.0f);
    set (synth, "cutoff", 300.0f);
    const auto closed = rms (player.play (0.3, { on (72) })[0], 0.1, 0.3);
    player.play (0.3, { off (72) });
    CHECK (closed < open * 0.5f);

    set (synth, "envelope", 1.0f);
    const auto opened = rms (player.play (0.3, { on (72) })[0], 0.1, 0.3);
    CHECK (opened > closed * 1.5f);
}

TEST_CASE ("3x Osc in Mono slides from note to note", "[dsp][3xosc]")
{
    ThreeOscProcessor synth;
    set (synth, ThreeOscProcessor::parameterId ("shape", 0), 0.0f); // sine
    set (synth, "voices", 1.0f);                                    // Mono (slides)
    set (synth, "glide", 0.2f);
    Player player (synth);

    auto out = player.play (0.5, { on (57) }); // A at 220 Hz
    CHECK (frequencyOf (out[0], 4800, 24000) == Catch::Approx (220.0).epsilon (0.01));
    const auto held = rms (out[0], 0.3, 0.5);

    // A second key while the first is held: the same voice slides up, without starting its envelope again.
    out = player.play (0.6, { on (69) });
    CHECK (synth.getNumActiveVoices() == 1);
    const auto during = frequencyOf (out[0], 2400, 7200); // 50 to 150 ms into the slide
    CHECK (during > 240.0);
    CHECK (during < 420.0);
    CHECK (frequencyOf (out[0], 14400, 28800) == Catch::Approx (440.0).epsilon (0.01));
    CHECK (rms (out[0], 0.0, 0.01) == Catch::Approx (held).epsilon (0.1)); // no attack from silence

    // Letting go of the second key slides back to the one still held.
    out = player.play (0.6, { off (69) });
    CHECK (synth.getNumActiveVoices() == 1);
    CHECK (frequencyOf (out[0], 14400, 28800) == Catch::Approx (220.0).epsilon (0.02));
    player.play (0.6, { off (57) });

    // Poly plays both notes on their own voices.
    set (synth, "voices", 0.0f);
    player.play (0.2, { on (57), on (69) });
    CHECK (synth.getNumActiveVoices() == 2);
}

TEST_CASE ("3x Osc in Mono forgets the keys held when all notes stop", "[dsp][3xosc]")
{
    ThreeOscProcessor synth;
    set (synth, ThreeOscProcessor::parameterId ("shape", 0), 0.0f); // sine
    set (synth, "voices", 1.0f);                                    // Mono (slides)
    set (synth, "glide", 0.2f);
    set (synth, "release", 0.05f);
    Player player (synth);

    // A key held when playback stops (all notes off, never its own note off)...
    player.play (0.3, { on (57) });
    player.play (0.5, { juce::MidiMessage::allNotesOff (1) });
    // ...is not held any more: the next note starts on its own pitch, and ends when let go.
    auto out = player.play (0.3, { on (69) });
    CHECK (frequencyOf (out[0], 0, 4800) == Catch::Approx (440.0).epsilon (0.01));
    out = player.play (0.5, { off (69) });
    CHECK (rms (out[0], 0.3, 0.5) < 1.0e-4f);
    CHECK (synth.getNumActiveVoices() == 0);

    // All sound off stops at once.
    player.play (0.3, { on (60) });
    out = player.play (0.1, { juce::MidiMessage::allSoundOff (1) });
    CHECK (rms (out[0], 0.0, 0.1) < 1.0e-4f);
    CHECK (synth.getNumActiveVoices() == 0);
}

TEST_CASE ("3x Osc follows an MPE controller: per-note pitch bend, pressure and slide", "[dsp][3xosc][mpe]")
{
    ThreeOscProcessor synth;
    set (synth, ThreeOscProcessor::parameterId ("shape", 0), 0.0f); // sine
    set (synth, "release", 0.01f);
    Player player (synth);
    auto bend = [] (int channel, double semitones)
    { return juce::MidiMessage::pitchWheel (channel, 8192 + juce::roundToInt (semitones / 48.0 * 8192.0)); };

    // Each note on its own channel (MPE's 2 to 16): an A at 220 Hz bent up an octave on channel 16.
    auto out = player.play (0.5, { bend (16, 12.0), juce::MidiMessage::noteOn (16, 57, (juce::uint8)100) });
    CHECK (frequencyOf (out[0], 4800, 24000) == Catch::Approx (440.0).epsilon (0.01));
    player.play (0.3, { juce::MidiMessage::noteOff (16, 57), bend (16, 0.0) });

    // Pressing harder swells the note: full pressure is about 6 dB louder.
    out = player.play (0.4, { juce::MidiMessage::noteOn (3, 60, (juce::uint8)100) });
    const auto light = rms (out[0], 0.2, 0.4);
    out = player.play (0.4, { juce::MidiMessage::channelPressureChange (3, 127) });
    const auto hard = rms (out[0], 0.2, 0.4);
    CHECK (hard / light == Catch::Approx (2.0f).epsilon (0.05));
    player.play (0.3, { juce::MidiMessage::noteOff (3, 60), juce::MidiMessage::channelPressureChange (3, 0) });

    // The slide (CC 74) opens or closes the filter.
    set (synth, ThreeOscProcessor::parameterId ("shape", 0), 2.0f); // saw
    set (synth, "filter", 1.0f);
    set (synth, "cutoff", 500.0f);
    out = player.play (
        0.4, { juce::MidiMessage::controllerEvent (4, 74, 0), juce::MidiMessage::noteOn (4, 48, (juce::uint8)100) });
    const auto dark = rms (out[0], 0.2, 0.4);
    out = player.play (0.4, { juce::MidiMessage::controllerEvent (4, 74, 127) });
    const auto bright = rms (out[0], 0.2, 0.4);
    CHECK (bright > dark * 1.3f);
}

TEST_CASE ("3x Osc plays 16 notes at once and keeps its settings", "[dsp][3xosc]")
{
    ThreeOscProcessor synth;
    Player player (synth);
    std::vector<juce::MidiMessage> chord;
    chord.reserve (20);
    for (int n = 0; n < 20; ++n)
        chord.push_back (on (40 + n));
    const auto out = player.play (0.2, chord);
    CHECK (synth.getNumActiveVoices() == ThreeOscEngine::maxVoices);
    float peak = 0.0f;
    for (auto v : out[0])
        peak = juce::jmax (peak, std::abs (v));
    CHECK (std::isfinite (peak));

    set (synth, ThreeOscProcessor::parameterId ("coarse", 2), -7.0f);
    set (synth, "cutoff", 1234.0f);
    juce::MemoryBlock state;
    synth.getStateInformation (state);
    ThreeOscProcessor restored;
    restored.setStateInformation (state.getData(), (int)state.getSize());
    CHECK (restored.getSettings().oscillators[2].coarse == Catch::Approx (-7.0f));
    CHECK (restored.getSettings().cutoffHz == Catch::Approx (1234.0f).epsilon (0.001));
}

TEST_CASE ("The 3x Osc window", "[dsp][3xosc][ui]")
{
    ThreeOscProcessor synth;
    std::unique_ptr<juce::AudioProcessorEditor> editor (synth.createEditorAndMakeActive());
    REQUIRE (dynamic_cast<ThreeOscEditor*> (editor.get()) != nullptr);
    for (auto size : { juce::Point<int> (660, 600), juce::Point<int> (1400, 1100) })
    {
        editor->setSize (size.x, size.y);
        CHECK (editor->createComponentSnapshot (editor->getLocalBounds()).isValid());
    }
    synth.editorBeingDeleted (editor.get());
}

TEST_CASE ("A 3x Osc channel plays in a Spinus project", "[dsp][3xosc][audio]")
{
    EngineHost host ({ .openAudioDevice = false });
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerSpinusDevices (*devices, host.getEngine());
    Session session (host.getEngine(), std::move (devices));
    ui::ChannelRack rack (session);
    test::RealtimeRunner runner (host.getEngine(), { .outputChannels = 2, .maxSeconds = 4.0, .paceInRealTime = false });

    REQUIRE (rack.addChannel ("spinus.3x-osc", "3x Osc"));
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
    CHECK (test::checks::rms (runner.getChannel (0), (int64_t)(0.1 * sr), (int64_t)(1.9 * sr)) > 0.01f);
}

TEST_CASE ("3x Osc window screenshot for the manual", "[.screenshot][3xosc]")
{
    ThreeOscProcessor synth;
    synth.applyPreset (threeOscPresets::find ("Warm Pad")); // the preset menu shows a preset
    std::unique_ptr<juce::AudioProcessorEditor> editor (synth.createEditorAndMakeActive());
    editor->setSize (740, 620);
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/3x-osc.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    synth.editorBeingDeleted (editor.get());
}

TEST_CASE ("3x Osc CPU cost: 16 voices, 3 oscillators, filter, 48 kHz, 128 samples", "[.bench][3xosc]")
{
    ThreeOscProcessor synth;
    set (synth, ThreeOscProcessor::parameterId ("level", 1), 1.0f);
    set (synth, ThreeOscProcessor::parameterId ("level", 2), 1.0f);
    set (synth, "filter", 1.0f);
    set (synth, "envelope", 0.5f);
    synth.prepareToPlay (rate, 128);
    juce::AudioBuffer<float> buffer (2, 128);
    juce::MidiBuffer midi;
    for (int n = 0; n < 16; ++n)
        midi.addEvent (on (40 + n), 0);
    synth.processBlock (buffer, midi);
    midi.clear();

    const int blocks = (int)(5.0 * rate / 128);
    const auto start = juce::Time::getHighResolutionTicks();
    for (int i = 0; i < blocks; ++i)
        synth.processBlock (buffer, midi);
    const auto seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
    std::cout << "3x Osc, 16 voices: " << seconds / 5.0 * 100.0 << " % of one core\n";
}
