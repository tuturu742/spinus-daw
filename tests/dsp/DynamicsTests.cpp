#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "TruePeakMeter.h"

#include "dsp/dynamics/DynamicsEditors.h"
#include "dsp/dynamics/DynamicsProcessors.h"
#include "dsp/loudness/LoudnessEngine.h"
#include "engine/BundledDevices.h"
#include "engine/EngineHost.h"
#include "ui/ChannelRack.h"
#include "ui/Mixer.h"

#include <iostream>

using namespace spinus;
using namespace spinus::dsp;
namespace te = tracktion;

namespace
{
constexpr double rate = 48000.0;

float db (float gain) { return juce::Decibels::gainToDecibels (gain, -200.0f); }

/** Runs `input` (the same on both channels) through `process (main, side)` in blocks; returns channel 0. */
template <typename Process>
std::vector<float> run (const std::vector<float>& input, int blockSize, Process&& process,
                        const std::vector<float>* sidechain = nullptr)
{
    std::vector<float> out;
    out.reserve (input.size());
    juce::AudioBuffer<float> main (2, blockSize), side (2, blockSize);
    for (size_t start = 0; start < input.size(); start += (size_t)blockSize)
    {
        const auto n = (int)juce::jmin ((size_t)blockSize, input.size() - start);
        main.setSize (2, n, false, false, true);
        side.setSize (sidechain != nullptr ? 2 : 0, n, false, false, true);
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c)
            {
                main.setSample (c, i, input[start + (size_t)i]);
                if (sidechain != nullptr)
                    side.setSample (c, i, (*sidechain)[start + (size_t)i]);
            }
        process (main, side);
        for (int i = 0; i < n; ++i)
            out.push_back (main.getSample (0, i));
    }
    return out;
}

std::vector<float> constant (double seconds, float level)
{
    return std::vector<float> ((size_t)(seconds * rate), level);
}

std::vector<float> sine (double seconds, double hz, float level, double phase = 0.0)
{
    std::vector<float> x ((size_t)(seconds * rate));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = level * (float)std::sin (phase + 2.0 * M_PI * hz * (double)i / rate);
    return x;
}

float at (const std::vector<float>& x, double seconds) { return x[(size_t)(seconds * rate)]; }

float peakOf (const std::vector<float>& x, size_t from = 0)
{
    float p = 0.0f;
    for (auto i = from; i < x.size(); ++i)
        p = juce::jmax (p, std::abs (x[i]));
    return p;
}

float truePeakDb (const std::vector<float>& x, double sampleRate)
{
    LoudnessEngine meter;
    meter.prepare (sampleRate);
    juce::AudioBuffer<float> buffer (1, (int)x.size());
    std::copy (x.begin(), x.end(), buffer.getWritePointer (0));
    meter.process (buffer);
    return meter.getTruePeakDb();
}

template <typename Processor> void screenshot (Processor& p, int width, int height, const char* name)
{
    std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditorAndMakeActive());
    editor->setSize (width, height);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200); // the displays read the engine
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    const auto file = juce::File (SPINUS_TEST_FILES_DIR)
                          .getParentDirectory()
                          .getChildFile (juce::String ("docs/user/images/") + name + ".png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    p.editorBeingDeleted (editor.get());
}
} // namespace

//==============================================================================
TEST_CASE ("Compressor static curve: unity below the threshold and the ratio above", "[dsp][compressor]")
{
    CompressorSettings s;
    s.thresholdDb = -20.0f;
    s.ratio = 4.0f;
    s.kneeDb = 0.0f;
    CHECK (CompressorEngine::staticCurve (s, -40.0f) == Catch::Approx (-40.0f));
    CHECK (CompressorEngine::staticCurve (s, -20.0f) == Catch::Approx (-20.0f));
    CHECK (CompressorEngine::staticCurve (s, 0.0f) == Catch::Approx (-15.0f));

    // A soft knee rounds the corner: at the threshold, a quarter of the knee times (1/R - 1).
    s.kneeDb = 8.0f;
    CHECK (CompressorEngine::staticCurve (s, -30.0f) == Catch::Approx (-30.0f));
    CHECK (CompressorEngine::staticCurve (s, -20.0f) == Catch::Approx (-20.0f + (0.25f - 1.0f) * 16.0f / 16.0f));
    CHECK (CompressorEngine::staticCurve (s, 0.0f) == Catch::Approx (-15.0f));
    CHECK (CompressorEngine::autoMakeupDb (s) == Catch::Approx (7.5f));
}

