#include "SandboxPluginFormat.h"

#include "core/Log.h"
#include "SandboxHost.h"
#include "SandboxProtocol.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace spinus::plugins::sandbox
{

namespace
{
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : items)
        o->setProperty (k, v);
    return juce::var (o);
}

/** The pipe to spinus-plugin-host: requests that wait for their answer, and messages it sends on its own.
    Its callbacks come on its own thread. */
class Connection final : public juce::ChildProcessCoordinator
{
public:
    ~Connection() override
    {
        killWorkerProcess();
        wakeAll();
    }

    /** The answer (a JSON object), or void when the host did not answer in time or has gone. */
    juce::var request (const juce::String& op, juce::var parameters, int timeoutMs)
    {
        if (lost)
            return {};
        auto pending = std::make_shared<Pending>();
        int id = 0;
        {
            const juce::ScopedLock sl (lock);
            id = ++nextId;
            pendings[id] = pending;
        }
        if (! parameters.isObject())
            parameters = object ({});
        parameters.getDynamicObject()->setProperty ("op", op);
        parameters.getDynamicObject()->setProperty ("id", id);
        const auto text = juce::JSON::toString (parameters, true);
        const bool sent = sendMessageToWorker (juce::MemoryBlock (text.toRawUTF8(), text.getNumBytesAsUTF8()));
        const bool answered = sent && pending->done.wait (timeoutMs);
        const juce::ScopedLock sl (lock);
        pendings.erase (id);
        return answered ? pending->result : juce::var();
    }

    /** Sends without waiting for the answer. */
    void post (const juce::String& op, juce::var parameters)
    {
        if (lost)
            return;
        if (! parameters.isObject())
            parameters = object ({});
        parameters.getDynamicObject()->setProperty ("op", op);
        const auto text = juce::JSON::toString (parameters, true);
        sendMessageToWorker (juce::MemoryBlock (text.toRawUTF8(), text.getNumBytesAsUTF8()));
    }

    std::function<void (const juce::var&)> onMessage; // unsolicited messages, on the connection's thread
    std::function<void()> onLost;
    std::atomic<bool> lost { false };

private:
    struct Pending
    {
        juce::WaitableEvent done;
        juce::var result;
    };

    void handleMessageFromWorker (const juce::MemoryBlock& message) override
    {
        const auto json = juce::JSON::parse (message.toString());
        if (json.hasProperty ("id"))
        {
            const juce::ScopedLock sl (lock);
            if (auto it = pendings.find ((int)json["id"]); it != pendings.end())
            {
                it->second->result = json;
                it->second->done.signal();
            }
        }
        else if (onMessage)
            onMessage (json);
    }

    void handleConnectionLost() override
    {
        lost = true;
        wakeAll();
        if (onLost)
            onLost();
    }

    void wakeAll()
    {
        const juce::ScopedLock sl (lock);
        for (auto& [id, pending] : pendings)
            pending->done.signal();
    }

    juce::CriticalSection lock;
    int nextId = 0;
    std::map<int, std::shared_ptr<Pending>> pendings;
};

class RemoteInstance;

/** A parameter of the plugin in the other process: its value is kept here, changes go across in the next
    block, and its text is asked for (on the message thread). */
class RemoteParameter final : public juce::AudioPluginInstance::HostedParameter
{
public:
    RemoteParameter (RemoteInstance& o, int i, const juce::var& info);

    float getValue() const override { return value.load(); }
    void setValue (float newValue) override;
    float getDefaultValue() const override { return defaultValue; }
    juce::String getName (int maximumLength) const override { return name.substring (0, maximumLength); }
    juce::String getLabel() const override { return label; }
    int getNumSteps() const override { return steps; }
    bool isDiscrete() const override { return discrete; }
    bool isBoolean() const override { return boolean; }
    bool isAutomatable() const override { return automatable; }
    juce::String getParameterID() const override { return id; }
    juce::String getText (float, int) const override;
    float getValueForText (const juce::String&) const override;

    /** A change from the plugin itself (its window): tells the host, without sending it back. */
    void receive (float newValue)
    {
        value = newValue;
        sendValueChangedMessageToListeners (newValue);
    }

private:
    RemoteInstance& owner;
    int index;
    juce::String id, name, label;
    float defaultValue = 0.0f;
    int steps = 0x7fffffff;
    bool discrete = false, boolean = false, automatable = true;
    std::atomic<float> value { 0.0f };
};

