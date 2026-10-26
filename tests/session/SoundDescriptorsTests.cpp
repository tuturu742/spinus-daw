#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "session/SoundDescriptors.h"

#include <cmath>
#include <random>

using namespace spinus;
using namespace spinus::sounds;
using Catch::Approx;

namespace
{
constexpr double rate = 48000.0;
constexpr double pi = 3.14159265358979323846;

juce::AudioBuffer<float> make (double seconds, const std::function<double (double)>& f)
{
    juce::AudioBuffer<float> b (1, (int)(seconds * rate));
    for (int i = 0; i < b.getNumSamples(); ++i)
        b.setSample (0, i, (float)f (i / rate));
    return b;
}

Descriptors describeBuffer (const juce::AudioBuffer<float>& b, double held)
{
    return describe (b.getReadPointer (0), b.getNumSamples(), rate, held);
}

/** A file name for the name-reading code (nothing is written there). */
juce::File fileAt (const juce::String& path)
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (path);
}

double frequencyOf (int note) { return 440.0 * std::pow (2.0, (note - 69) / 12.0); }

/** A naive saw's next value. */
double saw (double phase) { return 2.0 * (phase - std::floor (phase)) - 1.0; }

/** Band-limited noise: white noise through a high pass and a low pass (0: none). */
juce::AudioBuffer<float> noise (double seconds, double highPass, double lowPass,
                                const std::function<double (double)>& gain, unsigned seed = 1)
{
    std::mt19937 random (seed);
    std::uniform_real_distribution<float> uniform (-1.0f, 1.0f);
    juce::AudioBuffer<float> b (1, (int)(seconds * rate));
    juce::IIRFilter hp1, hp2, lp1, lp2;
    if (highPass > 0)
    {
        hp1.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, highPass));
        hp2.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, highPass));
    }
    if (lowPass > 0)
    {
        lp1.setCoefficients (juce::IIRCoefficients::makeLowPass (rate, lowPass));
        lp2.setCoefficients (juce::IIRCoefficients::makeLowPass (rate, lowPass));
    }
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        auto v = uniform (random);
        if (highPass > 0)
            v = hp2.processSingleSampleRaw (hp1.processSingleSampleRaw (v));
        if (lowPass > 0)
            v = lp2.processSingleSampleRaw (lp1.processSingleSampleRaw (v));
        b.setSample (0, i, v * (float)gain (i / rate));
    }
    return b;
}

//==============================================================================
// Synthetic drums (one-shots).

juce::AudioBuffer<float> kick()
{
    double phase = 0.0;
    return make (1.0,
                 [&] (double t)
                 {
                     phase += (50.0 + 100.0 * std::exp (-t / 0.03)) / rate;
                     return 0.9 * std::sin (2 * pi * phase) * std::exp (-t / 0.15);
                 });
}

juce::AudioBuffer<float> snare()
{
    auto b = noise (0.6, 1000.0, 6000.0, [] (double t) { return 1.5 * std::exp (-t / 0.07); });
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        b.addSample (0, i, (float)(0.25 * std::sin (2 * pi * 180.0 * t) * std::exp (-t / 0.05)));
    }
    return b;
}

juce::AudioBuffer<float> clap()
{
    return noise (0.6, 900.0, 3500.0,
                  [] (double t)
                  {
                      // Three bursts 12 ms apart, then a short tail.
                      double g = 0.0;
                      for (auto start : { 0.0, 0.012, 0.024 })
                          if (t >= start)
                              g = std::max (g, std::exp (-(t - start) / 0.003));
                      if (t >= 0.024)
                          g = std::max (g, 0.6 * std::exp (-(t - 0.024) / 0.07));
                      return 2.0 * g;
                  });
}

juce::AudioBuffer<float> hat()
{
    return noise (0.4, 7000.0, 0.0, [] (double t) { return 1.5 * std::exp (-t / 0.03); });
}

juce::AudioBuffer<float> cymbal()
{
    return noise (3.0, 5000.0, 0.0, [] (double t) { return 1.2 * std::exp (-t / 0.5); });
}

juce::AudioBuffer<float> tom()
{
    double phase = 0.0;
    return make (1.0,
                 [&] (double t)
                 {
                     phase += (150.0 + 10.0 * std::exp (-t / 0.05)) / rate;
                     return 0.8 * std::sin (2 * pi * phase) * std::exp (-t / 0.12);
                 });
}

