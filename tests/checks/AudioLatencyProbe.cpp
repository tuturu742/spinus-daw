/*  Audio latency and xrun behaviour on a real sound card.

    Opens an output device through JUCE (the same code Spinus uses), plays silence, optionally burns a
    fixed share of every callback's time budget to simulate DSP load, and reports:

      - the buffer size and sample rate the device actually gave us,
      - the output latency the driver reports,
      - xruns counted by the driver (ALSA under-runs, JACK xrun callbacks),
      - callback timing: how regularly the audio thread is called, and how many calls came late.

    Silence only: nothing is audible. Run with --help for options; tools/owner-checks/s5-audio-latency.sh
    runs the standard matrix.
*/

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

namespace
{

struct Options
{
    juce::String type = "ALSA";
    juce::String device; // empty = the type's default output
    int bufferSize = 128;
    double sampleRate = 48000.0;
    double seconds = 20.0;
    double load = 0.0; // share of each callback's time budget spent busy (0..0.95)
    bool list = false;
};

/** Records callback start times into a preallocated array; no allocation, locks or I/O. */
class Probe : public juce::AudioIODeviceCallback
{
public:
    Probe (size_t capacity, double loadShare) : starts (capacity), sizes (capacity), load (loadShare) {}

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override { rate = device->getCurrentSampleRate(); }

    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputs, int numOutputs,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        const auto start = juce::Time::getMillisecondCounterHiRes();
        const auto index = count.fetch_add (1, std::memory_order_relaxed);

        if (index < starts.size())
        {
            starts[index] = start;
            sizes[index] = numSamples;
        }

        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputs[ch], numSamples);

        // Simulated DSP: spin for the requested share of this block's duration.
        const auto budgetMs = load * 1000.0 * numSamples / rate;
        while (juce::Time::getMillisecondCounterHiRes() - start < budgetMs)
        {
        }
    }

    void audioDeviceStopped() override {}

    size_t getNumCallbacks() const { return std::min (count.load(), starts.size()); }

    std::vector<double> starts;
    std::vector<int> sizes;

private:
    double load = 0.0;
    double rate = 48000.0;
    std::atomic<size_t> count { 0 };
};

/** JUCE's JACK device reports latency through a deprecated call that PipeWire's libjack answers with 0.
    Ask JACK directly: the playback latency range of the device's playback ports (time from the port to
    the speaker), read by a short-lived second client. Returns -1 if unavailable.

    PipeWire expresses device latency in quanta (buffer cycles) and libjack converts it with the querying
    client's own buffer size, which for an idle second client is PipeWire's default (1024), not the
    running buffer. The result is rescaled to the buffer Spinus actually runs at. */
int queryJackPlaybackLatency (const juce::String& deviceName, int runningBufferSize)
{
    struct Range
    {
        uint32_t min, max;
    };

    using ClientOpen = void* (*) (const char*, int, int*, ...);
    using ClientClose = int (*) (void*);
    using GetPorts = const char** (*) (void*, const char*, const char*, unsigned long);
    using PortByName = void* (*) (void*, const char*);
    using LatencyRange = void (*) (void*, int, Range*);
    using Free = void (*) (void*);
    using BufferSize = uint32_t (*) (void*);

    auto* lib = dlopen ("libjack.so.0", RTLD_LAZY);
    if (lib == nullptr)
        return -1;

    auto open = reinterpret_cast<ClientOpen> (dlsym (lib, "jack_client_open"));
    auto close = reinterpret_cast<ClientClose> (dlsym (lib, "jack_client_close"));
    auto getPorts = reinterpret_cast<GetPorts> (dlsym (lib, "jack_get_ports"));
    auto portByName = reinterpret_cast<PortByName> (dlsym (lib, "jack_port_by_name"));
    auto latencyRange = reinterpret_cast<LatencyRange> (dlsym (lib, "jack_port_get_latency_range"));
    auto jackFree = reinterpret_cast<Free> (dlsym (lib, "jack_free"));
    auto bufferSize = reinterpret_cast<BufferSize> (dlsym (lib, "jack_get_buffer_size"));

    if (open == nullptr || close == nullptr || getPorts == nullptr || portByName == nullptr ||
        latencyRange == nullptr || jackFree == nullptr)
        return -1;

    constexpr int noStartServer = 0x01, portIsInput = 0x1, playbackLatency = 1;
    auto* client = open ("spinus-s5-latency", noStartServer, nullptr);
    if (client == nullptr)
        return -1;

    int latency = -1;
    if (auto** ports = getPorts (client, (juce::String ("^") + deviceName + ":").toRawUTF8(), nullptr, portIsInput))
    {
        for (auto** p = ports; *p != nullptr; ++p)
        {
            Range range {};
            latencyRange (portByName (client, *p), playbackLatency, &range);
            latency = std::max (latency, (int)range.max);
        }

        jackFree (static_cast<void*> (ports));
    }

    if (latency > 0 && bufferSize != nullptr)
        if (const auto queryBuffer = (int)bufferSize (client); queryBuffer > 0 && queryBuffer != runningBufferSize)
            latency = (int)((int64_t)latency * runningBufferSize / queryBuffer);

    close (client);
    return latency;
}

void printUsage()
{
    std::cout << "Usage: spinus_latency_probe [--type ALSA|JACK] [--device NAME] [--buffer N] [--rate HZ]\n"
                 "                       [--seconds S] [--load 0..0.95] [--list]\n";
}

