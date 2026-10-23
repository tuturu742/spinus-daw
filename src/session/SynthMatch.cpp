#include "SynthMatch.h"

#include "engine/ParameterIds.h"

#include <juce_dsp/juce_dsp.h>

#include <numeric>

namespace spinus
{

namespace te = tracktion;

//==============================================================================
namespace
{
/** Log-magnitude frames of a mono signal (frames x bins): Hann windows, a quarter of a frame apart. */
std::vector<std::vector<float>> logSpectra (const float* data, int length, int order)
{
    const int size = (int)(1u << (unsigned)order);
    juce::dsp::FFT fft (order);
    juce::dsp::WindowingFunction<float> window ((size_t)size, juce::dsp::WindowingFunction<float>::hann, false);
    std::vector<std::vector<float>> frames;
    std::vector<float> buffer ((size_t)size * 2);
    for (int start = 0; start + size <= length; start += size / 4)
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        std::copy (data + start, data + start + size, buffer.begin());
        window.multiplyWithWindowingTable (buffer.data(), (size_t)size);
        fft.performFrequencyOnlyForwardTransform (buffer.data(), true);
        std::vector<float> frame ((size_t)size / 2 + 1);
        for (size_t i = 0; i < frame.size(); ++i)
            frame[i] = std::log (buffer[i] + 1.0e-4f);
        frames.push_back (std::move (frame));
    }
    return frames;
}
} // namespace

double spectralDistance (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    const auto length = juce::jmin (a.getNumSamples(), b.getNumSamples());
    if (length < 4096 || a.getNumChannels() == 0 || b.getNumChannels() == 0)
        return 1.0e6;
    // Equally loud (the level is the mix's business, not the sound's).
    auto levelled = [length] (const juce::AudioBuffer<float>& x)
    {
        std::vector<float> v (x.getReadPointer (0), x.getReadPointer (0) + length);
        const auto rms = std::sqrt (std::inner_product (v.begin(), v.end(), v.begin(), 0.0) / (double)length);
        const auto gain = rms > 1.0e-6 ? (float)(0.1 / rms) : 0.0f;
        for (auto& s : v)
            s *= gain;
        return v;
    };
    const auto x = levelled (a), y = levelled (b);
    double total = 0.0;
    for (int order : { 8, 10, 12 })
    {
        const auto sx = logSpectra (x.data(), length, order), sy = logSpectra (y.data(), length, order);
        double sum = 0.0;
        size_t count = 0;
        for (size_t f = 0; f < sx.size(); ++f)
            for (size_t i = 0; i < sx[f].size(); ++i)
            {
                sum += std::abs (sx[f][i] - sy[f][i]);
                ++count;
            }
        total += count > 0 ? sum / (double)count : 0.0;
    }

    // How the loudness moves (attack, decay, release): 10 ms levels in dB, floored 60 dB down, an eighth.
    const int hop = 480;
    double envelope = 0.0;
    int frames = 0;
    for (int start = 0; start + hop <= length; start += hop, ++frames)
    {
        auto level = [&] (const std::vector<float>& v)
        {
            const auto power = std::inner_product (v.begin() + start, v.begin() + start + hop, v.begin() + start, 0.0);
            return juce::jmax (-60.0, 10.0 * std::log10 (power / hop + 1.0e-12) + 20.0); // RMS 0.1 is 0 dB
        };
        envelope += std::abs (level (x) - level (y));
    }
    return total / 3.0 + (frames > 0 ? envelope / frames / 8.0 : 0.0);
}

//==============================================================================
Excerpt chooseExcerpt (const std::vector<HiddenInstrument::Note>& notes, double longest)
{
    Excerpt best;
    double bestTime = -1.0;
    for (auto& first : notes)
    {
        const auto start = first.start, end = first.start + longest;
        double time = 0.0;
        for (auto& n : notes)
            if (n.start >= start && n.start < end)
                time += std::min (n.start + n.length, end) - n.start;
        if (time > bestTime + 1.0e-9)
        {
            bestTime = time;
            best = { start, 0.0, {} };
        }
    }
    if (bestTime < 0.0)
        return {};
    for (auto& n : notes)
        if (n.start >= best.start && n.start < best.start + longest)
        {
            auto note = n;
            note.start -= best.start;
            note.length = std::min (n.start + n.length, best.start + longest) - n.start;
            best.notes.push_back (note);
            best.length = std::max (best.length, note.start + note.length);
        }
    return best;
}

