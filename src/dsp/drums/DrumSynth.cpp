#include "DrumSynth.h"

namespace spinus::dsp
{

namespace
{
/** Builds one sound sample by sample: f (t, noise) -> value; then a short fade at the end. */
template <typename Fn> juce::AudioBuffer<float> render (double rate, double seconds, int seed, Fn&& fn)
{
    juce::AudioBuffer<float> b (1, (int)(seconds * rate));
    juce::Random random (seed);
    for (int i = 0; i < b.getNumSamples(); ++i)
        b.setSample (0, i, (float)fn (i / rate, random.nextFloat() * 2.0f - 1.0f));
    return b;
}

/** A sine whose frequency falls from `from` to `to` (Hz), as in a kick or tom. */
struct Sweep
{
    double from, to, speed, phase = 0.0, rate;
    double next (double t)
    {
        const auto f = to + (from - to) * std::exp (-t * speed);
        phase += 2.0 * M_PI * f / rate;
        return std::sin (phase);
    }
};

juce::AudioBuffer<float> filtered (juce::AudioBuffer<float> b, const juce::IIRCoefficients& c)
{
    juce::IIRFilter f;
    f.setCoefficients (c);
    f.processSamples (b.getWritePointer (0), b.getNumSamples());
    return b;
}

void normalise (juce::AudioBuffer<float>& b, float peak)
{
    if (const auto m = b.getMagnitude (0, b.getNumSamples()); m > 0.0f)
        b.applyGain (peak / m);
}

juce::AudioBuffer<float> kick (double rate, double length, double punch)
{
    Sweep s { 160.0, 45.0, punch, 0.0, rate };
    return render (rate, length, 1,
                   [&] (double t, float n)
                   {
                       const auto click = t < 0.003 ? n * 0.3 * (1.0 - t / 0.003) : 0.0;
                       return s.next (t) * std::exp (-t * 4.0 / length) + click;
                   });
}

juce::AudioBuffer<float> snare (double rate, double noiseAmount, int seed)
{
    Sweep s { 240.0, 180.0, 40.0, 0.0, rate };
    auto b = render (rate, 0.35, seed, [&] (double t, float n)
                     { return 0.6 * s.next (t) * std::exp (-t * 30.0) + noiseAmount * n * std::exp (-t * 14.0); });
    return filtered (std::move (b), juce::IIRCoefficients::makeHighPass (rate, 150.0));
}

juce::AudioBuffer<float> noiseHit (double rate, double seconds, double decay, double highPass, int seed)
{
    auto b = render (rate, seconds, seed, [&] (double t, float n) { return n * std::exp (-t * decay); });
    b = filtered (std::move (b), juce::IIRCoefficients::makeHighPass (rate, highPass));
    return filtered (std::move (b), juce::IIRCoefficients::makeHighPass (rate, highPass));
}

juce::AudioBuffer<float> tom (double rate, double frequency)
{
    Sweep s { frequency * 1.4, frequency, 18.0, 0.0, rate };
    return render (rate, 0.5, 5, [&] (double t, float) { return s.next (t) * std::exp (-t * 7.0); });
}
} // namespace

std::vector<SynthDrum> makeSynthKit (double rate)
{
    std::vector<SynthDrum> kit;
    auto add = [&] (const char* name, juce::AudioBuffer<float> audio, float peak = 0.9f, int choke = 0)
    {
        normalise (audio, peak);
        // A short fade to exactly zero at the end, so a sound never ends with a click.
        const int fade = juce::jmin (audio.getNumSamples(), (int)(0.005 * rate));
        audio.applyGainRamp (audio.getNumSamples() - fade, fade, 1.0f, 0.0f);
        audio.setSample (0, audio.getNumSamples() - 1, 0.0f);
        kit.push_back ({ name, std::move (audio), choke });
    };

    add ("Kick", kick (rate, 0.5, 30.0));
    add ("Snare", snare (rate, 0.5, 2));
    add ("Clap",
         filtered (render (rate, 0.4, 3,
                           [] (double t, float n)
                           {
                               // Three quick bursts, then a tail: several hands, slightly apart.
                               const auto burst = std::fmod (t, 0.011) < 0.004 && t < 0.033 ? 1.0 : 0.0;
                               return n * (burst + std::exp (-(t - 0.033) * 18.0) * (t >= 0.033 ? 0.7 : 0.0));
                           }),
                   juce::IIRCoefficients::makeBandPass (rate, 1200.0, 1.2)),
         0.8f);
    add ("Closed Hat", noiseHit (rate, 0.12, 60.0, 7000.0, 4), 0.6f, 1);
    add ("Open Hat", noiseHit (rate, 0.6, 7.0, 7000.0, 4), 0.55f, 1);
    add ("Low Tom", tom (rate, 80.0));
    add ("Mid Tom", tom (rate, 120.0));
    add ("High Tom", tom (rate, 170.0));
    add ("Rim",
         filtered (render (rate, 0.06, 6, [&] (double t, float n)
                           { return (std::sin (2.0 * M_PI * 1700.0 * t) + 0.5 * n) * std::exp (-t * 90.0); }),
                   juce::IIRCoefficients::makeHighPass (rate, 400.0)),
         0.7f);
    add ("Cowbell",
         filtered (render (rate, 0.35, 7,
                           [&] (double t, float)
                           {
                               auto square = [t] (double f)
                               { return std::sin (2.0 * M_PI * f * t) > 0.0 ? 1.0 : -1.0; };
                               return (square (540.0) + square (800.0)) * 0.5 * std::exp (-t * 12.0);
                           }),
                   juce::IIRCoefficients::makeBandPass (rate, 900.0, 2.0)),
         0.6f);
    add ("Crash", noiseHit (rate, 2.0, 2.2, 4000.0, 8), 0.5f);
    add ("Shaker",
         filtered (render (rate, 0.2, 9, [] (double t, float n)
                           { return n * (t < 0.03 ? t / 0.03 : std::exp (-(t - 0.03) * 25.0)); }),
                   juce::IIRCoefficients::makeBandPass (rate, 6000.0, 1.0)),
         0.5f);
    add ("Kick 2", kick (rate, 0.25, 60.0));
    add ("Snare 2", snare (rate, 0.9, 10));
    add ("Ride",
         render (rate, 1.5, 11,
                 [] (double t, float n)
                 {
                     const auto bell = std::sin (2.0 * M_PI * 3100.0 * t) + std::sin (2.0 * M_PI * 4250.0 * t);
                     return (0.25 * bell + 0.35 * n) * std::exp (-t * 3.0);
                 }),
         0.45f);
    add ("Perc",
         render (rate, 0.12, 12,
                 [] (double t, float) { return std::sin (2.0 * M_PI * 1000.0 * t) * std::exp (-t * 40.0); }),
         0.7f);
    return kit;
}

} // namespace spinus::dsp
