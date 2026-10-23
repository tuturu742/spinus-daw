#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "AudioChecks.h"
#include "RealtimeRunner.h"
#include "dsp/filter/FilterEditor.h"
#include "dsp/filter/FilterProcessor.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "ui/ChannelRack.h"
#include "ui/Mixer.h"

using namespace spinus;
using namespace spinus::dsp;
namespace te = tracktion;

namespace
{
constexpr double rate = 48000.0;
constexpr int block = 256;

std::vector<float> sine (double frequency, double seconds, float level = 0.5f)
{
    std::vector<float> x ((size_t)(seconds * rate));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = level * (float)std::sin (2.0 * M_PI * frequency * (double)i / rate);
    return x;
}

/** Runs a mono signal through the engine (copied to both channels) in blocks; returns the left channel. */
std::vector<float> run (FilterEngine& engine, const FilterSettings& s, const std::vector<float>& input,
                        int blockSize = block, double bpm = 120.0, double startBeat = -1.0)
{
    std::vector<float> out;
    juce::AudioBuffer<float> buffer (2, blockSize);
    for (size_t start = 0; start + (size_t)blockSize <= input.size(); start += (size_t)blockSize)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < blockSize; ++i)
                buffer.setSample (c, i, input[start + (size_t)i]);
        const auto beat = startBeat < 0.0 ? -1.0 : startBeat + (double)start / rate * bpm / 60.0;
        engine.process (buffer, s, bpm, beat);
        for (int i = 0; i < blockSize; ++i)
            out.push_back (buffer.getSample (0, i));
    }
    return out;
}

/** The filter's gain for a sine at `frequency`, measured after it settles. */
float measuredGain (const FilterSettings& s, double frequency)
{
    FilterEngine engine;
    engine.prepare (rate, block);
    const auto out = run (engine, s, sine (frequency, 0.5));
    const auto begin = (int64_t)(0.25 * rate), end = (int64_t)out.size();
    return test::checks::rms (out, begin, end) / (0.5f / std::sqrt (2.0f));
}

float decibels (float gain) { return juce::Decibels::gainToDecibels (gain, -120.0f); }

void set (FilterProcessor& f, const juce::String& id, float value)
{
    auto* parameter = f.getParameterState().getParameter (id);
    REQUIRE (parameter != nullptr);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}
} // namespace

TEST_CASE ("Filter types pass and cut the frequencies they should", "[dsp][filter]")
{
    FilterSettings s;
    s.cutoffHz = 1000.0f;
    s.resonance = 0.0f;

    SECTION ("Low pass, 12 and 24 dB per octave")
    {
        CHECK (decibels (measuredGain (s, 100.0)) > -0.5f);
        CHECK (decibels (measuredGain (s, 8000.0)) < -30.0f);
        s.steep = true;
        CHECK (decibels (measuredGain (s, 100.0)) > -0.5f);
        CHECK (decibels (measuredGain (s, 8000.0)) < -60.0f);
    }
    SECTION ("High pass")
    {
        s.type = FilterSettings::Type::highPass;
        CHECK (decibels (measuredGain (s, 100.0)) < -30.0f);
        CHECK (decibels (measuredGain (s, 8000.0)) > -0.5f);
    }
    SECTION ("Band pass")
    {
        s.type = FilterSettings::Type::bandPass;
        CHECK (decibels (measuredGain (s, 1000.0)) > -1.0f);
        CHECK (decibels (measuredGain (s, 100.0)) < -15.0f);
        CHECK (decibels (measuredGain (s, 10000.0)) < -15.0f);
    }
    SECTION ("Notch")
    {
        s.type = FilterSettings::Type::notch;
        CHECK (decibels (measuredGain (s, 1000.0)) < -30.0f);
        CHECK (decibels (measuredGain (s, 100.0)) > -1.0f);
        CHECK (decibels (measuredGain (s, 10000.0)) > -1.0f);
    }
}

TEST_CASE ("The window's curve matches what the filter does", "[dsp][filter]")
{
    for (auto type : { FilterSettings::Type::lowPass, FilterSettings::Type::highPass, FilterSettings::Type::bandPass })
        for (bool steep : { false, true })
            for (float resonance : { 0.0f, 0.7f })
            {
                FilterSettings s;
                s.type = type;
                s.steep = steep;
                s.resonance = resonance;
                s.cutoffHz = 1000.0f;
                for (double hz : { 250.0, 1000.0, 3000.0 })
                {
                    INFO ("type " << (int)type << " steep " << steep << " resonance " << resonance << " at " << hz);
                    const auto shown = decibels (FilterEngine::magnitudeAt (s, s.cutoffHz, (float)hz));
                    if (shown > -40.0f) // far below, measurement noise dominates
                        CHECK (std::abs (decibels (measuredGain (s, hz)) - shown) < 1.0f);
                }
            }
}

