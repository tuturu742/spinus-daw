#include "KeyProbe.h"

#include <cmath>
#include <numeric>

namespace spinus::keyprobe
{

namespace
{
using Job = InstrumentProfiles::Job;

constexpr double entry = 0.1; // the entry key's moment before each note, when the mode has one
constexpr float silentPeak = 1.0e-4f;

/** One channel: the left and right averaged. */
std::vector<float> monoOf (const juce::AudioBuffer<float>& audio)
{
    std::vector<float> mono ((size_t)audio.getNumSamples(), 0.0f);
    const auto channels = juce::jmax (1, audio.getNumChannels());
    for (int c = 0; c < audio.getNumChannels(); ++c)
        for (int i = 0; i < audio.getNumSamples(); ++i)
            mono[(size_t)i] += audio.getSample (c, i) / (float)channels;
    return mono;
}

/** What a sound is like, to tell the reference note's sound changed. */
struct Print
{
    float peak = 0.0f;
    double rmsDb = -120.0, centroid = 0.0;
    std::array<double, 12> classes {};

    static Print of (const std::vector<float>& x, double from, double to)
    {
        Print p;
        const auto rate = OfflineRate;
        const auto a = juce::jlimit (0, (int)x.size(), (int)(from * rate));
        const auto b = juce::jlimit (a, (int)x.size(), (int)(to * rate));
        double sum = 0.0;
        for (int i = a; i < b; ++i)
        {
            p.peak = juce::jmax (p.peak, std::abs (x[(size_t)i]));
            sum += (double)x[(size_t)i] * x[(size_t)i];
        }
        p.rmsDb = b > a ? 10.0 * std::log10 (juce::jmax (1.0e-12, sum / (b - a))) : -120.0;
        p.centroid = sounds::measureSpectrum (x.data(), (int)x.size(), rate, from, to).centroid;
        p.classes = sounds::chroma (x.data(), (int)x.size(), rate, from, to);
        return p;
    }

    /** How different two prints are: 1 is about a clear change (3 dB louder or quieter, a fifth brighter, or a
        different set of pitches). */
    double distance (const Print& o) const
    {
        double pitches = 0.0;
        for (size_t i = 0; i < 12; ++i)
            pitches += std::abs (classes[i] - o.classes[i]);
        const auto bright = centroid > 0.0 && o.centroid > 0.0 ? std::abs (std::log2 (centroid / o.centroid)) : 0.0;
        return juce::jmax (std::abs (rmsDb - o.rmsDb) / 3.0, bright / 0.58, pitches / 0.5);
    }

