#include "EqEngine.h"

#include <complex>

namespace spinus::dsp
{

namespace
{
constexpr double pi = juce::MathConstants<double>::pi;
constexpr int updateInterval = 16; // samples between section updates while settings glide
constexpr double glideSeconds = 0.05;

/** An analog second-order section, s normalised to the band's frequency:
    (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0). */
struct Analog
{
    double n0, n1, n2, d0, d1, d2;
};

double analogSquared (const Analog& a, double w) noexcept
{
    const auto nr = a.n0 - a.n2 * w * w, ni = a.n1 * w;
    const auto dr = a.d0 - a.d2 * w * w, di = a.d1 * w;
    return (nr * nr + ni * ni) / juce::jmax (1.0e-300, dr * dr + di * di);
}

/** The Qs of a Butterworth cascade of `stages` sections, the sharpest last. */
std::array<double, EqEngine::maxStages> butterworthQs (int stages) noexcept
{
    std::array<double, EqEngine::maxStages> qs {};
    const auto order = 2 * stages;
    for (int k = 0; k < stages; ++k)
        qs[(size_t)k] = 1.0 / (2.0 * std::cos (pi * (2.0 * k + 1.0) / (2.0 * order)));
    return qs;
}

int stagesFor (const EqBand& b) noexcept { return b.slope >= 2 ? 4 : b.slope == 1 ? 2 : 1; }

/** The band's analog sections; returns how many (none: the band changes nothing). */
int prototype (const EqBand& b, std::array<Analog, EqEngine::maxStages>& out) noexcept
{
    const auto q = juce::jlimit (0.1, 18.0, (double)b.q);
    const auto gain = juce::jlimit (-24.0, 24.0, (double)b.gainDb);
    const auto a = std::pow (10.0, gain / 40.0);
    const auto s = std::sqrt (a) / q;
    switch (b.type)
    {
    case EqBand::Type::bell:
        if (std::abs (gain) < 1.0e-3)
            return 0;
        out[0] = { 1.0, a / q, 1.0, 1.0, 1.0 / (a * q), 1.0 };
        return 1;
    case EqBand::Type::lowShelf:
        if (std::abs (gain) < 1.0e-3)
            return 0;
        out[0] = { a * a, a * s, a, 1.0, s, a };
        return 1;
    case EqBand::Type::highShelf:
        if (std::abs (gain) < 1.0e-3)
            return 0;
        out[0] = { a, a * s, a * a, a, s, 1.0 };
        return 1;
    case EqBand::Type::notch:
        out[0] = { 1.0, 0.0, 1.0, 1.0, 1.0 / q, 1.0 };
        return 1;
    case EqBand::Type::lowCut:
    case EqBand::Type::highCut:
    {
        const auto stages = stagesFor (b);
        auto qs = butterworthQs (stages);
        qs[(size_t)stages - 1] *= q / std::sqrt (0.5); // the band's Q sets the resonance at the corner
        if (stages == 1)
            qs[0] = q;
        for (int k = 0; k < stages; ++k)
            out[(size_t)k] = b.type == EqBand::Type::lowCut ? Analog { 0.0, 0.0, 1.0, 1.0, 1.0 / qs[(size_t)k], 1.0 }
                                                            : Analog { 1.0, 0.0, 0.0, 1.0, 1.0 / qs[(size_t)k], 1.0 };
        return stages;
    }
    }
    return 0;
}

enum class Zeros
{
    fitted,  // the magnitude matched at DC, Nyquist and the band's frequency
    atDc,    // a high-pass: both zeros at DC
    onCircle // a notch: both zeros at the band's frequency
};

Zeros zerosOf (const Analog& a) noexcept
{
    if (juce::exactlyEqual (a.n0, 0.0) && juce::exactlyEqual (a.n1, 0.0))
        return Zeros::atDc;
    if (juce::exactlyEqual (a.n1, 0.0) && a.n0 > 0.0 && a.n2 > 0.0)
        return Zeros::onCircle;
    return Zeros::fitted;
}

/** Vicanek's matched section for an analog one at w0 (radians per sample). */
EqEngine::Coefficients matchedSection (const Analog& a, double w0) noexcept
{
    // Poles: the analog ones mapped exactly (z = e^sT).
    const auto natural = std::sqrt (a.d0 / a.d2);
    const auto zeta = a.d1 / (2.0 * std::sqrt (a.d0 * a.d2));
    const auto w = juce::jmin (natural * w0, 0.98 * pi);
    EqEngine::Coefficients c;
    if (zeta < 1.0)
        c.a1 = -2.0 * std::exp (-zeta * w) * std::cos (w * std::sqrt (1.0 - zeta * zeta));
    else
    {
        const auto r = std::sqrt (zeta * zeta - 1.0);
        c.a1 = -(std::exp (w * (-zeta + r)) + std::exp (w * (-zeta - r)));
    }
    c.a2 = std::exp (-2.0 * zeta * w);

    const auto a0 = juce::square (1.0 + c.a1 + c.a2), a1 = juce::square (1.0 - c.a1 + c.a2), a2 = -4.0 * c.a2;
    auto target = [&] (double omega) { return analogSquared (a, omega / w0); };
    const auto wm = juce::jlimit (1.0e-6, 0.97 * pi, w0);
    const auto phi1 = juce::square (std::sin (wm / 2.0)), phi0 = 1.0 - phi1, phi2 = 4.0 * phi0 * phi1;

    switch (zerosOf (a))
    {
    case Zeros::atDc:
    {
        // b (1 - z^-1)^2, its gain matched at the band's frequency.
        const auto b = std::sqrt (target (wm) * (a0 * phi0 + a1 * phi1 + a2 * phi2)) / (4.0 * phi1);
        c.b0 = c.b2 = b;
        c.b1 = -2.0 * b;
        return c;
    }
    case Zeros::onCircle:
    {
        // g (1 - 2 cos w0 z^-1 + z^-2): the null exactly at the band's frequency, the gain matched at DC.
        const auto g = (1.0 + c.a1 + c.a2) * (a.n0 / a.d0) / (2.0 - 2.0 * std::cos (w0));
        c.b0 = c.b2 = g;
        c.b1 = -2.0 * std::cos (w0) * g;
        return c;
    }
    case Zeros::fitted:
        break;
    }

    // Zeros: the magnitude matched at DC, at the band's frequency and halfway from there to Nyquist (for
    // a band near Nyquist, at Nyquist as Vicanek does). Halfway follows the analog shape above the band
    // more closely than Nyquist alone.
    const auto big0 = a0 * target (0.0);
    auto big1 = a1 * target (pi);
    auto big2 = (target (wm) * (a0 * phi0 + a1 * phi1 + a2 * phi2) - big0 * phi0 - big1 * phi1) / phi2;
    if (const auto wt = 0.5 * (wm + pi); wt > wm * 1.05)
    {
        const auto t1 = juce::square (std::sin (wt / 2.0)), t0 = 1.0 - t1, t2 = 4.0 * t0 * t1;
        const auto atBand = target (wm) * (a0 * phi0 + a1 * phi1 + a2 * phi2) - big0 * phi0;
        const auto above = target (wt) * (a0 * t0 + a1 * t1 + a2 * t2) - big0 * t0;
        const auto det = phi1 * t2 - phi2 * t1;
        big1 = juce::jmax (0.0, (atBand * t2 - phi2 * above) / det);
        big2 = (phi1 * above - t1 * atBand) / det;
    }

    const auto r0 = std::sqrt (big0), r1 = std::sqrt (big1);
    const auto sum = 0.5 * (r0 + r1);
    c.b0 = 0.5 * (sum + std::sqrt (juce::jmax (0.0, sum * sum + big2)));
    c.b1 = 0.5 * (r0 - r1);
    c.b2 = c.b0 > 0.0 ? -big2 / (4.0 * c.b0) : 0.0;
    return c;
}

/** A matched section. The poles are mapped exactly and the zeros only fitted, so the pair that is higher
    in frequency or more resonant should be the poles: when the zeros are, the reciprocal section (a boost
    for a cut, the same shape upside down) is matched and turned over. */
EqEngine::Coefficients matched (const Analog& a, double w0) noexcept
{
    if (a.n0 > 0.0 && a.n2 > 0.0 && a.n1 > 0.0)
    {
        const auto zeroFrequency = std::sqrt (a.n0 / a.n2), poleFrequency = std::sqrt (a.d0 / a.d2);
        const auto zeroDamping = a.n1 / (2.0 * std::sqrt (a.n0 * a.n2));
        const auto poleDamping = a.d1 / (2.0 * std::sqrt (a.d0 * a.d2));
        const auto swap = std::abs (zeroFrequency - poleFrequency) > 1.0e-9 * poleFrequency
                              ? poleFrequency > zeroFrequency
                              : zeroDamping < poleDamping;
        if (swap)
        {
            const auto r = matchedSection ({ a.d0, a.d1, a.d2, a.n0, a.n1, a.n2 }, w0);
            return { 1.0 / r.b0, r.a1 / r.b0, r.a2 / r.b0, r.b1 / r.b0, r.b2 / r.b0 };
        }
    }
    return matchedSection (a, w0);
}

/** The RBJ cookbook section for an analog one (bilinear transform, prewarped at the band's frequency). */
EqEngine::Coefficients bilinear (const Analog& a, double w0) noexcept
{
    // s = K (1 - z^-1) / (1 + z^-1), K = 1 / tan (w0 / 2) in the normalised s plane.
    const auto k = 1.0 / std::tan (juce::jmin (w0, 0.999 * pi) / 2.0), k2 = k * k;
    const auto b0 = a.n2 * k2 + a.n1 * k + a.n0, b1 = 2.0 * (a.n0 - a.n2 * k2), b2 = a.n2 * k2 - a.n1 * k + a.n0;
    const auto d0 = a.d2 * k2 + a.d1 * k + a.d0, d1 = 2.0 * (a.d0 - a.d2 * k2), d2 = a.d2 * k2 - a.d1 * k + a.d0;
    return { b0 / d0, b1 / d0, b2 / d0, d1 / d0, d2 / d0 };
}

template <typename Make> EqEngine::Design makeDesign (const EqBand& band, double sampleRate, Make&& make) noexcept
{
    EqEngine::Design d;
    std::array<Analog, EqEngine::maxStages> sections {};
    d.numStages = prototype (band, sections);
    const auto w0 = 2.0 * pi * juce::jlimit (10.0, 0.49 * sampleRate, (double)band.frequencyHz) / sampleRate;
    for (int k = 0; k < d.numStages; ++k)
        d.stages[(size_t)k] = make (sections[(size_t)k], w0);
    return d;
}
} // namespace

EqEngine::Design EqEngine::design (const EqBand& band, double sampleRate) noexcept
{
    return makeDesign (band, sampleRate, matched);
}

EqEngine::Design EqEngine::designBilinear (const EqBand& band, double sampleRate) noexcept
{
    return makeDesign (band, sampleRate, bilinear);
}

double EqEngine::analogMagnitude (const EqBand& band, double hz) noexcept
{
    std::array<Analog, maxStages> sections {};
    const auto n = prototype (band, sections);
    double squared = 1.0;
    for (int k = 0; k < n; ++k)
        squared *= analogSquared (sections[(size_t)k], hz / juce::jmax (1.0, (double)band.frequencyHz));
    return std::sqrt (squared);
}

double EqEngine::magnitude (const Design& d, double hz, double sampleRate) noexcept
{
    const auto z1 = std::polar (1.0, -2.0 * pi * hz / sampleRate), z2 = z1 * z1;
    double gain = 1.0;
    for (int k = 0; k < d.numStages; ++k)
    {
        const auto& c = d.stages[(size_t)k];
        gain *= std::abs ((c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2));
    }
    return gain;
}

double EqEngine::responseAt (const EqSettings& s, double hz, double sampleRate) noexcept
{
    auto gain = (double)juce::Decibels::decibelsToGain (s.outputDb, -100.0f);
    for (auto& band : s.bands)
        if (band.on)
            gain *= magnitude (design (band, sampleRate), hz, sampleRate);
    return gain;
}

void EqEngine::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    for (auto& b : bands)
        for (auto* s : { &b.logFrequency, &b.gainDb, &b.q, &b.wet })
            s->reset (sampleRate, glideSeconds);
    output.reset (sampleRate, glideSeconds);
    reset();
}

