#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "dsp/loudness/LoudnessMeterEditor.h"
#include "dsp/loudness/LoudnessMeterProcessor.h"

using namespace spinus::dsp;

namespace
{
constexpr double rate = 48000.0;

/** Feeds `seconds` of a sine (peak `dbfs`, both channels) to the engine, in 512-sample blocks. */
void feedSine (LoudnessEngine& engine, double seconds, float dbfs, double frequency = 1000.0, double phase = 0.0)
{
    const auto amplitude = juce::Decibels::decibelsToGain (dbfs);
    juce::AudioBuffer<float> buffer (2, 512);
    static double t = 0.0;
    for (int done = 0; done < (int)(seconds * rate); done += 512)
    {
        for (int n = 0; n < 512; ++n)
        {
            const auto x = amplitude * (float)std::sin (2.0 * M_PI * frequency * t + phase);
            buffer.setSample (0, n, x);
            buffer.setSample (1, n, x);
            t += 1.0 / rate;
        }
        engine.process (buffer);
    }
}
} // namespace

TEST_CASE ("Loudness: a stereo 1 kHz sine at -23 dBFS reads -23 LUFS (EBU Tech 3341, case 1)", "[dsp][loudness]")
{
    LoudnessEngine engine;
    engine.prepare (rate);
    feedSine (engine, 20.0, -23.0f);
    CHECK (engine.getMomentary() == Catch::Approx (-23.0f).margin (0.1f));
    CHECK (engine.getShortTerm() == Catch::Approx (-23.0f).margin (0.1f));
    CHECK (engine.getIntegrated() == Catch::Approx (-23.0f).margin (0.1f));
    CHECK (engine.getTruePeakDb() == Catch::Approx (-23.0f).margin (0.2f));
    CHECK (engine.getMeasuredSeconds() == Catch::Approx (20.0).margin (0.2));

    // The same at 44.1 kHz.
    LoudnessEngine other;
    other.prepare (44100.0);
    juce::AudioBuffer<float> buffer (2, 441);
    for (int block = 0; block < 1000; ++block)
    {
        for (int n = 0; n < 441; ++n)
            for (int c = 0; c < 2; ++c)
                buffer.setSample (c, n,
                                  juce::Decibels::decibelsToGain (-23.0f) *
                                      (float)std::sin (2.0 * M_PI * 1000.0 * (block * 441 + n) / 44100.0));
        other.process (buffer);
    }
    CHECK (other.getIntegrated() == Catch::Approx (-23.0f).margin (0.1f));
}

TEST_CASE ("Loudness: quiet passages are gated out of the integrated loudness", "[dsp][loudness]")
{
    // EBU Tech 3341 case 3 style: -36, -23, -36 LUFS; the quiet parts are more than 10 LU below and don't count.
    LoudnessEngine engine;
    engine.prepare (rate);
    feedSine (engine, 10.0, -36.0f);
    feedSine (engine, 60.0, -23.0f);
    feedSine (engine, 10.0, -36.0f);
    CHECK (engine.getIntegrated() == Catch::Approx (-23.0f).margin (0.1f));
    CHECK (engine.getShortTerm() == Catch::Approx (-36.0f).margin (0.2f));

    // Silence below -70 LUFS doesn't count either; reset starts again.
    feedSine (engine, 10.0, -100.0f);
    CHECK (engine.getIntegrated() == Catch::Approx (-23.0f).margin (0.1f));
    engine.requestReset();
    feedSine (engine, 5.0, -18.0f);
    CHECK (engine.getIntegrated() == Catch::Approx (-18.0f).margin (0.1f));

    std::array<float, LoudnessEngine::historySteps> history {};
    CHECK (engine.copyHistory (history) == 50);
    CHECK (history[49] == Catch::Approx (-18.0f).margin (0.1f));
}

