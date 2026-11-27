#include <catch2/catch_test_macros.hpp>

#include "session/KeyProbe.h"

#include <cmath>
#include <set>

using namespace spinus;
using Job = InstrumentProfiles::Job;

namespace
{
/** A sampled guitar as probing meets it: keys 40 to 88 play in tune, 24 is a switch key (silent, then everything
    plays 12 dB quieter until it is put back), 30 strums a chord, 32 sounds a fixed note, and the rest are silent.
    `entered` is the mode's entry key: without it, nothing plays. */
struct FakeGuitar
{
    int enterKey = -1;
    int octaves = 0; // how many octaves from its keys it sounds (a bass written an octave up: -1)
    bool switched = false;
    int resets = 0;

    static void addSine (juce::AudioBuffer<float>& b, double hz, double from, double length, float gain)
    {
        const auto rate = 48000.0;
        for (int i = (int)(from * rate); i < juce::jmin (b.getNumSamples(), (int)((from + length) * rate)); ++i)
        {
            const auto v = gain * (float)std::sin (2.0 * juce::MathConstants<double>::pi * hz * (i / rate));
            for (int c = 0; c < b.getNumChannels(); ++c)
                b.addSample (c, i, v);
        }
    }

    juce::AudioBuffer<float> play (const std::vector<sounds::Note>& notes, double tail)
    {
        double end = 0.0;
        for (const auto& n : notes)
            end = juce::jmax (end, n.start + n.length);
        juce::AudioBuffer<float> b (2, (int)((end + tail) * 48000.0));
        b.clear();
        bool entered = enterKey < 0;
        for (const auto& n : notes)
        {
            if (n.note == enterKey)
            {
                entered = true;
                continue;
            }
            if (! entered)
                continue;
            const auto gain = switched ? 0.05f : 0.2f;
            if (n.note == 24)
                switched = true;
            else if (n.note >= 40 && n.note <= 88)
                addSine (b, keyprobe::frequencyOf (n.note + 12 * octaves), n.start, n.length, gain);
            else if (n.note == 30)
                for (const auto chordNote : { 48, 52, 55, 60 })
                    addSine (b, keyprobe::frequencyOf (chordNote), n.start, n.length, gain * 0.5f);
            else if (n.note == 32)
                addSine (b, keyprobe::frequencyOf (69), n.start, n.length, gain);
        }
        return b;
    }
};
} // namespace

TEST_CASE ("Listen to every key: playing keys, switch keys, special keys and silent ones", "[profiles]")
{
    FakeGuitar guitar;
    int last = 0;
    const auto result = keyprobe::probe ([&] (const auto& notes, double tail) { return guitar.play (notes, tail); },
                                         [&]
                                         {
                                             guitar.switched = false;
                                             ++guitar.resets;
                                         },
                                         {},
                                         [&] (int done, int)
                                         {
                                             last = done;
                                             return true;
                                         });
    INFO (result.problem);
    REQUIRE (result.reference == 60);
    CHECK (last == 128);
    for (int n = 40; n <= 88; ++n)
    {
        INFO (n);
        CHECK (result.keys[(size_t)n].job == Job::plays);
        CHECK (result.keys[(size_t)n].source == InstrumentProfiles::Source::probing);
    }
    CHECK (result.keys[24].job == Job::switches);
    CHECK (guitar.resets == 1); // put back after the switch
    CHECK (result.keys[30].job == Job::special);
    CHECK (result.keys[30].what == "plays a chord");
    CHECK (result.keys[32].job == Job::special);
    CHECK (result.keys[32].what == "sounds as A5");
    CHECK (result.keys[10].job == Job::unused);
    CHECK (result.keys[100].job == Job::unused);

    // Merged into a mode: what the user set stays theirs.
    InstrumentProfiles::Mode mode;
    mode.setKeys (40, 40, { Job::switches, "mine", InstrumentProfiles::Source::user });
    const auto changes = keyprobe::merge (mode, result.keys);
    CHECK (mode.keys[40].what == "mine");
    CHECK (mode.keys[41].job == Job::plays);
    CHECK (changes == 127);
    CHECK (keyprobe::merge (mode, result.keys) == 0);

    // A manual counts more than listening, the plugin's own names less than a manual and more than listening.
    using Source = InstrumentProfiles::Source;
    InstrumentProfiles::Mode ranked;
    ranked.setKeys (24, 24, { Job::unused, {}, Source::manual });
    ranked.setKeys (32, 32, { Job::plays, {}, Source::plugin });
    ranked.setKeys (41, 41, { Job::special, "from a shared file", Source::imported });
    keyprobe::merge (ranked, result.keys);
    CHECK (ranked.keys[24].job == Job::unused);           // the manual's word stays
    CHECK (ranked.keys[32].job == Job::plays);            // the plugin's too
    CHECK (ranked.keys[41].what == "from a shared file"); // and a shared file's
    CHECK (ranked.keys[30].source == Source::probing);    // keys nobody said anything about are as heard
    std::array<InstrumentProfiles::Key, 128> manual {};
    manual[32] = { Job::switches, "palm mute", Source::manual };
    manual[50] = { Job::switches, "slide", Source::manual };
    CHECK (keyprobe::merge (ranked, manual) == 2); // a manual read later replaces what was heard and named
    CHECK (ranked.keys[32].what == "palm mute");
    std::array<InstrumentProfiles::Key, 128> named {};
    named[32] = { Job::plays, {}, Source::plugin };
    CHECK (keyprobe::merge (ranked, named) == 0);
}