TEST_CASE ("Compressor: steady reduction matches the curve with peak and RMS detection", "[dsp][compressor]")
{
    for (bool rms : { false, true })
    {
        CompressorSettings s;
        s.thresholdDb = -24.0f;
        s.ratio = 3.0f;
        s.kneeDb = 0.0f;
        s.rms = rms;
        CompressorEngine engine;
        engine.prepare (rate);
        // A constant (DC) level of -6 dB: 18 dB over, 12 dB of reduction.
        const auto out = run (constant (1.0, 0.5f), 256, [&] (auto& m, auto& sc) { engine.process (m, sc, s); });
        INFO ("rms " << rms);
        CHECK (db (at (out, 0.9) / 0.5f) == Catch::Approx (rms ? -12.0f - 3.01f * (2.0f / 3.0f) : -12.0f).margin (0.1));
        CHECK (engine.getGainReductionDb() == Catch::Approx (db (at (out, 0.99) / 0.5f)).margin (0.1));
    }
}

TEST_CASE ("Compressor attack and release take their times", "[dsp][compressor]")
{
    CompressorSettings s;
    s.thresholdDb = -40.0f;
    s.ratio = 20.0f;
    s.kneeDb = 0.0f;
    s.attackMs = 20.0f;
    s.releaseMs = 200.0f;
    CompressorEngine engine;
    engine.prepare (rate);
    // Silence-ish, then full scale for 0.5 s, then quiet again.
    auto input = constant (0.1, 0.001f);
    const auto loud = constant (0.5, 1.0f), quiet = constant (1.0, 0.001f);
    input.insert (input.end(), loud.begin(), loud.end());
    input.insert (input.end(), quiet.begin(), quiet.end());
    const auto out = run (input, 64, [&] (auto& m, auto& sc) { engine.process (m, sc, s); });

    const auto target = CompressorEngine::staticCurve (s, 0.0f); // -38 dB of reduction
    auto reductionAt = [&] (double t) { return db (std::abs (at (out, t)) / std::abs (at (input, t))); };
    CHECK (reductionAt (0.1 + 0.020) == Catch::Approx (target * (1.0f - std::exp (-1.0f))).margin (0.5));
    CHECK (reductionAt (0.5) == Catch::Approx (target).margin (0.1));
    // After the release time the reduction has recovered by 63%.
    CHECK (reductionAt (0.6 + 0.200) == Catch::Approx (target * std::exp (-1.0f)).margin (0.5));
}

TEST_CASE ("Compressor makeup and parallel mix", "[dsp][compressor]")
{
    CompressorSettings s;
    s.thresholdDb = -20.0f;
    s.ratio = 2.0f;
    s.kneeDb = 0.0f;
    s.makeupDb = 4.0f;
    CompressorEngine engine;
    engine.prepare (rate);
    // -10 dB in: 5 dB of reduction, then 4 dB of makeup.
    auto out = run (constant (0.5, 0.316228f), 128, [&] (auto& m, auto& sc) { engine.process (m, sc, s); });
    CHECK (db (at (out, 0.45) / 0.316228f) == Catch::Approx (-1.0f).margin (0.05));

    s.autoMakeup = true; // half of the 10 dB a full-scale sound would lose
    engine.reset();
    out = run (constant (0.5, 0.316228f), 128, [&] (auto& m, auto& sc) { engine.process (m, sc, s); });
    CHECK (db (at (out, 0.45) / 0.316228f) == Catch::Approx (4.0f).margin (0.05));

    s.autoMakeup = false;
    s.makeupDb = 0.0f;
    s.mix = 0.5f; // half dry, half compressed
    engine.reset();
    out = run (constant (0.5, 0.316228f), 128, [&] (auto& m, auto& sc) { engine.process (m, sc, s); });
    CHECK (at (out, 0.45) / 0.316228f ==
           Catch::Approx (0.5f + 0.5f * juce::Decibels::decibelsToGain (-5.0f)).margin (0.002));
}