//==============================================================================
class RemoteInstance final : public juce::AudioPluginInstance, public SandboxedInstance, private juce::Timer
{
public:
    RemoteInstance (std::unique_ptr<Connection> c, std::unique_ptr<SharedMemory> m, juce::PluginDescription d,
                    juce::PluginDescription r, const juce::var& info)
        : juce::AudioPluginInstance (layoutFor (info)), connection (std::move (c)), memory (std::move (m)),
          description (std::move (d)), real (std::move (r)), name (info["name"].toString()),
          numIn ((int)info["inputs"]), numOut ((int)info["outputs"]), midiIn ((bool)info["acceptsMidi"]),
          midiOut ((bool)info["producesMidi"]), editor ((bool)info["hasEditor"]), pluginLatency ((int)info["latency"]),
          tail ((double)info["tail"]), watch ((juce::int64)info["process"])
    {
        if (auto* list = info["parameters"].getArray())
            for (int i = 0; i < list->size(); ++i)
                addHostedParameter (std::make_unique<RemoteParameter> (*this, i, list->getReference (i)));
        changedCount = (size_t)getParameters().size();
        changed = std::make_unique<std::atomic<bool>[]> (changedCount);
        for (size_t i = 0; i < changedCount; ++i)
            changed[i] = false;
        memory->get().numChannels = juce::jlimit (1, maxChannels, juce::jmax (numIn, numOut));

        connection->onMessage = [this] (const juce::var& message) { fromHost (message); };
        connection->onLost = [this]
        {
            if (failure == Failure::none)
                failure = Failure::crashed;
        };
        startTimerHz (30);
    }

    ~RemoteInstance() override
    {
        stopTimer();
        connection->onMessage = nullptr;
        connection->onLost = nullptr;
        memory->get().quit = 1;
        connection = nullptr; // ends the host
    }

    //==============================================================================
    Failure getFailure() const override { return failure.load(); }

    void fillInPluginDescription (juce::PluginDescription& d) const override { d = description; }
    const juce::String getName() const override { return name; }

    void prepareToPlay (double rate, int block) override
    {
        auto& s = memory->get();
        s.posted = 0;
        s.done = 0;
        posted = collected = 0;
        const int extra = juce::jlimit (1, maxBlock, block);
        connection->request ("prepare", object ({ { "rate", rate }, { "block", extra } }), 10000);
        // One block of silence ahead: the plugin has a block's time to answer (more when blocks vary).
        fifo.setSize (juce::jmax (1, numOut), 4 * maxBlock + extra);
        fifo.clear();
        fifoRead = 0;
        fifoCount = extra;
        bufferedLatency = extra;
        setLatencySamples (pluginLatency + bufferedLatency);
        stalledSamples = 0;
        hangLimit = (int64_t)(rate * 2.0);
    }

    void releaseResources() override { connection->request ("release", {}, 5000); }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (int start = 0; start < buffer.getNumSamples(); start += maxBlock)
        {
            const int n = juce::jmin (maxBlock, buffer.getNumSamples() - start);
            juce::AudioBuffer<float> part (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), start, n);
            processPart (part, midi, start);
        }
        if (! midiOut)
            midi.clear();
    }

    using juce::AudioPluginInstance::processBlock;

    double getTailLengthSeconds() const override { return tail; }
    bool acceptsMidi() const override { return midiIn; }
    bool producesMidi() const override { return midiOut; }
    bool hasEditor() const override
    {
#if JUCE_LINUX || JUCE_BSD
        return editor;
#else
        return false;
#endif
    }
    juce::AudioProcessorEditor* createEditor() override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& data) override
    {
        // After a failure, the last state it had: what a reload starts from.
        if (failure == Failure::none)
            if (auto answer = connection->request ("getState", {}, 5000); answer.hasProperty ("state"))
                lastState.fromBase64Encoding (answer["state"].toString());
        data = lastState;
    }

    void setStateInformation (const void* data, int size) override
    {
        lastState = juce::MemoryBlock (data, (size_t)size);
        connection->request ("setState", object ({ { "state", lastState.toBase64Encoding() } }), 5000);
        // The state sets parameters too: show their new values.
        if (auto answer = ask ("values", {}, 5000); answer["values"].isArray())
            for (int i = 0; i < answer["values"].size() && i < getParameters().size(); ++i)
                if (auto* p = dynamic_cast<RemoteParameter*> (getParameters()[i]))
                    p->receive ((float)(double)answer["values"][i]);
    }

    bool isBusesLayoutSupported (const BusesLayout& layout) const override
    {
        return layout.getMainInputChannels() == numIn && layout.getMainOutputChannels() == numOut;
    }

    //==============================================================================
    void queueChange (int index, float)
    {
        if (index >= 0 && (size_t)index < changedCount)
        {
            changed[(size_t)index].store (true, std::memory_order_release);
            anyChanged.store (true, std::memory_order_release);
        }
    }

    juce::var ask (const juce::String& op, const juce::var& parameters, int timeoutMs) const
    {
        return failure == Failure::none ? connection->request (op, parameters, timeoutMs) : juce::var();
    }

    Connection& getConnection() { return *connection; }
    std::function<void (int, int)> onEditorSize; // the plugin's window changed size

