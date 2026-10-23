#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "dsp/utility/StereoUtilityEditor.h"
#include "dsp/utility/StereoUtilityProcessor.h"

using namespace spinus::dsp;

namespace
{
constexpr double rate = 48000.0;

/** One block of a stereo input after the settings have settled: returns {left, right} of the last sample. */
std::pair<float, float> processed (const StereoUtilitySettings& s, float left, float right)
{
    StereoUtilityEngine engine;
    engine.prepare (rate);
    juce::AudioBuffer<float> buffer (2, 256);
    for (int i = 0; i < 4; ++i) // the first block starts at the settings; later ones are steady
    {
        for (int n = 0; n < 256; ++n)
        {
            buffer.setSample (0, n, left);
            buffer.setSample (1, n, right);
        }
        engine.process (buffer, s);
    }
    return { buffer.getSample (0, 255), buffer.getSample (1, 255) };
}

void set (StereoUtilityProcessor& p, const juce::String& id, float value)
{
    auto* parameter = p.getParameterState().getParameter (id);
    REQUIRE (parameter != nullptr);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}
} // namespace

TEST_CASE ("Stereo Utility: unchanged at its defaults", "[dsp][utility]")
{
    const auto [l, r] = processed ({}, 0.3f, -0.6f);
    CHECK (l == Catch::Approx (0.3f));
    CHECK (r == Catch::Approx (-0.6f));
}

TEST_CASE ("Stereo Utility: gain, pan, width, mono and polarity", "[dsp][utility]")
{
    StereoUtilitySettings s;

    SECTION ("Gain")
    {
        s.gainDb = -6.0f;
        CHECK (processed (s, 1.0f, 1.0f).first == Catch::Approx (0.501f).margin (0.001));
    }
    SECTION ("Pan: the far side goes down")
    {
        s.pan = -0.5f;
        const auto [l, r] = processed (s, 1.0f, 1.0f);
        CHECK (l == Catch::Approx (1.0f));
        CHECK (r == Catch::Approx (0.5f));
    }
    SECTION ("Width 0 and Mono make both sides the same")
    {
        s.width = 0.0f;
        auto [l, r] = processed (s, 1.0f, 0.0f);
        CHECK (l == Catch::Approx (0.5f));
        CHECK (r == Catch::Approx (0.5f));
        s.width = 1.0f;
        s.mono = true;
        std::tie (l, r) = processed (s, 1.0f, 0.0f);
        CHECK (l == Catch::Approx (0.5f));
        CHECK (r == Catch::Approx (0.5f));
    }
    SECTION ("Width 200% doubles the difference between the sides")
    {
        s.width = 2.0f;
        const auto [l, r] = processed (s, 1.0f, 0.0f);
        CHECK (l == Catch::Approx (1.5f));
        CHECK (r == Catch::Approx (-0.5f));
    }
    SECTION ("Polarity per side")
    {
        s.invertLeft = true;
        auto [l, r] = processed (s, 0.4f, 0.4f);
        CHECK (l == Catch::Approx (-0.4f));
        CHECK (r == Catch::Approx (0.4f));
        s.invertLeft = false;
        s.invertRight = true;
        std::tie (l, r) = processed (s, 0.4f, 0.4f);
        CHECK (l == Catch::Approx (0.4f));
        CHECK (r == Catch::Approx (-0.4f));
    }
}

TEST_CASE ("Stereo Utility: changes are smoothed and the meters read the output", "[dsp][utility]")
{
    StereoUtilityEngine engine;
    engine.prepare (rate);
    juce::AudioBuffer<float> buffer (2, 64);
    StereoUtilitySettings s;
    for (int n = 0; n < 64; ++n)
    {
        buffer.setSample (0, n, 1.0f);
        buffer.setSample (1, n, 1.0f);
    }
    engine.process (buffer, s);
    CHECK (engine.getCorrelation() == Catch::Approx (1.0f));

    // A sudden -24 dB: the first samples after the change are still near full level (no click).
    s.gainDb = -24.0f;
    for (int n = 0; n < 64; ++n)
    {
        buffer.setSample (0, n, 1.0f);
        buffer.setSample (1, n, 1.0f);
    }
    engine.process (buffer, s);
    CHECK (buffer.getSample (0, 0) > 0.95f);
    CHECK (buffer.getSample (0, 63) < buffer.getSample (0, 0));
    CHECK (engine.getPeak (0) > 0.9f);

    // Opposite sides read as -1.
    s = {};
    s.invertRight = true;
    StereoUtilityEngine other;
    other.prepare (rate);
    for (int n = 0; n < 64; ++n)
    {
        buffer.setSample (0, n, 0.5f);
        buffer.setSample (1, n, 0.5f);
    }
    other.process (buffer, s);
    CHECK (other.getCorrelation() == Catch::Approx (-1.0f));
}

TEST_CASE ("The Stereo Utility's state holds its settings, and its window opens", "[dsp][utility]")
{
    StereoUtilityProcessor a;
    set (a, "width", 1.5f);
    set (a, "mono", 1.0f);
    set (a, "invert_right", 1.0f);
    juce::MemoryBlock state;
    a.getStateInformation (state);

    StereoUtilityProcessor b;
    b.setStateInformation (state.getData(), (int)state.getSize());
    const auto s = b.getSettings();
    CHECK (s.width == Catch::Approx (1.5f));
    CHECK (s.mono);
    CHECK (s.invertRight);
    CHECK (! s.invertLeft);

    std::unique_ptr<juce::AudioProcessorEditor> editor (b.createEditorAndMakeActive());
    REQUIRE (editor != nullptr);
    b.editorBeingDeleted (editor.get());
}

TEST_CASE ("Stereo Utility window screenshot for the manual", "[.screenshot][utility]")
{
    StereoUtilityProcessor utility;
    set (utility, "width", 1.4f);
    set (utility, "gain", -3.0f);
    utility.prepareToPlay (rate, 512);
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    for (int n = 0; n < 512; ++n)
    {
        buffer.setSample (0, n, 0.5f * (float)std::sin (n * 0.05));
        buffer.setSample (1, n, 0.4f * (float)std::sin (n * 0.05 + 0.6));
    }
    utility.processBlock (buffer, midi);

    std::unique_ptr<juce::AudioProcessorEditor> editor (utility.createEditorAndMakeActive());
    editor->setSize (480, 300);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100); // the meters read the block
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    const auto file =
        juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/stereo-utility.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    utility.editorBeingDeleted (editor.get());
}

TEST_CASE ("Stereo Utility CPU cost: stereo, 48 kHz, 128 samples", "[.bench][utility]")
{
    StereoUtilityEngine engine;
    engine.prepare (rate);
    StereoUtilitySettings s;
    s.width = 1.5f;
    s.pan = 0.2f;
    juce::AudioBuffer<float> buffer (2, 128);
    buffer.clear();
    const int blocks = (int)(5.0 * rate / 128);
    const auto start = juce::Time::getHighResolutionTicks();
    for (int i = 0; i < blocks; ++i)
        engine.process (buffer, s);
    const auto seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
    std::cout << "Stereo Utility: " << seconds / 5.0 * 100.0 << " % of one core\n";
}