TEST_CASE ("The Compressor follows its sidechain when asked", "[dsp][compressor]")
{
    CompressorSettings s;
    s.thresholdDb = -20.0f;
    s.ratio = 10.0f;
    s.kneeDb = 0.0f;
    const auto main = constant (0.5, 0.05f); // -26 dB: below the threshold
    const auto side = constant (0.5, 1.0f);
    CompressorEngine engine;
    engine.prepare (rate);
    auto out = run (main, 256, [&] (auto& m, auto& sc) { engine.process (m, sc, s); }, &side);
    CHECK (at (out, 0.45) == Catch::Approx (0.05f).margin (1.0e-5)); // own input: nothing to do

    s.sidechain = true;
    engine.reset();
    out = run (main, 256, [&] (auto& m, auto& sc) { engine.process (m, sc, s); }, &side);
    CHECK (db (at (out, 0.45) / 0.05f) == Catch::Approx (-18.0f).margin (0.2)); // the sidechain's 20 dB over

    // Set to the sidechain but nothing connected: it follows its own sound.
    engine.reset();
    const auto loud = constant (0.5, 1.0f);
    out = run (loud, 256, [&] (auto& m, auto& sc) { engine.process (m, sc, s); });
    CHECK (db (at (out, 0.45)) == Catch::Approx (-18.0f).margin (0.2));
}

TEST_CASE ("The Compressor and Gate processors read the sidechain bus", "[dsp][compressor][gate]")
{
    CompressorProcessor compressor;
    GateProcessor gate;
    for (auto* p : std::initializer_list<juce::AudioProcessor*> { &compressor, &gate })
    {
        REQUIRE (p->getBusCount (true) == 2);
        auto& own = dynamic_cast<OwnEffect&> (*p);
        own.setParameterValue ("sidechain", 1.0f);
        own.setParameterValue ("threshold", -20.0f);
        p->prepareToPlay (rate, 512);
        juce::AudioBuffer<float> buffer (4, 512);
        juce::MidiBuffer midi;
        float last = 0.0f;
        for (int b = 0; b < 40; ++b)
        {
            for (int i = 0; i < 512; ++i)
            {
                buffer.setSample (0, i, 0.05f);
                buffer.setSample (1, i, 0.05f);
                buffer.setSample (2, i, 1.0f); // the sidechain: loud
                buffer.setSample (3, i, 1.0f);
            }
            p->processBlock (buffer, midi);
            last = buffer.getSample (0, 511);
        }
        INFO (p->getName());
        if (p == &compressor)
            CHECK (last < 0.05f * 0.5f); // turned down by the loud sidechain
        else
            CHECK (last == Catch::Approx (0.05f).margin (1.0e-4)); // opened by it, though the sound is quiet
    }
}

TEST_CASE ("Compressor and Gate on a mixer insert take a sidechain from another insert", "[dsp][compressor][gate]")
{
    EngineHost host { { .openAudioDevice = false } };
    auto devices = std::make_unique<engine::DefaultDeviceFactory>();
    engine::registerBundledDevices (*devices, host.getEngine(), {});
    Session session (host.getEngine(), std::move (devices));
    ui::ChannelRack rack (session);
    ui::Mixer mixer (session);
    REQUIRE (rack.addChannel ("spinus.3x-osc", "Kick"));
    REQUIRE (rack.addChannel ("spinus.3x-osc", "Bass"));
    const auto kick = session.getProject().channels[0].targetInsert;
    const auto bass = session.getProject().channels[1].targetInsert;
    for (auto type : { "spinus.compressor", "spinus.gate" })
    {
        REQUIRE (mixer.addEffect (bass, type));
        const auto device = session.getProject().findInsert (bass)->effects.back().device.id;
        auto* plugin = dynamic_cast<te::ExternalPlugin*> (session.getAdapter().getDevicePlugin (device));
        REQUIRE (plugin != nullptr);
        CHECK (plugin->canSidechain());
    }
    REQUIRE (mixer.setSidechain (bass, kick));
    session.getEdit().dispatchPendingUpdatesSynchronously();
    for (auto& slot : session.getProject().findInsert (bass)->effects)
    {
        auto* plugin = session.getAdapter().getDevicePlugin (slot.device.id);
        REQUIRE (plugin != nullptr);
        CHECK (plugin->getSidechainSourceID().isValid());
    }
    CHECK (session.getAdapter().getMissingDevices().empty());
}

