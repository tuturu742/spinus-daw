#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "session/StemAutomation.h"

#include <tracktion_engine/tracktion_engine.h>

using namespace spinus;

namespace
{
constexpr double rate = 8000.0;

/** 32 beats at 120 BPM from 0.5 s, the first downbeat on the first. */
std::vector<double> beats()
{
    std::vector<double> times;
    times.reserve (33);
    for (int i = 0; i <= 32; ++i)
        times.push_back (0.5 + 0.5 * i);
    return times;
}

/** A 220 Hz tone over the beats, each beat's gain (left, right) as `gains` says. */
juce::AudioBuffer<float> tone (const std::function<std::pair<float, float> (int beat)>& gains)
{
    juce::AudioBuffer<float> buffer (2, (int)(17.0 * rate));
    buffer.clear();
    for (int s = (int)(0.5 * rate); s < buffer.getNumSamples(); ++s)
    {
        const auto beat = std::min (31, (int)(((double)s / rate - 0.5) / 0.5));
        const auto [left, right] = gains (beat);
        const auto value = (float)std::sin (juce::MathConstants<double>::twoPi * 220.0 * s / rate);
        buffer.setSample (0, s, value * left);
        buffer.setSample (1, s, value * right);
    }
    return buffer;
}
} // namespace

TEST_CASE ("Ticks follow the song's own beats", "[analyzer][stemautomation]")
{
    const std::vector<double> times { 1.0, 1.5, 2.0, 2.6 };
    CHECK (stemautomation::toTicks (1.5, times, 1.5) == Catch::Approx (0.0));
    CHECK (stemautomation::toTicks (2.3, times, 1.5) == Catch::Approx (960.0 * 1.5));
    CHECK (stemautomation::toTicks (0.75, times, 1.5) == Catch::Approx (-960.0 * 1.5));
    CHECK (stemautomation::toTicks (2.9, times, 1.5) == Catch::Approx (960.0 * 2.5));
    CHECK (stemautomation::toTicks (2.0, {}, 1.0) == Catch::Approx (1920.0)); // 120 BPM without beats
}

TEST_CASE ("A stem's fades and moves become volume and pan curves", "[analyzer][stemautomation]")
{
    // Fades in over the first 8 beats, then steady; moves from the left to the right over the last 16.
    const auto stem = tone (
        [] (int beat)
        {
            const auto level = beat < 8 ? std::pow (10.0f, (-24.0f + 3.0f * (float)beat) / 20.0f) : 1.0f;
            const auto place = beat < 16 ? 0.0f : (float)(beat - 16) / 15.0f; // 0 left .. 1 right
            return std::pair { level * std::cos (place * juce::MathConstants<float>::halfPi),
                               level * std::sin (place * juce::MathConstants<float>::halfPi) };
        });
    const auto curves = stemautomation::measure (stem, rate, beats(), 0.5, -3.0f);

    REQUIRE (curves.volume.size() >= 2);
    CHECK (curves.volume.size() < 12); // a few points, not one a beat
    const auto loud = tracktion::decibelsToVolumeFaderPosition (-3.0f);
    CHECK (curves.volume.front().value < tracktion::decibelsToVolumeFaderPosition (-15.0f)); // starts quiet
    CHECK (curves.volume.back().value == Catch::Approx (loud).margin (0.02)); // ends at the channel's volume
    for (size_t i = 1; i < curves.volume.size(); ++i)
        CHECK (curves.volume[i].time > curves.volume[i - 1].time);

    REQUIRE (curves.pan.size() >= 2);
    CHECK (curves.pan.front().value < 0.1f); // left
    CHECK (curves.pan.back().value > 0.85f); // right
    CHECK_FALSE (curves.steadyPan.has_value());
}

TEST_CASE ("A steady stem gets no curves, one off the centre a fixed pan", "[analyzer][stemautomation]")
{
    const auto centred =
        stemautomation::measure (tone ([] (int) { return std::pair { 0.5f, 0.5f }; }), rate, beats(), 0.5);
    CHECK (centred.volume.empty());
    CHECK (centred.pan.empty());
    CHECK_FALSE (centred.steadyPan.has_value());

    // Mostly on the right, with a beat of silence now and then (not a drop to automate).
    const auto right = stemautomation::measure (
        tone ([] (int beat) { return beat % 8 == 7 ? std::pair { 0.0f, 0.0f } : std::pair { 0.2f, 0.6f }; }), rate,
        beats(), 0.5);
    CHECK (right.volume.empty());
    CHECK (right.pan.empty());
    REQUIRE (right.steadyPan.has_value());
    CHECK (right.steadyPan.value() > 0.5f);

    CHECK (stemautomation::measure ({}, rate, beats(), 0.5).volume.empty());
}

