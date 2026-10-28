#include "RealtimeRunner.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

namespace spinus::test
{

namespace
{
int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds> (steady_clock::now().time_since_epoch()).count();
}

/** Tracktion applies MIDI device changes (an input made or turned on, as Spinus's computer keyboard on a
    first start) on a 5 ms timer of its DeviceManager, and applying them stops every playhead. Whether that
    timer fires before or after a test presses Play depends on the machine; apply them now, until no more
    come (each applied change can make its listeners turn on another input). */
void applyPendingDeviceChanges (tracktion::DeviceManager& devices)
{
    // The DeviceManager is a private juce::Timer, whose only use is this rescan (or a slow hardware check).
    auto& rescan = (juce::Timer&)devices; // NOLINT: a C-style cast is the only way to reach a private base
    for (int i = 0; i < 16; ++i)
    {
        devices.dispatchPendingMessages();
        devices.dispatchPendingUpdates();
        if (! rescan.isTimerRunning() || rescan.getTimerInterval() >= 1000)
            break;
        rescan.stopTimer();
        rescan.timerCallback();
    }
}

/** Runs the message loop until every message posted so far has been delivered (they come in order). */
void deliverPostedMessages()
{
    auto delivered = std::make_shared<bool> (false);
    juce::MessageManager::callAsync ([delivered] { *delivered = true; });
    for (int i = 0; i < 10000 && ! *delivered; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
}

/** Brings every playback graph up to date with the edits made so far. Tracktion rebuilds a graph on a
    1 ms timer after a change, and some changes (an input routed to another track) only ask for that from
    a posted message. A sound card plays in real time, so those land within a block; blocks pumped faster
    than real time, or a busy machine, would render seconds with the old graph, or rebuild it mid-note. */
void applyPendingGraphChanges (tracktion::Engine& engine)
{
    for (int pass = 0; pass < 8; ++pass)
    {
        deliverPostedMessages();
        applyPendingDeviceChanges (engine.getDeviceManager());
        bool rebuilt = false;
        for (auto* edit : engine.getActiveEdits().getEdits())
        {
            // An Edit is a private juce::Timer, run only to rebuild its graph.
            if (auto& rebuild = (juce::Timer&)*edit; rebuild.isTimerRunning()) // NOLINT: private base
            {
                edit->dispatchPendingUpdatesSynchronously();
                rebuilt = true;
            }
        }
        if (! rebuilt)
            return;
    }
}
} // namespace

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
    applyPendingDeviceChanges (engine.getDeviceManager());

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
    applyPendingGraphChanges (engine);
    running = true;
    thread = std::thread ([this] { audioThreadLoop(); });
    watching = true;
    watchdog = std::thread ([this] { watchdogLoop(); });
}

void RealtimeRunner::stop()
{
    running = false;

    if (thread.joinable())
        thread.join();

    watching = false;
    if (watchdog.joinable())
        watchdog.join();
}

void RealtimeRunner::watchdogLoop()
{
    auto last = samplesProcessed.load();
    auto lastMoved = nowMs();

    while (watching)
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        const auto now = nowMs();
        if (const auto done = samplesProcessed.load(); done != last || ! running)
        {
            last = done;
            lastMoved = now;
            continue;
        }
        if ((double)(now - lastMoved) < stallSeconds * 1000.0)
            continue;

        const auto processing = processingSinceMs.load(), dispatching = dispatchingSinceMs.load();
        const auto audio = processing != 0
                               ? "inside processBlock for " + std::to_string ((now - processing) / 1000) + " s"
                               : std::string ("not inside processBlock");
        const auto message = dispatching != 0
                                 ? "inside a message dispatch for " + std::to_string ((now - dispatching) / 1000) + " s"
                                 : std::string ("not dispatching messages inside runUntil()");
        std::fprintf (stderr,
                      "RealtimeRunner: no sound processed for %.0f s (stuck at %.3f s).\n"
                      "  Audio thread: %s\n  Message thread: %s\n",
                      stallSeconds, getSecondsProcessed(), audio.c_str(), message.c_str());
        std::fflush (stderr);
        std::abort();
    }
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
        dispatchingSinceMs = nowMs();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (2);
        dispatchingSinceMs = 0;
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
        processingSinceMs = nowMs();
        device.processBlock (block, midi);
        processingSinceMs = 0;
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