//==============================================================================
TEST_CASE ("The Limiter never goes above its ceiling", "[dsp][limiter]")
{
    juce::Random random (42);
    std::vector<std::pair<const char*, std::vector<float>>> signals;
    {
        std::vector<float> noise ((size_t)rate);
        for (auto& x : noise)
            x = (random.nextFloat() * 2.0f - 1.0f) * 4.0f;
        signals.push_back ({ "loud noise", noise });
        std::vector<float> square ((size_t)rate);
        for (size_t i = 0; i < square.size(); ++i)
            square[i] = ((i / 50) % 2 == 0) ? 1.5f : -1.5f;
        signals.push_back ({ "square", square });
        std::vector<float> clicks ((size_t)rate, 0.0f);
        for (size_t i = 100; i < clicks.size(); i += 997)
            clicks[i] = (i % 2 == 0) ? 8.0f : -8.0f;
        signals.push_back ({ "clicks", clicks });
        std::vector<float> alternating ((size_t)rate);
        for (size_t i = 0; i < alternating.size(); ++i)
            alternating[i] = (i % 2 == 0 ? 1.0f : -1.0f) * (i > 20000 ? 3.0f : 0.2f);
        signals.push_back ({ "alternating with a jump", alternating });
        auto bursts = sine (1.0, 3000.0, 1.0f);
        for (size_t i = 0; i < bursts.size(); ++i)
            bursts[i] *= (i / 4000) % 2 == 0 ? 0.01f : 6.0f;
        signals.push_back ({ "bursts", bursts });
    }

    for (double sampleRate : { 44100.0, 48000.0, 96000.0 })
        for (bool truePeak : { false, true })
            for (float lookahead : { 0.5f, 3.0f, 10.0f })
                for (auto& [name, input] : signals)
                {
                    LimiterEngine engine;
                    engine.prepare (sampleRate, lookahead);
                    LimiterSettings s;
                    s.ceilingDb = -1.0f;
                    s.inputDb = 6.0f;
                    s.releaseMs = 30.0f;
                    s.truePeak = truePeak;
                    juce::Random sizes (1);
                    std::vector<float> out;
                    size_t start = 0;
                    while (start < input.size())
                    {
                        const auto n = juce::jmin ((size_t)1 + (size_t)sizes.nextInt (700), input.size() - start);
                        juce::AudioBuffer<float> buffer (2, (int)n);
                        for (int c = 0; c < 2; ++c)
                            for (size_t i = 0; i < n; ++i)
                                buffer.setSample (c, (int)i, input[start + i] * (c == 0 ? 1.0f : -0.7f));
                        engine.process (buffer, s);
                        for (size_t i = 0; i < n; ++i)
                            out.push_back (buffer.getSample (0, (int)i));
                        start += n;
                    }
                    INFO (name << " at " << sampleRate << " Hz, true peak " << truePeak << ", look-ahead " << lookahead
                               << " ms");
                    CHECK (db (peakOf (out)) <= -1.0f + 1.0e-4f);
                    if (truePeak)
                        CHECK (truePeakDb (out, sampleRate) <= -1.0f + 0.01f);
                }
}

TEST_CASE ("The Limiter's true peak catches peaks between samples", "[dsp][limiter]")
{
    // A quarter-rate sine at 45 degrees: its samples are at 0.707 of its true peak.
    const auto input = sine (1.0, rate / 4.0, 1.0f, M_PI / 4.0);
    REQUIRE (db (peakOf (input)) == Catch::Approx (-3.01f).margin (0.01));
    REQUIRE (truePeakDb (input, rate) > -0.1f);
    for (bool truePeak : { false, true })
    {
        LimiterEngine engine;
        engine.prepare (rate, 3.0f);
        LimiterSettings s;
        s.ceilingDb = -2.0f;
        s.truePeak = truePeak;
        const auto out = run (input, 512, [&] (auto& m, auto&) { engine.process (m, s); });
        const auto tp = truePeakDb (out, rate); // all of it: a meter started mid-sound would ring
        INFO ("true peak " << truePeak << ": " << tp << " dBTP");
        if (truePeak)
            CHECK (tp <= -2.0f + 0.01f);
        else
            CHECK (tp > -1.5f); // the samples are under the ceiling; the sound between them is not
    }
}