void EqEngine::reset() noexcept
{
    started = false;
    for (auto& b : bands)
    {
        b.z = {};
        b.needsDesign = true;
    }
    spectrumFifo.reset();
}

void EqEngine::updateBand (BandState& b, const EqBand& target, int samples) noexcept
{
    const auto logFrequency = std::log (juce::jlimit (10.0f, 24000.0f, target.frequencyHz));
    const auto wet = target.on ? 1.0f : 0.0f;
    if (! started)
    {
        b.logFrequency.setCurrentAndTargetValue (logFrequency);
        b.gainDb.setCurrentAndTargetValue (target.gainDb);
        b.q.setCurrentAndTargetValue (target.q);
        b.wet.setCurrentAndTargetValue (wet);
        b.needsDesign = true;
    }
    b.logFrequency.setTargetValue (logFrequency);
    b.gainDb.setTargetValue (target.gainDb);
    b.q.setTargetValue (target.q);
    b.wet.setTargetValue (wet);

    if (target.type != b.designed.type || target.slope != b.designed.slope)
        b.needsDesign = true;
    for (auto* s : { &b.logFrequency, &b.gainDb, &b.q })
        if (s->isSmoothing())
        {
            s->skip (samples);
            b.needsDesign = true;
        }

    // A band that is off and silent is skipped; it starts from rest when it comes back.
    if (! target.on && ! b.wet.isSmoothing() && b.wet.getCurrentValue() <= 0.0f)
        b.z = {};

    if (b.needsDesign)
    {
        auto now = target;
        now.frequencyHz = std::exp (b.logFrequency.getCurrentValue());
        now.gainDb = b.gainDb.getCurrentValue();
        now.q = b.q.getCurrentValue();
        b.current = design (now, sampleRate);
        b.designed = now;
        b.needsDesign = false;
    }
}

