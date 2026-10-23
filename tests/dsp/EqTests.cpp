#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "dsp/eq/EqEditor.h"
#include "dsp/eq/EqProcessor.h"

#include <iostream>

using namespace spinus::dsp;

namespace
{
float decibels (double gain) { return juce::Decibels::gainToDecibels ((float)gain, -200.0f); }

EqBand band (EqBand::Type type, float hz, float gainDb, float q, int slope = 0)
{
    EqBand b;
    b.on = true;
    b.type = type;
    b.frequencyHz = hz;
    b.gainDb = gainDb;
    b.q = q;
    b.slope = slope;
    return b;
}

/** The largest difference in dB between the matched sections and the analog prototype, from 20 Hz up to
    `top` Hz, ignoring frequencies where the prototype is below -24 dB (deep in a cut or notch). */
float worstError (const EqBand& b, double rate, double top, bool bilinear = false)
{
    const auto d = bilinear ? EqEngine::designBilinear (b, rate) : EqEngine::design (b, rate);
    float worst = 0.0f;
    for (int i = 0; i <= 400; ++i)
    {
        const auto hz = 20.0 * std::pow (top / 20.0, i / 400.0);
        const auto digital = decibels (EqEngine::magnitude (d, hz, rate));
        const auto analog = decibels (EqEngine::analogMagnitude (b, hz));
        if (analog < -24.0f)
            continue; // deep in a cut or a notch: what matters is that it is deep
        worst = juce::jmax (worst, std::abs (digital - analog));
    }
    return worst;
}

/** The gain of the running engine for a sine, after it settles. */
float measured (EqEngine& engine, const EqSettings& s, double hz, double rate, int channels = 2)
{
    engine.prepare (rate, 512);
    juce::AudioBuffer<float> buffer (channels, 512);
    double phase = 0.0, sum = 0.0;
    int count = 0;
    for (int blockIndex = 0; blockIndex < (int)(0.4 * rate / 512); ++blockIndex)
    {
        for (int i = 0; i < 512; ++i)
        {
            const auto x = (float)std::sin (phase);
            phase += 2.0 * M_PI * hz / rate;
            for (int c = 0; c < channels; ++c)
                buffer.setSample (c, i, x);
        }
        engine.process (buffer, s);
        if (blockIndex * 512 > 0.2 * rate)
            for (int i = 0; i < 512; ++i)
            {
                sum += (double)buffer.getSample (0, i) * buffer.getSample (0, i);
                ++count;
            }
    }
    return (float)std::sqrt (2.0 * sum / count);
}
} // namespace

TEST_CASE ("EQ bands follow their analog shapes at 44.1 48 and 96 kHz", "[dsp][eq]")
{
    using T = EqBand::Type;
    // The worst difference from the analog prototype, from 20 Hz to 20 kHz (or 0.45 of the sample rate),
    // for bands in three ranges of frequency; Q from 0.5 to 3, gains up to 12 dB either way.
    struct Range
    {
        std::initializer_list<float> frequencies;
        float tolerance44, tolerance96;
    };
    const std::array<Range, 3> ranges { {
        { { 30.0f, 100.0f, 400.0f, 1000.0f, 3000.0f }, 0.4f, 0.05f },
        { { 6000.0f, 10000.0f, 14000.0f }, 1.5f, 0.6f },
        { { 18000.0f }, 2.7f, 0.6f },
    } };
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto top = juce::jmin (20000.0, 0.45 * rate);
        for (auto& range : ranges)
        {
            float worst = 0.0f;
            for (float hz : range.frequencies)
            {
                for (auto type : { T::bell, T::lowShelf, T::highShelf, T::lowCut, T::highCut })
                    for (float gain : { -12.0f, -3.0f, 6.0f, 12.0f })
                        for (float q : { 0.5f, 0.71f, 1.0f, 1.5f, 3.0f })
                            worst = juce::jmax (worst, worstError (band (type, hz, gain, q), rate, top));
                for (float q : { 2.0f, 4.0f, 8.0f })
                    worst = juce::jmax (worst, worstError (band (T::notch, hz, 0.0f, q), rate, top));
            }
            INFO ("rate " << rate << ", bands from " << *range.frequencies.begin() << " Hz: worst error " << worst
                          << " dB");
            CHECK (worst < (rate > 90000.0 ? range.tolerance96 : range.tolerance44));
        }
    }
}

