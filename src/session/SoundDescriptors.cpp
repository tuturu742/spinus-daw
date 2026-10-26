#include "SoundDescriptors.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <regex>

namespace spinus::sounds
{

namespace
{
constexpr double silentPeak = 1.0e-4;    // -80 dBFS: nothing there
constexpr double whiteFlatness = 0.5615; // a Hann-windowed periodogram of white noise (e^-gamma)

double round3 (double v) { return std::round (v * 1000.0) / 1000.0; }

double toDb (double linear) { return linear > 1.0e-10 ? 20.0 * std::log10 (linear) : -200.0; }

double median (std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort (v.begin(), v.end());
    const auto n = v.size();
    return n % 2 == 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/** Power spectra of frames of one size (a Hann window). */
class Analyser
{
public:
    explicit Analyser (int order)
        : fft (order), size ((int)(1u << (unsigned)order)), window ((size_t)size), work ((size_t)size * 2)
    {
        for (int i = 0; i < size; ++i)
            window[(size_t)i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float)i / (float)size);
    }

    int getSize() const noexcept { return size; }

    /** The power of bins 0 to size / 2 of the frame starting at `at` (zeros past the end). */
    const std::vector<float>& power (const float* x, int count, int at)
    {
        std::fill (work.begin(), work.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            if (const auto j = at + i; j >= 0 && j < count)
                work[(size_t)i] = x[j] * window[(size_t)i];
        fft.performFrequencyOnlyForwardTransform (work.data(), true);
        spectrum.resize ((size_t)size / 2 + 1);
        for (size_t i = 0; i < spectrum.size(); ++i)
            spectrum[i] = work[i] * work[i];
        return spectrum;
    }

private:
    juce::dsp::FFT fft;
    int size;
    std::vector<float> window, work, spectrum;
};

int orderFor (double rate, double seconds)
{
    // About `seconds` long, as a power of two.
    return juce::jlimit (8, 15, (int)std::round (std::log2 (juce::jmax (256.0, rate * seconds))));
}

/** Level in steps of `hop` samples, each the RMS of `window` samples around it. */
std::vector<double> levels (const float* x, int count, int hop, int window)
{
    std::vector<double> result;
    if (count <= 0 || hop <= 0)
        return result;
    // Running sum of squares.
    std::vector<double> sums ((size_t)count + 1, 0.0);
    for (int i = 0; i < count; ++i)
        sums[(size_t)i + 1] = sums[(size_t)i] + (double)x[i] * (double)x[i];
    const auto frames = (count + hop - 1) / hop;
    result.reserve ((size_t)frames);
    for (int f = 0; f < frames; ++f)
    {
        const auto centre = f * hop;
        const auto a = juce::jmax (0, centre - window / 2), b = juce::jmin (count, centre + window / 2);
        result.push_back (b > a ? std::sqrt ((sums[(size_t)b] - sums[(size_t)a]) / (double)(b - a)) : 0.0);
    }
    return result;
}

/** A frame's YIN: the period in samples (0: none) and the clarity (1 - the dip's depth). */
std::pair<double, double> yin (const std::vector<float>& x, int at, int window, int shortest, int longest)
{
    std::vector<double> d ((size_t)longest + 2, 0.0);
    for (int tau = 1; tau <= longest + 1; ++tau)
    {
        double sum = 0.0;
        for (int j = 0; j < window; ++j)
        {
            const double diff = (double)x[(size_t)at + (size_t)j] - (double)x[(size_t)at + (size_t)j + (size_t)tau];
            sum += diff * diff;
        }
        d[(size_t)tau] = sum;
    }
    // Cumulative mean normalised difference.
    std::vector<double> cmnd ((size_t)longest + 2, 1.0);
    double running = 0.0;
    for (int tau = 1; tau <= longest + 1; ++tau)
    {
        running += d[(size_t)tau];
        cmnd[(size_t)tau] = running > 0.0 ? d[(size_t)tau] * tau / running : 1.0;
    }
    int best = -1;
    for (int tau = shortest; tau <= longest; ++tau)
        if (cmnd[(size_t)tau] < 0.15)
        {
            while (tau + 1 <= longest && cmnd[(size_t)tau + 1] < cmnd[(size_t)tau])
                ++tau;
            best = tau;
            break;
        }
    if (best < 0)
    {
        best = shortest;
        for (int tau = shortest; tau <= longest; ++tau)
            if (cmnd[(size_t)tau] < cmnd[(size_t)best])
                best = tau;
    }
    const auto clarity = juce::jlimit (0.0, 1.0, 1.0 - cmnd[(size_t)best]);
    // Parabolic interpolation around the dip.
    double period = best;
    if (best > 1 && best < longest)
    {
        const auto a = cmnd[(size_t)best - 1], b = cmnd[(size_t)best], c = cmnd[(size_t)best + 1];
        if (const auto denominator = a - 2.0 * b + c; std::abs (denominator) > 1.0e-12)
            period += juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / denominator);
    }
    return { period, clarity };
}

/** Lower-case words of a name (letters and digits). */
juce::StringArray wordsOf (const juce::String& text)
{
    juce::StringArray words;
    juce::String word;
    auto flush = [&]
    {
        if (word.isNotEmpty())
            words.add (word.toLowerCase());
        word.clear();
    };
    for (auto c : text)
    {
        // "BassLead" and "Bass_Lead" are two words; "808" is one.
        if (juce::CharacterFunctions::isLetterOrDigit (c))
        {
            if (word.isNotEmpty() && juce::CharacterFunctions::isUpperCase (c) &&
                juce::CharacterFunctions::isLowerCase (word.getLastCharacter()))
                flush();
            word += juce::String::charToString (c);
        }
        else
            flush();
    }
    flush();
    return words;
}

/** The first word list entry that a word is (`exact`) or starts with. */
bool matches (const juce::StringArray& words, std::initializer_list<const char*> starts,
              std::initializer_list<const char*> exact = {})
{
    for (const auto& w : words)
    {
        for (auto* e : exact)
            if (w == e)
                return true;
        for (auto* s : starts)
            if (w.startsWith (s))
                return true;
    }
    return false;
}

double noteFrequency (int note) { return 440.0 * std::pow (2.0, (note - 69) / 12.0); }

/** Semitones from `expected` to `heard`, an octave or two off counting as none. */
double pitchClassError (double heard, double expected)
{
    const auto e = 12.0 * std::log2 (heard / expected);
    return e - 12.0 * std::round (e / 12.0);
}

/** Onset strength in steps of `hop` samples: how much the level rises in 24 bands from 40 Hz to 16 kHz (dB,
    over 4096-sample frames, each band at least 4 bins wide), summed. Bands of long frames rather than bins, so a
    steady note (whose harmonics beat in the bins between them as the frames slide along) shows no onsets. */
std::vector<double> onsetStrength (const float* x, int count, double rate, int hop)
{
    constexpr int bands = 24;
    Analyser analyser (orderFor (rate, 0.085));
    const auto size = analyser.getSize();
    const auto binHz = rate / size;
    std::array<int, bands + 1> edges {};
    edges[0] = 1;
    for (int b = 1; b <= bands; ++b)
        edges[(size_t)b] =
            juce::jmax (edges[(size_t)b - 1] + 4, (int)std::round (40.0 * std::pow (400.0, (double)b / bands) / binHz));
    std::vector<double> flux;
    std::array<double, bands> previous {}, now {};
    bool first = true;
    for (int at = -size / 2; at < count - size / 2; at += hop)
    {
        const auto& p = analyser.power (x, count, at);
        double loudest = -200.0;
        for (int b = 0; b < bands; ++b)
        {
            double e = 0.0;
            for (int i = edges[(size_t)b];
                 i < juce::jmax (edges[(size_t)b] + 1, edges[(size_t)b + 1]) && i < (int)p.size(); ++i)
                e += (double)p[(size_t)i];
            now[(size_t)b] = 10.0 * std::log10 (e + 1.0e-12);
            loudest = juce::jmax (loudest, now[(size_t)b]);
        }
        double sum = 0.0;
        for (int b = 0; b < bands; ++b)
        {
            now[(size_t)b] = juce::jmax (now[(size_t)b], loudest - 60.0, -100.0);
            if (! first)
                sum += juce::jmax (0.0, now[(size_t)b] - previous[(size_t)b]);
        }
        flux.push_back (sum);
        previous = now;
        first = false;
    }
    return flux;
}

const char* const keyNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
} // namespace

//==============================================================================
juce::var toVar (const Descriptors& d)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("attack", round3 (d.attack));
    o->setProperty ("onset", round3 (d.onset));
    o->setProperty ("decay", round3 (d.decay));
    o->setProperty ("sustain", round3 (d.sustain));
    o->setProperty ("release", round3 (d.release));
    o->setProperty ("length", round3 (d.length));
    o->setProperty ("brightness", std::round (d.brightness));
    o->setProperty ("harmonicBrightness", round3 (d.harmonicBrightness));
    o->setProperty ("noisiness", round3 (d.noisiness));
    o->setProperty ("pitched", round3 (d.pitched));
    o->setProperty ("fundamental", std::round (d.fundamental * 10.0) / 10.0);
    o->setProperty ("low", round3 (d.low));
    o->setProperty ("high", round3 (d.high));
    o->setProperty ("peakDb", std::round (d.peakDb * 10.0) / 10.0);
    return juce::var (o);
}

