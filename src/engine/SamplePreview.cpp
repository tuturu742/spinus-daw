#include "SamplePreview.h"

namespace spinus::engine
{

/** Tracktion's global output processor: adds the preview after the song has been rendered. */
class SamplePreview::Output final : public juce::AudioProcessor
{
public:
    explicit Output (SamplePreview& p) : preview (p) {}

    void prepareToPlay (double rate, int) override
    {
        if (rate > 0.0)
            preview.setSampleRate (rate);
    }
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        preview.mix (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples());
    }

    const juce::String getName() const override { return "Spinus sample preview"; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    SamplePreview& preview;
};

SamplePreview::SamplePreview (tracktion::DeviceManager* d) : devices (d)
{
    if (devices != nullptr && devices->getGlobalOutputAudioProcessor() == nullptr)
    {
        auto processor = std::make_unique<Output> (*this);
        output = processor.get();
        devices->setGlobalOutputAudioProcessor (std::move (processor));
    }
    startTimerHz (10);
}

SamplePreview::~SamplePreview()
{
    // Taken out under the audio callback's lock: it is not called again.
    if (devices != nullptr && output != nullptr && devices->getGlobalOutputAudioProcessor() == output)
        devices->setGlobalOutputAudioProcessor (nullptr);
    stopTimer();
    freeRetired();
    delete pending.exchange (nullptr);
    delete current;
}

bool SamplePreview::play (const juce::File& f, juce::AudioFormatManager& formats)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return false;

    const auto length = (int)std::min (reader->lengthInSamples, (juce::int64)(maxSeconds * reader->sampleRate));
    juce::AudioBuffer<float> source ((int)juce::jlimit (1u, 2u, reader->numChannels), length);
    reader->read (&source, 0, length, 0, true, source.getNumChannels() > 1);

    // At the device's rate, in stereo.
    const auto rate = sampleRate.load();
    const auto ratio = reader->sampleRate / rate;
    const auto outLength = juce::jmax (1, (int)((double)length / ratio));
    auto sound = std::make_unique<Sound>();
    sound->audio.setSize (2, outLength);
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto* in = source.getReadPointer (juce::jmin (ch, source.getNumChannels() - 1));
        auto* out = sound->audio.getWritePointer (ch);
        if (juce::approximatelyEqual (ratio, 1.0))
            juce::FloatVectorOperations::copy (out, in, juce::jmin (length, outLength));
        else
        {
            juce::LagrangeInterpolator interpolator;
            interpolator.process (ratio, in, out, outLength, length, 0);
        }
    }
    sound->audio.applyGain (gain);

    // A short fade at the end, so a cut-off long file does not click.
    const auto fade = juce::jmin (outLength, (int)(0.01 * rate));
    sound->audio.applyGainRamp (outLength - fade, fade, 1.0f, 0.0f);

    file = f;
    // Without a sound card nothing plays it, so it is not "playing" (rendered by hand in tests: no manager).
    playing.store (devices == nullptr ||
                   (output != nullptr && devices->deviceManager.getCurrentAudioDevice() != nullptr));
    publish (sound.release());
    return true;
}

void SamplePreview::stop()
{
    file = juce::File();
    playing.store (false);
    publish (new Sound()); // empty: silence
}

void SamplePreview::publish (Sound* sound)
{
    // A sound published before and not yet taken by the audio thread was never seen by it.
    delete pending.exchange (sound);
}

void SamplePreview::freeRetired()
{
    const auto scope = retiredFifo.read (retiredFifo.getNumReady());
    for (int i = 0; i < scope.blockSize1; ++i)
        delete retired[(size_t)scope.startIndex1 + (size_t)i];
    for (int i = 0; i < scope.blockSize2; ++i)
        delete retired[(size_t)scope.startIndex2 + (size_t)i];
}

void SamplePreview::mix (float* const* outputs, int numChannels, int numSamples) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    // A new sound replaces the one playing, if the old one can be handed back to be freed.
    if (pending.load() != nullptr && retiredFifo.getFreeSpace() > 0)
        if (auto* next = pending.exchange (nullptr))
        {
            if (current != nullptr)
            {
                const auto scope = retiredFifo.write (1);
                retired[(size_t)(scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)] = current;
            }
            current = next;
            position = 0;
        }

    if (current == nullptr || position >= current->audio.getNumSamples())
        return;

    const auto count = juce::jmin (numSamples, current->audio.getNumSamples() - position);
    for (int ch = 0; ch < numChannels; ++ch)
        if (outputs[ch] != nullptr)
            juce::FloatVectorOperations::copy (outputs[ch],
                                               current->audio.getReadPointer (juce::jmin (ch, 1), position), count);
    position += count;
    if (position >= current->audio.getNumSamples())
        playing.store (false);
}

} // namespace spinus::engine