TEST_CASE ("The Limiter keeps the BS.1770-4 true peak under its ceiling on worst cases", "[dsp][limiter]")
{
    // Measured with the standard's own 4x filter (tests/support/TruePeakMeter.h), not the Limiter's.
    for (double sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto length = (size_t)(0.3 * sampleRate);
        juce::Random random (11);
        std::vector<std::pair<const char*, std::vector<float>>> signals;
        auto make = [&] (const char* name, auto&& sampleAt)
        {
            std::vector<float> x (length);
            for (size_t i = 0; i < length; ++i)
                x[i] = sampleAt (i, (double)i / sampleRate);
            signals.push_back ({ name, std::move (x) });
        };
        make ("quarter-rate sine at 45 degrees",
              [&] (size_t i, double) { return (float)std::sin (M_PI / 4.0 + M_PI / 2.0 * (double)i); });
        make ("hard-clipped 1 kHz sine", [] (size_t, double t)
              { return juce::jlimit (-0.5f, 0.5f, 4.0f * (float)std::sin (2.0 * M_PI * 997.0 * t)); });
        make ("hard-clipped 5 kHz sine", [] (size_t, double t)
              { return juce::jlimit (-0.5f, 0.5f, 4.0f * (float)std::sin (2.0 * M_PI * 5003.0 * t)); });
        make ("white noise", [&] (size_t, double) { return random.nextFloat() * 2.0f - 1.0f; });
        make ("bursts at half the sample rate",
              [] (size_t i, double) { return i % 4801 < 6 ? (i % 2 == 0 ? 1.0f : -1.0f) : 0.0f; });
        make ("drum-like hits",
              [&] (size_t i, double t)
              {
                  const auto since = (double)(i % (size_t)(0.1 * sampleRate)) / sampleRate;
                  return (float)(std::exp (-since * 40.0) * std::sin (2.0 * M_PI * 60.0 * since) +
                                 std::exp (-since * 400.0) * (random.nextDouble() * 2.0 - 1.0)) +
                         0.2f * (float)std::sin (2.0 * M_PI * 220.0 * t);
              });
        make ("two tones near the top",
              [] (size_t, double t)
              {
                  return (float)(0.5 * std::sin (2.0 * M_PI * 11025.0 * t + 0.3) +
                                 0.5 * std::sin (2.0 * M_PI * 15000.0 * t));
              });

        for (auto& [name, signal] : signals)
            for (float ceiling : { -0.1f, -1.2f, -1.8f })
                for (float inputDb : { 0.0f, 6.0f, 12.0f })
                    for (float lookahead : { 0.5f, 3.0f })
                    {
                        LimiterEngine engine;
                        engine.prepare (sampleRate, lookahead);
                        LimiterSettings s;
                        s.ceilingDb = ceiling;
                        s.inputDb = inputDb;
                        s.releaseMs = 40.0f;
                        s.truePeak = true;
                        auto input = signal;
                        input.resize (length + (size_t)(0.05 * sampleRate), 0.0f); // the sound stops in the limiter
                        std::vector<float> left, right;
                        juce::AudioBuffer<float> buffer (2, 300);
                        for (size_t start = 0; start < input.size(); start += 300)
                        {
                            const auto n = (int)juce::jmin ((size_t)300, input.size() - start);
                            buffer.setSize (2, n, false, false, true);
                            for (int i = 0; i < n; ++i)
                            {
                                buffer.setSample (0, i, input[start + (size_t)i]);
                                buffer.setSample (1, i, -0.8f * input[start + (size_t)i]);
                            }
                            engine.process (buffer, s);
                            for (int i = 0; i < n; ++i)
                            {
                                left.push_back (buffer.getSample (0, i));
                                right.push_back (buffer.getSample (1, i));
                            }
                        }
                        INFO (name << " at " << sampleRate << " Hz, ceiling " << ceiling << " dB, input +" << inputDb
                                   << " dB, look-ahead " << lookahead << " ms");
                        CHECK (test::toDb (test::bs1770TruePeak (left)) <= ceiling + 0.02);
                        CHECK (test::toDb (test::bs1770TruePeak (right)) <= ceiling + 0.02);
                    }
    }
}

TEST_CASE ("The Loudness Meter's true peak is BS.1770-4's", "[dsp][loudness][limiter]")
{
    juce::Random random (5);
    std::vector<float> x ((size_t)rate / 4);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = (random.nextFloat() * 2.0f - 1.0f) * 0.5f + 0.4f * (float)std::sin (M_PI / 2.0 * (double)i + 0.7);
    x.resize (x.size() + 24, 0.0f);
    CHECK (truePeakDb (x, rate) == Catch::Approx (test::toDb (test::bs1770TruePeak (x))).margin (0.001));
}