Descriptors descriptorsFromVar (const juce::var& v)
{
    Descriptors d;
    d.attack = v["attack"];
    d.onset = v["onset"];
    d.decay = v["decay"];
    d.sustain = v["sustain"];
    d.release = v["release"];
    d.length = v["length"];
    d.brightness = v["brightness"];
    d.harmonicBrightness = v["harmonicBrightness"];
    d.noisiness = v["noisiness"];
    d.pitched = v["pitched"];
    d.fundamental = v["fundamental"];
    d.low = v["low"];
    d.high = v["high"];
    d.peakDb = v.hasProperty ("peakDb") ? (double)v["peakDb"] : -100.0;
    return d;
}

//==============================================================================
Envelope measureEnvelope (const float* x, int count, double rate, double held)
{
    Envelope e;
    const auto hop = juce::jmax (1, (int)std::round (rate * 0.002));
    const auto level = levels (x, count, hop, juce::jmax (2, (int)std::round (rate * 0.01)));
    if (level.empty())
        return e;
    const auto seconds = (double)hop / rate;
    const auto peakAt = (size_t)std::distance (level.begin(), std::max_element (level.begin(), level.end()));
    e.peak = level[peakAt];
    if (e.peak < silentPeak)
        return e;
    const auto quiet = e.peak * 0.01; // -40 dB
    size_t start = 0;
    while (start < level.size() && level[start] < quiet)
        ++start;
    size_t loud = start;
    while (loud < level.size() && level[loud] < 0.9 * e.peak)
        ++loud;
    size_t end = level.size() - 1;
    while (end > start && level[end] < quiet)
        --end;
    e.start = (double)start * seconds;
    e.attack = (double)(loud - start) * seconds;
    e.onset = level[juce::jmin (level.size() - 1, start + (size_t)std::round (0.03 / seconds))] / e.peak;
    e.length = (double)(end + 1 - start) * seconds;

    const auto releasedAt = held > 0.0 ? juce::jmin (level.size(), (size_t)(held / seconds)) : level.size();
    if (held > 0.0)
    {
        // The last 0.1 s before the key is released.
        const auto from = (size_t)(juce::jmax (0.0, held - 0.1) / seconds);
        double sum = 0.0;
        int n = 0;
        for (auto i = from; i < releasedAt; ++i, ++n)
            sum += level[i];
        e.sustain = n > 0 ? juce::jlimit (0.0, 1.0, sum / n / e.peak) : 0.0;
        e.release = juce::jmax (0.0, (double)(end + 1) * seconds - held);
    }
    auto fallen = peakAt;
    while (fallen < releasedAt && level[fallen] > e.peak * 0.316) // -10 dB
        ++fallen;
    e.decay = (double)(juce::jmax (fallen, peakAt) - peakAt) * seconds;
    return e;
}

