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
    // The note moves where it starts and ends, not earlier (the detector looks 33 ms ahead of its moment).
    CHECK (std::abs (cents (hzAt (rendered, 0.43), 220.0)) < 5.0);
    CHECK (std::abs (cents (hzAt (rendered, 0.52), 246.94)) < 5.0);
    CHECK (std::abs (cents (hzAt (rendered, 1.15), 246.94)) < 5.0);
    CHECK (std::abs (cents (hzAt (rendered, 1.24), 220.0)) < 5.0);
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

TEST_CASE ("Spinus Tune renders a note's volume and formant, leaving its pitch and the rest alone",
           "[tune][engine][audio]")
{
    const juce::TemporaryFile in (".wav"), out (".wav");
    const auto voice = saw (1.4, 220.0);
    writeWav (in.getFile(), voice);

    TuneNote quieter { 0.1, 0.6, 57.0f };
    quieter.gainDb = -12.0f;
    TuneNote smaller { 0.7, 1.3, 57.0f };
    smaller.formant = 7.0f;
    const auto track = trackOf (1.4, [] (double) { return 57.0; });
    const auto result = TuneRender::render (in.getFile(), track, { quieter, smaller }, out.getFile());
    INFO (result.getErrorMessage());
    REQUIRE (result.wasOk());
    const auto rendered = readWav (out.getFile());
    REQUIRE (rendered.size() == voice.size());

    auto rms = [] (const std::vector<float>& x, double from, double to)
    {
        double sum = 0.0;
        const auto a = (size_t)(from * rate), b = (size_t)(to * rate);
        for (auto i = a; i < b; ++i)
            sum += (double)x[i] * x[i];
        return std::sqrt (sum / (double)(b - a));
    };
    // How bright: the level of the sample-to-sample change against the level itself.
    auto brightness = [] (const std::vector<float>& x, double from, double to)
    {
        double change = 0.0, level = 0.0;
        for (auto i = (size_t)(from * rate); i < (size_t)(to * rate); ++i)
        {
            change += (double)(x[i] - x[i - 1]) * (x[i] - x[i - 1]);
            level += (double)x[i] * x[i];
        }
        return std::sqrt (change / level);
    };

    // 12 dB quieter inside the first note, as loud before it.
    CHECK (juce::Decibels::gainToDecibels (rms (rendered, 0.25, 0.45) / rms (voice, 0.25, 0.45)) ==
           Catch::Approx (-12.0).margin (1.0));
    CHECK (juce::Decibels::gainToDecibels (rms (rendered, 0.02, 0.08) / rms (voice, 0.02, 0.08)) ==
           Catch::Approx (0.0).margin (1.0));

    // Brighter (the formant up) inside the second note, at the same pitch.
    CHECK (brightness (rendered, 0.85, 1.15) > 1.15 * brightness (voice, 0.85, 1.15));
    CHECK (std::abs (cents (hzAt (rendered, 1.0), 220.0)) < 10.0);
    CHECK (brightness (rendered, 0.3, 0.5) == Catch::Approx (brightness (voice, 0.3, 0.5)).epsilon (0.1));
}