juce::AudioBuffer<float> readExcerpt (const juce::File& file, double start, double length, double tail)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0)
        return {};
    const auto from = (juce::int64)(start * reader->sampleRate);
    const auto count = (int)((length + tail) * reader->sampleRate);
    juce::AudioBuffer<float> source ((int)juce::jmax (1u, juce::jmin (2u, reader->numChannels)), count);
    reader->read (&source, 0, count, from, true, source.getNumChannels() > 1);
    if (source.getNumChannels() > 1)
    {
        source.addFrom (0, 0, source, 1, 0, count);
        source.applyGain (0, 0, count, 0.5f);
    }
    const auto ratio = reader->sampleRate / HiddenInstrument::sampleRate;
    juce::AudioBuffer<float> result (1, (int)((double)count / ratio));
    juce::LagrangeInterpolator resampler;
    resampler.process (ratio, source.getReadPointer (0), result.getWritePointer (0), result.getNumSamples(), count, 0);
    return result;
}

//==============================================================================
double Cmaes::fold (double x)
{
    // Reflected at 0 and 1 (a triangle wave): a step past an edge comes back, so it still changes something.
    const auto t = std::fmod (std::abs (x), 2.0);
    return t <= 1.0 ? t : 2.0 - t;
}

Cmaes::Cmaes (std::vector<double> start, double initialSigma, uint32_t seed)
    : n (start.size()), mean (std::move (start)), sigma (initialSigma), random (seed)
{
    const auto dims = (double)n;
    lambda = 4 + (int)std::floor (3.0 * std::log (dims));
    mu = lambda / 2;
    for (int i = 0; i < mu; ++i)
        weights.push_back (std::log (mu + 0.5) - std::log (i + 1.0));
    const auto sum = std::accumulate (weights.begin(), weights.end(), 0.0);
    for (auto& w : weights)
        w /= sum;
    muEff = 1.0 / std::inner_product (weights.begin(), weights.end(), weights.begin(), 0.0);
    cSigma = (muEff + 2.0) / (dims + muEff + 5.0);
    dSigma = 1.0 + 2.0 * std::max (0.0, std::sqrt ((muEff - 1.0) / (dims + 1.0)) - 1.0) + cSigma;
    cC = (4.0 + muEff / dims) / (dims + 4.0 + 2.0 * muEff / dims);
    // sep-CMA-ES learns the diagonal faster: (n + 2) / 3 times the full-covariance rates.
    c1 = std::min (1.0, (dims + 2.0) / 3.0 * 2.0 / ((dims + 1.3) * (dims + 1.3) + muEff));
    cMu = std::min (1.0 - c1,
                    (dims + 2.0) / 3.0 * 2.0 * (muEff - 2.0 + 1.0 / muEff) / ((dims + 2.0) * (dims + 2.0) + muEff));
    chiN = std::sqrt (dims) * (1.0 - 1.0 / (4.0 * dims) + 1.0 / (21.0 * dims * dims));
    diagonal.assign (n, 1.0);
    pathSigma.assign (n, 0.0);
    pathC.assign (n, 0.0);
    best = mean;
}

std::vector<std::vector<double>> Cmaes::ask()
{
    std::normal_distribution<double> normal;
    lastZ.clear();
    std::vector<std::vector<double>> points;
    for (int k = 0; k < lambda; ++k)
    {
        std::vector<double> z (n), x (n);
        for (size_t i = 0; i < n; ++i)
        {
            z[i] = normal (random);
            x[i] = mean[i] + sigma * std::sqrt (diagonal[i]) * z[i];
        }
        lastZ.push_back (std::move (z));
        points.push_back (std::move (x));
    }
    return points;
}