Spectrum measureSpectrum (const float* x, int count, double rate, double from, double to)
{
    Spectrum s;
    const auto a = juce::jmax (0, (int)(from * rate)), b = juce::jmin (count, juce::jmax (a + 1, (int)(to * rate)));
    // Frames as long as the part measured, from 1024 to 8192 samples at 48 kHz (long enough to see a low note's
    // harmonics apart), inside it; a short sound gets one frame centred on it.
    Analyser analyser (juce::jlimit (orderFor (rate, 0.021), orderFor (rate, 0.17),
                                     (int)std::ceil (std::log2 (juce::jmax (2, b - a)))));
    const auto size = analyser.getSize();
    const auto binHz = rate / size;
    std::vector<int> starts;
    if (b - a <= size)
        starts.push_back ((a + b) / 2 - size / 2);
    else
        for (int at = a; at + size <= b + size / 4; at += size / 2)
            starts.push_back (at);
    double total = 0.0, weighted = 0.0, low = 0.0, high = 0.0, flatness = 0.0, flatnessWeight = 0.0;
    std::vector<double> cumulative;
    for (const auto at : starts)
    {
        const auto& p = analyser.power (x, count, at);
        double frame = 0.0;
        cumulative.assign (p.size(), 0.0);
        for (size_t i = 1; i < p.size(); ++i)
        {
            const auto f = (double)i * binHz;
            const auto power = (double)p[i];
            frame += power;
            cumulative[i] = frame;
            weighted += f * power;
            if (f < 150.0)
                low += power;
            if (f > 5000.0)
                high += power;
        }
        total += frame;
        if (frame <= 0.0)
            continue;
        // Flatness where the sound is (from 5 % to 95 % of its power, at least 16 bins): noise in a band is as
        // noisy as white noise, a tone is peaky wherever it is.
        size_t first = 1, last = p.size() - 1;
        while (first < last && cumulative[first] < 0.05 * frame)
            ++first;
        while (last > first && cumulative[last - 1] >= 0.95 * frame)
            --last;
        while (last - first < 16 && (first > 1 || last + 1 < p.size()))
        {
            first = first > 1 ? first - 1 : first;
            last = last + 1 < p.size() ? last + 1 : last;
        }
        double logSum = 0.0, linSum = 0.0;
        for (auto i = first; i <= last; ++i)
        {
            logSum += std::log ((double)p[i] + 1.0e-20);
            linSum += (double)p[i] + 1.0e-20;
        }
        const auto bins = (double)(last - first + 1);
        flatness += frame * std::exp (logSum / bins) / (linSum / bins);
        flatnessWeight += frame;
    }
    if (total <= 0.0)
        return s;
    s.centroid = weighted / total;
    s.low = low / total;
    s.high = high / total;
    s.flatness = flatnessWeight > 0.0 ? flatness / flatnessWeight : 0.0;
    return s;
}

Pitch detectPitch (const float* x, int count, double rate, double from, double to)
{
    // Halved to about 24 kHz first when the rate is high (a [1 2 1] low pass): a quarter of the work.
    const int factor = rate > 30000.0 ? 2 : 1;
    const auto r = rate / factor;
    std::vector<float> y;
    y.reserve ((size_t)count / (size_t)factor + 1);
    for (int i = 0; i < count; i += factor)
        y.push_back (factor == 1
                         ? x[i]
                         : 0.25f * (i > 0 ? x[i - 1] : 0.0f) + 0.5f * x[i] + 0.25f * (i + 1 < count ? x[i + 1] : 0.0f));
    const auto window = (int)std::round (r * 0.035);
    const auto shortest = juce::jmax (2, (int)std::floor (r / 2000.0)), longest = (int)std::ceil (r / 30.0);
    // Frames start from `from` and end by `to`; their lags may reach past it.
    const auto a = juce::jmax (0, (int)(from * r));
    const auto last = juce::jmin ((int)(to * r) - window, (int)y.size() - window - longest - 2);
    if (last < a)
        return {};
    // Up to six frames, evenly spread; silent ones left out.
    const int frames = juce::jlimit (1, 6, (last - a) / juce::jmax (1, window / 2) + 1);
    std::vector<double> found, clarities;
    double loudest = 0.0;
    for (int i = a; i <= last + window; ++i)
        loudest = juce::jmax (loudest, (double)std::abs (y[(size_t)i]));
    for (int k = 0; k < frames; ++k)
    {
        const auto at = frames == 1 ? a : a + (last - a) * k / (frames - 1);
        double peak = 0.0;
        for (int j = 0; j < window; ++j)
            peak = juce::jmax (peak, (double)std::abs (y[(size_t)at + (size_t)j]));
        if (peak < juce::jmax (silentPeak, loudest * 0.05))
            continue;
        const auto [period, clarity] = yin (y, at, window, shortest, longest);
        clarities.push_back (clarity);
        if (clarity >= 0.5 && period > 0.0)
            found.push_back (r / period);
    }
    Pitch p;
    if (clarities.empty())
        return p;
    p.clarity = std::accumulate (clarities.begin(), clarities.end(), 0.0) / (double)clarities.size();
    if (found.size() * 2 >= clarities.size())
        p.frequency = median (found);
    return p;
}

std::array<double, 12> chroma (const float* x, int count, double rate, double from, double to)
{
    std::array<double, 12> c {};
    Analyser analyser (orderFor (rate, 0.17)); // 8192 at 48 kHz: 6 Hz per bin
    const auto size = analyser.getSize();
    const auto binHz = rate / size;
    const auto a = juce::jmax (0, (int)(from * rate)), b = juce::jmin (count, juce::jmax (a + 1, (int)(to * rate)));
    for (int at = a; at < juce::jmax (a + 1, b - size / 2); at += size / 2)
    {
        const auto& p = analyser.power (x, count, at);
        for (size_t i = 1; i < p.size(); ++i)
        {
            const auto f = (double)i * binHz;
            if (f < 60.0 || f > 5000.0)
                continue;
            const auto note = (int)std::round (12.0 * std::log2 (f / 440.0)) + 69;
            c[(size_t)(((note % 12) + 12) % 12)] += std::sqrt ((double)p[i]);
        }
    }
    const auto sum = std::accumulate (c.begin(), c.end(), 0.0);
    if (sum > 0.0)
        for (auto& v : c)
            v /= sum;
    return c;
}

std::vector<double> findOnsets (const float* x, int count, double rate)
{
    const auto hop = juce::jmax (1, (int)std::round (rate * 0.01));
    const auto flux = onsetStrength (x, count, rate, hop);
    std::vector<double> onsets;
    if (flux.empty())
        return onsets;
    const auto highest = *std::max_element (flux.begin(), flux.end());
    if (highest <= 0.0)
        return onsets;
    int lastOnset = -1000;
    for (int t = 0; t < (int)flux.size(); ++t)
    {
        const auto v = flux[(size_t)t];
        if (v < 0.15 * highest)
            continue;
        bool isPeak = true;
        double around = 0.0;
        int n = 0;
        for (int k = t - 15; k <= t + 15; ++k)
            if (k >= 0 && k < (int)flux.size())
            {
                if (std::abs (k - t) <= 3 && flux[(size_t)k] > v)
                    isPeak = false;
                around += flux[(size_t)k];
                ++n;
            }
        if (! isPeak || v < around / n + 0.1 * highest || t - lastOnset < 5)
            continue;
        onsets.push_back (juce::jmax (0.0, (double)t * hop / rate));
        lastOnset = t;
    }
    return onsets;
}