TEST_CASE ("Spinus Tune's time map: a moved or stretched note, its neighbours in place", "[tune][engine]")
{
    TuneNote first { 0.5, 1.0, 57.0f }, second { 1.5, 2.0, 60.0f }, third { 2.0, 2.5, 62.0f };
    CHECK (TimeMap::of ({ first, second, third }, 4.0).isIdentity());

    first.move = 0.2;     // sounds 0.7..1.2
    second.stretch = 1.5; // sounds 1.5..2.25, though the third note touches it
    third.move = 0.3;     // sounds 2.3..2.8
    const auto map = TimeMap::of ({ first, second, third }, 4.0);
    REQUIRE (! map.isIdentity());
    CHECK (map.toOutput (0.0) == Catch::Approx (0.0));
    CHECK (map.toOutput (0.5) == Catch::Approx (0.7));
    CHECK (map.toOutput (0.75) == Catch::Approx (0.95));
    CHECK (map.toOutput (1.0) == Catch::Approx (1.2));
    CHECK (map.toOutput (1.5) == Catch::Approx (1.5));
    CHECK (map.toOutput (1.75) == Catch::Approx (1.875));
    CHECK (map.toOutput (2.25) == Catch::Approx (2.55));
    CHECK (map.toOutput (2.5) == Catch::Approx (2.8));
    CHECK (map.toOutput (4.0) == Catch::Approx (4.0));
    // Where the notes touch, a little of each is lent to fill the gap (2.25..2.3 as it sounds).
    CHECK (map.toOutput (1.98) == Catch::Approx (2.22));
    CHECK (map.toOutput (2.02) == Catch::Approx (2.32));
    for (const auto t : { 0.3, 0.8, 1.3, 1.9, 2.0, 2.1, 3.0 })
        CHECK (map.toSource (map.toOutput (t)) == Catch::Approx (t));

    // Never backwards, and the file keeps its length, even past its end.
    TuneNote late { 3.5, 3.9, 60.0f };
    late.move = 0.5;
    const auto clamped = TimeMap::of ({ late }, 4.0);
    for (size_t i = 1; i < clamped.points.size(); ++i)
        CHECK (clamped.points[i].second >= clamped.points[i - 1].second);
    CHECK (clamped.toOutput (4.0) == Catch::Approx (4.0));
}

TEST_CASE ("Spinus Tune renders notes moved in time and stretched, the rest in place", "[tune][engine][audio]")
{
    // Two sung notes in silence: A3 at 0.5..0.8 s and E4 at 1.2..1.5 s.
    std::vector<float> voice ((size_t)(2.0 * rate), 0.0f);
    const auto a = saw (0.3, 220.0), e = saw (0.3, 329.63);
    std::copy (a.begin(), a.end(), voice.begin() + (int64_t)(0.5 * rate));
    std::copy (e.begin(), e.end(), voice.begin() + (int64_t)(1.2 * rate));
    const juce::TemporaryFile in (".wav"), out (".wav");
    writeWav (in.getFile(), voice);

    TuneNote moved { 0.5, 0.8, 57.0f }, longer { 1.2, 1.5, 64.0f };
    moved.move = 0.2;     // sounds 0.7..1.0
    longer.stretch = 1.5; // sounds 1.2..1.65
    const auto track = trackOf (2.0, [] (double t) { return t < 1.0 ? 57.0 : 64.0; });
    const auto result = TuneRender::render (in.getFile(), track, { moved, longer }, out.getFile());
    INFO (result.getErrorMessage());
    REQUIRE (result.wasOk());
    const auto rendered = readWav (out.getFile());
    REQUIRE (rendered.size() == voice.size());

    // Where the sound starts and ends after a moment, in 5 ms steps.
    auto edge = [] (const std::vector<float>& x, double from, bool starts)
    {
        auto loud = [&] (double t)
        {
            double sum = 0.0;
            const auto first = (size_t)(t * rate);
            for (auto i = first; i < first + 240; ++i)
                sum += (double)x[i] * x[i];
            return std::sqrt (sum / 240.0) > 0.03;
        };
        for (auto t = from; t < 1.95; t += 0.005)
            if (loud (t) == starts)
                return t;
        return 2.0;
    };
    CHECK (edge (rendered, 0.3, true) == Catch::Approx (0.7).margin (0.01));
    CHECK (edge (rendered, 0.75, false) == Catch::Approx (1.0).margin (0.01));
    CHECK (edge (rendered, 1.05, true) == Catch::Approx (1.2).margin (0.01));
    CHECK (edge (rendered, 1.25, false) == Catch::Approx (1.65).margin (0.01));

    // Each at its own pitch.
    CHECK (std::abs (cents (hzAt (rendered, 0.85), 220.0)) < 10.0);
    CHECK (std::abs (cents (hzAt (rendered, 1.45), 329.63)) < 10.0);
}