juce::AudioBuffer<float> wood()
{
    return make (0.3, [] (double t) { return 0.8 * std::sin (2 * pi * 900.0 * t) * std::exp (-t / 0.02); });
}

juce::AudioBuffer<float> bassNote()
{
    return make (1.5,
                 [] (double t)
                 {
                     const auto g = t < 1.0 ? 1.0 : std::max (0.0, 1.0 - (t - 1.0) / 0.1);
                     return 0.5 * saw (55.0 * t) * g;
                 });
}

juce::String classOf (const juce::AudioBuffer<float>& b)
{
    return describeSample (b, rate, fileAt ("samples/pack/a1.wav")).sampleClass;
}

//==============================================================================
// Fake instruments.

struct Voice
{
    double attack = 0.005, decayTime = 0.0, release = 0.05; // decayTime > 0: exp decay while held
    double sustain = 1.0;
    std::function<double (double phase)> wave = [] (double p) { return std::sin (2 * pi * p); };
    bool mono = false;             // one note at a time: the last one
    int lowest = 0, highest = 127; // notes outside play nothing
};

Player fake (const Voice& v)
{
    return [v] (const std::vector<Note>& notes, double tail)
    {
        double end = 0.0;
        for (auto& n : notes)
            end = std::max (end, n.start + n.length);
        juce::AudioBuffer<float> b (1, (int)((end + tail) * rate));
        b.clear();
        std::vector<Note> playing = notes;
        if (v.mono && ! playing.empty())
            playing = { playing.back() };
        for (auto& n : playing)
        {
            if (n.note < v.lowest || n.note > v.highest)
                continue;
            const auto f = frequencyOf (n.note);
            double releasedAt = -1.0;
            for (int i = (int)(n.start * rate); i < b.getNumSamples(); ++i)
            {
                const auto t = i / rate - n.start;
                double g = t < v.attack ? t / v.attack : 1.0;
                if (v.decayTime > 0.0)
                    g *= std::exp (-std::max (0.0, t - v.attack) / v.decayTime);
                else if (t >= v.attack)
                    g = v.sustain;
                if (t >= n.length)
                {
                    if (releasedAt < 0)
                        releasedAt = g;
                    g = releasedAt * std::max (0.0, 1.0 - (t - n.length) / v.release);
                }
                b.addSample (0, i, (float)(0.3 * v.wave (f * t) * g));
            }
        }
        return b;
    };
}
} // namespace

//==============================================================================
TEST_CASE ("Descriptors: the envelope of a held note", "[sounds]")
{
    // 50 ms rising, held for a second, 200 ms falling.
    const auto b = make (2.0,
                         [] (double t)
                         {
                             const auto g = t < 0.05 ? t / 0.05 : t < 1.0 ? 1.0 : std::max (0.0, 1.0 - (t - 1.0) / 0.2);
                             return 0.5 * std::sin (2 * pi * 440.0 * t) * g;
                         });
    const auto d = describeBuffer (b, 1.0);
    CHECK (d.attack == Approx (0.045).margin (0.012));
    CHECK (d.sustain == Approx (1.0).margin (0.05));
    CHECK (d.release == Approx (0.2).margin (0.03));
    CHECK (d.length == Approx (1.2).margin (0.04));
    CHECK (d.peakDb == Approx (-9.0).margin (0.5)); // RMS of a 0.5 sine
    CHECK (d.fundamental == Approx (440.0).margin (2.0));
    CHECK (d.pitched > 0.9);
    CHECK (d.harmonicBrightness == Approx (1.0).margin (0.1));
    CHECK (d.noisiness < 0.1);
}

TEST_CASE ("Descriptors: a pluck decays while held", "[sounds]")
{
    // 10 dB under the peak after 0.1 * ln(10^0.5) = 0.115 s.
    const auto b = make (1.5, [] (double t) { return 0.5 * saw (220.0 * t) * std::exp (-t / 0.1); });
    const auto d = describeBuffer (b, 1.0);
    CHECK (d.attack < 0.01);
    CHECK (d.decay == Approx (0.115).margin (0.015));
    CHECK (d.sustain < 0.01);
    CHECK (d.fundamental == Approx (220.0).margin (2.0));
}

