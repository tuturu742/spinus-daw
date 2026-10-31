#include "SandboxHost.h"
#include "SandboxProtocol.h"

#include "core/Log.h"
#include "plugins/PluginCatalog.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <mutex>

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

/** The plugin's window, inside the window Spinus made for it (XEmbed's host-initiated way). */
class EditorHolder final : private juce::ComponentListener
{
public:
    EditorHolder (std::unique_ptr<juce::AudioProcessorEditor> e, unsigned long parentWindow,
                  std::function<void (int, int)> resized)
        : editor (std::move (e)), onResize (std::move (resized))
    {
        // An X11 window ID, as JUCE takes a native parent window.
        editor->addToDesktop (0, (void*)(juce::pointer_sized_int)parentWindow); // NOLINT(performance-no-int-to-ptr)
        editor->setTopLeftPosition (0, 0);
        editor->setVisible (true);
        editor->addComponentListener (this);
    }

    ~EditorHolder() override
    {
        editor->removeComponentListener (this);
        editor = nullptr;
    }

    juce::AudioProcessorEditor& get() { return *editor; }

private:
    void componentMovedOrResized (juce::Component& c, bool, bool wasResized) override
    {
        if (wasResized)
            onResize (c.getWidth(), c.getHeight());
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::function<void (int, int)> onResize;
};

class Host final : public juce::ChildProcessWorker,
                   private juce::Thread,
                   private juce::AudioProcessorParameter::Listener,
                   private juce::AudioProcessorListener
{
public:
    Host() : juce::Thread ("Sandboxed plugin audio") { PluginCatalog::addFormats (formats); }

    ~Host() override
    {
        stopProcessing();
        editor = nullptr;
        if (instance != nullptr)
        {
            instance->removeListener (this);
            for (auto* p : instance->getParameters())
                p->removeListener (this);
        }
        instance = nullptr;
    }

    void handleMessageFromCoordinator (const juce::MemoryBlock& message) override
    {
        // Plugins expect the message thread for loading, state and windows.
        const auto request = juce::JSON::parse (message.toString());
        juce::MessageManager::callAsync (
            [this, request]
            {
                auto result = handle (request);
                if (! result.isObject())
                    result = object ({});
                result.getDynamicObject()->setProperty ("id", request["id"]);
                send (result);
            });
    }

    void handleConnectionLost() override
    {
        juce::MessageManager::callAsync ([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
    }

private:
    void send (const juce::var& message)
    {
        const auto text = juce::JSON::toString (message, true);
        sendMessageToCoordinator (juce::MemoryBlock (text.toRawUTF8(), text.getNumBytesAsUTF8()));
    }

    juce::var handle (const juce::var& request)
    {
        const auto op = request["op"].toString();
        if (op == "load")
            return load (request);
        if (instance == nullptr)
            return object ({ { "error", "no plugin loaded" } });

        if (op == "prepare")
        {
            stopProcessing();
            const double rate = request["rate"];
            const int block = request["block"];
            instance->setRateAndBufferSizeDetails (rate, block);
            instance->prepareToPlay (rate, block);
            processed = shared->get().posted.load();
            shared->get().done.store (processed, std::memory_order_release); // nothing older is still coming
            midi.ensureSize ((size_t)maxMidiEvents * 8);
            startThread (juce::Thread::Priority::highest);
            return object ({ { "latency", instance->getLatencySamples() } });
        }
        if (op == "release")
        {
            stopProcessing();
            instance->releaseResources();
            return object ({});
        }
        if (op == "getState")
        {
            juce::MemoryBlock state;
            instance->getStateInformation (state);
            return object ({ { "state", state.toBase64Encoding() } });
        }
        if (op == "setState")
        {
            juce::MemoryBlock state;
            state.fromBase64Encoding (request["state"].toString());
            instance->setStateInformation (state.getData(), (int)state.getSize());
            return object ({});
        }
        if (op == "values")
        {
            juce::Array<juce::var> values;
            for (auto* p : instance->getParameters())
                values.add (p->getValue());
            return object ({ { "values", values } });
        }
        if (op == "text" || op == "valueForText")
        {
            auto* parameter = instance->getParameters()[(int)request["index"]];
            if (parameter == nullptr)
                return object ({ { "error", "no such parameter" } });
            if (op == "text")
                return object ({ { "text", parameter->getText ((float)(double)request["value"], 256) } });
            return object ({ { "value", parameter->getValueForText (request["text"].toString()) } });
        }
        if (op == "openEditor")
        {
            editor = nullptr;
            std::unique_ptr<juce::AudioProcessorEditor> made (instance->createEditorAndMakeActive());
            if (made == nullptr)
                return object ({ { "error", "the plugin has no window" } });
            const auto parent = (unsigned long)(juce::int64)request["parent"];
            editor = std::make_unique<EditorHolder> (
                std::move (made), parent,
                [this] (int w, int h) { send (object ({ { "op", "editorSize" }, { "width", w }, { "height", h } })); });
            return object ({ { "width", editor->get().getWidth() }, { "height", editor->get().getHeight() } });
        }
        if (op == "closeEditor")
        {
            editor = nullptr;
            return object ({});
        }
        return object ({ { "error", "unknown request " + op } });
    }

    juce::var load (const juce::var& request)
    {
        auto xml = juce::parseXML (request["description"].toString());
        juce::PluginDescription description;
        if (xml == nullptr || ! description.loadFromXml (*xml))
            return object ({ { "error", "bad plugin description" } });
        shared = SharedMemory::open (request["memory"].toString());
        if (shared == nullptr)
            return object ({ { "error", "cannot open the shared memory" } });

        juce::String error;
        instance = formats.createPluginInstance (description, 48000.0, 512, error);
        if (instance == nullptr)
            return object ({ { "error", error.isNotEmpty() ? error : juce::String ("the plugin did not load") } });

        juce::Array<juce::var> parameters;
        int index = 0;
        for (auto* p : instance->getParameters())
        {
            auto* hosted = dynamic_cast<juce::AudioPluginInstance::HostedParameter*> (p);
            parameters.add (object ({ { "id", hosted != nullptr ? hosted->getParameterID() : juce::String (index) },
                                      { "name", p->getName (256) },
                                      { "label", p->getLabel() },
                                      { "default", p->getDefaultValue() },
                                      { "steps", p->getNumSteps() },
                                      { "discrete", p->isDiscrete() },
                                      { "boolean", p->isBoolean() },
                                      { "automatable", p->isAutomatable() },
                                      { "value", p->getValue() } }));
            p->addListener (this);
            ++index;
        }
        instance->addListener (this);
        return object ({ { "name", instance->getName() },
                         { "inputs", instance->getTotalNumInputChannels() },
                         { "outputs", instance->getTotalNumOutputChannels() },
                         { "acceptsMidi", instance->acceptsMidi() },
                         { "producesMidi", instance->producesMidi() },
                         { "latency", instance->getLatencySamples() },
                         { "tail", instance->getTailLengthSeconds() },
                         { "hasEditor", instance->hasEditor() },
                         { "process", (juce::int64)currentProcessId() },
                         { "parameters", parameters } });
    }

    //==============================================================================
    void stopProcessing()
    {
        signalThreadShouldExit();
        if (shared != nullptr)
            shared->postWork();
        stopThread (4000);
    }

    void run() override
    {
        auto& s = shared->get();
        juce::AudioBuffer<float> buffer;
        float* channels[maxChannels] {};
        const auto params = instance->getParameters();

        while (! threadShouldExit())
        {
            shared->waitForWork (100);

            while (! threadShouldExit() && processed != s.posted.load (std::memory_order_acquire))
            {
                auto& slot = s.slots[processed % slotCount];
                for (int i = 0; i < slot.numChanges; ++i)
                    if (auto* p = params[slot.changes[i].index])
                        p->setValue (slot.changes[i].value);

                midi.clear();
                for (int i = 0; i < slot.numMidi; ++i)
                    midi.addEvent (slot.midi[i].bytes, slot.midi[i].size, slot.midi[i].sample);

                const int numChannels = juce::jlimit (1, maxChannels, s.numChannels);
                for (int c = 0; c < numChannels; ++c)
                    channels[c] = slot.audio[c];
                buffer.setDataToReferTo (channels, numChannels, slot.numSamples);
                instance->processBlock (buffer, midi);

                ++processed;
                s.done.store (processed, std::memory_order_release);
                shared->postFinished();
            }
        }
    }

    //==============================================================================
    void parameterValueChanged (int index, float value) override
    {
        // From the plugin's own window (or the plugin itself): Spinus collects these on its message thread.
        const std::scoped_lock lock (ringLock);
        auto& s = shared->get();
        const auto write = s.ringWrite.load (std::memory_order_relaxed);
        if (write - s.ringRead.load (std::memory_order_acquire) >= (uint32_t)ringSize)
            return; // Spinus is not collecting: drop it
        s.ring[write % ringSize] = { index, value };
        s.ringWrite.store (write + 1, std::memory_order_release);
    }

    void parameterGestureChanged (int index, bool starting) override
    {
        send (object ({ { "op", "gesture" }, { "index", index }, { "begin", starting } }));
    }

    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}

    void audioProcessorChanged (juce::AudioProcessor* p, const ChangeDetails& details) override
    {
        if (details.latencyChanged)
            send (object ({ { "op", "latency" }, { "samples", p->getLatencySamples() } }));
    }

    juce::AudioPluginFormatManager formats;
    std::unique_ptr<juce::AudioPluginInstance> instance;
    std::unique_ptr<SharedMemory> shared;
    std::unique_ptr<EditorHolder> editor;
    juce::MidiBuffer midi;
    uint32_t processed = 0;
    std::mutex ringLock;
};
} // namespace

int runSandboxHost (const juce::String& commandLine)
{
    auto host = std::make_unique<Host>();
    if (! host->initialiseFromCommandLine (commandLine, processId, 10000))
    {
        core::Logs::error ("sandbox", "Could not connect to Spinus (started with: " + commandLine + ")");
        return 1;
    }
    juce::MessageManager::getInstance()->runDispatchLoop();
    host = nullptr;
    return 0;
}

} // namespace spinus::plugins::sandbox