Descriptors describe (const float* x, int count, double rate, double held)
{
    Descriptors d;
    const auto e = measureEnvelope (x, count, rate, held);
    d.peakDb = toDb (e.peak);
    if (e.peak < silentPeak)
        return d;
    d.attack = e.attack;
    d.onset = e.onset;
    d.decay = e.decay;
    d.sustain = e.sustain;
    d.release = e.release;
    d.length = e.length;
    // The spectrum and pitch while it sounds clearly: half a second from the start (or less, a short one).
    const auto until = e.start + juce::jlimit (0.03, 0.5, e.length);
    const auto s = measureSpectrum (x, count, rate, e.start, until);
    d.brightness = s.centroid;
    d.noisiness = juce::jlimit (0.0, 1.0, s.flatness / whiteFlatness);
    d.low = s.low;
    d.high = s.high;
    // Pitch past the attack's first moments.
    const auto pitchFrom = e.start + juce::jmin (0.03, e.length * 0.1);
    const auto pitch = detectPitch (x, count, rate, pitchFrom, e.start + juce::jlimit (0.1, 0.8, e.length));
    d.pitched = pitch.clarity;
    d.fundamental = pitch.frequency;
    if (d.fundamental > 0.0)
        d.harmonicBrightness = d.brightness / d.fundamental;
    return d;
}

//==============================================================================
juce::String roleFromName (const juce::String& name, const juce::String& category)
{
    // The category first (Surge XT's "Basses", "Pads"...), then the name.
    for (const auto& text : { category, name })
    {
        const auto words = wordsOf (text);
        if (matches (words, { "drum", "kick", "snare", "perc", "hihat", "cymbal", "clap", "kit", "tom" },
                     { "bd", "sd", "hh", "toms" }))
            return "drums";
        if (matches (words, { "bass", "808", "reese" }, { "sub", "subs" }))
            return "bass";
        if (matches (words, { "pad", "atmos", "ambien", "choir", "string" }))
            return "pad";
        if (matches (words, { "pluck", "harp", "pizz", "mallet", "marimba", "kalimba" }, { "plk" }))
            return "pluck";
        if (matches (words, { "key", "piano", "organ", "rhodes", "epiano", "clav", "chord", "poly", "bell" }, { "ep" }))
            return "keys";
        if (matches (words, { "lead", "solo" }, { "ld" }))
            return "lead";
        if (matches (words, { "sfx", "riser", "sweep", "impact", "noise", "texture", "drone" }, { "fx" }))
            return "fx";
    }
    return {};
}

juce::String roleFromSound (const InstrumentSound& s)
{
    if (s.silent || s.invalid)
        return "none";
    const auto& d = s.sound;
    if (! s.tracksPitch)
    {
        if (s.drumMap.size() >= 3)
            return "drums";
        if (d.length < 1.2 && d.sustain < 0.3) // one short hit, whatever key
            return "drums";
        return "fx";
    }
    // Swells in (a hit that only grows in its reverb starts loud, so the level at 30 ms decides).
    if (d.attack >= 0.12 && d.onset < 0.5)
        return "pad";
    if (d.sustain < 0.25) // dies away while the key is held
        return d.decay < 0.35 ? "pluck" : "keys";
    if (s.polyphonic)
        return d.release >= 0.5 && d.onset < 0.7 ? "pad" : "keys";
    // One note at a time and sustained: dark and short is a bass, the rest a lead.
    return d.harmonicBrightness > 0.0 && d.harmonicBrightness < 2.5 && d.release < 0.4 ? "bass" : "lead";
}

