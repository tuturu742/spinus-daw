#include "RealtimeRunner.h"

#include <chrono>
#include <utility>

namespace spinus::test
{

RealtimeRunner::RealtimeRunner (tracktion::Engine& e, Params p)
    : engine (e), params (std::move (p)), device (e.getDeviceManager().getHostedAudioDeviceInterface())
{
    // Blocks may be pumped faster than real time; never let the engine mute itself for "CPU overload".
    engine.getDeviceManager().setCpuLimitBeforeMuting (1000.0);

    tracktion::HostedAudioDeviceInterface::Parameters hp;
    hp.sampleRate = params.sampleRate;
    hp.blockSize = params.blockSize;
    hp.inputChannels = params.inputChannels;
    hp.outputChannels = params.outputChannels;
    device.initialise (hp);
    device.prepareToPlay (params.sampleRate, params.blockSize);
    engine.getDeviceManager().dispatchPendingUpdates();

    capacity = (int64_t)(params.maxSeconds * params.sampleRate);
    output.assign ((size_t)params.outputChannels, std::vector<float> ((size_t)capacity, 0.0f));
    blockTimesMs.reserve ((size_t)(capacity / params.blockSize + 1));
    block.setSize (std::max (params.outputChannels, params.inputChannels), params.blockSize);
    previous.setSize (2, params.blockSize);
    midi.ensureSize (4096);
}

RealtimeRunner::~RealtimeRunner()
{
    stop();
    engine.getDeviceManager().deviceManager.closeAudioDevice();
    engine.getDeviceManager().removeHostedAudioDeviceInterface();
}

juce::String RealtimeRunner::getOutputDeviceID (int pairIndex) const
{
    auto devices = engine.getDeviceManager().getWaveOutputDevices();
    jassert (pairIndex < (int)devices.size());
    return devices[(size_t)pairIndex]->getDeviceID();
}

void RealtimeRunner::start()
{
    jassert (! running);
    running = true;
    thread = std::thread ([this] { audioThreadLoop(); });
}

void RealtimeRunner::stop()
{
    running = false;

    if (thread.joinable())
        thread.join();
}

void RealtimeRunner::restartDevice()
{
    const bool wasRunning = running.load();
    stop();
    device.prepareToPlay (params.sampleRate, params.blockSize);
    engine.getDeviceManager().dispatchPendingUpdates();
    if (wasRunning)
        start();
}

void RealtimeRunner::runUntil (double secondsProcessed)
{
    using clock = std::chrono::steady_clock;

    while (running && getSecondsProcessed() < secondsProcessed)
    {
        const auto t0 = clock::now();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (2);
        const auto ms = std::chrono::duration<double, std::milli> (clock::now() - t0).count();
        maxDispatchMs = std::max (maxDispatchMs, ms);
    }
}

int RealtimeRunner::getNumOverrunBlocks() const
{
    const auto budget = (float)getBlockPeriodMs();
    return (int)std::count_if (blockTimesMs.begin(), blockTimesMs.end(), [budget] (float t) { return t > budget; });
}

void RealtimeRunner::audioThreadLoop()
{
    using clock = std::chrono::steady_clock;
    const auto period =
        std::chrono::duration_cast<clock::duration> (std::chrono::duration<double, std::milli> (getBlockPeriodMs()));
    auto deadline = clock::now();

    while (running)
    {
        const auto start = samplesProcessed.load();

        if (start + params.blockSize > capacity)
            break;

        block.clear();
        midi.clear();
        if (params.loopback)
            for (int ch = 0; ch < params.inputChannels; ++ch)
                block.copyFrom (ch, 0, previous, ch % 2, 0, params.blockSize);
        if (params.inputSource)
            params.inputSource (block, params.inputChannels, start);

        const auto t0 = clock::now();
        device.processBlock (block, midi);
        const auto t1 = clock::now();

        blockTimesMs.push_back (std::chrono::duration<float, std::milli> (t1 - t0).count());

        for (int ch = 0; ch < params.outputChannels; ++ch)
            std::copy_n (block.getReadPointer (ch), params.blockSize, output[(size_t)ch].begin() + start);
        for (int ch = 0; ch < 2 && ch < params.outputChannels; ++ch)
            previous.copyFrom (ch, 0, block, ch, 0, params.blockSize);

        samplesProcessed = start + params.blockSize;

        if (params.paceInRealTime)
        {
            deadline += period;
            std::this_thread::sleep_until (deadline);
        }
    }

    running = false;
}

} // namespace spinus::test