void Cmaes::tell (const std::vector<std::vector<double>>& points, const std::vector<double>& losses)
{
    std::vector<size_t> order (points.size());
    std::iota (order.begin(), order.end(), 0);
    std::sort (order.begin(), order.end(), [&] (auto a, auto b) { return losses[a] < losses[b]; });
    if (losses[order[0]] < bestLoss)
    {
        bestLoss = losses[order[0]];
        best = points[order[0]];
        for (auto& v : best)
            v = fold (v);
    }

    std::vector<double> zw (n, 0.0), yw (n, 0.0);
    for (int k = 0; k < mu; ++k)
        for (size_t i = 0; i < n; ++i)
        {
            const auto& z = lastZ[order[(size_t)k]];
            zw[i] += weights[(size_t)k] * z[i];
            yw[i] += weights[(size_t)k] * std::sqrt (diagonal[i]) * z[i];
        }
    for (size_t i = 0; i < n; ++i)
        mean[i] += sigma * yw[i];

    double norm = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        pathSigma[i] = (1.0 - cSigma) * pathSigma[i] + std::sqrt (cSigma * (2.0 - cSigma) * muEff) * zw[i];
        norm += pathSigma[i] * pathSigma[i];
    }
    norm = std::sqrt (norm);
    const bool steady = norm < (1.4 + 2.0 / ((double)n + 1.0)) * chiN;
    for (size_t i = 0; i < n; ++i)
    {
        pathC[i] = (1.0 - cC) * pathC[i] + (steady ? std::sqrt (cC * (2.0 - cC) * muEff) * yw[i] : 0.0);
        double rankMu = 0.0;
        for (int k = 0; k < mu; ++k)
        {
            const auto y = std::sqrt (diagonal[i]) * lastZ[order[(size_t)k]][i];
            rankMu += weights[(size_t)k] * y * y;
        }
        diagonal[i] = (1.0 - c1 - cMu) * diagonal[i] +
                      c1 * (pathC[i] * pathC[i] + (steady ? 0.0 : cC * (2.0 - cC) * diagonal[i])) + cMu * rankMu;
        diagonal[i] = juce::jlimit (1.0e-8, 1.0e4, diagonal[i]);
    }
    sigma = juce::jlimit (1.0e-4, 0.5, sigma * std::exp ((cSigma / dSigma) * (norm / chiN - 1.0)));
}

//==============================================================================
juce::String SynthMatcher::getParameterId (const juce::AudioProcessorParameter& parameter)
{
    return engine::getParameterId (parameter);
}

juce::AudioProcessorParameter* SynthMatcher::findParameter (const juce::AudioPluginInstance& instance,
                                                            const juce::String& id)
{
    return engine::ParameterFinder (instance).findSaved (id);
}

SynthMatcher::SynthMatcher (Session& session, SynthProfile p)
    : juce::Thread ("Spinus synth match"), profile (std::move (p)), instrument (session, profile.stockType)
{
    findSettings();
}

SynthMatcher::SynthMatcher (Session& session, const model::Device& device, std::optional<SynthProfile> p)
    : juce::Thread ("Spinus synth match"), instrument (session, device)
{
    if (p)
        profile = std::move (*p);
    else if (auto* instance = instrument.getInstance())
    {
        // None of its own: one made from the names of the plugin's parameters.
        std::vector<SynthProfile::Parameter> described;
        for (auto* parameter : instance->getParameters())
            described.push_back (SynthProfile::describe (*parameter));
        const auto name = device.kind == model::Device::Kind::plugin ? device.plugin.name : instance->getName();
        profile = SynthProfile::makeAutomatic (name, described).value_or (SynthProfile {});
        profile.name = name;
        profile.automatic = true;
    }
    findSettings();
}

void SynthMatcher::findSettings()
{
    auto* instance = instrument.getInstance();
    if (instance == nullptr)
        return;
    const engine::ParameterFinder finder (*instance);
    for (auto& setting : profile.settings)
    {
        auto* found = setting.id.isNotEmpty() ? finder.findSaved (setting.id) : nullptr;
        for (auto* parameter : instance->getParameters())
            if (found == nullptr && setting.name.isNotEmpty() && parameter->getName (200) == setting.name)
                found = parameter;
        if (found != nullptr)
            parameters.emplace_back (found, setting);
    }
}

SynthMatcher::~SynthMatcher()
{
    cancelPendingUpdate();
    stopThread (10000);
}

void SynthMatcher::start (juce::AudioBuffer<float> wanted, std::vector<HiddenInstrument::Note> playing, int tries,
                          std::function<void (int, double)> progress, std::function<void()> finished)
{
    target = std::move (wanted);
    notes = std::move (playing);
    budget = tries;
    onProgress = std::move (progress);
    onFinished = std::move (finished);
    startThread();
}