TEST_CASE ("Descriptors: brightness, noise and pitch", "[sounds]")
{
    const auto sine = describeBuffer (make (1.0, [] (double t) { return 0.5 * std::sin (2 * pi * 1000.0 * t); }), 0.0);
    CHECK (sine.brightness == Approx (1000.0).margin (30.0));

    // The same saw, bright and through a low pass at 500 Hz.
    auto bright = make (1.0, [] (double t) { return 0.5 * saw (110.0 * t); });
    auto dark = bright;
    juce::IIRFilter lp1, lp2;
    lp1.setCoefficients (juce::IIRCoefficients::makeLowPass (rate, 500.0));
    lp2.setCoefficients (juce::IIRCoefficients::makeLowPass (rate, 500.0));
    lp1.processSamples (dark.getWritePointer (0), dark.getNumSamples());
    lp2.processSamples (dark.getWritePointer (0), dark.getNumSamples());
    const auto b = describeBuffer (bright, 0.0), d = describeBuffer (dark, 0.0);
    CHECK (b.brightness > 2.0 * d.brightness);
    CHECK (b.fundamental == Approx (110.0).margin (1.5));
    CHECK (d.fundamental == Approx (110.0).margin (1.5));
    CHECK (b.harmonicBrightness > 2.5);
    CHECK (d.harmonicBrightness < 2.5);

    const auto white = describeBuffer (noise (1.0, 0.0, 0.0, [] (double) { return 0.5; }), 0.0);
    CHECK (white.noisiness > 0.8);
    CHECK (white.pitched < 0.5);
    CHECK (white.fundamental <= 0.0);
    // Noise in a band is as noisy.
    const auto band = describeBuffer (noise (1.0, 2000.0, 4000.0, [] (double) { return 0.5; }), 0.0);
    CHECK (band.noisiness > 0.7);

    const auto low = describeBuffer (make (1.0, [] (double t) { return 0.5 * saw (41.2 * t); }), 0.0);
    CHECK (low.fundamental == Approx (41.2).margin (1.0));

    const auto silent = describeBuffer (make (1.0, [] (double) { return 0.0; }), 0.0);
    CHECK (silent.peakDb < -80.0);
    CHECK (silent.length <= 0.0);
}

TEST_CASE ("Descriptors: they survive JSON", "[sounds]")
{
    Descriptors d;
    d.attack = 0.0123;
    d.brightness = 1234.4;
    d.noisiness = 0.5;
    d.fundamental = 220.04;
    d.peakDb = -12.34;
    const auto back = descriptorsFromVar (juce::JSON::parse (juce::JSON::toString (toVar (d))));
    CHECK (back.attack == Approx (0.012));
    CHECK (back.brightness == Approx (1234.0));
    CHECK (back.noisiness == Approx (0.5));
    CHECK (back.fundamental == Approx (220.0));
    CHECK (back.peakDb == Approx (-12.3));
}

//==============================================================================
TEST_CASE ("Instruments: roles from the test notes", "[sounds]")
{
    SECTION ("a pad: slow to start, chords, long release")
    {
        Voice v;
        v.attack = 0.4;
        v.release = 0.8;
        const auto s = describeInstrument (fake (v), rate, "Untitled 3", "");
        CHECK_FALSE (s.silent);
        CHECK (s.tracksPitch);
        CHECK (s.polyphonic);
        CHECK (s.low == 24);
        CHECK (s.high == 96);
        CHECK (s.sound.attack == Approx (0.36).margin (0.05));
        CHECK (s.role == "pad");
        CHECK (s.roleFrom == "sound");
    }
    SECTION ("a pluck")
    {
        Voice v;
        v.decayTime = 0.08;
        v.wave = saw;
        const auto s = describeInstrument (fake (v), rate, "A", "");
        CHECK (s.tracksPitch);
        CHECK (s.polyphonic);
        CHECK (s.role == "pluck");
    }
    SECTION ("keys: a slower decay, chords")
    {
        Voice v;
        v.decayTime = 0.6;
        v.wave = [] (double p) { return std::sin (2 * pi * p) + 0.5 * std::sin (4 * pi * p); };
        const auto s = describeInstrument (fake (v), rate, "B", "");
        CHECK (s.polyphonic);
        CHECK (s.role == "keys");
    }
    SECTION ("a lead: one note at a time, bright and held")
    {
        Voice v;
        v.mono = true;
        v.wave = saw;
        const auto s = describeInstrument (fake (v), rate, "C", "");
        CHECK (s.tracksPitch);
        CHECK_FALSE (s.polyphonic);
        CHECK (s.role == "lead");
    }
    SECTION ("a bass: one note at a time, dark, only low notes")
    {
        Voice v;
        v.mono = true;
        v.highest = 60;
        const auto s = describeInstrument (fake (v), rate, "D", "");
        CHECK (s.tracksPitch);
        CHECK_FALSE (s.polyphonic);
        CHECK (s.low == 24);
        CHECK (s.high == 60);
        CHECK (s.role == "bass");
    }
    SECTION ("the name or category comes first")
    {
        Voice v;
        v.attack = 0.4;
        v.release = 0.8;
        const auto s = describeInstrument (fake (v), rate, "Deep Thing", "Basses");
        CHECK (s.role == "bass");
        CHECK (s.roleFrom == "name");
        CHECK (s.soundRole == "pad");
    }
    SECTION ("a silent preset")
    {
        Voice v;
        v.lowest = 128;
        const auto s = describeInstrument (fake (v), rate, "Init", "");
        CHECK (s.silent);
        CHECK (s.role == "none");
    }
}