InstrumentSound describeInstrument (const Player& player, double rate, const juce::String& name,
                                    const juce::String& category)
{
    InstrumentSound result;
    // Invalid samples (a plugin's filter blowing up) are silence here, and noted.
    const auto play = [&] (const std::vector<Note>& notes, double tail)
    {
        auto audio = player (notes, tail);
        for (int c = 0; c < audio.getNumChannels(); ++c)
        {
            auto* v = audio.getWritePointer (c);
            for (int i = 0; i < audio.getNumSamples(); ++i)
                if (! std::isfinite (v[i]) || std::abs (v[i]) > 100.0f) // +40 dBFS: blown up
                {
                    v[i] = 0.0f;
                    result.invalid = true;
                }
        }
        return audio;
    };
    const auto held = play ({ { TestNotes::held, 0.0, TestNotes::hold, 0.8f } }, TestNotes::ringOut);
    result.sound = describe (held.getReadPointer (0), held.getNumSamples(), rate, TestNotes::hold);
    if (result.sound.peakDb < toDb (silentPeak))
    {
        // Silent on C3 (a drum kit with no sound there, perhaps): the probes still tell.
        result.silent = true;
    }

    // The register: short notes from C1 to C7.
    struct Probe
    {
        int note;
        double peak;
        Pitch pitch;
        std::array<double, 12> chroma;
    };
    std::vector<Probe> probes;
    double loudest = 0.0; // of the probes (a pad's slow attack makes them quieter than the held note)
    for (auto note : TestNotes::probes)
    {
        const auto audio = play ({ { note, 0.0, TestNotes::probeHold, 0.8f } }, TestNotes::probeTail);
        const auto* x = audio.getReadPointer (0);
        const auto e = measureEnvelope (x, audio.getNumSamples(), rate, TestNotes::probeHold);
        Probe p { note, e.peak, {}, {} };
        if (e.peak >= silentPeak)
        {
            p.pitch = detectPitch (x, audio.getNumSamples(), rate, e.start + 0.005, TestNotes::probeHold);
            if (note == TestNotes::chord[0])
                p.chroma = chroma (x, audio.getNumSamples(), rate, 0.0, TestNotes::probeHold);
        }
        loudest = juce::jmax (loudest, e.peak);
        probes.push_back (p);
    }
    auto sounds = [&] (const Probe& p) { return p.peak >= silentPeak && p.peak >= loudest * 0.03; };
    auto inTune = [] (const Probe& p)
    {
        return p.pitch.clarity >= 0.6 && p.pitch.frequency > 0.0 &&
               std::abs (pitchClassError (p.pitch.frequency, noteFrequency (p.note))) <= 0.5;
    };
    const auto sounding = std::count_if (probes.begin(), probes.end(), sounds);
    const auto tuned =
        std::count_if (probes.begin(), probes.end(), [&] (const Probe& p) { return sounds (p) && inTune (p); });
    if (sounding == 0)
    {
        result.silent = true;
        result.role = result.soundRole = "none";
        result.roleFrom = "sound";
        return result;
    }
    result.silent = false;
    result.tracksPitch = tuned >= 2 && (double)tuned >= 0.6 * (double)sounding;
    result.low = 127;
    result.high = 0;
    for (const auto& p : probes)
        if (sounds (p) && (! result.tracksPitch || inTune (p)))
        {
            result.low = juce::jmin (result.low, p.note);
            result.high = juce::jmax (result.high, p.note);
        }
    if (result.low > result.high)
        result.low = result.high = TestNotes::held;

    if (result.tracksPitch)
    {
        // Polyphony: the chord's three notes are all there, and the third (E) much more than in C alone (where
        // only C's fifth harmonic puts some there).
        const auto audio = play ({ { TestNotes::chord[0], 0.0, TestNotes::chordHold, 0.8f },
                                   { TestNotes::chord[1], 0.0, TestNotes::chordHold, 0.8f },
                                   { TestNotes::chord[2], 0.0, TestNotes::chordHold, 0.8f } },
                                 0.2);
        const auto c3 = chroma (audio.getReadPointer (0), audio.getNumSamples(), rate, 0.05, TestNotes::chordHold);
        const auto single =
            std::find_if (probes.begin(), probes.end(), [] (const Probe& p) { return p.note == TestNotes::chord[0]; });
        const auto most = *std::max_element (c3.begin(), c3.end());
        const auto e = (size_t)(TestNotes::chord[1] % 12), g = (size_t)(TestNotes::chord[2] % 12),
                   c = (size_t)(TestNotes::chord[0] % 12);
        result.polyphonic = most > 0.0 && c3[c] >= 0.25 * most && c3[e] >= 0.25 * most && c3[g] >= 0.25 * most &&
                            (single == probes.end() || c3[e] - single->chroma[e] >= 0.06);
    }
    else
    {
        // A drum kit: each note's sound, by class.
        std::vector<int> notes;
        for (int n = 35; n <= 51; ++n)
            notes.push_back (n);
        for (int n = 60; n <= 75; ++n)
            notes.push_back (n);
        juce::StringArray classes;
        for (auto n : notes)
        {
            const auto audio = play ({ { n, 0.0, 0.3, 0.8f } }, 1.2);
            const auto* x = audio.getReadPointer (0);
            const auto d = describe (x, audio.getNumSamples(), rate, 0.0);
            if (d.peakDb < toDb (juce::jmax (silentPeak, loudest * 0.03)))
                continue;
            const auto cls = classifyHit (d, measureHit (x, audio.getNumSamples(), rate));
            result.drumMap.emplace_back (n, cls);
            if (cls != "bass" && cls != "other")
                classes.addIfNotAlreadyThere (cls);
        }
        const bool drums = classes.contains ("kick") || classes.contains ("snare") || classes.contains ("clap") ||
                           classes.contains ("hat") || classes.contains ("cymbal");
        if (classes.size() < 2 || ! drums)
            result.drumMap.clear(); // one sound on every key, or no drums among them: not a kit
    }

    result.soundRole = roleFromSound (result);
    const auto named = roleFromName (name, category);
    result.role = named.isNotEmpty() ? named : result.soundRole;
    result.roleFrom = named.isNotEmpty() ? "name" : "sound";
    return result;
}

juce::var toVar (const InstrumentSound& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("role", s.role);
    o->setProperty ("roleFrom", s.roleFrom);
    o->setProperty ("soundRole", s.soundRole);
    o->setProperty ("silent", s.silent);
    if (s.invalid)
        o->setProperty ("invalid", true);
    o->setProperty ("tracksPitch", s.tracksPitch);
    o->setProperty ("polyphonic", s.polyphonic);
    o->setProperty ("low", s.low);
    o->setProperty ("high", s.high);
    o->setProperty ("sound", toVar (s.sound));
    if (! s.drumMap.empty())
    {
        juce::Array<juce::var> map;
        for (const auto& [note, cls] : s.drumMap)
        {
            auto* m = new juce::DynamicObject();
            m->setProperty ("note", note);
            m->setProperty ("class", cls);
            map.add (juce::var (m));
        }
        o->setProperty ("drumMap", map);
    }
    return juce::var (o);
}

//==============================================================================
HitShape measureHit (const float* x, int count, double rate)
{
    HitShape h;
    const auto e = measureEnvelope (x, count, rate, 0.0);
    if (e.peak < silentPeak)
        return h;
    // Bursts: 1 ms steps of a 3 ms level over the first 80 ms; a burst is a peak after the level fell at least
    // 6 dB under the one before.
    const auto hop = juce::jmax (1, (int)std::round (rate * 0.001));
    const auto start = juce::jmax (0, (int)((e.start - 0.005) * rate));
    const auto level = levels (x + start, juce::jmin (count - start, (int)(0.085 * rate)), hop,
                               juce::jmax (2, (int)std::round (rate * 0.003)));
    const auto loudest = level.empty() ? 0.0 : *std::max_element (level.begin(), level.end());
    h.bursts = 0;
    double lastPeak = 0.0, dip = 0.0;
    for (size_t i = 0; i + 1 < level.size(); ++i)
    {
        const auto v = level[i];
        dip = juce::jmin (dip, v);
        if ((i > 0 && v < level[i - 1]) || v <= level[i + 1] || v < 0.2 * loudest)
            continue;
        if (h.bursts == 0 || dip <= 0.5 * juce::jmin (v, lastPeak))
        {
            ++h.bursts;
            lastPeak = dip = v;
        }
        else if (v > lastPeak)
            lastPeak = dip = v;
    }
    h.bursts = juce::jmax (1, h.bursts);
    const auto early = detectPitch (x, count, rate, e.start + 0.005, e.start + 0.055);
    const auto later = detectPitch (x, count, rate, e.start + 0.1, e.start + 0.2);
    h.startPitch = early.frequency;
    h.laterPitch = later.frequency;
    if (early.frequency > 0.0 && later.frequency > 0.0)
        h.pitchDrop = 12.0 * std::log2 (early.frequency / later.frequency);
    return h;
}

