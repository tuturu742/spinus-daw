#include <catch2/catch_test_macros.hpp>

#include "session/KeyProbe.h"

#include <cmath>

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
                addSine (b, keyprobe::frequencyOf (n.note), n.start, n.length, gain);
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