TEST_CASE ("Matched EQ bands keep the top octave where the bilinear transform cramps it", "[dsp][eq]")
{
    // A wide boost at 12 kHz: the bilinear transform pulls its top half towards Nyquist.
    const auto b = band (EqBand::Type::bell, 12000.0f, 9.0f, 0.7f);
    const auto matched = worstError (b, 44100.0, 20000.0);
    const auto bilinear = worstError (b, 44100.0, 20000.0, true);
    INFO ("matched " << matched << " dB, bilinear " << bilinear << " dB");
    CHECK (bilinear > 3.0f);
    CHECK (matched < 1.0f);

    // The exact values that matter: the bell's gain at its own frequency, and a notch's null.
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        CHECK (decibels (EqEngine::magnitude (EqEngine::design (b, rate), 12000.0, rate)) ==
               Catch::Approx (9.0f).margin (0.01));
        const auto notch = band (EqBand::Type::notch, 15000.0f, 0.0f, 2.0f);
        CHECK (decibels (EqEngine::magnitude (EqEngine::design (notch, rate), 15000.0, rate)) < -60.0f);
    }
}

TEST_CASE ("EQ cuts fall at 12 24 and 48 dB per octave", "[dsp][eq]")
{
    for (int slope : { 0, 1, 2 })
    {
        const auto expected = 12.0f * (slope == 2 ? 4.0f : (float)slope + 1.0f);
        const auto cut = band (EqBand::Type::lowCut, 1000.0f, 0.0f, 0.71f, slope);
        const auto d = EqEngine::design (cut, 48000.0);
        CHECK (decibels (EqEngine::magnitude (d, 1000.0, 48000.0)) == Catch::Approx (-3.0f).margin (0.3));
        const auto octave =
            decibels (EqEngine::magnitude (d, 125.0, 48000.0)) - decibels (EqEngine::magnitude (d, 250.0, 48000.0));
        CHECK (octave == Catch::Approx (-expected).margin (1.0));
        CHECK (decibels (EqEngine::magnitude (d, 10000.0, 48000.0)) == Catch::Approx (0.0f).margin (0.05));
    }
}

TEST_CASE ("The running EQ does what its curve shows in mono and stereo", "[dsp][eq]")
{
    EqSettings s;
    s.bands[2] = band (EqBand::Type::bell, 1000.0f, 6.0f, 1.0f);
    s.bands[6] = band (EqBand::Type::highShelf, 8000.0f, -6.0f, 0.71f);
    s.bands[0] = band (EqBand::Type::lowCut, 80.0f, 0.0f, 0.71f, 1);
    s.outputDb = -2.0f;
    EqEngine engine;
    for (double rate : { 44100.0, 96000.0 })
        for (int channels : { 1, 2 })
            for (double hz : { 50.0, 1000.0, 3000.0, 15000.0 })
            {
                INFO (rate << " Hz, " << channels << " channels, " << hz << " Hz");
                CHECK (decibels (measured (engine, s, hz, rate, channels)) ==
                       Catch::Approx (decibels (EqEngine::responseAt (s, hz, rate))).margin (0.1));
            }
}

TEST_CASE ("The EQ at its defaults leaves the sound unchanged", "[dsp][eq]")
{
    EqProcessor eq;
    eq.prepareToPlay (48000.0, 256);
    juce::Random random (7);
    juce::AudioBuffer<float> buffer (2, 256), copy (2, 256);
    juce::MidiBuffer midi;
    for (int b = 0; b < 20; ++b)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i)
                buffer.setSample (c, i, random.nextFloat() * 2.0f - 1.0f);
        copy.makeCopyOf (buffer);
        eq.processBlock (buffer, midi);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i)
                REQUIRE (buffer.getSample (c, i) == Catch::Approx (copy.getSample (c, i)).margin (1.0e-6));
    }
}