private:
    static BusesProperties layoutFor (const juce::var& info)
    {
        BusesProperties buses;
        if ((int)info["inputs"] > 0)
            buses = buses.withInput ("Input", juce::AudioChannelSet::canonicalChannelSet ((int)info["inputs"]), true);
        if ((int)info["outputs"] > 0)
            buses =
                buses.withOutput ("Output", juce::AudioChannelSet::canonicalChannelSet ((int)info["outputs"]), true);
        return buses;
    }

    void processPart (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, int offset)
    {
        const int n = buffer.getNumSamples();
        auto& s = memory->get();

        if (failure != Failure::none)
        {
            // Bypassed: an effect lets its input through, an instrument is silent.
            for (int c = numIn; c < buffer.getNumChannels(); ++c)
                buffer.clear (c, 0, n);
            if (numIn == 0)
                buffer.clear();
            return;
        }

        // Send this block, unless the host is a whole ring behind (then it is dropped).
        if (posted - collected < (uint32_t)slotCount)
        {
            auto& slot = s.slots[posted % slotCount];
            slot.numSamples = n;
            const int channels = juce::jlimit (1, maxChannels, s.numChannels);
            for (int c = 0; c < channels; ++c)
                if (c < numIn && c < buffer.getNumChannels())
                    std::copy_n (buffer.getReadPointer (c), n, slot.audio[c]);
                else
                    std::fill_n (slot.audio[c], n, 0.0f);

            slot.numMidi = 0;
            for (const auto event : midi)
                if (event.samplePosition >= offset && event.samplePosition < offset + n && event.numBytes <= 3 &&
                    slot.numMidi < maxMidiEvents)
                {
                    auto& m = slot.midi[slot.numMidi++];
                    m.sample = event.samplePosition - offset;
                    m.size = (uint8_t)event.numBytes;
                    std::copy_n (event.data, event.numBytes, m.bytes);
                }

            slot.numChanges = 0;
            if (anyChanged.exchange (false, std::memory_order_acq_rel))
            {
                const auto& parameters = getParameters();
                for (size_t i = 0; i < changedCount; ++i)
                    if (changed[i].exchange (false, std::memory_order_acq_rel))
                    {
                        if (slot.numChanges == maxParameterChanges)
                        {
                            changed[i] = true; // next block
                            anyChanged = true;
                            continue;
                        }
                        slot.changes[slot.numChanges++] = { (int32_t)i, parameters.getUnchecked ((int)i)->getValue() };
                    }
            }

            ++posted;
            s.posted.store (posted, std::memory_order_release);
            memory->postWork();
        }

        // Rendering offline: wait for this block's answer (nothing here runs in real time).
        collect();
        if (isNonRealtime())
            while (fifoCount < n && failure == Failure::none)
            {
                if (! memory->waitForFinished (5000))
                    failure = Failure::hung;
                collect();
            }

        // Out of the FIFO; what is missing is silence.
        const int available = juce::jmin (n, fifoCount);
        for (int c = 0; c < buffer.getNumChannels(); ++c)
        {
            auto* out = buffer.getWritePointer (c);
            if (c < numOut)
                for (int i = 0; i < available; ++i)
                    out[i] = fifo.getSample (c, (fifoRead + i) % fifo.getNumSamples());
            else
                std::fill_n (out, available, 0.0f);
            std::fill (out + available, out + n, 0.0f);
        }
        fifoRead = (fifoRead + available) % fifo.getNumSamples();
        fifoCount -= available;

        // Not answering for two seconds while blocks wait: hung.
        stalledSamples = posted != collected ? stalledSamples + n : 0;
        if (stalledSamples > hangLimit && failure == Failure::none)
            failure = Failure::hung;
    }

    void collect()
    {
        auto& s = memory->get();
        const auto done = s.done.load (std::memory_order_acquire);
        while (collected != done)
        {
            const auto& slot = s.slots[collected % slotCount];
            const int n = slot.numSamples;
            if (fifoCount + n <= fifo.getNumSamples())
            {
                const int write = (fifoRead + fifoCount) % fifo.getNumSamples();
                for (int c = 0; c < fifo.getNumChannels(); ++c)
                    for (int i = 0; i < n; ++i)
                        fifo.setSample (c, (write + i) % fifo.getNumSamples(), slot.audio[c][i]);
                fifoCount += n;
            }
            ++collected;
            stalledSamples = 0;
        }
    }

    void fromHost (const juce::var& message)
    {
        // On the connection's thread: hand over to the message thread.
        juce::MessageManager::callAsync (
            [weak = juce::WeakReference<RemoteInstance> (this), message]
            {
                if (weak == nullptr)
                    return;
                const auto op = message["op"].toString();
                if (op == "gesture")
                {
                    if (auto* p = weak->getParameters()[(int)message["index"]])
                        (bool)message["begin"] ? p->beginChangeGesture() : p->endChangeGesture();
                }
                else if (op == "latency")
                {
                    weak->pluginLatency = (int)message["samples"];
                    weak->setLatencySamples (weak->pluginLatency + weak->bufferedLatency);
                }
                else if (op == "editorSize" && weak->onEditorSize)
                    weak->onEditorSize ((int)message["width"], (int)message["height"]);
            });
    }

    void timerCallback() override
    {
        // Changes made in the plugin's own window.
        auto& s = memory->get();
        const auto write = s.ringWrite.load (std::memory_order_acquire);
        auto read = s.ringRead.load (std::memory_order_relaxed);
        const auto& parameters = getParameters();
        for (; read != write; ++read)
        {
            const auto change = s.ring[read % ringSize];
            if (auto* p = dynamic_cast<RemoteParameter*> (parameters[change.index]))
                p->receive (change.value);
        }
        s.ringRead.store (read, std::memory_order_release);

        // A crash: on Windows the process shows it at once, where the pipe would only time out.
        if (watch.hasEnded())
        {
            auto expected = Failure::none;
            failure.compare_exchange_strong (expected, Failure::crashed);
        }

        // A hang is noticed on the audio thread; the process is ended here.
        if (failure == Failure::hung && ! killed)
        {
            killed = true;
            connection->killWorkerProcess();
        }

        // Noted here, on the message thread (the audio thread only sets the flag).
        if (const auto f = failure.load(); f != Failure::none && ! reportedFailure)
        {
            reportedFailure = true;
            core::Logs::error ("sandbox",
                               name +
                                   (f == Failure::hung ? " stopped answering in its own process and was ended"
                                                       : " crashed in its own process") +
                                   "; Spinus carries on without it (" + real.pluginFormatName + " " +
                                   real.fileOrIdentifier + ")");
        }
    }

    std::unique_ptr<Connection> connection;
    std::unique_ptr<SharedMemory> memory;
    juce::PluginDescription description, real;
    juce::String name;
    int numIn = 0, numOut = 0;
    bool midiIn = false, midiOut = false, editor = false, killed = false, reportedFailure = false;
    int pluginLatency = 0, bufferedLatency = 0;
    double tail = 0.0;
    ProcessWatch watch;
    std::atomic<Failure> failure { Failure::none };
    juce::MemoryBlock lastState;

    // Audio thread only.
    uint32_t posted = 0, collected = 0;
    juce::AudioBuffer<float> fifo;
    int fifoRead = 0, fifoCount = 0;
    int64_t stalledSamples = 0, hangLimit = 96000;

    std::unique_ptr<std::atomic<bool>[]> changed; // parameters set since the last block
    size_t changedCount = 0;
    std::atomic<bool> anyChanged { false };

    JUCE_DECLARE_WEAK_REFERENCEABLE (RemoteInstance)
};