TEST_CASE ("Instruments: a drum kit is mapped key by key", "[sounds]")
{
    const Player kit = [] (const std::vector<Note>& notes, double tail)
    {
        double end = 0.0;
        for (auto& n : notes)
            end = std::max (end, n.start + n.length);
        juce::AudioBuffer<float> b (1, (int)((end + tail) * rate));
        b.clear();
        for (auto& n : notes)
        {
            const auto sound = n.note == 36   ? kick()
                               : n.note == 38 ? snare()
                               : n.note == 42 ? hat()
                                              : juce::AudioBuffer<float>();
            const auto at = (int)(n.start * rate);
            for (int i = 0; i < sound.getNumSamples() && at + i < b.getNumSamples(); ++i)
                b.addSample (0, at + i, sound.getSample (0, i));
        }
        return b;
    };
    const auto s = describeInstrument (kit, rate, "Kit 1", "");
    CHECK_FALSE (s.tracksPitch);
    REQUIRE (s.drumMap.size() == 3);
    CHECK (s.drumMap[0] == std::pair<int, juce::String> (36, "kick"));
    CHECK (s.drumMap[1] == std::pair<int, juce::String> (38, "snare"));
    CHECK (s.drumMap[2] == std::pair<int, juce::String> (42, "hat"));
    CHECK (s.role == "drums");
    const auto json = juce::JSON::toString (toVar (s));
    CHECK (json.contains ("drumMap"));
}

TEST_CASE ("Instruments: role words in names", "[sounds]")
{
    CHECK (roleFromName ("Fat Reese", "") == "bass");
    CHECK (roleFromName ("Anything", "Basses") == "bass");
    CHECK (roleFromName ("Warm Pad", "") == "pad");
    CHECK (roleFromName ("SyncLead", "") == "lead");
    CHECK (roleFromName ("EP Classic", "") == "keys");
    CHECK (roleFromName ("Harp Pluck", "") == "pluck");
    CHECK (roleFromName ("808 Kit", "Drums") == "drums");
    CHECK (roleFromName ("Riser FX", "") == "fx");
    CHECK (roleFromName ("Subtle Glass", "") == ""); // "sub" is a word of its own
    CHECK (roleFromName ("Bright Saw", "") == "");
}

//==============================================================================
TEST_CASE ("Samples: synthetic drums are classified by their sound", "[sounds]")
{
    CHECK (classOf (kick()) == "kick");
    CHECK (classOf (snare()) == "snare");
    CHECK (classOf (clap()) == "clap");
    CHECK (classOf (hat()) == "hat");
    CHECK (classOf (cymbal()) == "cymbal");
    CHECK (classOf (tom()) == "tom");
    CHECK (classOf (wood()) == "perc");
    CHECK (classOf (bassNote()) == "bass");

    const auto shape = measureHit (clap().getReadPointer (0), clap().getNumSamples(), rate);
    CHECK (shape.bursts >= 3);
    const auto k = kick();
    CHECK (measureHit (k.getReadPointer (0), k.getNumSamples(), rate).pitchDrop > 4.0);

    const auto info = describeSample (kick(), rate, fileAt ("samples/a1.wav"));
    CHECK (info.kind == "one-shot");
    CHECK (info.classFrom == "sound");
}