TEST_CASE ("The Limiter is transparent below its ceiling and delays by its latency", "[dsp][limiter]")
{
    LimiterProcessor limiter;
    limiter.setParameterValue ("lookahead", 5.0f);
    limiter.prepareToPlay (rate, 256);
    const auto latency = limiter.getLatencySamples();
    CHECK (latency == LimiterEngine::lookaheadSamplesFor (5.0f, rate) + LimiterEngine::truePeakSpan - 1);
    CHECK (latency == 240 + 30); // 5 ms at 48 kHz and the true-peak window

    std::vector<float> input ((size_t)(0.2 * rate), 0.0f);
    input[1000] = 0.5f;
    input[3000] = 0.85f; // under -1 dBFS (0.891)
    juce::MidiBuffer midi;
    const auto out = run (input, 256, [&] (auto& m, auto&) { limiter.processBlock (m, midi); });
    CHECK (out[(size_t)(1000 + latency)] == Catch::Approx (0.5f).margin (1.0e-6));
    CHECK (out[(size_t)(3000 + latency)] == Catch::Approx (0.85f).margin (1.0e-6));

    // A new look-ahead changes the latency that delay compensation uses.
    limiter.setParameterValue ("lookahead", 1.0f);
    limiter.updateLatency();
    CHECK (limiter.getLatencySamples() == LimiterEngine::latencyFor (1.0f, rate));
}

TEST_CASE ("The Limiter turns a loud peak down to the ceiling and recovers over its release", "[dsp][limiter]")
{
    LimiterEngine engine;
    engine.prepare (rate, 2.0f);
    LimiterSettings s;
    s.ceilingDb = -6.0f;
    s.releaseMs = 50.0f;
    s.truePeak = false;
    auto input = constant (0.5, 0.25f);
    for (size_t i = 4800; i < 4900; ++i)
        input[i] = 1.0f; // 12 dB over the -12 dB the rest is at, 6 dB over the ceiling
    const auto out = run (input, 128, [&] (auto& m, auto&) { engine.process (m, s); });
    const auto latency = (size_t)LimiterEngine::latencyFor (2.0f, rate);
    CHECK (out[4850 + latency] == Catch::Approx (juce::Decibels::decibelsToGain (-6.0f)).margin (1.0e-4));
    CHECK (out[4000 + latency] == Catch::Approx (0.25f).margin (1.0e-6)); // well before: untouched
    // 50 ms after the peak, 63% of the 6 dB has come back; at 250 ms nearly all.
    CHECK (db (out[4900 + latency + (size_t)(0.05 * rate)] / 0.25f) ==
           Catch::Approx (-6.0f * std::exp (-1.0f)).margin (0.6));
    CHECK (db (out[4900 + latency + (size_t)(0.25 * rate)] / 0.25f) > -0.1f);
}

//==============================================================================
TEST_CASE ("The Gate opens over the threshold and closes below it after the hold", "[dsp][gate]")
{
    GateSettings s;
    s.thresholdDb = -30.0f;
    s.hysteresisDb = 0.0f;
    s.attackMs = 0.1f;
    s.holdMs = 50.0f;
    s.releaseMs = 5.0f;
    GateEngine engine;
    engine.prepare (rate);
    // Quiet (-40 dB), loud (-20 dB) from 0.1 to 0.3 s, quiet again.
    auto input = constant (0.5, 0.01f);
    for (size_t i = (size_t)(0.1 * rate); i < (size_t)(0.3 * rate); ++i)
        input[i] = 0.1f;
    const auto out = run (input, 32, [&] (auto& m, auto& sc) { engine.process (m, sc, s); });
    CHECK (at (out, 0.05) == Catch::Approx (0.0f).margin (1.0e-6)); // closed: silent
    CHECK (at (out, 0.2) == Catch::Approx (0.1f).margin (1.0e-4));  // open
    // The level detector falls in about 10 ms; then the 50 ms hold; then it closes in about 5 ms.
    CHECK (at (out, 0.3 + 0.045) == Catch::Approx (0.01f).margin (1.0e-4)); // holding
    CHECK (std::abs (at (out, 0.3 + 0.12)) < 1.0e-4f);                      // closed
    CHECK (engine.getNumOpenings() == 1);
    CHECK (engine.getState() == GateEngine::State::closed);
}

TEST_CASE ("Hysteresis keeps the Gate from chattering", "[dsp][gate]")
{
    // A level that wobbles 2 dB either side of the threshold, slowly (so the detector follows it).
    std::vector<float> input ((size_t)rate);
    for (size_t i = 0; i < input.size(); ++i)
        input[i] =
            juce::Decibels::decibelsToGain (-30.0f + 2.0f * (float)std::sin (2.0 * M_PI * 4.0 * (double)i / rate));
    for (float hysteresis : { 0.0f, 6.0f })
    {
        GateSettings s;
        s.thresholdDb = -30.0f;
        s.hysteresisDb = hysteresis;
        s.holdMs = 0.0f;
        GateEngine engine;
        engine.prepare (rate);
        run (input, 128, [&] (auto& m, auto& sc) { engine.process (m, sc, s); });
        INFO ("hysteresis " << hysteresis);
        if (hysteresis > 0.0f)
            CHECK (engine.getNumOpenings() == 1);
        else
            CHECK (engine.getNumOpenings() >= 4);
    }
}