void EqEngine::process (juce::AudioBuffer<float>& buffer, const EqSettings& s) noexcept
{
    const auto numSamples = buffer.getNumSamples();
    const auto channels = juce::jmin (maxChannels, buffer.getNumChannels());
    const auto outputGain = juce::Decibels::decibelsToGain (s.outputDb, -100.0f);
    if (! started)
        output.setCurrentAndTargetValue (outputGain);
    output.setTargetValue (outputGain);

    for (int start = 0; start < numSamples; start += updateInterval)
    {
        const auto length = juce::jmin (updateInterval, numSamples - start);
        for (size_t i = 0; i < bands.size(); ++i)
            updateBand (bands[i], s.bands[i], length);
        started = true;

        for (auto& b : bands)
        {
            const auto wetFrom = b.wet.getCurrentValue();
            const auto wetTo = b.wet.skip (length);
            if ((wetFrom <= 0.0f && wetTo <= 0.0f) || b.current.numStages == 0)
                continue;
            const auto wetStep = (wetTo - wetFrom) / (float)length;
            for (int c = 0; c < channels; ++c)
            {
                auto* x = buffer.getWritePointer (c, start);
                auto& z = b.z[(size_t)c];
                for (int i = 0; i < length; ++i)
                {
                    const auto in = (double)x[i];
                    auto y = in;
                    for (int k = 0; k < b.current.numStages; ++k)
                    {
                        const auto& co = b.current.stages[(size_t)k];
                        auto& st = z[(size_t)k];
                        const auto out = co.b0 * y + st[0];
                        st[0] = co.b1 * y - co.a1 * out + st[1];
                        st[1] = co.b2 * y - co.a2 * out;
                        y = out;
                    }
                    const auto w = (double)(wetFrom + wetStep * (float)(i + 1));
                    x[i] = (float)(in + w * (y - in));
                }
            }
        }

        for (int i = 0; i < length; ++i)
        {
            const auto g = output.getNextValue();
            for (int c = 0; c < channels; ++c)
                buffer.getWritePointer (c, start)[i] *= g;
        }
    }

    if (spectrumWanted.load (std::memory_order_relaxed) && channels > 0)
    {
        const auto scope = spectrumFifo.write (juce::jmin (numSamples, spectrumFifo.getFreeSpace()));
        auto put = [&] (int from, int count, int offset)
        {
            for (int i = 0; i < count; ++i)
            {
                float sum = 0.0f;
                for (int c = 0; c < channels; ++c)
                    sum += buffer.getSample (c, offset + i);
                spectrumBuffer[(size_t)from + (size_t)i] = sum / (float)channels;
            }
        };
        put (scope.startIndex1, scope.blockSize1, 0);
        put (scope.startIndex2, scope.blockSize2, scope.blockSize1);
    }
}

int EqEngine::readSpectrumSamples (float* destination, int max) noexcept
{
    const auto scope = spectrumFifo.read (juce::jmin (max, spectrumFifo.getNumReady()));
    std::copy_n (spectrumBuffer.data() + scope.startIndex1, scope.blockSize1, destination);
    std::copy_n (spectrumBuffer.data() + scope.startIndex2, scope.blockSize2, destination + scope.blockSize1);
    return scope.blockSize1 + scope.blockSize2;
}

} // namespace spinus::dsp
