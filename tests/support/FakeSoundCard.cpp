#include "FakeSoundCard.h"

#include <chrono>

namespace spinus::test
{

class FakeDevice final : public juce::AudioIODevice
{
public:
    FakeDevice (const juce::String& deviceName, const juce::String& input, FakeSoundCard& c)
        : juce::AudioIODevice (deviceName, FakeSoundCard::typeName), card (c), inputName (input)
    {
    }
    ~FakeDevice() override { close(); }

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override
    {
        return inputName.isNotEmpty() ? juce::StringArray { "Mic 1", "Mic 2" } : juce::StringArray();
    }
    juce::Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 128, 256, 512 }; }
    int getDefaultBufferSize() override { return 256; }

    juce::String open (const juce::BigInteger& inputs, const juce::BigInteger& outputs, double, int bufferSize) override
    {
        block = bufferSize > 0 ? bufferSize : 256;
        activeOutputs = outputs;
        activeInputs = inputs;
        activeInputs.setRange (getInputChannelNames().size(), activeInputs.getHighestBit() + 1, false);
        if (! activeInputs.isZero())
            card.inputOpens++;
        isOpenNow = true;
        return {};
    }
    void close() override
    {
        stop();
        isOpenNow = false;
    }
    bool isOpen() override { return isOpenNow; }

    void start (juce::AudioIODeviceCallback* newCallback) override
    {
        if (newCallback == nullptr || callback != nullptr)
            return;
        newCallback->audioDeviceAboutToStart (this);
        callback = newCallback;
        running = true;
        card.starts++;
        thread = std::thread ([this] { run(); });
    }
    void stop() override
    {
        if (callback == nullptr)
            return;
        running = false;
        if (thread.joinable())
            thread.join();
        auto* old = callback;
        callback = nullptr;
        old->audioDeviceStopped();
    }
    bool isPlaying() override { return callback != nullptr; }
    juce::String getLastError() override { return {}; }

    int getCurrentBufferSizeSamples() override { return block; }
    double getCurrentSampleRate() override { return 48000.0; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return activeOutputs; }
    juce::BigInteger getActiveInputChannels() const override { return activeInputs; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }

private:
    void run()
    {
        using clock = std::chrono::steady_clock;
        juce::AudioBuffer<float> out (2, block), in (2, block);
        const int inputs = activeInputs.countNumberOfSetBits();
        int64_t sample = 0;
        const auto period =
            std::chrono::duration_cast<clock::duration> (std::chrono::duration<double> ((double)block / 48000.0));
        auto deadline = clock::now();
        while (running)
        {
            out.clear();
            for (int i = 0; i < block; ++i, ++sample) // a 400 Hz tone on each input in use
                for (int ch = 0; ch < inputs; ++ch)
                    in.setSample (
                        ch, i, 0.5f * std::sin (juce::MathConstants<float>::twoPi * 400.0f * (float)sample / 48000.0f));
            callback->audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), inputs,
                                                        out.getArrayOfWritePointers(), 2, block, {});
            card.record (out.getReadPointer (0), block);
            deadline += period;
            std::this_thread::sleep_until (deadline);
        }
    }

    FakeSoundCard& card;
    juce::AudioIODeviceCallback* callback = nullptr;
    std::atomic<bool> running { false };
    std::thread thread;
    juce::BigInteger activeOutputs, activeInputs;
    const juce::String inputName;
    int block = 256;
    bool isOpenNow = false;
};

class FakeDeviceType final : public juce::AudioIODeviceType
{
public:
    explicit FakeDeviceType (FakeSoundCard& c) : juce::AudioIODeviceType (FakeSoundCard::typeName), card (c) {}
    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool wantInputNames) const override
    {
        return wantInputNames ? juce::StringArray { "Fake mic" }
                              : juce::StringArray { "Fake output A", "Fake output B" };
    }
    int getDefaultDeviceIndex (bool) const override { return 0; }
    int getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const override
    {
        if (device == nullptr)
            return -1;
        if (asInput)
            return device->getInputChannelNames().isEmpty() ? -1 : 0;
        return getDeviceNames (false).indexOf (device->getName());
    }
    bool hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice (const juce::String& outputName, const juce::String& inputName) override
    {
        return getDeviceNames (false).contains (outputName)
                   ? new FakeDevice (outputName, getDeviceNames (true).contains (inputName) ? inputName : "", card)
                   : nullptr;
    }

private:
    FakeSoundCard& card;
};

std::unique_ptr<juce::AudioIODeviceType> FakeSoundCard::createType()
{
    return std::make_unique<FakeDeviceType> (*this);
}

void FakeSoundCard::record (const float* samples, int count)
{
    const std::scoped_lock l (lock);
    recording.insert (recording.end(), samples, samples + count);
    played += count;
}

std::vector<float> FakeSoundCard::getRecording() const
{
    const std::scoped_lock l (lock);
    return recording;
}

} // namespace spinus::test