juce::String classifyHit (const Descriptors& d, const HitShape& h)
{
    if (d.peakDb < toDb (silentPeak) || d.length < 0.02)
        return "other";
    const auto settled = h.laterPitch > 0.0 ? h.laterPitch : d.fundamental > 0.0 ? d.fundamental : h.startPitch;
    // One clear pitch with its partials near it (a ride's inharmonic partials can make YIN hear a "pitch" far
    // under its brightness).
    const bool tonal = d.pitched >= 0.6 && settled > 0.0 && d.brightness < 12.0 * settled;
    const bool noisy = ! tonal && d.noisiness >= 0.3;
    // A long, steady, low note: a bass sample (an 808's glide is small next to a kick's sweep).
    if (tonal && settled < 250.0 && d.length >= 0.6 && h.pitchDrop < 3.0 && d.noisiness < 0.3)
        return "bass";
    // Mostly under 150 Hz and short: a kick (very low, or its pitch sweeping down far; a tom's falls a little).
    // (The lowest pitch heard: a short kick's tail may be too short for the later one.)
    const auto lowest = d.fundamental > 0.0 ? juce::jmin (settled, d.fundamental) : settled;
    if (d.low >= 0.45 && d.brightness < 600.0 && d.length < 1.5 &&
        (lowest < 70.0 || h.pitchDrop >= 6.0 || ! tonal || (d.length < 0.25 && lowest < 120.0 && d.low >= 0.8)))
        return "kick";
    // Several hands a few milliseconds apart (two bursts are as often a snare's rough start).
    if (h.bursts >= 3 && d.noisiness >= 0.35 && d.brightness >= 600.0 && d.brightness <= 6000.0)
        return "clap";
    // Long, bright and without one pitch: a cymbal, however noisy its measure (a crash's ringing partials).
    if (! tonal && d.length >= 0.8 && d.brightness >= 3000.0 && d.high >= 0.25)
        return "cymbal";
    if (noisy)
    {
        if (d.high >= 0.85) // nearly all over 5 kHz
            return d.length >= 0.7 ? "cymbal" : "hat";
        if (d.attack >= 0.02 && d.length <= 0.5) // swells in: a shaker
            return "perc";
        if (d.length <= 0.8)
            return "snare";
        // Long: a hit that dies fast and then rings in its room is a snare or a clap with reverb; noise that
        // keeps on is a cymbal.
        if (d.decay < 0.15)
            return h.bursts >= 3 ? "clap" : "snare";
        return d.high >= 0.25 || d.brightness >= 2000.0 ? "cymbal" : "other";
    }
    // Bright, rough and without one pitch, though not as noisy as noise: a tighter snare.
    if (! tonal && d.noisiness >= 0.2 && d.brightness >= 1500.0 && d.length <= 0.6 && d.attack < 0.02)
        return "snare";
    // A pitched hit with few overtones: a tom (a cowbell's are many).
    if (tonal && settled >= 60.0 && settled <= 450.0 && d.length <= 1.5 && d.length >= 0.12 &&
        d.brightness <= 2.5 * settled)
        return "tom";
    if (d.length <= 0.6)
        return "perc";
    if (tonal && settled < 250.0)
        return "bass";
    return "other";
}

juce::String classFromName (const juce::File& file)
{
    // The file's own name first, then its folder's ("Kicks/001.wav").
    for (const auto& text : { file.getFileNameWithoutExtension(), file.getParentDirectory().getFileName() })
    {
        const auto words = wordsOf (text);
        if (matches (words, { "kick", "kik" }, { "bd", "bassdrum" }))
            return "kick";
        if (matches (words, { "snare", "snr" }, { "sd", "rimshot" }))
            return "snare";
        if (matches (words, { "clap" }, { "clp" }))
            return "clap";
        if (matches (words, { "hat", "hihat", "hh" }, { "oh", "ch" }))
            return "hat";
        if (matches (words, { "cymbal", "crash", "ride", "splash", "china" }, { "cym" }))
            return "cymbal";
        if (matches (words, { "tom" }))
            return "tom";
        if (matches (words, { "perc", "shaker", "rim", "conga", "bongo", "cowbell", "tamb", "clave", "wood", "snap" }))
            return "perc";
        if (matches (words, { "bass", "808", "reese" }, { "sub" }))
            return "bass";
    }
    return {};
}

double tempoFromName (const juce::String& name)
{
    // "120bpm", "120 BPM", "120_bpm", "bpm120".
    const std::string text = name.toLowerCase().toStdString();
    static const std::regex after ("(^|[^0-9])([0-9]{2,3}(\\.[0-9])?)[ _-]?bpm");
    static const std::regex before ("bpm[ _-]?([0-9]{2,3}(\\.[0-9])?)($|[^0-9])");
    std::smatch m;
    double tempo = 0.0;
    if (std::regex_search (text, m, after))
        tempo = std::stod (m[2].str());
    else if (std::regex_search (text, m, before))
        tempo = std::stod (m[1].str());
    return tempo >= 40.0 && tempo <= 250.0 ? tempo : 0.0;
}

juce::String keyFromName (const juce::String& name)
{
    // Words such as "Am", "C#m", "Ebmin", "Fmaj", "F#", or a note then "minor"/"major". A lone letter is not
    // a key ("Loop B").
    juce::StringArray words;
    {
        juce::String word;
        for (auto c : name + " ")
        {
            if (juce::CharacterFunctions::isLetterOrDigit (c) || c == '#')
                word += juce::String::charToString (c);
            else
            {
                if (word.isNotEmpty())
                    words.add (word);
                word.clear();
            }
        }
    }
    auto noteIndex = [] (const juce::String& w, int& used) -> int
    {
        if (w.isEmpty())
            return -1;
        const auto letter = juce::CharacterFunctions::toUpperCase (w[0]);
        static const int base[] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
        if (letter < 'A' || letter > 'G')
            return -1;
        int index = base[letter - 'A'];
        used = 1;
        if (w.length() > 1 && (w[1] == '#' || w[1] == 'b'))
        {
            index += w[1] == '#' ? 1 : -1;
            used = 2;
        }
        return (index + 12) % 12;
    };
    for (int i = 0; i < words.size(); ++i)
    {
        const auto& w = words[i];
        int used = 0;
        const auto index = noteIndex (w, used);
        if (index < 0 || ! juce::CharacterFunctions::isUpperCase (w[0]))
            continue;
        const auto rest = w.substring (used);
        const auto next = words[i + 1].toLowerCase();
        juce::String mode;
        if (i > 0 && words[i - 1].equalsIgnoreCase ("key"))
            // "Key D#m7", "Key Gaug5": a chord named as the key; minor when its third is.
            mode = rest.startsWith ("m") && ! rest.startsWith ("maj") ? "minor" : "major";
        else if (rest == "m" || rest == "min" || rest == "minor" ||
                 (rest.isEmpty() && (next == "minor" || next == "min")))
            mode = "minor";
        // ("F#", "Eb": a key named by its note alone is major.)
        else if (rest == "maj" || rest == "major" || rest == "M" ||
                 (rest.isEmpty() && (next == "major" || next == "maj" || used == 2)))
            mode = "major";
        if (mode.isNotEmpty())
            return juce::String (keyNames[index]) + " " + mode;
    }
    return {};
}