bool parse (const juce::StringArray& args, Options& o)
{
    for (int i = 1; i < args.size(); ++i)
    {
        const auto& a = args[i];
        const auto next = [&] { return i + 1 < args.size() ? args[++i] : juce::String(); };

        if (a == "--type")
            o.type = next();
        else if (a == "--device")
            o.device = next();
        else if (a == "--buffer")
            o.bufferSize = next().getIntValue();
        else if (a == "--rate")
            o.sampleRate = next().getDoubleValue();
        else if (a == "--seconds")
            o.seconds = next().getDoubleValue();
        else if (a == "--load")
            o.load = juce::jlimit (0.0, 0.95, next().getDoubleValue());
        else if (a == "--list")
            o.list = true;
        else
            return false;
    }

    return true;
}

double percentile (std::vector<double> v, double p)
{
    if (v.empty())
        return 0.0;

    std::sort (v.begin(), v.end());
    return v[std::min (v.size() - 1, (size_t)std::lround (p * (double)(v.size() - 1)))];
}

int run (const Options& o)
{
    juce::AudioDeviceManager manager;
    juce::OwnedArray<juce::AudioIODeviceType> types;
    manager.createAudioDeviceTypes (types);

    juce::AudioIODeviceType* type = nullptr;
    for (auto* t : types)
        if (t->getTypeName() == o.type)
            type = t;

    if (type == nullptr)
    {
        std::cout << "Latency | error | device type " << o.type << " not available\n";
        return 2;
    }

    type->scanForDevices();

    if (o.list)
    {
        for (auto& name : type->getDeviceNames (false))
            std::cout << o.type << " output: " << name << '\n';
        return 0;
    }

    const auto names = type->getDeviceNames (false);
    const auto deviceName = o.device.isNotEmpty() ? o.device : names[type->getDefaultDeviceIndex (false)];
    std::unique_ptr<juce::AudioIODevice> device (type->createDevice (deviceName, {}));

    if (device == nullptr)
    {
        std::cout << "Latency | error | could not create " << o.type << " device '" << deviceName << "'\n";
        return 2;
    }

    juce::BigInteger outputs;
    outputs.setRange (0, 2, true);
    const auto error = device->open ({}, outputs, o.sampleRate, o.bufferSize);

    if (error.isNotEmpty())
    {
        std::cout << "Latency | error | " << error << '\n';
        return 2;
    }

    const auto rate = device->getCurrentSampleRate();
    const auto buffer = device->getCurrentBufferSizeSamples();
    const auto expectedCallbacks = (size_t)(o.seconds * rate / std::max (16, buffer) * 4.0) + 1024;
    Probe probe (expectedCallbacks, o.load);

    device->start (&probe);
    const auto xrunsAtStart = device->getXRunCount();
    juce::Thread::sleep ((int)(o.seconds * 1000.0));
    const auto xruns = device->getXRunCount() - xrunsAtStart;
    device->stop();

    // Ignore the first second: devices settle (PipeWire may switch its quantum) after starting.
    const auto n = probe.getNumCallbacks();
    std::vector<double> intervals, ratios;
    int late = 0;
    int minSize = std::numeric_limits<int>::max(), maxSize = 0;

    for (size_t i = 1; i < n; ++i)
    {
        if (probe.starts[i] - probe.starts[0] < 1000.0)
            continue;

        const auto interval = probe.starts[i] - probe.starts[i - 1];
        const auto expected = 1000.0 * probe.sizes[i - 1] / rate;
        intervals.push_back (interval);
        ratios.push_back (interval / expected);
        late += interval > 1.5 * expected ? 1 : 0;
        minSize = std::min (minSize, probe.sizes[i]);
        maxSize = std::max (maxSize, probe.sizes[i]);
    }

    // JACK: one buffer in Spinus (written now, played next cycle) plus the device's playback latency.
    auto latency = device->getOutputLatencyInSamples();
    if (o.type == "JACK" && latency == 0)
        if (const auto deviceLatency = queryJackPlaybackLatency (deviceName, buffer); deviceLatency >= 0)
            latency = buffer + deviceLatency;

    const auto line = [&] (const juce::String& name, const juce::String& value)
    { std::cout << "Latency | " << name << " | " << value << '\n'; };

    line ("type / device", o.type + " / " + deviceName);
    line ("requested buffer / rate", juce::String (o.bufferSize) + " / " + juce::String (o.sampleRate, 0));
    line ("actual buffer / rate", juce::String (buffer) + " / " + juce::String (rate, 0));
    line ("callback block sizes seen (min-max)", juce::String (minSize) + "-" + juce::String (maxSize));
    line ("reported output latency samples / ms",
          juce::String (latency) + " / " + juce::String (1000.0 * latency / rate, 2));
    line ("simulated DSP load %", juce::String (100.0 * o.load, 0));
    line ("seconds measured / callbacks",
          juce::String (o.seconds - 1.0, 0) + " / " + juce::String ((int)intervals.size()));
    line ("driver xruns", juce::String (xruns));
    line ("late callbacks (> 1.5 x block time)", juce::String (late));
    line ("callback interval ms p50 / p99 / max", juce::String (percentile (intervals, 0.5), 3) + " / " +
                                                      juce::String (percentile (intervals, 0.99), 3) + " / " +
                                                      juce::String (percentile (intervals, 1.0), 3));
    line ("summary", juce::String ("type=") + o.type + " buffer=" + juce::String (buffer) + " latency_ms=" +
                         juce::String (1000.0 * latency / rate, 2) + " load=" + juce::String (100.0 * o.load, 0) +
                         " xruns=" + juce::String (xruns) + " late=" + juce::String (late) +
                         " max_interval_ratio=" + juce::String (percentile (ratios, 1.0), 2));
    return 0;
}

} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juce;
    juce::StringArray args (argv, argc);
    Options options;

    if (args.contains ("--help") || ! parse (args, options))
    {
        printUsage();
        return args.contains ("--help") ? 0 : 1;
    }

    return run (options);
}