TEST_CASE ("Resonance raises a peak at the cutoff", "[dsp][filter]")
{
    FilterSettings s;
    s.cutoffHz = 1000.0f;
    s.resonance = 0.0f;
    const auto flat = measuredGain (s, 1000.0);
    s.resonance = 0.8f;
    CHECK (decibels (measuredGain (s, 1000.0)) > decibels (flat) + 12.0f);
}

TEST_CASE ("A tempo-synced LFO follows the song position", "[dsp][filter]")
{
    FilterSettings s;
    s.cutoffHz = 500.0f;
    s.lfoAmount = 0.25f; // one octave each way
    s.lfoSync = true;
    s.lfoBeats = 1.0;

    // A quarter of the way through a beat, the sine LFO is at its top: one octave up.
    auto cutoffAt = [&] (double beat)
    {
        FilterEngine engine;
        engine.prepare (rate, 16);
        juce::AudioBuffer<float> buffer (2, 16);
        buffer.clear();
        engine.process (buffer, s, 120.0, beat);
        return engine.getModulatedCutoff();
    };
    CHECK (cutoffAt (0.0) == Catch::Approx (500.0f).epsilon (0.01));
    CHECK (cutoffAt (4.25) == Catch::Approx (1000.0f).epsilon (0.01));
    CHECK (cutoffAt (10.75) == Catch::Approx (250.0f).epsilon (0.01));

    // Free-running at 2 Hz: after a quarter second (half a cycle) it is back at the middle, going down.
    s.lfoSync = false;
    s.lfoRateHz = 2.0f;
    FilterEngine engine;
    engine.prepare (rate, 16);
    juce::AudioBuffer<float> buffer (2, 16);
    buffer.clear();
    for (int i = 0; i < (int)(0.25 * rate / 16); ++i)
        engine.process (buffer, s, 120.0, -1.0);
    CHECK (engine.getModulatedCutoff() == Catch::Approx (500.0f).epsilon (0.02));
}

TEST_CASE ("The envelope follower opens the filter on loud input", "[dsp][filter]")
{
    FilterSettings s;
    s.cutoffHz = 200.0f;
    s.envelopeAmount = 1.0f; // up to 4 octaves
    FilterEngine engine;
    engine.prepare (rate, block);

    run (engine, s, std::vector<float> ((size_t)(0.3 * rate), 0.0f));
    CHECK (engine.getModulatedCutoff() == Catch::Approx (200.0f).epsilon (0.01));

    run (engine, s, sine (100.0, 0.3, 1.0f));
    CHECK (engine.getModulatedCutoff() > 2000.0f); // near 0 dB: close to 4 octaves up (3200 Hz)
}

TEST_CASE ("Mix, volume and stability", "[dsp][filter]")
{
    const auto input = sine (5000.0, 0.2);
    FilterSettings s;
    s.cutoffHz = 300.0f;

    SECTION ("Mix at 0 leaves the sound untouched")
    {
        s.mix = 0.0f;
        FilterEngine engine;
        engine.prepare (rate, block);
        const auto out = run (engine, s, input);
        CHECK (out == std::vector<float> (input.begin(), input.begin() + (long)out.size()));
    }
    SECTION ("Volume")
    {
        s.type = FilterSettings::Type::highPass;
        s.cutoffHz = 20.0f;
        s.gainDb = -6.0f;
        CHECK (decibels (measuredGain (s, 5000.0)) == Catch::Approx (-6.0f).margin (0.3f));
    }
    SECTION ("Full resonance at the top of the range stays finite, and the random LFO repeats")
    {
        s.resonance = 1.0f;
        s.cutoffHz = 20000.0f;
        s.steep = true;
        s.lfoAmount = 1.0f;
        s.lfoSync = false;
        s.lfoRateHz = 20.0f;
        s.lfoShape = FilterSettings::LfoShape::random;
        juce::Random noise (7);
        std::vector<float> loud ((size_t)rate);
        for (auto& x : loud)
            x = noise.nextFloat() * 2.0f - 1.0f;

        FilterEngine a, b;
        a.prepare (rate, block);
        b.prepare (rate, block);
        const auto first = run (a, s, loud);
        CHECK (std::all_of (first.begin(), first.end(),
                            [] (float x) { return std::isfinite (x) && std::abs (x) < 100.0f; }));
        CHECK (first == run (b, s, loud));
    }
}