RemoteParameter::RemoteParameter (RemoteInstance& o, int i, const juce::var& info)
    : owner (o), index (i), id (info["id"].toString()), name (info["name"].toString()),
      label (info["label"].toString()), defaultValue ((float)(double)info["default"]), steps ((int)info["steps"]),
      discrete ((bool)info["discrete"]), boolean ((bool)info["boolean"]), automatable ((bool)info["automatable"]),
      value ((float)(double)info["value"])
{
}

void RemoteParameter::setValue (float newValue)
{
    value = newValue;
    owner.queueChange (index, newValue);
}

juce::String RemoteParameter::getText (float v, int maximumLength) const
{
    if (juce::MessageManager::existsAndIsCurrentThread())
        if (auto answer = owner.ask ("text", object ({ { "index", index }, { "value", v } }), 500);
            answer.hasProperty ("text"))
            return answer["text"].toString().substring (0, maximumLength);
    return juce::String (v, 2);
}

float RemoteParameter::getValueForText (const juce::String& text) const
{
    if (juce::MessageManager::existsAndIsCurrentThread())
        if (auto answer = owner.ask ("valueForText", object ({ { "index", index }, { "text", text } }), 500);
            answer.hasProperty ("value"))
            return (float)(double)answer["value"];
    return text.getFloatValue();
}