juce::String keyOf (const std::array<double, 12>& c, double* fit)
{
    // Krumhansl and Kessler's profiles, from C (as spinus_ai.analysis).
    static const double major[] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static const double minor[] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
    auto correlation = [&] (const double* profile, int tonic)
    {
        double mx = 0.0, my = 0.0;
        for (int i = 0; i < 12; ++i)
        {
            mx += c[(size_t)i];
            my += profile[((i - tonic) % 12 + 12) % 12];
        }
        mx /= 12.0;
        my /= 12.0;
        double sxy = 0.0, sxx = 0.0, syy = 0.0;
        for (int i = 0; i < 12; ++i)
        {
            const auto dx = c[(size_t)i] - mx, dy = profile[((i - tonic) % 12 + 12) % 12] - my;
            sxy += dx * dy;
            sxx += dx * dx;
            syy += dy * dy;
        }
        return sxx > 0.0 && syy > 0.0 ? sxy / std::sqrt (sxx * syy) : 0.0;
    };
    double best = -2.0;
    juce::String name;
    for (int tonic = 0; tonic < 12; ++tonic)
        for (const bool isMinor : { false, true })
            if (const auto r = correlation (isMinor ? minor : major, tonic); r > best)
            {
                best = r;
                name = juce::String (keyNames[tonic]) + (isMinor ? " minor" : " major");
            }
    if (fit != nullptr)
        *fit = best;
    return name;
}

double tempoFromOnsets (const float* x, int count, double rate)
{
    // The onset strength (spectral flux, 10 ms steps) and its autocorrelation, 60 to 200 BPM, gently
    // preferring tempos near 120.
    const auto hop = juce::jmax (1, (int)std::round (rate * 0.01));
    auto flux = onsetStrength (x, count, rate, hop);
    if (flux.size() < 200)
        return 0.0;
    const auto mean = std::accumulate (flux.begin(), flux.end(), 0.0) / (double)flux.size();
    for (auto& v : flux)
        v -= mean;
    const auto step = (double)hop / rate;
    const auto shortest = (int)std::floor (60.0 / 200.0 / step), longest = (int)std::ceil (1.0 / step); // 60 BPM
    std::vector<double> r ((size_t)longest + 2, 0.0);
    for (int lag = shortest - 1; lag <= longest + 1; ++lag)
    {
        double sum = 0.0;
        for (size_t i = 0; i + (size_t)lag < flux.size(); ++i)
            sum += flux[i] * flux[i + (size_t)lag];
        r[(size_t)lag] = sum / (double)(flux.size() - (size_t)lag);
    }
    int best = -1;
    double bestScore = 0.0;
    for (int lag = shortest; lag <= longest; ++lag)
    {
        const auto bpm = 60.0 / (lag * step);
        const auto prior = std::exp (-0.5 * std::pow (std::log2 (bpm / 120.0) / 1.0, 2.0));
        const auto score = r[(size_t)lag] * prior;
        if (r[(size_t)lag] > r[(size_t)lag - 1] && r[(size_t)lag] >= r[(size_t)lag + 1] && score > bestScore)
        {
            bestScore = score;
            best = lag;
        }
    }
    if (best < 0)
        return 0.0;
    double lag = best;
    const auto a = r[(size_t)best - 1], b = r[(size_t)best], c = r[(size_t)best + 1];
    if (const auto denominator = a - 2.0 * b + c; std::abs (denominator) > 1.0e-12)
        lag += juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / denominator);
    return 60.0 / (lag * step);
}

//==============================================================================
namespace
{
/** The class of a loop from the sounds between its onsets: mostly drum hits make a drum loop; otherwise it
    plays notes, low ones a bass line. */
juce::String loopClassOf (const float* x, int count, double rate, const std::vector<double>& onsets,
                          const Descriptors& whole)
{
    int hits = 0, segments = 0;
    std::vector<double> pitches;
    for (size_t i = 0; i < onsets.size() && segments < 64; ++i)
    {
        const auto from = (int)(onsets[i] * rate);
        const auto to =
            juce::jmin (count, i + 1 < onsets.size() ? (int)(onsets[i + 1] * rate) : count, from + (int)rate);
        if (to - from < (int)(0.03 * rate))
            continue;
        const auto d = describe (x + from, to - from, rate, 0.0);
        if (d.peakDb < -60.0)
            continue;
        const auto shape = measureHit (x + from, to - from, rate);
        const auto cls = classifyHit (d, shape);
        ++segments;
        if (cls == "snare" || cls == "clap" || cls == "hat" || cls == "cymbal" ||
            (cls == "kick" && shape.pitchDrop >= 2.0) || d.noisiness >= 0.35)
            ++hits;
        else if (d.fundamental > 0.0 && d.pitched >= 0.6)
            pitches.push_back (d.fundamental);
    }
    const bool drums = segments == 0 ? whole.noisiness >= 0.35 : hits * 2 >= segments;
    return drums ? "drums" : pitches.empty() ? "other" : median (pitches) < 180.0 ? "bass" : "melodic";
}

/** The tempo of a loop from its sound: the whole bars of 4/4 that fit the onsets' tempo (a loop is cut to
    whole bars), or the onsets' tempo alone; `from` says which. 0 when none is found. */
double loopTempoOf (const float* x, int count, double rate, double duration, juce::String& from)
{
    const auto heard = tempoFromOnsets (x, count, rate);
    if (heard <= 0.0)
        return 0.0;
    double tempo = 0.0, bestDistance = std::log (1.06);
    for (const int bars : { 1, 2, 4, 8, 16, 32 })
    {
        const auto candidate = 240.0 * bars / duration;
        if (candidate < 60.0 || candidate > 200.0)
            continue;
        // The onsets' tempo, or twice or half of it (a little less likely), or 3/2, 3/4, 4/3, 2/3 of it (a
        // dotted or triplet pattern repeats at those; less likely again).
        for (const auto [factor, penalty] :
             { std::pair (1.0, 0.0), std::pair (2.0, 0.01), std::pair (0.5, 0.01), std::pair (1.5, 0.02),
               std::pair (0.75, 0.02), std::pair (4.0 / 3.0, 0.02), std::pair (2.0 / 3.0, 0.02) })
            if (const auto distance = std::abs (std::log (candidate / (heard * factor))) + penalty;
                distance < bestDistance)
            {
                bestDistance = distance;
                tempo = candidate;
                from = "length";
            }
    }
    if (tempo <= 0.0)
    {
        tempo = heard;
        while (tempo < 70.0)
            tempo *= 2.0;
        while (tempo >= 180.0)
            tempo /= 2.0;
        from = "onsets";
    }
    return std::round (tempo * 10.0) / 10.0;
}
} // namespace