TEST_CASE ("EQ changes glide and an automated band moves the sound without jumps", "[dsp][eq]")
{
    EqProcessor eq;
    constexpr double rate = 48000.0;
    eq.prepareToPlay (rate, 64);
    juce::AudioBuffer<float> buffer (2, 64);
    juce::MidiBuffer midi;
    double phase = 0.0;
    std::vector<float> out;
    for (int b = 0; b < (int)(0.6 * rate / 64); ++b)
    {
        if (b == (int)(0.2 * rate / 64))
        {
            eq.setParameterValue (EqProcessor::bandParameter (4, "gain"), 12.0f); // band 5: 1.5 kHz
            eq.setParameterValue (EqProcessor::bandParameter (4, "freq"), 1000.0f);
        }
        for (int i = 0; i < 64; ++i)
        {
            const auto x = 0.25f * (float)std::sin (phase);
            phase += 2.0 * M_PI * 1000.0 / rate;
            buffer.setSample (0, i, x);
            buffer.setSample (1, i, x);
        }
        eq.processBlock (buffer, midi);
        for (int i = 0; i < 64; ++i)
            out.push_back (buffer.getSample (0, i));
    }
    // Before: unchanged; after: 12 dB up (0.25 to 1.0); in between no step bigger than the sine's own.
    auto peakIn = [&] (double from, double to)
    {
        float p = 0.0f;
        for (auto i = (size_t)(from * rate); i < (size_t)(to * rate); ++i)
            p = juce::jmax (p, std::abs (out[i]));
        return p;
    };
    CHECK (peakIn (0.1, 0.2) == Catch::Approx (0.25f).margin (0.005));
    CHECK (peakIn (0.45, 0.6) == Catch::Approx (1.0f).margin (0.03));
    float biggestStep = 0.0f;
    for (size_t i = 1; i < out.size(); ++i)
        biggestStep = juce::jmax (biggestStep, std::abs (out[i] - out[i - 1]));
    CHECK (biggestStep < 1.0f * (float)(2.0 * M_PI * 1000.0 / rate) * 1.2f);
}

TEST_CASE ("Any block size gives the same sound", "[dsp][eq]")
{
    EqSettings s;
    s.bands[3] = band (EqBand::Type::bell, 500.0f, -8.0f, 2.0f);
    s.bands[7] = band (EqBand::Type::highCut, 5000.0f, 0.0f, 0.9f, 2);
    auto render = [&] (int blockSize)
    {
        EqEngine engine;
        engine.prepare (44100.0, blockSize);
        juce::Random random (3);
        std::vector<float> out;
        juce::AudioBuffer<float> buffer (2, blockSize);
        for (int done = 0; done < 8192; done += blockSize)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const auto x = random.nextFloat() - 0.5f;
                buffer.setSample (0, i, x);
                buffer.setSample (1, i, x);
            }
            engine.process (buffer, s);
            for (int i = 0; i < blockSize; ++i)
                out.push_back (buffer.getSample (0, i));
        }
        out.resize (8192);
        return out;
    };
    const auto reference = render (512);
    for (int size : { 1, 7, 64, 1024 })
    {
        const auto other = render (size);
        float worst = 0.0f;
        for (size_t i = 0; i < reference.size(); ++i)
            worst = juce::jmax (worst, std::abs (reference[i] - other[i]));
        CHECK (worst < 1.0e-6f);
    }
}

TEST_CASE ("The EQ's state holds its bands", "[dsp][eq]")
{
    EqProcessor a;
    a.setParameterValue (EqProcessor::bandParameter (2, "type"), 5.0f);
    a.setParameterValue (EqProcessor::bandParameter (2, "freq"), 3150.0f);
    a.setParameterValue (EqProcessor::bandParameter (2, "q"), 4.0f);
    a.setParameterValue (EqProcessor::bandParameter (7, "on"), 1.0f);
    a.setParameterValue (EqProcessor::bandParameter (7, "slope"), 2.0f);
    a.setParameterValue ("output", -3.5f);
    juce::MemoryBlock state;
    a.getStateInformation (state);

    EqProcessor b;
    b.setStateInformation (state.getData(), (int)state.getSize());
    const auto s = b.getSettings();
    CHECK (s.bands[2].type == EqBand::Type::notch);
    CHECK (s.bands[2].frequencyHz == Catch::Approx (3150.0f).epsilon (0.01));
    CHECK (s.bands[2].q == Catch::Approx (4.0f).epsilon (0.01));
    CHECK (s.bands[7].on);
    CHECK (s.bands[7].slope == 2);
    CHECK (s.outputDb == Catch::Approx (-3.5f).margin (0.05));

    // Settings the Automixer writes as text.
    auto* frequency = b.getParameterState().getParameter (EqProcessor::bandParameter (1, "freq"));
    CHECK (frequency->convertFrom0to1 (frequency->getValueForText ("1.5 kHz")) ==
           Catch::Approx (1500.0f).epsilon (0.01));
    auto* type = b.getParameterState().getParameter (EqProcessor::bandParameter (1, "type"));
    CHECK (juce::roundToInt (type->convertFrom0to1 (type->getValueForText ("High shelf"))) == 2);
}