TEST_CASE ("Listen to every key: a mode entered by a key, and stopping", "[profiles]")
{
    FakeGuitar guitar;
    guitar.enterKey = 97;
    const auto play = [&] (const auto& notes, double tail) { return guitar.play (notes, tail); };
    // Without the entry key nothing plays: said so.
    const auto none = keyprobe::probe (play, {});
    CHECK (none.reference < 0);
    CHECK (none.problem == "It played no note in tune.");
    keyprobe::Options options;
    options.enterKey = 97;
    options.from = 50;
    options.to = 100;
    const auto heard = keyprobe::probe (play, {}, options, [] (int done, int) { return done < 20; });
    CHECK (heard.keys[50].job == Job::plays);
    CHECK (heard.keys[69].job == Job::plays);
    CHECK (heard.keys[70].job == Job::unknown); // stopped there
    CHECK (heard.keys[97].job == Job::unknown);
    options.from = 95;
    const auto rest = keyprobe::probe (play, {}, options);
    CHECK (rest.keys[97].job == Job::switches);
    CHECK (rest.keys[97].what == "enters this mode");
}

TEST_CASE ("Listen to every key: an instrument that sounds an octave under its keys says so", "[profiles]")
{
    for (const auto octaves : { 0, -1, 1 })
    {
        FakeGuitar guitar;
        guitar.octaves = octaves;
        const auto result = keyprobe::probe ([&] (const auto& notes, double tail) { return guitar.play (notes, tail); },
                                             [&] { guitar.switched = false; });
        CHECK (result.octaves == octaves);
        CHECK (result.keys[52].job == Job::plays); // an octave off still plays
    }
}

TEST_CASE ("Listen to every key: a drum kit, which plays no note in tune", "[profiles]")
{
    // A kit with drums on its GM keys only: a burst of noise each (no pitch), silence elsewhere. Its crash (49)
    // rings on for two seconds, whatever is played next: a drum sampler doesn't stop it.
    const std::set<int> drums { 36, 38, 42, 46, 49 };
    double ringLeft = 0.0;
    juce::Random random (7);
    const auto play = [&] (const std::vector<sounds::Note>& notes, double tail)
    {
        double end = 0.0;
        for (const auto& n : notes)
            end = juce::jmax (end, n.start + n.length);
        const auto rate = 48000.0;
        juce::AudioBuffer<float> b (2, (int)((end + tail) * rate));
        b.clear();
        const auto noise = [&] (int from, int to, float gain)
        {
            for (int i = juce::jmax (0, from); i < juce::jmin (b.getNumSamples(), to); ++i)
                for (int c = 0; c < 2; ++c)
                    b.addSample (c, i, gain * (random.nextFloat() * 2.0f - 1.0f));
        };
        noise (0, (int)(ringLeft * rate), 0.05f); // the crash still ringing from before
        ringLeft = juce::jmax (0.0, ringLeft - b.getNumSamples() / rate);
        for (const auto& n : notes)
            if (drums.count (n.note) != 0)
            {
                noise ((int)(n.start * rate), (int)((n.start + 0.15) * rate), 0.3f);
                if (n.note == 49)
                {
                    noise ((int)((n.start + 0.15) * rate), b.getNumSamples(), 0.05f);
                    ringLeft = 2.0 - (b.getNumSamples() / rate - n.start);
                }
            }
        return b;
    };
    const auto heard = keyprobe::probe (play, {});
    CHECK (heard.problem.isEmpty());
    CHECK (heard.reference == 36); // the first drum tried
    for (int key = 0; key < 128; ++key)
    {
        INFO ("key " << key);
        if (drums.count (key) != 0)
            CHECK (heard.keys[(size_t)key].job == Job::special);
        else
            CHECK (heard.keys[(size_t)key].job == Job::unused); // the keys after the crash too
    }
}