TEST_CASE ("The Gate's range turns it down instead of off", "[dsp][gate]")
{
    GateSettings s;
    s.thresholdDb = -10.0f;
    s.rangeDb = -12.0f;
    GateEngine engine;
    engine.prepare (rate);
    const auto out = run (constant (0.5, 0.1f), 64, [&] (auto& m, auto& sc) { engine.process (m, sc, s); });
    CHECK (db (at (out, 0.45) / 0.1f) == Catch::Approx (-12.0f).margin (0.05));
}

TEST_CASE ("The Gate opens on its sidechain", "[dsp][gate]")
{
    GateSettings s;
    s.thresholdDb = -30.0f;
    s.sidechain = true;
    s.holdMs = 0.0f;
    s.releaseMs = 5.0f;
    std::vector<float> side ((size_t)(0.6 * rate), 0.0f);
    for (size_t i = (size_t)(0.2 * rate); i < (size_t)(0.3 * rate); ++i)
        side[i] = 0.5f;
    GateEngine engine;
    engine.prepare (rate);
    const auto out = run (constant (0.6, 0.2f), 64, [&] (auto& m, auto& sc) { engine.process (m, sc, s); }, &side);
    CHECK (at (out, 0.1) == Catch::Approx (0.0f).margin (1.0e-6)); // the sound is loud, the sidechain silent
    CHECK (at (out, 0.25) == Catch::Approx (0.2f).margin (1.0e-4));
    CHECK (at (out, 0.5) == Catch::Approx (0.0f).margin (1.0e-5));
}

//==============================================================================
TEST_CASE ("The dynamics effects keep their settings and follow automation", "[dsp][compressor][limiter][gate]")
{
    CompressorProcessor c;
    c.setParameterValue ("threshold", -30.0f);
    c.setParameterValue ("ratio", 8.0f);
    c.setParameterValue ("detector", 1.0f);
    c.setParameterValue ("sidechain", 1.0f);
    c.setParameterValue ("auto_makeup", 1.0f);
    LimiterProcessor l;
    l.setParameterValue ("ceiling", -0.3f);
    l.setParameterValue ("true_peak", 0.0f);
    l.setParameterValue ("lookahead", 7.0f);
    GateProcessor g;
    g.setParameterValue ("hold", 120.0f);
    g.setParameterValue ("hysteresis", 5.0f);

    auto roundTrip = [] (juce::AudioProcessor& from, juce::AudioProcessor& to)
    {
        juce::MemoryBlock state;
        from.getStateInformation (state);
        to.setStateInformation (state.getData(), (int)state.getSize());
    };
    CompressorProcessor c2;
    LimiterProcessor l2;
    GateProcessor g2;
    roundTrip (c, c2);
    roundTrip (l, l2);
    roundTrip (g, g2);
    CHECK (c2.getSettings().thresholdDb == Catch::Approx (-30.0f).margin (0.01));
    CHECK (c2.getSettings().ratio == Catch::Approx (8.0f).margin (0.01));
    CHECK (c2.getSettings().rms);
    CHECK (c2.getSettings().sidechain);
    CHECK (c2.getSettings().autoMakeup);
    CHECK (l2.getSettings().ceilingDb == Catch::Approx (-0.3f).margin (0.01));
    CHECK (! l2.getSettings().truePeak);
    CHECK (l2.getLookaheadMs() == Catch::Approx (7.0f).margin (0.01));
    CHECK (g2.getSettings().holdMs == Catch::Approx (120.0f).margin (0.5));
    CHECK (g2.getSettings().hysteresisDb == Catch::Approx (5.0f).margin (0.01));

    // Automation: a ceiling moved while playing applies from the next block.
    LimiterProcessor live;
    live.setParameterValue ("true_peak", 0.0f);
    live.prepareToPlay (rate, 256);
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer (2, 256);
    float peak = 0.0f;
    for (int b = 0; b < 100; ++b)
    {
        if (b == 50)
            live.setParameterValue ("ceiling", -6.0f);
        for (int c2i = 0; c2i < 2; ++c2i)
            for (int i = 0; i < 256; ++i)
                buffer.setSample (c2i, i, 0.9f * (float)std::sin (i * 0.3 + b));
        live.processBlock (buffer, midi);
        if (b >= 51)
            peak = juce::jmax (peak, buffer.getMagnitude (0, 0, 256));
    }
    CHECK (db (peak) <= -6.0f + 1.0e-4f);
    CHECK (db (peak) > -7.0f);
    CHECK (! l.getParameters().isEmpty());
    // The look-ahead cannot be automated (it changes the latency); everything else can.
    for (auto* parameter : l.getParameters())
        if (auto* p = dynamic_cast<juce::AudioProcessorParameterWithID*> (parameter))
            CHECK (p->isAutomatable() == (p->paramID != "lookahead"));
}