namespace
{
/** A saw over the beats at each beat's frequency, through a two-pole low pass at each beat's cutoff (0: open). */
juce::AudioBuffer<float> saw (const std::function<std::pair<double, double> (int beat)>& hertzAndCutoff)
{
    juce::AudioBuffer<float> buffer (1, (int)(17.0 * rate));
    buffer.clear();
    double phase = 0.0, low = 0.0, lower = 0.0;
    for (int s = (int)(0.5 * rate); s < buffer.getNumSamples(); ++s)
    {
        const auto beat = std::min (31, (int)(((double)s / rate - 0.5) / 0.5));
        const auto [hertz, cutoff] = hertzAndCutoff (beat);
        phase += hertz / rate;
        phase -= std::floor (phase);
        const auto x = 0.5 * (2.0 * phase - 1.0);
        const auto k = cutoff > 0.0 ? 1.0 - std::exp (-juce::MathConstants<double>::twoPi * cutoff / rate) : 1.0;
        low += k * (x - low);
        lower += k * (low - lower);
        buffer.setSample (0, s, (float)lower);
    }
    return buffer;
}

/** One note a beat, at each beat's pitch, in ticks from the first downbeat. */
std::vector<model::Note> notesOf (const std::function<int (int beat)>& pitch)
{
    std::vector<model::Note> notes;
    notes.reserve (32);
    for (int beat = 0; beat < 32; ++beat)
    {
        model::Note n;
        n.start = (model::Ticks)beat * model::ticksPerQuarterNote;
        n.length = model::ticksPerQuarterNote;
        n.pitch = pitch (beat);
        notes.push_back (n);
    }
    return notes;
}
} // namespace

TEST_CASE ("A stem whose tone opens gets a filter's cutoff curve", "[analyzer][stemautomation]")
{
    // Muffled for the first 16 beats (a low pass at 150 Hz), then open, on the same A3.
    const auto stem = saw ([] (int beat) { return std::pair { 220.0, beat < 16 ? 150.0 : 0.0 }; });
    const auto curves = stemautomation::measure (stem, rate, beats(), 0.5, 0.0f, notesOf ([] (int) { return 57; }));
    REQUIRE (curves.cutoff.size() >= 2);
    CHECK (curves.cutoff.size() < 12);
    CHECK (curves.cutoff.front().value < stemautomation::cutoffSetting (3000.0)); // closed at first
    CHECK (curves.cutoff.back().value == Catch::Approx (1.0f));                   // open at the end
    for (size_t i = 1; i < curves.cutoff.size(); ++i)
        CHECK (curves.cutoff[i].time > curves.cutoff[i - 1].time);

    // Without notes its tone can't be told from its pitch: no curve.
    CHECK (stemautomation::measure (stem, rate, beats(), 0.5).cutoff.empty());
}

TEST_CASE ("A stem that only plays higher gets no cutoff curve", "[analyzer][stemautomation]")
{
    // Low notes, then two octaves higher: brighter in hertz, the same tone for the notes it plays (its first four
    // harmonics, as loud at every pitch).
    juce::AudioBuffer<float> stem (1, (int)(17.0 * rate));
    stem.clear();
    for (int s = (int)(0.5 * rate); s < stem.getNumSamples(); ++s)
    {
        const auto beat = std::min (31, (int)(((double)s / rate - 0.5) / 0.5));
        const auto hertz = beat < 16 ? 110.0 : 440.0;
        double x = 0.0;
        for (int h = 1; h <= 4; ++h)
            x += std::sin (juce::MathConstants<double>::twoPi * hertz * h * s / rate) / h;
        stem.setSample (0, s, (float)(0.3 * x));
    }
    const auto curves = stemautomation::measure (stem, rate, beats(), 0.5, 0.0f,
                                                 notesOf ([] (int beat) { return beat < 16 ? 45 : 69; }));
    CHECK (curves.cutoff.empty());
    CHECK (stemautomation::cutoffSetting (1000.0) == Catch::Approx (0.5f).margin (0.001));
    CHECK (stemautomation::cutoffSetting (20000.0) == Catch::Approx (1.0f));
}