TEST_CASE ("The EQ window drags bands and shows the spectrum", "[dsp][eq][ui]")
{
    EqProcessor eq;
    eq.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor (eq.createEditorAndMakeActive());
    auto* window = dynamic_cast<EqEditor*> (editor.get());
    REQUIRE (window != nullptr);

    // Band 1 starts off; dragging it turns it on and moves it.
    const auto target = juce::Point<float> (window->getBandPosition (0).x + 60.0f, window->getBandPosition (0).y);
    window->dragBandTo (0, target);
    const auto s = eq.getSettings();
    CHECK (s.bands[0].on);
    CHECK (s.bands[0].frequencyHz == Catch::Approx (window->frequencyAt (target.x)).epsilon (0.01));
    // A bell moves in frequency and gain.
    window->dragBandTo (3, { window->getBandPosition (3).x, (float)window->getDisplayArea().getY() + 20.0f });
    CHECK (eq.getSettings().bands[3].gainDb > 15.0f);
    CHECK (window->bandAt (window->getBandPosition (3)) == 3);
    window->selectBand (5);
    CHECK (window->getSelectedBand() == 5);

    // The audio thread hands the output to the window while it is open.
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    for (int i = 0; i < 512; ++i)
        for (int c = 0; c < 2; ++c)
            buffer.setSample (c, i, 0.5f * (float)std::sin (i * 0.1));
    eq.processBlock (buffer, midi);
    std::array<float, 1024> read {};
    CHECK (eq.getEngine().readSpectrumSamples (read.data(), (int)read.size()) == 512);
    eq.editorBeingDeleted (editor.get());
}

TEST_CASE ("EQ window screenshot for the manual", "[.screenshot][eq]")
{
    EqProcessor eq;
    eq.setParameterValue (EqProcessor::bandParameter (0, "on"), 1.0f);
    eq.setParameterValue (EqProcessor::bandParameter (0, "freq"), 40.0f);
    eq.setParameterValue (EqProcessor::bandParameter (2, "gain"), -4.5f);
    eq.setParameterValue (EqProcessor::bandParameter (2, "freq"), 320.0f);
    eq.setParameterValue (EqProcessor::bandParameter (2, "q"), 1.8f);
    eq.setParameterValue (EqProcessor::bandParameter (5, "gain"), 3.0f);
    eq.setParameterValue (EqProcessor::bandParameter (5, "freq"), 3500.0f);
    eq.setParameterValue (EqProcessor::bandParameter (6, "gain"), 2.5f);
    eq.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor (eq.createEditorAndMakeActive());
    editor->setSize (760, 470);
    auto* window = dynamic_cast<EqEditor*> (editor.get());
    REQUIRE (window != nullptr);
    window->selectBand (2);

    // A spectrum to show behind the curve: pink-ish noise with a few tones, as a mix would look.
    juce::Random random (11);
    std::vector<float> sound (8192);
    float pink = 0.0f;
    for (size_t i = 0; i < sound.size(); ++i)
    {
        pink = 0.97f * pink + 0.03f * (random.nextFloat() * 2.0f - 1.0f);
        sound[i] = pink * 2.0f + 0.2f * (float)std::sin ((double)i * 2.0 * M_PI * 110.0 / 48000.0) +
                   0.05f * (float)std::sin ((double)i * 2.0 * M_PI * 2200.0 / 48000.0);
    }
    window->showSpectrumOf (sound.data(), (int)sound.size());
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
    const auto file = juce::File (SPINUS_TEST_FILES_DIR).getParentDirectory().getChildFile ("docs/user/images/eq.png");
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    eq.editorBeingDeleted (editor.get());
}

TEST_CASE ("EQ CPU cost with eight bands at 48 kHz with 128 samples in stereo", "[.bench][eq]")
{
    EqEngine engine;
    engine.prepare (48000.0, 128);
    EqSettings s;
    for (int b = 0; b < EqSettings::numBands; ++b)
        s.bands[(size_t)b] = band (EqBand::Type::bell, 100.0f * (float)(b + 1), 3.0f, 1.0f);
    s.bands[0] = band (EqBand::Type::lowCut, 30.0f, 0.0f, 0.71f, 2);
    juce::AudioBuffer<float> buffer (2, 128);
    buffer.clear();
    const int blocks = (int)(5.0 * 48000.0 / 128);
    const auto start = juce::Time::getHighResolutionTicks();
    for (int i = 0; i < blocks; ++i)
        engine.process (buffer, s);
    const auto seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
    std::cout << "EQ: " << seconds / 5.0 * 100.0 << " % of one core\n";
}