TEST_CASE ("The dynamics windows open and say what they follow", "[dsp][compressor][gate][ui]")
{
    CompressorProcessor c;
    c.prepareToPlay (rate, 256);
    std::unique_ptr<juce::AudioProcessorEditor> editor (c.createEditorAndMakeActive());
    auto* window = dynamic_cast<CompressorEditor*> (editor.get());
    REQUIRE (window != nullptr);
    CHECK (window->getStatusText().startsWith ("Follows its own sound"));
    c.setParameterValue ("sidechain", 1.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    CHECK (window->getStatusText().startsWith ("Set to follow the sidechain"));
    c.editorBeingDeleted (editor.get());

    GateProcessor g;
    g.prepareToPlay (rate, 256);
    editor.reset (g.createEditorAndMakeActive());
    auto* gateWindow = dynamic_cast<GateEditor*> (editor.get());
    REQUIRE (gateWindow != nullptr);
    CHECK (gateWindow->getStateText() == "Closed");
    g.editorBeingDeleted (editor.get());

    LimiterProcessor l;
    l.prepareToPlay (rate, 256);
    editor.reset (l.createEditorAndMakeActive());
    REQUIRE (dynamic_cast<LimiterEditor*> (editor.get()) != nullptr);
    l.editorBeingDeleted (editor.get());
    editor.reset(); // before the processors it belongs to
}

TEST_CASE ("Compressor Limiter and Gate window screenshots for the manual", "[.screenshot][compressor][limiter][gate]")
{
    juce::MidiBuffer midi;
    auto feed = [&] (juce::AudioProcessor& p, float level, int blocks)
    {
        juce::AudioBuffer<float> buffer (p.getTotalNumInputChannels(), 512);
        buffer.clear();
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < 512; ++i)
                for (int c = 0; c < 2; ++c)
                    buffer.setSample (c, i, level * (float)std::sin ((b * 512 + i) * 0.05));
            p.processBlock (buffer, midi);
        }
    };
    CompressorProcessor c;
    c.setParameterValue ("threshold", -24.0f);
    c.setParameterValue ("ratio", 4.0f);
    c.setParameterValue ("makeup", 6.0f);
    c.prepareToPlay (rate, 512);
    feed (c, 0.5f, 40);
    screenshot (c, 700, 420, "compressor");

    LimiterProcessor l;
    l.setParameterValue ("input", 6.0f);
    l.prepareToPlay (rate, 512);
    feed (l, 0.9f, 40);
    screenshot (l, 620, 400, "limiter");

    GateProcessor g;
    g.setParameterValue ("threshold", -36.0f);
    g.prepareToPlay (rate, 512);
    feed (g, 0.3f, 40);
    screenshot (g, 660, 420, "gate");
}

TEST_CASE ("Dynamics CPU cost at 48 kHz with 128 samples in stereo", "[.bench][compressor][limiter][gate]")
{
    juce::AudioBuffer<float> buffer (2, 128), none;
    juce::Random random (5);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 128; ++i)
            buffer.setSample (c, i, random.nextFloat() - 0.5f);
    const int blocks = (int)(5.0 * rate / 128);
    auto bench = [&] (const char* name, auto&& process)
    {
        const auto start = juce::Time::getHighResolutionTicks();
        for (int i = 0; i < blocks; ++i)
            process();
        const auto seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
        std::cout << name << ": " << seconds / 5.0 * 100.0 << " % of one core\n";
    };
    CompressorEngine compressor;
    compressor.prepare (rate);
    CompressorSettings cs;
    cs.rms = true;
    bench ("Compressor", [&] { compressor.process (buffer, none, cs); });
    LimiterEngine limiter;
    limiter.prepare (rate, 3.0f);
    LimiterSettings ls;
    bench ("Limiter (true peak)", [&] { limiter.process (buffer, ls); });
    GateEngine gate;
    gate.prepare (rate);
    GateSettings gs;
    bench ("Gate", [&] { gate.process (buffer, none, gs); });
}