TEST_CASE ("Loudness: the true peak finds peaks between samples", "[dsp][loudness]")
{
    // A sine at a quarter of the sample rate, 45 degrees off: every sample is at 71% of the real peak.
    LoudnessEngine engine;
    engine.prepare (rate);
    feedSine (engine, 1.0, -6.0f, rate / 4.0, M_PI / 4.0);
    CHECK (engine.getTruePeakDb() == Catch::Approx (-6.0f).margin (0.5f)); // the samples alone say -9 dB
}

TEST_CASE ("Loudness Meter passes the sound unchanged and keeps its target", "[dsp][loudness]")
{
    LoudnessMeterProcessor meter;
    meter.prepareToPlay (rate, 256);
    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;
    for (int n = 0; n < 256; ++n)
    {
        buffer.setSample (0, n, 0.25f * (float)std::sin (n * 0.1));
        buffer.setSample (1, n, -0.5f);
    }
    juce::AudioBuffer<float> copy (buffer);
    meter.processBlock (buffer, midi);
    for (int c = 0; c < 2; ++c)
        for (int n = 0; n < 256; ++n)
            REQUIRE (juce::exactlyEqual (buffer.getSample (c, n), copy.getSample (c, n)));

    auto* target = meter.getParameterState().getParameter ("target");
    target->setValueNotifyingHost (target->convertTo0to1 (2.0f));
    CHECK (meter.getTarget().lufs == Catch::Approx (-23.0f));
    juce::MemoryBlock state;
    meter.getStateInformation (state);
    LoudnessMeterProcessor restored;
    restored.setStateInformation (state.getData(), (int)state.getSize());
    CHECK (restored.getTarget().lufs == Catch::Approx (-23.0f));
}

TEST_CASE ("The Loudness Meter window says how far from the target the song is", "[dsp][loudness][ui]")
{
    LoudnessMeterProcessor meter;
    meter.prepareToPlay (rate, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor (meter.createEditorAndMakeActive());
    auto* window = dynamic_cast<LoudnessMeterEditor*> (editor.get());
    REQUIRE (window != nullptr);
    CHECK (window->getVerdict().startsWith ("Play the song"));

    feedSine (meter.getEngine(), 6.0, -11.0f); // about -11 LUFS against Streaming's -14
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    INFO (window->getVerdict());
    CHECK (window->getVerdict().startsWith ("3.0 dB louder than the target"));

    meter.getEngine().requestReset();
    feedSine (meter.getEngine(), 6.0, -14.5f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    CHECK (window->getVerdict().startsWith ("On target."));
    meter.editorBeingDeleted (editor.get());
}

TEST_CASE ("Loudness Meter window screenshot for the manual", "[.screenshot][loudness]")
{
    LoudnessMeterProcessor meter;
    meter.prepareToPlay (rate, 512);
    // A song-like curve: a quieter intro, a loud chorus, a little over the target.
    for (int part = 0; part < 12; ++part)
        feedSine (meter.getEngine(), 3.0, part < 4 ? -24.0f + (float)part * 2.0f : -13.0f + 0.4f * (float)(part % 3));
    std::unique_ptr<juce::AudioProcessorEditor> editor (meter.createEditorAndMakeActive());
    editor->setSize (560, 360);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/loudness-meter.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    meter.editorBeingDeleted (editor.get());
}

TEST_CASE ("Loudness Meter CPU cost: stereo, 48 kHz, 128 samples", "[.bench][loudness]")
{
    LoudnessEngine engine;
    engine.prepare (rate);
    juce::AudioBuffer<float> buffer (2, 128);
    for (int n = 0; n < 128; ++n)
        for (int c = 0; c < 2; ++c)
            buffer.setSample (c, n, 0.3f * (float)std::sin (n * 0.2 + c));
    const int blocks = (int)(5.0 * rate / 128);
    const auto start = juce::Time::getHighResolutionTicks();
    for (int i = 0; i < blocks; ++i)
        engine.process (buffer);
    const auto seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
    std::cout << "Loudness Meter: " << seconds / 5.0 * 100.0 << " % of one core\n";
}