TEST_CASE ("Samples: names and folders name the class", "[sounds]")
{
    CHECK (classFromName (fileAt ("p/Kicks/001.wav")) == "kick");
    CHECK (classFromName (fileAt ("p/x/BD_Hard.wav")) == "kick");
    CHECK (classFromName (fileAt ("p/x/OpenHat 2.wav")) == "hat");
    CHECK (classFromName (fileAt ("p/x/Crash.wav")) == "cymbal");
    CHECK (classFromName (fileAt ("p/x/Shaker.wav")) == "perc");
    CHECK (classFromName (fileAt ("p/x/808 Long.wav")) == "bass");
    CHECK (classFromName (fileAt ("p/x/thing.wav")) == "");
    // A hat named so is a hat, whatever it sounds like (the sound's own guess is kept too).
    const auto named = describeSample (kick(), rate, fileAt ("p/Hats/a.wav"));
    CHECK (named.sampleClass == "hat");
    CHECK (named.classFrom == "name");
    CHECK (named.soundClass == "kick");
}

TEST_CASE ("Samples: loops get a tempo, bars and a key", "[sounds]")
{
    SECTION ("a drum loop of two bars at 120 BPM")
    {
        juce::AudioBuffer<float> loop (1, (int)(4.0 * rate));
        loop.clear();
        auto put = [&] (const juce::AudioBuffer<float>& hit, double at, float gain)
        {
            const auto start = (int)(at * rate);
            for (int i = 0; i < hit.getNumSamples() && start + i < loop.getNumSamples(); ++i)
                loop.addSample (0, start + i, gain * hit.getSample (0, i));
        };
        const auto k = kick(), s = snare(), h = hat();
        for (int beat = 0; beat < 8; ++beat)
        {
            put (beat % 2 == 0 ? k : s, beat * 0.5, 0.8f);
            put (h, beat * 0.5 + 0.25, 0.4f);
        }
        const auto info = describeSample (loop, rate, fileAt ("p/x/a.wav"));
        CHECK (info.kind == "loop");
        CHECK (info.sampleClass == "drums");
        CHECK (info.tempo == Approx (120.0));
        CHECK (info.tempoFrom == "length");
        CHECK (info.bars == Approx (2.0));
        CHECK (info.key.isEmpty());
    }
    SECTION ("an A minor arpeggio of two bars at 100 BPM")
    {
        const int notes[] = { 57, 60, 64, 69, 64, 60, 57, 64 }; // A C E A E C A E
        const auto eighth = 0.3;
        const auto loop = make (4.8,
                                [&] (double t)
                                {
                                    const auto i = (int)(t / eighth);
                                    const auto local = t - i * eighth;
                                    const auto f = frequencyOf (notes[i % 8]);
                                    const auto g = std::min (1.0, local / 0.005) * std::exp (-local / 0.15);
                                    return 0.4 * g * (std::sin (2 * pi * f * t) + 0.3 * std::sin (4 * pi * f * t));
                                });
        const auto info = describeSample (loop, rate, fileAt ("p/x/b.wav"));
        CHECK (info.kind == "loop");
        CHECK (info.sampleClass == "melodic");
        CHECK (info.tempo == Approx (100.0));
        CHECK (info.bars == Approx (2.0));
        CHECK (info.key == "A minor");
        CHECK (info.keyFrom == "sound");
    }
    SECTION ("tempo and key from the name")
    {
        CHECK (tempoFromName ("Loop_128bpm_Fm") == Approx (128.0));
        CHECK (tempoFromName ("90 BPM groove") == Approx (90.0));
        CHECK (tempoFromName ("bpm140 dnb") == Approx (140.0));
        CHECK (tempoFromName ("Kick 100") <= 0.0);
        CHECK (keyFromName ("Loop_128bpm_Fm") == "F minor");
        CHECK (keyFromName ("Chords C#min") == "C# minor");
        CHECK (keyFromName ("Pad Eb") == "D# major");
        CHECK (keyFromName ("Lead A minor 2") == "A minor");
        CHECK (keyFromName ("Loop B") == "");
        CHECK (keyFromName ("Drums") == "");
    }
}

TEST_CASE ("Samples: anything over a minute is not a sample", "[sounds]")
{
    juce::AudioBuffer<float> song (1, (int)(61.0 * 8000.0));
    song.clear();
    const auto info = describeSample (song, 8000.0, fileAt ("p/song.wav"));
    CHECK (info.kind == "long");
}