std::vector<float> SynthMatcher::toValues (const std::vector<double>& point) const
{
    std::vector<float> values;
    for (size_t i = 0; i < parameters.size(); ++i)
    {
        const auto& s = parameters[i].second;
        auto v = s.min + (s.max - s.min) * (float)Cmaes::fold (point[i]);
        if (s.steps > 1)
            v = std::round (v * (float)(s.steps - 1)) / (float)(s.steps - 1);
        values.push_back (v);
    }
    return values;
}

double SynthMatcher::tryValues (const std::vector<float>& values)
{
    // Spinus's own instruments keep their settings up to date through the parameters' listeners, so they
    // must be told (CLAP plugins take the change on their next block either way).
    for (size_t i = 0; i < parameters.size(); ++i)
        parameters[i].first->setValueNotifyingHost (values[i]);
    return spectralDistance (instrument.render (notes, 0.2), target);
}

void SynthMatcher::run()
{
    // Start where the synth is now (its default or the preset), in the profile's ranges.
    std::vector<double> start;
    start.reserve (parameters.size());
    for (auto& [parameter, s] : parameters)
        start.push_back (
            s.max > s.min ? juce::jlimit (0.0, 1.0, (double)((parameter->getValue() - s.min) / (s.max - s.min))) : 0.0);
    const auto startValues = toValues (start);
    result.startDistance = tryValues (startValues);
    double bestDistance = result.startDistance;
    auto bestValues = startValues;
    int tries = 1;

    // First each setting on its own at a few values, keeping what helps (catches one big difference, such as
    // a slow attack, quickly); then all together with CMA-ES, from there, in small steps.
    auto point = start;
    for (size_t i = 0; i < parameters.size() && tries < budget && ! threadShouldExit(); ++i)
    {
        const auto steps = parameters[i].second.steps;
        const auto count = steps > 1 && steps <= 8 ? steps : 5;
        for (int k = 0; k < count && tries < budget && ! threadShouldExit(); ++k)
        {
            auto candidate = point;
            candidate[i] = (double)k / (double)(count - 1);
            const auto values = toValues (candidate);
            const auto distance = tryValues (values);
            ++tries;
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestValues = values;
                point = candidate;
            }
        }
        triesDone = tries;
        bestSoFar = bestDistance;
        triggerAsyncUpdate();
    }

    if (! parameters.empty())
    {
        Cmaes search (point, 0.1);
        while (tries < budget && ! threadShouldExit())
        {
            const auto points = search.ask();
            std::vector<double> losses;
            for (auto& candidate : points)
            {
                if (threadShouldExit())
                    break;
                const auto values = toValues (candidate);
                const auto distance = tryValues (values);
                losses.push_back (distance);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    bestValues = values;
                }
                ++tries;
            }
            if (losses.size() < points.size())
                break;
            search.tell (points, losses);
            triesDone = tries;
            bestSoFar = bestDistance;
            triggerAsyncUpdate();
        }
    }

    result.settings.clear();
    for (size_t i = 0; i < parameters.size(); ++i)
        result.settings.emplace_back (getParameterId (*parameters[i].first), bestValues[i]);
    result.distance = bestDistance;
    result.tries = tries;
    triesDone = tries;
    bestSoFar = bestDistance;
    isFinished = true;
    triggerAsyncUpdate();
}

void SynthMatcher::handleAsyncUpdate()
{
    if (onProgress)
        onProgress (triesDone.load(), bestSoFar.load());
    if (isFinished.exchange (false))
        if (auto done = std::move (onFinished))
            done();
}

int SynthMatcher::apply (Session& session, model::ChannelId channel, const Result& found)
{
    const auto* c = session.getProject().findChannel (channel);
    if (c == nullptr || ! c->instrument)
        return 0;
    auto* plugin = dynamic_cast<te::ExternalPlugin*> (
        session.getAdapter().getDevicePlugin (c->instrument.value_or (model::Device {}).id));
    auto* instance = plugin != nullptr ? plugin->getAudioPluginInstance() : nullptr;
    if (instance == nullptr)
        return 0;
    // Found by the same IDs the match kept (the channel's instrument is the same plugin as the hidden one).
    int set = 0;
    for (auto& [id, value] : found.settings)
        if (auto* parameter = findParameter (*instance, id))
        {
            parameter->setValueNotifyingHost (value);
            ++set;
        }
    return set;
}

} // namespace spinus