    static constexpr double OfflineRate = 48000.0;
};

std::vector<sounds::Note> notesFor (int key, const Options& options)
{
    std::vector<sounds::Note> notes;
    if (options.enterKey >= 0)
        notes.push_back ({ options.enterKey, 0.0, 0.05, 0.8f });
    notes.push_back ({ key, entry, options.hold, 0.8f });
    return notes;
}

/** A key that sounds: does it play at its pitch? */
InstrumentProfiles::Key heardAs (int key, const std::vector<float>& x, const Options& options)
{
    const auto rate = Print::OfflineRate;
    const auto from = entry + 0.03, to = entry + options.hold;
    const auto expected = frequencyOf (key);
    // In tune when an octave or a few off (the lowest keys are heard by their harmonics).
    const auto matches = [expected] (double hz, double mostOctaves)
    {
        const auto cents = 1200.0 * std::log2 (hz / expected);
        const auto octaves = std::round (cents / 1200.0);
        return std::abs (octaves) <= mostOctaves && std::abs (cents - 1200.0 * octaves) < 60.0;
    };
    const auto classes = sounds::chroma (x.data(), (int)x.size(), rate, from, to);
    const auto total = std::accumulate (classes.begin(), classes.end(), 0.0);
    const auto strong = std::count_if (classes.begin(), classes.end(), [] (double c) { return c > 0.12; });
    InstrumentProfiles::Key plays { Job::plays, {}, InstrumentProfiles::Source::probing };
    InstrumentProfiles::Key chord { Job::special, "plays a chord", InstrumentProfiles::Source::probing };
    double heardHz = 0.0;
    if (expected <= 2000.0)
    {
        heardHz = sounds::detectPitch (x.data(), (int)x.size(), rate, from, to).frequency;
        if (heardHz > 0.0 && matches (heardHz, expected < 30.0 ? 4.0 : 2.0))
            return plays;
    }
    if (expected > 1500.0) // too high for the pitch detector: its zero crossings, two a period for a plain tone
    {
        const auto a = juce::jlimit (0, (int)x.size(), (int)(from * rate));
        const auto b = juce::jlimit (a, (int)x.size(), (int)(to * rate));
        int crossings = 0;
        for (int i = a + 1; i < b; ++i)
            crossings += (x[(size_t)i - 1] < 0.0f) != (x[(size_t)i] < 0.0f) ? 1 : 0;
        if (b > a && matches (crossings * rate / (2.0 * (b - a)), 1.0))
            return plays;
    }
    if (heardHz > 0.0)
    {
        if (strong >= 3)
            return chord;
        const auto heard = (int)std::round (12.0 * std::log2 (heardHz / 440.0)) + 69;
        return { Job::special, "sounds as " + keyName (heard), InstrumentProfiles::Source::probing };
    }
    if (total <= 0.0 || expected < 30.0) // too high or too low to hear its pitch: it sounds, so it is taken to play
        return plays;
    const auto top = (int)(std::max_element (classes.begin(), classes.end()) - classes.begin());
    if (top == key % 12 && classes[(size_t)top] > 0.3 && strong <= 2)
        return plays;
    if (strong >= 3)
        return chord;
    return { Job::special, "a sound, not a note", InstrumentProfiles::Source::probing };
}
} // namespace

double frequencyOf (int note) { return 440.0 * std::pow (2.0, (note - 69) / 12.0); }

Result probe (const Player& play, const std::function<void()>& reset, const Options& options,
              const std::function<bool (int, int)>& progress)
{
    Result result;
    const auto render = [&] (int key) { return monoOf (play (notesFor (key, options), options.tail)); };
    const auto heardFrom = entry, heardTo = entry + options.hold + options.tail;

    // The reference: a key in the middle that plays, heard three times (a sampler varies from one to the next).
    std::vector<Print> prints;
    for (const auto candidate : { 60, 57, 64, 48, 72, 52, 67, 45, 36, 84, 40, 76 })
    {
        if (candidate == options.enterKey)
            continue;
        const auto x = render (candidate);
        const auto print = Print::of (x, heardFrom, heardTo);
        if (print.peak > silentPeak && heardAs (candidate, x, options).job == Job::plays)
        {
            result.reference = candidate;
            prints.push_back (print);
            break;
        }
    }
    if (result.reference < 0)
    {
        result.problem = options.enterKey >= 0
                             ? "It played no note in tune in this mode (after " + keyName (options.enterKey) + ")."
                             : juce::String ("It played no note in tune.");
        return result;
    }
    for (int i = 0; i < 2; ++i)
        prints.push_back (Print::of (render (result.reference), heardFrom, heardTo));
    double variation = 0.0;
    for (size_t i = 1; i < prints.size(); ++i)
        variation = juce::jmax (variation, prints[i].distance (prints[0]));
    const auto changed = juce::jmax (1.0, 2.0 * variation);
    const auto loudest =
        std::max_element (prints.begin(), prints.end(), [] (const Print& a, const Print& b) { return a.peak < b.peak; })
            ->peak;
    const auto silent = juce::jmax (silentPeak, loudest * 0.003f); // 50 dB under the reference

    const auto from = juce::jlimit (0, 127, juce::jmin (options.from, options.to));
    const auto to = juce::jlimit (0, 127, juce::jmax (options.from, options.to));
    for (int key = from; key <= to; ++key)
    {
        if (progress && ! progress (key - from, to - from + 1))
            break;
        auto& found = result.keys[(size_t)key];
        if (key == options.enterKey)
        {
            found = { Job::switches, "enters this mode", InstrumentProfiles::Source::probing };
            continue;
        }
        const auto x = render (key);
        if (Print::of (x, heardFrom, heardTo).peak > silent)
        {
            found = heardAs (key, x, options);
            continue;
        }
        // Silent: did it change how the reference sounds?
        if (Print::of (render (result.reference), heardFrom, heardTo).distance (prints[0]) > changed)
        {
            found = { Job::switches, "changes how it plays", InstrumentProfiles::Source::probing };
            if (reset)
                reset();
        }
        else
            found = { Job::unused, {}, InstrumentProfiles::Source::probing };
    }
    if (progress)
        progress (to - from + 1, to - from + 1);
    return result;
}

int merge (InstrumentProfiles::Mode& mode, const std::array<InstrumentProfiles::Key, 128>& heard)
{
    int changed = 0;
    for (size_t n = 0; n < heard.size(); ++n)
    {
        auto& key = mode.keys[n];
        if (heard[n].job == Job::unknown || (key.job != Job::unknown && key.source == InstrumentProfiles::Source::user))
            continue;
        if (key != heard[n])
        {
            key = heard[n];
            ++changed;
        }
    }
    return changed;
}

} // namespace spinus::keyprobe