//==============================================================================
#if JUCE_LINUX || JUCE_BSD
/** The plugin's window from the other process, inside Spinus's window (XEmbed). */
class RemoteEditor final : public juce::AudioProcessorEditor
{
public:
    explicit RemoteEditor (RemoteInstance& i) : juce::AudioProcessorEditor (i), instance (i)
    {
        addAndMakeVisible (embed);
        setSize (400, 300);
        instance.onEditorSize = [this] (int w, int h) { setSize (w, h); };
    }

    ~RemoteEditor() override
    {
        instance.onEditorSize = nullptr;
        instance.getConnection().post ("closeEditor", {});
    }

    void parentHierarchyChanged() override
    {
        // The host window exists once this is on screen: then the plugin's window goes into it.
        if (opened || getPeer() == nullptr || embed.getHostWindowID() == 0)
            return;
        opened = true;
        const auto answer =
            instance.ask ("openEditor", object ({ { "parent", (juce::int64)embed.getHostWindowID() } }), 10000);
        if (answer.hasProperty ("width"))
            setSize ((int)answer["width"], (int)answer["height"]);
    }

    void resized() override { embed.setBounds (getLocalBounds()); }

private:
    RemoteInstance& instance;
    juce::XEmbedComponent embed { juce::XEmbedComponentOptions().withWantsKeyboardFocus (true) };
    bool opened = false;
};

juce::AudioProcessorEditor* RemoteInstance::createEditor() { return editor ? new RemoteEditor (*this) : nullptr; }
#else
juce::AudioProcessorEditor* RemoteInstance::createEditor() { return nullptr; }
#endif
} // namespace

//==============================================================================
SandboxPluginFormat::SandboxPluginFormat (
    juce::File host,
    std::function<std::optional<juce::PluginDescription> (const juce::String&, const juce::String&, int)> real)
    : hostExecutable (std::move (host)), findReal (std::move (real))
{
}

juce::PluginDescription SandboxPluginFormat::describe (const juce::PluginDescription& real)
{
    auto d = real;
    d.pluginFormatName = formatName;
    d.fileOrIdentifier = real.pluginFormatName + ":" + real.fileOrIdentifier;
    return d;
}

std::pair<juce::String, juce::String> SandboxPluginFormat::realFormatAndFile (const juce::PluginDescription& d)
{
    return { d.fileOrIdentifier.upToFirstOccurrenceOf (":", false, false),
             d.fileOrIdentifier.fromFirstOccurrenceOf (":", false, false) };
}

void SandboxPluginFormat::createPluginInstance (const juce::PluginDescription& description, double rate, int block,
                                                PluginCreationCallback callback)
{
    const auto [format, file] = realFormatAndFile (description);
    auto fail = [&callback, name = description.name] (const juce::String& why)
    {
        core::Logs::warning ("sandbox", "Could not load " + name + " in its own process: " + why);
        callback (nullptr, why);
    };
    const auto real = findReal ? findReal (format, file, description.uniqueId) : std::nullopt;
    if (! real)
        return fail ("Spinus does not know the plugin " + description.name);
    const auto withHostLibraries = needsHostLibraries && needsHostLibraries (*real);
    const auto& host = withHostLibraries ? hostLibrariesLauncher : hostExecutable;
    if (! host.existsAsFile())
        return fail (host.getFileName() + " is missing");

    auto memory = SharedMemory::create();
    if (memory == nullptr)
        return fail ("Spinus could not share memory with the plugin's process");
    auto connection = std::make_unique<Connection>();
    if (! connection->launchWorkerProcess (host, processId, 5000, 0))
        return fail ("Spinus could not start the plugin's process");

    auto xml = real->createXml();
    const auto info = connection->request (
        "load", object ({ { "description", xml->toString() }, { "memory", memory->getName() } }), 60000);
    memory->unlink(); // both sides have it now
    if (! info.hasProperty ("name"))
        return fail (info.hasProperty ("error") ? info["error"].toString()
                                                : juce::String ("the plugin's process did not answer"));
    core::Logs::info ("sandbox", "Loaded " + real->name + " in its own process (" +
                                     juce::String ((juce::int64)info["process"]) +
                                     (withHostLibraries ? ", with the computer's libraries)" : ")"));

    auto instance =
        std::make_unique<RemoteInstance> (std::move (connection), std::move (memory), description, *real, info);
    instance->setRateAndBufferSizeDetails (rate, block);
    callback (std::move (instance), {});
}

} // namespace spinus::plugins::sandbox