SampleSound describeSample (const juce::AudioBuffer<float>& mono, double rate, const juce::File& file)
{
    SampleSound s;
    const auto* x = mono.getReadPointer (0);
    const auto count = mono.getNumSamples();
    s.duration = count / rate;
    if (s.duration > 60.0)
    {
        s.kind = s.soundKind = "long";
        s.sampleClass = s.soundClass = "other";
        s.classFrom = "sound";
        return s;
    }
    s.sound = describe (x, count, rate, 0.0);
    const auto onsets = findOnsets (x, count, rate);
    const auto name = file.getFileNameWithoutExtension();
    const auto words = wordsOf (name + " " + file.getParentDirectory().getFileName());
    const auto namedTempo = tempoFromName (name);

    // A loop or a one-shot: the name says (loop, BPM, one shot), else the sound: onsets over at least half of
    // a second and a half or more.
    // Only onsets where the level really rises (by 3 dB over the 50 ms before) to within 20 dB of the peak count:
    // a crash's or a long kick's decaying tail flickers, but does not start again.
    std::vector<double> strong;
    {
        const auto hop = juce::jmax (1, (int)std::round (rate * 0.005));
        const auto level = levels (x, count, hop, hop * 2);
        const auto peak = level.empty() ? 0.0 : *std::max_element (level.begin(), level.end());
        auto at = [&] (double t)
        { return (size_t)juce::jlimit (0, (int)level.size() - 1, (int)std::round (t * rate / hop)); };
        for (const auto t : onsets)
        {
            double after = 0.0, before = 0.0;
            for (auto i = at (t); i <= at (t + 0.05); ++i)
                after = juce::jmax (after, level[i]);
            int n = 0;
            for (auto i = at (t - 0.06); i <= at (t - 0.01); ++i, ++n)
                before += level[i];
            before /= juce::jmax (1, n);
            if (after >= 0.1 * peak && (t < 0.06 || after >= 1.4 * before))
                strong.push_back (t);
        }
    }
    const bool spread = strong.size() >= 4 && strong.back() - strong.front() >= 0.5 * s.duration;
    s.soundKind = s.duration >= 1.5 && spread ? "loop" : "one-shot";
    const bool namedLoop = matches (words, { "loop" }) || namedTempo > 0.0;
    const bool namedOneShot = matches (words, { "oneshot" }) || (words.contains ("one") && words.contains ("shot")) ||
                              (words.contains ("one") && words.contains ("shots"));
    s.kind = namedLoop && s.duration >= 1.0 ? "loop" : namedOneShot ? "one-shot" : s.soundKind;

    // What the sound alone says of its class, tempo and key, whatever the name (kept to check the guesses).
    s.soundClass = s.kind == "loop" ? loopClassOf (x, count, rate, onsets, s.sound)
                                    : classifyHit (s.sound, measureHit (x, count, rate));
    if (s.kind == "loop")
        s.soundTempo = loopTempoOf (x, count, rate, s.duration, s.soundTempoFrom);
    // A key for what has notes: a loop that is not drums, a one-shot that is a note or a chord (bass, other).
    const bool hasNotes =
        s.kind == "loop" ? s.soundClass != "drums" : s.soundClass == "bass" || s.soundClass == "other";
    if (hasNotes && s.sound.noisiness < 0.35 && s.duration >= 0.3)
    {
        const auto c = chroma (x, count, rate, 0.0, juce::jmin (s.duration, 30.0));
        double fit = 0.0;
        const auto heardKey = keyOf (c, &fit);
        if (fit >= 0.55 && *std::max_element (c.begin(), c.end()) >= 1.6 / 12.0)
            s.soundKey = heardKey;
    }

    // The name first, as for presets.
    if (s.kind == "loop")
    {
        juce::String named;
        if (matches (words, { "drum", "beat", "perc", "kick", "snare", "hat", "break", "groove", "top", "bongo" }))
            named = "drums";
        else if (matches (words, { "bass", "808" }, { "sub" }))
            named = "bass";
        else if (matches (words, { "chord", "key", "piano", "synth", "guitar", "melod", "lead", "pad", "arp", "string",
                                   "vocal", "vox", "pluck", "zither", "mal" }))
            named = "melodic";
        s.sampleClass = named.isNotEmpty() ? named : s.soundClass;
        s.classFrom = named.isNotEmpty() ? "name" : "sound";
        s.tempo = namedTempo > 0.0 ? namedTempo : s.soundTempo;
        s.tempoFrom = namedTempo > 0.0 ? juce::String ("name") : s.soundTempoFrom;
        if (s.tempo > 0.0)
            s.bars = std::round (s.duration * s.tempo / 240.0 * 100.0) / 100.0;
    }
    else
    {
        const auto named = classFromName (file);
        s.sampleClass = named.isNotEmpty() ? named : s.soundClass;
        s.classFrom = named.isNotEmpty() ? "name" : "sound";
    }
    const auto namedKey = keyFromName (name);
    s.key = namedKey.isNotEmpty() ? namedKey : s.soundKey;
    s.keyFrom = namedKey.isNotEmpty() ? juce::String ("name") : s.soundKey.isNotEmpty() ? "sound" : "";
    return s;
}

juce::var toVar (const SampleSound& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("kind", s.kind);
    o->setProperty ("class", s.sampleClass);
    o->setProperty ("classFrom", s.classFrom);
    o->setProperty ("soundClass", s.soundClass);
    o->setProperty ("duration", round3 (s.duration));
    if (s.kind != "long")
        o->setProperty ("sound", toVar (s.sound));
    if (s.tempo > 0.0)
    {
        o->setProperty ("tempo", s.tempo);
        o->setProperty ("tempoFrom", s.tempoFrom);
        o->setProperty ("bars", s.bars);
    }
    if (s.key.isNotEmpty())
    {
        o->setProperty ("key", s.key);
        o->setProperty ("keyFrom", s.keyFrom);
    }
    // What the sound alone says, where the name decided.
    o->setProperty ("soundKind", s.soundKind);
    if (s.soundTempo > 0.0)
        o->setProperty ("soundTempo", s.soundTempo);
    if (s.soundKey.isNotEmpty())
        o->setProperty ("soundKey", s.soundKey);

    return juce::var (o);
}

} // namespace spinus::sounds
