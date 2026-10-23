#include "AudioGuardPlugin.h"

#if defined(__FAST_MATH__)
#error "AudioGuardPlugin relies on IEEE NaN/infinity behaviour; do not build with -ffast-math"
#endif

namespace spinus::engine
{

namespace te = tracktion;

const char* AudioGuardPlugin::xmlTypeName = "spinusAudioGuard";

AudioGuardPlugin::AudioGuardPlugin (te::PluginCreationInfo info) : te::Plugin (std::move (info)) {}

AudioGuardPlugin::~AudioGuardPlugin() { notifyListenersOfDeletion(); }

juce::ValueTree AudioGuardPlugin::create()
{
    juce::ValueTree v (te::IDs::PLUGIN);
    v.setProperty (te::IDs::type, xmlTypeName, nullptr);
    return v;
}

bool AudioGuardPlugin::containsInvalidSamples (const juce::AudioBuffer<float>& buffer, int start,
                                               int numSamples) noexcept
{
    // x * 0 is 0 for every finite x and NaN for NaN or infinity, so one sum covers both cases without
    // a branch per sample (the loop vectorises).
    float sum = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const auto* data = buffer.getReadPointer (ch, start);
        for (int i = 0; i < numSamples; ++i)
            sum += data[i] * 0.0f;
    }

    return ! std::isfinite (sum);
}

float AudioGuardPlugin::meanSquare (const juce::AudioBuffer<float>& buffer, int start, int numSamples) noexcept
{
    if (numSamples <= 0 || buffer.getNumChannels() == 0)
        return 0.0f;
    float sum = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const auto* data = buffer.getReadPointer (ch, start);
        for (int i = 0; i < numSamples; ++i)
            sum += data[i] * data[i];
    }
    return sum / (float)(numSamples * buffer.getNumChannels());
}

void AudioGuardPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
        return;

    if (probe)
    {
        if (meter != nullptr)
        {
            meter->inputMeanSquare.store (meanSquare (*fc.destBuffer, fc.bufferStartSample, fc.bufferNumSamples),
                                          std::memory_order_relaxed);
            meter->startTicks.store (juce::Time::getHighResolutionTicks(), std::memory_order_release);
        }
        return;
    }

    if (containsInvalidSamples (*fc.destBuffer, fc.bufferStartSample, fc.bufferNumSamples))
    {
        fc.destBuffer->clear (fc.bufferStartSample, fc.bufferNumSamples);
        silencedBlocks.fetch_add (1, std::memory_order_relaxed);
    }

    if (meter != nullptr)
        measureAfter (*fc.destBuffer, fc.bufferStartSample, fc.bufferNumSamples);
}

void AudioGuardPlugin::measureAfter (const juce::AudioBuffer<float>& buffer, int start, int numSamples) noexcept
{
    auto& m = *meter;
    const auto blockSeconds = (double)numSamples / sampleRate;

    // Time since the probe: the device's own processing (plus any wait for it, e.g. a plugin in its own process).
    const auto started = m.startTicks.load (std::memory_order_acquire);
    if (started != 0)
    {
        const auto seconds = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - started);
        const auto share = (float)juce::jlimit (0.0, 4.0, seconds / blockSeconds);
        const auto load = m.load.load (std::memory_order_relaxed);
        m.load.store (load + 0.05f * (share - load), std::memory_order_relaxed);
    }

    // Gain change: the level out against the level in. Its resting value (no reduction) is the highest gain seen
    // lately; it drifts down slowly, so a change of the device's own volume is learned in a few seconds.
    constexpr float quiet = 1.0e-7f; // -70 dBFS
    constexpr float driftDbPerSecond = 1.0f, releaseDbPerSecond = 20.0f;
    const auto in = m.inputMeanSquare.load (std::memory_order_relaxed);
    const auto out = meanSquare (buffer, start, numSamples);
    auto shown = m.gainReductionDb.load (std::memory_order_relaxed);
    shown = juce::jmin (0.0f, shown + releaseDbPerSecond * (float)blockSeconds);
    if (in > quiet)
    {
        const auto gainDb = 10.0f * std::log10 (juce::jmax (out, 1.0e-12f) / in);
        m.restingGainDb =
            m.hasResting ? juce::jmax (gainDb, m.restingGainDb - driftDbPerSecond * (float)blockSeconds) : gainDb;
        m.hasResting = true;
        shown = juce::jmin (shown, gainDb - m.restingGainDb);
    }
    m.gainReductionDb.store (shown, std::memory_order_relaxed);
}

} // namespace spinus::engine
