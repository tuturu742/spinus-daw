#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "dsp/tune/PitchDetector.h"
#include "engine/TuneRender.h"

#include <juce_audio_formats/juce_audio_formats.h>

using namespace spinus;
using namespace spinus::engine;
using spinus::model::TuneNote;

namespace
{
constexpr double rate = 48000.0;

/** A track of `seconds` with the pitch (MIDI) given by a function of time. */
PitchTrack trackOf (double seconds, const std::function<double (double)>& midiAt)
{
    PitchTrack track;
    for (int frame = 0; frame * track.frameSeconds < seconds; ++frame)
        track.hz.push_back ((float)(440.0 * std::pow (2.0, (midiAt (frame * track.frameSeconds) - 69.0) / 12.0)));
    return track;
}

std::vector<float> saw (double seconds, double hz)
{
    std::vector<float> x ((size_t)(seconds * rate));
    for (size_t i = 0; i < x.size(); ++i)
    {
        double v = 0.0;
        for (int h = 1; h * hz < rate * 0.4 && h < 30; ++h)
            v += std::sin (2.0 * M_PI * h * hz * (double)i / rate) / h;
        x[i] = (float)(0.3 * v);
    }
    return x;
}

juce::File writeWav (const juce::File& file, const std::vector<float>& x)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (
        stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    const float* channels[] = { x.data() };
    writer->writeFromFloatArrays (channels, 1, (int)x.size());
    return file;
}

std::vector<float> readWav (const juce::File& file)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (new juce::FileInputStream (file), true));
    REQUIRE (reader != nullptr);
    std::vector<float> x ((size_t)reader->lengthInSamples);
    float* channels[] = { x.data() };
    reader->read (channels, 1, 0, (int)x.size());
    return x;
}

double hzAt (const std::vector<float>& x, double seconds)
{
    dsp::PitchDetector detector;
    detector.prepare (rate, 60.0f, 1200.0f);
    const auto r = detector.analyse (x.data() + (size_t)(seconds * rate));
    return r.isVoiced() ? rate / r.period : 0.0;
}

double cents (double hz, double reference) { return 1200.0 * std::log2 (hz / reference); }
} // namespace

TEST_CASE ("Spinus Tune's corrections: shift, straightened drift and vibrato, eased at the edges", "[tune][engine]")
{
    // A steady A3: shifted up a tone inside the note, nothing outside it.
    const auto steady = trackOf (1.0, [] (double) { return 57.0; });
    const std::vector<TuneNote> moved { { 0.2, 0.8, 57.0f, 2.0f, 1.0f, 1.0f } };
    auto c = TuneRender::corrections (steady, moved, 1.0);
    CHECK (juce::approximatelyEqual (c[(size_t)(0.1 / 0.005)], 0.0f));
    CHECK (c[(size_t)(0.5 / 0.005)] == Catch::Approx (2.0f));
    CHECK (c[(size_t)(0.205 / 0.005)] < 1.0f); // eased in
    CHECK (juce::approximatelyEqual (c[(size_t)(0.9 / 0.005)], 0.0f));

    // Vibrato removed: the correction cancels the wobble, leaving its centre.
    const auto wobbly = trackOf (1.0, [] (double t) { return 57.0 + 0.4 * std::sin (2.0 * M_PI * 5.5 * t); });
    c = TuneRender::corrections (wobbly, { { 0.0, 1.0, 57.0f, 0.0f, 1.0f, 0.0f } }, 1.0);
    for (int step = 0; step < 30; ++step)
    {
        const auto t = 0.3 + step * 0.013;
        const auto f = (size_t)(t / 0.005);
        CHECK (std::abs (wobbly.pitchAt (t).value_or (0.0f) + c[f] - 57.0f) < 0.1f);
    }

    // A slow drift straightened onto the note.
    const auto drifting = trackOf (1.0, [] (double t) { return 56.6 + 0.8 * t; });
    c = TuneRender::corrections (drifting, { { 0.0, 1.0, 57.0f, 0.0f, 0.0f, 1.0f } }, 1.0);
    CHECK (std::abs (drifting.pitchAt (0.3).value_or (0.0f) + c[(size_t)(0.3 / 0.005)] - 57.0f) < 0.05f);
    CHECK (std::abs (drifting.pitchAt (0.7).value_or (0.0f) + c[(size_t)(0.7 / 0.005)] - 57.0f) < 0.05f);
}

TEST_CASE ("Spinus Tune renders a clip with its notes moved, in time and the same length", "[tune][engine][audio]")
{
    const juce::TemporaryFile in (".wav"), out (".wav");
    const auto voice = saw (1.4, 220.0);
    writeWav (in.getFile(), voice);

    const auto track = trackOf (1.4, [] (double) { return 57.0; });
    const std::vector<TuneNote> notes { { 0.5, 1.2, 57.0f, 2.0f, 1.0f, 1.0f } }; // A3 up to B3
    std::atomic<float> progress { 0.0f };
    const auto result = TuneRender::render (in.getFile(), track, notes, out.getFile(), &progress);
    INFO (result.getErrorMessage());
    REQUIRE (result.wasOk());
    CHECK (progress.load() == Catch::Approx (1.0f));

    const auto rendered = readWav (out.getFile());
    REQUIRE (rendered.size() == voice.size());
    CHECK (std::abs (cents (hzAt (rendered, 0.2), 220.0)) < 5.0);
    CHECK (std::abs (cents (hzAt (rendered, 0.9), 246.94)) < 5.0);

    // In time: before the note the output follows the input (no delay added).
    double best = 0.0;
    int bestLag = 0;
    for (int lag = -200; lag <= 200; ++lag)
    {
        double sum = 0.0;
        for (size_t i = 6000; i < 12000; ++i)
            sum += (double)rendered[i] * voice[(size_t)((int64_t)i + lag)];
        if (sum > best)
        {
            best = sum;
            bestLag = lag;
        }
    }
    CHECK (std::abs (bestLag) < 48); // within 1 ms
}