TEST_CASE ("The Filter's state holds its settings", "[dsp][filter]")
{
    FilterProcessor a;
    set (a, "type", 2.0f);
    set (a, "cutoff", 2500.0f);
    set (a, "lfo_length", 4.0f);
    juce::MemoryBlock state;
    a.getStateInformation (state);

    FilterProcessor b;
    b.setStateInformation (state.getData(), (int)state.getSize());
    const auto s = b.getSettings();
    CHECK (s.type == FilterSettings::Type::bandPass);
    CHECK (s.cutoffHz == Catch::Approx (2500.0f).epsilon (0.01));
    CHECK (s.lfoBeats == Catch::Approx (4.0));
}

TEST_CASE ("The Filter on a mixer insert filters a channel", "[dsp][filter][audio]")
{
    EngineHost host { { .openAudioDevice = false } };
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerBundledDevices (*devices, host.getEngine(), {});
    Session session (host.getEngine(), std::move (devices));
    ui::ChannelRack rack (session);
    ui::Mixer mixer (session);
    test::RealtimeRunner runner (host.getEngine(), { .outputChannels = 2, .maxSeconds = 4.0, .paceInRealTime = false });

    REQUIRE (rack.addChannel ("spinus.3x-osc", "Lead"));
    const auto lead = session.getProject().channels[0].id;
    for (int step : { 0, 4, 8, 12 })
        REQUIRE (rack.toggleStep (lead, step));

    auto render = [&]
    {
        session.play();
        runner.start();
        runner.runUntil (runner.getSecondsProcessed() + 1.0);
        runner.stop();
        session.stop();
        const auto end = runner.getNumRecordedSamples();
        return test::checks::rms (runner.getChannel (0), end - (int64_t)(0.8 * rate), end);
    };
    const auto open = render();
    REQUIRE (open > 0.01f);

    REQUIRE (mixer.addEffect (model::masterInsertId, "spinus.filter"));
    const auto& effects = session.getProject().mixer.master.effects;
    REQUIRE (effects.size() == 1);
    CHECK (session.getAdapter().getMissingDevices().empty());
    auto* plugin = dynamic_cast<te::ExternalPlugin*> (session.getAdapter().getDevicePlugin (effects[0].device.id));
    REQUIRE (plugin != nullptr);
    auto* filter = dynamic_cast<FilterProcessor*> (plugin->getAudioPluginInstance());
    REQUIRE (filter != nullptr);
    set (*filter, "cutoff", 100.0f);
    set (*filter, "slope", 1.0f);
    session.getEdit().dispatchPendingUpdatesSynchronously(); // the graph with the Filter, before playing

    CHECK (render() < open * 0.3f); // a saw at C5 loses most of its energy below 100 Hz
}

TEST_CASE ("Filter window screenshot for the manual", "[.screenshot][filter]")
{
    FilterProcessor filter;
    set (filter, "cutoff", 1800.0f);
    set (filter, "resonance", 0.6f);
    set (filter, "lfo_amount", 0.4f);
    std::unique_ptr<juce::AudioProcessorEditor> editor (filter.createEditorAndMakeActive());
    editor->setSize (620, 470);
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/filter.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    filter.editorBeingDeleted (editor.get());
}

TEST_CASE ("Filter CPU cost: 24 dB, LFO and envelope follower, stereo, 48 kHz, 128 samples", "[.bench][filter]")
{
    FilterProcessor filter;
    set (filter, "slope", 1.0f);
    set (filter, "resonance", 0.7f);
    set (filter, "lfo_amount", 0.5f);
    set (filter, "env_amount", 0.5f);
    filter.prepareToPlay (rate, 128);
    juce::AudioBuffer<float> buffer (2, 128);
    juce::MidiBuffer midi;
    juce::Random noise (3);

    const int blocks = (int)(5.0 * rate / 128);
    const auto start = juce::Time::getHighResolutionTicks();
    for (int i = 0; i < blocks; ++i)
    {
        for (int c = 0; c < 2; ++c)
            buffer.setSample (c, 0, noise.nextFloat()); // keep it from settling into silence
        filter.processBlock (buffer, midi);
    }
    const auto seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
    std::cout << "Filter: " << seconds / 5.0 * 100.0 << " % of one core\n";
}
