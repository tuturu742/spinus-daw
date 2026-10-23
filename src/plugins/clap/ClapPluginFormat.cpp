#include "ClapPluginFormat.h"

#include <SpinusVersion.h>

#include "dsp/NoteExpression.h"

#include <clap/clap.h>
#include <juce_gui_extra/juce_gui_extra.h>

#if JUCE_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <poll.h>
#endif

#include <atomic>
#include <map>
#include <mutex>
#include <numeric>
#include <thread>

namespace spinus::plugins
{

namespace
{

//==============================================================================
// A .clap file is a shared library (Linux) or a DLL (Windows).
#if JUCE_WINDOWS
void* openLibrary (const juce::String& path) { return LoadLibraryW (path.toWideCharPointer()); }
void* findSymbol (void* library, const char* name)
{
    return reinterpret_cast<void*> (GetProcAddress (static_cast<HMODULE> (library), name));
}
void closeLibrary (void* library) { FreeLibrary (static_cast<HMODULE> (library)); }
juce::String lastLibraryError() { return "Windows error " + juce::String ((int)GetLastError()); }
// CLAP plugins open their windows in a window of the host's.
constexpr const char* windowApi = CLAP_WINDOW_API_WIN32;
#else
void* openLibrary (const juce::String& path) { return dlopen (path.toRawUTF8(), RTLD_LOCAL | RTLD_NOW); }
void* findSymbol (void* library, const char* name) { return dlsym (library, name); }
void closeLibrary (void* library) { dlclose (library); }
juce::String lastLibraryError() { return dlerror(); }
constexpr const char* windowApi = CLAP_WINDOW_API_X11;
#endif

//==============================================================================
/** A loaded .clap binary. clap_entry::init/deinit run exactly once per load; instances keep the
    module alive through a shared_ptr, and loaded modules are cached by path. */
class ClapModule
{
public:
    static std::shared_ptr<ClapModule> load (const juce::File& file, juce::String& error)
    {
        auto& cache = getCache();
        const std::scoped_lock lock (cache.mutex);
        const auto& path = file.getFullPathName();

        if (auto existing = cache.modules[path].lock())
            return existing;

        auto* handle = openLibrary (path);

        if (handle == nullptr)
        {
            error = "Could not load " + path + ": " + lastLibraryError();
            return {};
        }

        auto* entry = static_cast<const clap_plugin_entry_t*> (findSymbol (handle, "clap_entry"));

        if (entry == nullptr || ! clap_version_is_compatible (entry->clap_version) || ! entry->init (path.toRawUTF8()))
        {
            error = "Not a compatible CLAP plugin: " + path;
            closeLibrary (handle);
            return {};
        }

        auto* factory = static_cast<const clap_plugin_factory_t*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID));

        if (factory == nullptr)
        {
            error = "CLAP plugin has no plugin factory: " + path;
            entry->deinit();
            closeLibrary (handle);
            return {};
        }

        auto module = std::shared_ptr<ClapModule> (new ClapModule (handle, entry, factory, file));
        cache.modules[path] = module;
        return module;
    }

    /** The files of the modules loaded now. */
    static juce::StringArray getLoadedFiles()
    {
        auto& cache = getCache();
        const std::scoped_lock lock (cache.mutex);
        juce::StringArray files;
        for (auto& [path, module] : cache.modules)
            if (! module.expired())
                files.add (path);
        return files;
    }

    ~ClapModule()
    {
        entry->deinit();
        closeLibrary (handle);
    }

    const clap_plugin_factory_t& getFactory() const noexcept { return *factory; }
    const clap_plugin_entry_t& getEntry() const noexcept { return *entry; }
    const juce::File& getFile() const noexcept { return file; }

    const clap_plugin_descriptor_t* findDescriptor (int uniqueId) const
    {
        for (uint32_t i = 0; i < factory->get_plugin_count (factory); ++i)
            if (auto* d = factory->get_plugin_descriptor (factory, i);
                d != nullptr && juce::String (d->id).hashCode() == uniqueId)
                return d;

        return nullptr;
    }

private:
    struct Cache
    {
        std::mutex mutex;
        std::map<juce::String, std::weak_ptr<ClapModule>> modules;
    };

    static Cache& getCache()
    {
        static Cache cache;
        return cache;
    }

    ClapModule (void* h, const clap_plugin_entry_t* e, const clap_plugin_factory_t* f, juce::File fileToUse)
        : handle (h), entry (e), factory (f), file (std::move (fileToUse))
    {
    }

    void* handle;
    const clap_plugin_entry_t* entry;
    const clap_plugin_factory_t* factory;
    juce::File file;
};

bool hasFeature (const clap_plugin_descriptor_t& d, const char* feature)
{
    if (d.features != nullptr)
        for (auto* f = d.features; *f != nullptr; ++f)
            if (std::strcmp (*f, feature) == 0)
                return true;

    return false;
}

//==============================================================================
/** Fixed-capacity input event list handed to the plugin. Filled on the audio thread without
    allocating. Events must be appended in time order. */
class InputEventList
{
public:
    InputEventList()
    {
        events.resize (capacity);
        list.ctx = this;
        list.size = [] (const clap_input_events_t* l) { return static_cast<const InputEventList*> (l->ctx)->count; };
        list.get = [] (const clap_input_events_t* l, uint32_t index) -> const clap_event_header_t*
        {
            auto* self = static_cast<const InputEventList*> (l->ctx);
            return index < self->count ? &self->events[index].header : nullptr;
        };
    }

    void clear() noexcept { count = 0; }

    void addNote (uint16_t type, uint32_t time, int16_t channel, int16_t key, double velocity) noexcept
    {
        if (auto* e = next())
        {
            e->note = {};
            e->note.header = { sizeof (clap_event_note_t), time, CLAP_CORE_EVENT_SPACE_ID, type, 0 };
            e->note.note_id = -1;
            e->note.port_index = 0;
            e->note.channel = channel;
            e->note.key = key;
            e->note.velocity = velocity;
        }
    }

    void addExpression (uint32_t time, clap_note_expression expression, int16_t channel, int16_t key,
                        double value) noexcept
    {
        if (auto* e = next())
        {
            e->expression = {};
            e->expression.header = { sizeof (clap_event_note_expression_t), time, CLAP_CORE_EVENT_SPACE_ID,
                                     CLAP_EVENT_NOTE_EXPRESSION, 0 };
            e->expression.expression_id = expression;
            e->expression.note_id = -1;
            e->expression.port_index = 0;
            e->expression.channel = channel;
            e->expression.key = key;
            e->expression.value = value;
        }
    }

    void addMidi (uint32_t time, const uint8_t* data, int size) noexcept
    {
        if (auto* e = next())
        {
            e->midi = {};
            e->midi.header = { sizeof (clap_event_midi_t), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
            e->midi.port_index = 0;
            std::copy_n (data, std::min (size, 3), e->midi.data);
        }
    }

    void addParam (uint32_t time, clap_id id, void* cookie, double value) noexcept
    {
        if (auto* e = next())
        {
            e->param = {};
            e->param.header = { sizeof (clap_event_param_value_t), time, CLAP_CORE_EVENT_SPACE_ID,
                                CLAP_EVENT_PARAM_VALUE, 0 };
            e->param.param_id = id;
            e->param.cookie = cookie;
            e->param.note_id = -1;
            e->param.port_index = -1;
            e->param.channel = -1;
            e->param.key = -1;
            e->param.value = value;
        }
    }

    const clap_input_events_t* get() const noexcept { return &list; }

private:
    union AnyEvent
    {
        clap_event_header_t header;
        clap_event_note_t note;
        clap_event_note_expression_t expression;
        clap_event_midi_t midi;
        clap_event_param_value_t param;
    };

    AnyEvent* next() noexcept { return count < capacity ? &events[count++] : nullptr; }

    static constexpr uint32_t capacity = 4096;
    std::vector<AnyEvent> events;
    uint32_t count = 0;
    clap_input_events_t list {};
};

//==============================================================================
class ClapPluginInstance;
class ClapEditor;

class ClapParameter : public juce::HostedAudioProcessorParameter
{
public:
    ClapParameter (ClapPluginInstance& o, const clap_param_info_t& i, double initialValue)
        : owner (o), info (i), current (initialValue)
    {
    }

    juce::String getParameterID() const override { return juce::String (info.id); }
    float getValue() const override { return toNormalised (current.load()); }
    void setValue (float newValue) override;
    float getDefaultValue() const override { return toNormalised (info.default_value); }
    juce::String getName (int maximumLength) const override
    {
        return juce::String (info.name).substring (0, maximumLength);
    }
    juce::String getLabel() const override { return {}; }
    juce::String getText (float normalisedValue, int maximumLength) const override;
    float getValueForText (const juce::String& text) const override;
    bool isAutomatable() const override { return (info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0; }
    bool isDiscrete() const override { return (info.flags & CLAP_PARAM_IS_STEPPED) != 0; }
    bool isBoolean() const override { return isDiscrete() && info.max_value - info.min_value <= 1.0; }

    int getNumSteps() const override
    {
        return isDiscrete() ? juce::jmax (2, (int)(info.max_value - info.min_value) + 1)
                            : juce::AudioProcessor::getDefaultNumParameterSteps();
    }

    /** The plugin renamed the parameter (e.g. Surge XT's macros after a preset load). */
    void updateInfo (const clap_param_info_t& newInfo) noexcept
    {
        std::copy (std::begin (newInfo.name), std::end (newInfo.name), std::begin (info.name));
        std::copy (std::begin (newInfo.module), std::end (newInfo.module), std::begin (info.module));
    }

    /** Called when the plugin itself changes the value (output events, rescans). */
    void updateFromPlugin (double plainValue)
    {
        current = plainValue;
        sendValueChangedMessageToListeners (toNormalised (plainValue));
    }

    clap_id getClapId() const noexcept { return info.id; }
    void* getCookie() const noexcept { return info.cookie; }

    double toPlain (float normalised) const
    {
        auto v = info.min_value + (double)normalised * (info.max_value - info.min_value);
        return isDiscrete() ? std::round (v) : v;
    }

    float toNormalised (double plain) const
    {
        const auto range = info.max_value - info.min_value;
        return range > 0.0 ? (float)juce::jlimit (0.0, 1.0, (plain - info.min_value) / range) : 0.0f;
    }

private:
    ClapPluginInstance& owner;
    clap_param_info_t info;
    std::atomic<double> current;
};

//==============================================================================
class ClapPluginInstance : public juce::AudioPluginInstance
{
public:
    static std::unique_ptr<ClapPluginInstance> create (std::shared_ptr<ClapModule> module,
                                                       const clap_plugin_descriptor_t& descriptor, juce::String& error)
    {
        std::unique_ptr<ClapPluginInstance> instance (new ClapPluginInstance (std::move (module), descriptor));

        if (! instance->initialise (error))
            return {};

        return instance;
    }

    ~ClapPluginInstance() override
    {
        *alive = false;
        timers.clear();
        fdPoller = nullptr;

        if (plugin != nullptr)
        {
            releaseResources();
            plugin->destroy (plugin);
        }
    }

    //==============================================================================
    const juce::String getName() const override { return descriptor.name; }

    void fillInPluginDescription (juce::PluginDescription& d) const override
    {
        d.name = descriptor.name;
        d.descriptiveName = descriptor.description != nullptr ? descriptor.description : descriptor.name;
        d.pluginFormatName = ClapPluginFormat::formatName;
        d.category = hasFeature (descriptor, CLAP_PLUGIN_FEATURE_INSTRUMENT) ? "Instrument" : "Effect";
        d.manufacturerName = descriptor.vendor != nullptr ? descriptor.vendor : "";
        d.version = descriptor.version != nullptr ? descriptor.version : "";
        d.fileOrIdentifier = module->getFile().getFullPathName();
        d.lastFileModTime = module->getFile().getLastModificationTime();
        d.lastInfoUpdateTime = juce::Time::getCurrentTime();
        d.uniqueId = d.deprecatedUid = juce::String (descriptor.id).hashCode();
        d.isInstrument = hasFeature (descriptor, CLAP_PLUGIN_FEATURE_INSTRUMENT);
        d.numInputChannels = (int)numInputChannels;
        d.numOutputChannels = (int)numOutputChannels;
        d.hasSharedContainer = false;
        d.hasARAExtension = false;
    }

    //==============================================================================
    void prepareToPlay (double sampleRate, int maximumBlockSize) override
    {
        JUCE_ASSERT_MESSAGE_THREAD
        releaseResources();

        setRateAndBufferSizeDetails (sampleRate, maximumBlockSize);
        maxBlockSize = juce::jmax (1, maximumBlockSize);
        inputScratch.setSize ((int)numInputChannels, maxBlockSize, false, true, true);
        outputScratch.setSize ((int)numOutputChannels, maxBlockSize, false, true, true);
        outputPointers.resize (numOutputChannels);
        inputPointers.resize (numInputChannels);
        prepareAudioPortBuffers();

        active = plugin->activate (plugin, sampleRate, 1, (uint32_t)maximumBlockSize);

        if (active)
            refreshLatency();
        noteExpressions.reset();
    }

    /** The CLAP spec requires a buffer for every declared audio port. Port 0 is the main port and
        maps to the JUCE buffer; other ports (e.g. sidechains) are not routed yet, so extra inputs
        read silence and extra outputs are discarded. */
    void prepareAudioPortBuffers()
    {
        auto countExtra = [] (const std::vector<uint32_t>& ports)
        { return ports.size() > 1 ? std::accumulate (ports.begin() + 1, ports.end(), 0u) : 0u; };

        extraInputChannels = countExtra (inputPortChannels);
        const auto extraChannels = extraInputChannels + countExtra (outputPortChannels);
        extraScratch.setSize ((int)extraChannels, maxBlockSize, false, true, true);
        extraScratch.clear();
        extraPointers.resize (extraChannels);

        for (uint32_t ch = 0; ch < extraChannels; ++ch)
            extraPointers[ch] = extraScratch.getWritePointer ((int)ch);

        uint32_t next = 0;

        auto assign = [&] (std::vector<clap_audio_buffer_t>& buffers, const std::vector<uint32_t>& ports,
                           std::vector<float*>& mainPointers)
        {
            buffers.assign (ports.size(), {});

            for (size_t i = 0; i < ports.size(); ++i)
            {
                buffers[i].channel_count = ports[i];
                buffers[i].data32 = i == 0 ? mainPointers.data() : extraPointers.data() + next;
                next += i == 0 ? 0 : ports[i];
            }
        };

        assign (inputBuffers, inputPortChannels, inputPointers);
        assign (outputBuffers, outputPortChannels, outputPointers);
    }

    void releaseResources() override
    {
        if (processing)
        {
            // stop_processing belongs to the audio thread; audio has stopped, so this thread stands in for it.
            stoppingOnThisThread = true;
            plugin->stop_processing (plugin);
            stoppingOnThisThread = false;
            processing = false;
        }

        if (active)
        {
            plugin->deactivate (plugin);
            active = false;
        }
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;
        audioThreadId = std::this_thread::get_id();

        if (! active)
        {
            buffer.clear();
            midi.clear();
            return;
        }

        if (! processing)
        {
            // start_processing is an audio-thread call: the plugin may ask (and strict plugins, such as
            // Odin 2, end the program when the answer is no).
            startingOnThisThread = true;
            processing = plugin->start_processing (plugin);
            startingOnThisThread = false;
        }

        // The plugin was activated for at most maxBlockSize frames, so larger blocks are split.
        const auto total = buffer.getNumSamples();

        for (int start = 0; start < total; start += maxBlockSize)
            processChunk (buffer, midi, start, std::min (maxBlockSize, total - start));

        midi.clear();
    }

    using juce::AudioPluginInstance::processBlock;

    //==============================================================================
    bool isBusesLayoutSupported (const BusesLayout& layout) const override { return layout == supportedLayout; }
    bool canAddBus (bool isInput) const override { return isInput && getBusCount (true) < 2; }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return hasNotePorts; }
    bool producesMidi() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return guiExt != nullptr; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override
    {
        JUCE_ASSERT_MESSAGE_THREAD
        destData.reset();

        if (stateExt == nullptr)
            return;

        juce::MemoryOutputStream out (destData, false);
        clap_ostream_t stream {};
        stream.ctx = &out;
        stream.write = [] (const clap_ostream_t* s, const void* data, uint64_t size) -> int64_t
        { return static_cast<juce::MemoryOutputStream*> (s->ctx)->write (data, (size_t)size) ? (int64_t)size : -1; };

        if (! stateExt->save (plugin, &stream))
            destData.reset();
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        JUCE_ASSERT_MESSAGE_THREAD

        if (stateExt == nullptr)
            return;

        juce::MemoryInputStream in (data, (size_t)sizeInBytes, false);
        clap_istream_t stream {};
        stream.ctx = &in;
        stream.read = [] (const clap_istream_t* s, void* buffer, uint64_t size) -> int64_t
        { return static_cast<juce::MemoryInputStream*> (s->ctx)->read (buffer, (int)size); };

        if (stateExt->load (plugin, &stream))
            refreshParameterValues();
    }

    //==============================================================================
    void queueParameterChange (const ClapParameter& p, double plainValue)
    {
        if (paramFifo.getFreeSpace() == 0)
            return; // full: drop rather than block

        paramFifo.write (1).forEach (
            [&] (int index)
            { paramQueue[static_cast<size_t> (index)] = { p.getClapId(), p.getCookie(), plainValue }; });

        if (! active && paramsExt != nullptr && juce::MessageManager::existsAndIsCurrentThread())
            flushParametersWhileInactive();
    }

    juce::String parameterValueToText (clap_id id, double plain) const
    {
        char text[CLAP_NAME_SIZE] {};

        if (paramsExt != nullptr && paramsExt->value_to_text (plugin, id, plain, text, sizeof (text)))
            return juce::String::fromUTF8 (text);

        return juce::String (plain, 3);
    }

    std::optional<double> parameterTextToValue (clap_id id, const juce::String& text) const
    {
        double value = 0.0;

        if (paramsExt != nullptr && paramsExt->text_to_value (plugin, id, text.toRawUTF8(), &value))
            return value;

        return std::nullopt;
    }

private:
    ClapPluginInstance (std::shared_ptr<ClapModule> m, const clap_plugin_descriptor_t& d)
        : juce::AudioPluginInstance (BusesProperties()
                                         .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                         .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          module (std::move (m)), descriptor (d)
    {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = productName;
        host.vendor = "Spinus DAW";
        host.url = "https://github.com/tuturu742/spinus-daw";
        host.version = versionString;
        host.get_extension = [] (const clap_host_t* h, const char* id) { return self (h)->getHostExtension (id); };
        host.request_restart = [] (const clap_host_t* h)
        { self (h)->onMainThread ([] (ClapPluginInstance& p) { p.restart(); }); };
        host.request_process = [] (const clap_host_t*) {};
        host.request_callback = [] (const clap_host_t* h)
        { self (h)->onMainThread ([] (ClapPluginInstance& p) { p.plugin->on_main_thread (p.plugin); }); };

        outputEvents.ctx = this;
        outputEvents.try_push = [] (const clap_output_events_t* list, const clap_event_header_t* e)
        {
            static_cast<ClapPluginInstance*> (list->ctx)->handleOutputEvent (*e);
            return true;
        };

        paramQueue.resize ((size_t)paramFifo.getTotalSize());
    }

    friend class ClapEditor;

    static ClapPluginInstance* self (const clap_host_t* h) { return static_cast<ClapPluginInstance*> (h->host_data); }

    bool initialise (juce::String& error)
    {
        plugin = module->getFactory().create_plugin (&module->getFactory(), &host, descriptor.id);

        if (plugin == nullptr || ! plugin->init (plugin))
        {
            error = juce::String ("Could not create CLAP plugin ") + descriptor.name;

            if (plugin != nullptr)
                plugin->destroy (plugin);

            plugin = nullptr;
            return false;
        }

        paramsExt = static_cast<const clap_plugin_params_t*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
        stateExt = static_cast<const clap_plugin_state_t*> (plugin->get_extension (plugin, CLAP_EXT_STATE));
        latencyExt = static_cast<const clap_plugin_latency_t*> (plugin->get_extension (plugin, CLAP_EXT_LATENCY));
        timerExt =
            static_cast<const clap_plugin_timer_support_t*> (plugin->get_extension (plugin, CLAP_EXT_TIMER_SUPPORT));
        fdExt = static_cast<const clap_plugin_posix_fd_support_t*> (
            plugin->get_extension (plugin, CLAP_EXT_POSIX_FD_SUPPORT));

        // Plugin windows: embedded in Spinus's (X11 or Win32; floating windows are not used).
        if (auto* gui = static_cast<const clap_plugin_gui_t*> (plugin->get_extension (plugin, CLAP_EXT_GUI)))
            if (gui->is_api_supported (plugin, windowApi, false))
                guiExt = gui;

        if (auto* notePorts =
                static_cast<const clap_plugin_note_ports_t*> (plugin->get_extension (plugin, CLAP_EXT_NOTE_PORTS)))
        {
            hasNotePorts = notePorts->count (plugin, true) > 0;
            clap_note_port_info_t info {};
            clapNoteDialect = hasNotePorts && notePorts->get (plugin, 0, true, &info) &&
                              (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
        }

        if (auto* audioPorts =
                static_cast<const clap_plugin_audio_ports_t*> (plugin->get_extension (plugin, CLAP_EXT_AUDIO_PORTS)))
        {
            auto portChannels = [&] (bool isInput)
            {
                std::vector<uint32_t> channels;

                for (uint32_t i = 0; i < audioPorts->count (plugin, isInput); ++i)
                {
                    clap_audio_port_info_t info {};
                    channels.push_back (audioPorts->get (plugin, i, isInput, &info) ? info.channel_count : 0);
                }

                return channels;
            };

            inputPortChannels = portChannels (true);
            outputPortChannels = portChannels (false);
            numInputChannels = inputPortChannels.empty() ? 0 : inputPortChannels[0];
            numOutputChannels = outputPortChannels.empty() ? 0 : outputPortChannels[0];
        }

        // Expose the plugin's main ports as JUCE buses; hosts (Tracktion) size buffers from these.
        auto channelSet = [] (uint32_t n)
        { return n == 0 ? juce::AudioChannelSet::disabled() : juce::AudioChannelSet::canonicalChannelSet ((int)n); };

        supportedLayout.inputBuses.add (channelSet (numInputChannels));
        supportedLayout.outputBuses.add (channelSet (numOutputChannels));

        // A second input port is the sidechain: exposed as a second JUCE input bus so hosts (Tracktion's
        // sidechain routing) can feed it. Further ports still get silence.
        sidechainChannels = inputPortChannels.size() > 1 ? inputPortChannels[1] : 0;
        if (sidechainChannels > 0 && addBus (true))
            supportedLayout.inputBuses.add (channelSet (sidechainChannels));
        else
            sidechainChannels = 0;

        setBusesLayout (supportedLayout);

        createParameters();
        return true;
    }

    void createParameters()
    {
        if (paramsExt == nullptr)
            return;

        juce::AudioProcessorParameterGroup group;

        for (uint32_t i = 0; i < paramsExt->count (plugin); ++i)
        {
            clap_param_info_t info {};

            if (! paramsExt->get_info (plugin, i, &info) || (info.flags & CLAP_PARAM_IS_HIDDEN) != 0)
                continue;

            double value = info.default_value;
            paramsExt->get_value (plugin, info.id, &value);

            auto param = std::make_unique<ClapParameter> (*this, info, value);
            parametersById[info.id] = param.get();
            group.addChild (std::move (param));
        }

        setHostedParameterTree (std::move (group));
    }

    void refreshParameterValues()
    {
        if (paramsExt == nullptr)
            return;

        for (auto& [id, param] : parametersById)
        {
            double value = 0.0;

            if (paramsExt->get_value (plugin, id, &value))
                param->updateFromPlugin (value);
        }
    }

    /** Names may change at any time (CLAP_PARAM_RESCAN_INFO / TEXT); the list itself does not here. */
    void refreshParameterInfo()
    {
        if (paramsExt == nullptr)
            return;

        for (uint32_t i = 0; i < paramsExt->count (plugin); ++i)
        {
            clap_param_info_t info {};
            if (paramsExt->get_info (plugin, i, &info))
                if (auto it = parametersById.find (info.id); it != parametersById.end())
                    it->second->updateInfo (info);
        }

        updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withParameterInfoChanged (true));
    }

    //==============================================================================
    // Presets in the plugin's own file format (clap.preset-load), e.g. Surge XT's .fxp patches.
public:
    const clap_plugin_preset_load_t* getPresetLoad() const noexcept
    {
        if (auto* ext = plugin->get_extension (plugin, CLAP_EXT_PRESET_LOAD))
            return static_cast<const clap_plugin_preset_load_t*> (ext);
        return static_cast<const clap_plugin_preset_load_t*> (
            plugin->get_extension (plugin, CLAP_EXT_PRESET_LOAD_COMPAT));
    }

    juce::Result loadPresetFile (const juce::File& file)
    {
        return loadPresetFrom (CLAP_PRESET_DISCOVERY_LOCATION_FILE, file.getFullPathName(), {});
    }

    /** A preset by its preset-discovery location: a file (with a load key for a file holding several), or one
        of the plugin's own (CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, by load key). */
    juce::Result loadPresetFrom (uint32_t kind, const juce::String& location, const juce::String& loadKey)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        const auto* ext = getPresetLoad();
        if (ext == nullptr)
            return juce::Result::fail (getName() + " cannot load presets from Spinus.");

        presetError.clear();
        const bool isFile = kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE;
        if (! ext->from_location (plugin, kind, isFile ? location.toRawUTF8() : nullptr,
                                  loadKey.isNotEmpty() ? loadKey.toRawUTF8() : nullptr))
            return juce::Result::fail (presetError.isNotEmpty()
                                           ? presetError
                                           : "Could not load the preset " +
                                                 (isFile ? juce::File (location).getFileName() : loadKey));

        refreshParameterInfo();
        refreshParameterValues();
        return juce::Result::ok();
    }

    /** The presets the plugin's preset-discovery providers list for this plugin (clap.preset-discovery-factory),
        at most `most`: those inside the plugin, and the files in the folders it names (with the file types it
        declares). Message thread; it reads the plugin's preset files, so it is not instant. */
    std::vector<ClapPluginFormat::DiscoveredPreset> discoverPresets (int most)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        std::vector<ClapPluginFormat::DiscoveredPreset> found;
        const auto& entry = module->getEntry();
        const auto* factory =
            static_cast<const clap_preset_discovery_factory_t*> (entry.get_factory (CLAP_PRESET_DISCOVERY_FACTORY_ID));
        if (factory == nullptr)
            factory = static_cast<const clap_preset_discovery_factory_t*> (
                entry.get_factory (CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT));
        if (factory == nullptr || getPresetLoad() == nullptr)
            return found;

        struct Declared
        {
            juce::StringArray extensions;
            struct Location
            {
                uint32_t kind = CLAP_PRESET_DISCOVERY_LOCATION_FILE, flags = 0;
                juce::String name, location;
            };
            std::vector<Location> locations;
        };
        struct Receiving
        {
            std::vector<ClapPluginFormat::DiscoveredPreset>* found = nullptr;
            ClapPluginFormat::DiscoveredPreset base, next; // base: where the presets being read are
            bool open = false, forUs = true, anyId = false;
            juce::String pluginId;
            int most = 0;

            void finish()
            {
                if (open && (forUs || ! anyId) && (int)found->size() < most)
                    found->push_back (next);
                open = false;
            }
        };

        for (uint32_t p = 0; p < factory->count (factory) && (int)found.size() < most; ++p)
        {
            const auto* providerInfo = factory->get_descriptor (factory, p);
            if (providerInfo == nullptr || providerInfo->id == nullptr)
                continue;
            Declared declared;
            clap_preset_discovery_indexer_t indexer {};
            indexer.clap_version = CLAP_VERSION;
            indexer.name = "Spinus DAW";
            indexer.vendor = "Spinus";
            indexer.url = "";
            indexer.version = spinus::versionString;
            indexer.indexer_data = &declared;
            indexer.declare_filetype =
                [] (const clap_preset_discovery_indexer_t* i, const clap_preset_discovery_filetype_t* type)
            {
                if (type != nullptr && type->file_extension != nullptr)
                    static_cast<Declared*> (i->indexer_data)->extensions.add (type->file_extension);
                return true;
            };
            indexer.declare_location =
                [] (const clap_preset_discovery_indexer_t* i, const clap_preset_discovery_location_t* location)
            {
                if (location != nullptr)
                    static_cast<Declared*> (i->indexer_data)
                        ->locations.push_back (
                            { location->kind, location->flags,
                              juce::String::fromUTF8 (location->name != nullptr ? location->name : ""),
                              juce::String::fromUTF8 (location->location != nullptr ? location->location : "") });
                return true;
            };
            indexer.declare_soundpack = [] (const clap_preset_discovery_indexer_t*,
                                            const clap_preset_discovery_soundpack_t*) { return true; };
            indexer.get_extension = [] (const clap_preset_discovery_indexer_t*, const char*) -> const void*
            { return nullptr; };

            const auto* provider = factory->create (factory, &indexer, providerInfo->id);
            if (provider == nullptr)
                continue;
            if (provider->init (provider))
            {
                Receiving receiving;
                receiving.found = &found;
                receiving.pluginId = descriptor.id;
                receiving.most = most;
                clap_preset_discovery_metadata_receiver_t receiver {};
                receiver.receiver_data = &receiving;
                receiver.on_error = [] (const clap_preset_discovery_metadata_receiver_t*, int32_t, const char*) {};
                receiver.begin_preset =
                    [] (const clap_preset_discovery_metadata_receiver_t* r, const char* name, const char* key)
                {
                    auto& in = *static_cast<Receiving*> (r->receiver_data);
                    in.finish();
                    if ((int)in.found->size() >= in.most)
                        return false;
                    in.next = in.base;
                    in.next.name = juce::String::fromUTF8 (name != nullptr ? name : "");
                    in.next.loadKey = juce::String::fromUTF8 (key != nullptr ? key : "");
                    in.open = in.next.name.isNotEmpty();
                    in.forUs = false;
                    in.anyId = false;
                    return true;
                };
                receiver.add_plugin_id =
                    [] (const clap_preset_discovery_metadata_receiver_t* r, const clap_universal_plugin_id_t* id)
                {
                    auto& in = *static_cast<Receiving*> (r->receiver_data);
                    in.anyId = true;
                    if (id != nullptr && id->abi != nullptr && id->id != nullptr &&
                        std::strcmp (id->abi, "clap") == 0 && in.pluginId == id->id)
                        in.forUs = true;
                };
                receiver.set_soundpack_id = [] (const clap_preset_discovery_metadata_receiver_t*, const char*) {};
                receiver.set_flags = [] (const clap_preset_discovery_metadata_receiver_t* r, uint32_t flags)
                {
                    auto& in = *static_cast<Receiving*> (r->receiver_data);
                    in.next.factory = (flags & CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT) != 0;
                };
                receiver.add_creator = [] (const clap_preset_discovery_metadata_receiver_t*, const char*) {};
                receiver.set_description = [] (const clap_preset_discovery_metadata_receiver_t*, const char*) {};
                receiver.set_timestamps = [] (const clap_preset_discovery_metadata_receiver_t*, clap_timestamp,
                                              clap_timestamp) {};
                receiver.add_feature = [] (const clap_preset_discovery_metadata_receiver_t*, const char*) {};
                receiver.add_extra_info = [] (const clap_preset_discovery_metadata_receiver_t*, const char*,
                                              const char*) {};

                const auto read =
                    [&] (uint32_t kind, const juce::String& location, const juce::String& category, uint32_t flags)
                {
                    receiving.base = {};
                    receiving.base.kind = (int)kind;
                    receiving.base.location = location;
                    receiving.base.category = category;
                    receiving.base.factory = (flags & CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT) != 0;
                    receiving.open = false;
                    provider->get_metadata (
                        provider, kind, kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE ? location.toRawUTF8() : nullptr,
                        &receiver);
                    receiving.finish();
                };
                for (const auto& location : declared.locations)
                {
                    if ((int)found.size() >= most)
                        break;
                    if (location.kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
                    {
                        read (location.kind, {}, location.name, location.flags);
                        continue;
                    }
                    const juce::File where (location.location);
                    if (where.existsAsFile())
                    {
                        read (location.kind, where.getFullPathName(), location.name, location.flags);
                        continue;
                    }
                    if (! where.isDirectory())
                        continue;
                    juce::StringArray patterns;
                    for (const auto& extension : declared.extensions)
                        patterns.add ("*." + extension.trimCharactersAtStart ("."));
                    std::vector<juce::File> files;
                    for (const auto& item : juce::RangedDirectoryIterator (
                             where, true, patterns.isEmpty() ? "*" : patterns.joinIntoString (";"),
                             juce::File::findFiles))
                    {
                        files.push_back (item.getFile());
                        if ((int)files.size() >= 4 * most)
                            break;
                    }
                    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
                               { return a.getFullPathName().compareNatural (b.getFullPathName()) < 0; });
                    for (const auto& file : files)
                    {
                        if ((int)found.size() >= most)
                            break;
                        const auto folder = file.getParentDirectory();
                        read (location.kind, file.getFullPathName(),
                              folder == where ? location.name : folder.getRelativePathFrom (where).replace ("\\", "/"),
                              location.flags);
                    }
                }
            }
            provider->destroy (provider);
        }
        return found;
    }

private:
    void refreshLatency()
    {
        if (latencyExt != nullptr)
            setLatencySamples ((int)latencyExt->get (plugin));
    }

    void restart()
    {
        if (active)
            prepareToPlay (getSampleRate(), getBlockSize());
    }

    void flushParametersWhileInactive()
    {
        inputEvents.clear();
        drainParameterQueue();
        paramsExt->flush (plugin, inputEvents.get(), &outputEvents);
        inputEvents.clear();
    }

    void drainParameterQueue() noexcept
    {
        paramFifo.read (paramFifo.getNumReady())
            .forEach (
                [this] (int index)
                {
                    const auto& change = paramQueue[static_cast<size_t> (index)];
                    inputEvents.addParam (0, change.id, change.cookie, change.value);
                });
    }

    void processChunk (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, int start,
                       int numSamples) noexcept
    {
        const auto bufferChannels = (uint32_t)buffer.getNumChannels();

        // Inputs are copied so the plugin never sees aliased input/output buffers. Channels the
        // host buffer lacks are fed silence / written to scratch.
        for (uint32_t ch = 0; ch < numInputChannels; ++ch)
        {
            if (ch < bufferChannels)
                inputScratch.copyFrom ((int)ch, 0, buffer, (int)ch, start, numSamples);
            else
                inputScratch.clear ((int)ch, 0, numSamples);

            inputPointers[ch] = inputScratch.getWritePointer ((int)ch);
        }

        for (uint32_t ch = 0; ch < numOutputChannels; ++ch)
            outputPointers[ch] =
                ch < bufferChannels ? buffer.getWritePointer ((int)ch, start) : outputScratch.getWritePointer ((int)ch);

        // Extra input ports: the sidechain (port 1) comes from the buffer channels after the main input,
        // anything else is silent.
        for (uint32_t ch = 0; ch < extraInputChannels; ++ch)
        {
            const auto source = numInputChannels + ch;
            if (ch < sidechainChannels && source < bufferChannels)
                extraScratch.copyFrom ((int)ch, 0, buffer, (int)source, start, numSamples);
            else
                extraScratch.clear ((int)ch, 0, numSamples);
        }

        fillInputEvents (midi, start, numSamples);

        clap_event_transport_t transport {};
        const bool hasTransport = start == 0 && fillTransport (transport);

        clap_process_t process {};
        process.steady_time = steadyTime;
        process.frames_count = (uint32_t)numSamples;
        process.transport = hasTransport ? &transport : nullptr;
        process.audio_inputs = inputBuffers.empty() ? nullptr : inputBuffers.data();
        process.audio_inputs_count = (uint32_t)inputBuffers.size();
        process.audio_outputs = outputBuffers.empty() ? nullptr : outputBuffers.data();
        process.audio_outputs_count = (uint32_t)outputBuffers.size();
        process.in_events = inputEvents.get();
        process.out_events = &outputEvents;

        const bool failed = plugin->process (plugin, &process) == CLAP_PROCESS_ERROR;

        for (uint32_t ch = 0; ch < bufferChannels; ++ch)
            if (failed || ch >= numOutputChannels)
                buffer.clear ((int)ch, start, numSamples);

        steadyTime += numSamples;
    }

    void fillInputEvents (const juce::MidiBuffer& midi, int start, int numSamples) noexcept
    {
        inputEvents.clear();
        drainParameterQueue();

        for (const auto metadata : midi)
        {
            if (metadata.samplePosition < start || metadata.samplePosition >= start + numSamples)
                continue;

            const auto msg = metadata.getMessage();
            const auto time = (uint32_t)(metadata.samplePosition - start);
            const auto channel = (int16_t)(msg.getChannel() - 1);

            // Per-note fine pitch, pan and volume become CLAP note expressions: for the notes
            // already playing on the channel now, and for the next note when it starts.
            if (clapNoteDialect && noteExpressions.handle (msg))
                addNoteExpressions (time, &msg, channel, -1);
            else if (msg.isNoteOn())
            {
                const auto key = (int16_t)msg.getNoteNumber();
                inputEvents.addNote (CLAP_EVENT_NOTE_ON, time, channel, key, msg.getFloatVelocity());
                if (clapNoteDialect && dsp::noteExpression::isPerNoteChannel (msg.getChannel()))
                    addNoteExpressions (time, nullptr, channel, key);
            }
            else if (msg.isNoteOff())
                inputEvents.addNote (CLAP_EVENT_NOTE_OFF, time, channel, (int16_t)msg.getNoteNumber(),
                                     msg.getFloatVelocity());
            else if (msg.getRawDataSize() <= 3)
                inputEvents.addMidi (time, msg.getRawData(), msg.getRawDataSize());
        }
    }

    /** Sends the channel's per-note values: only the one `changed` sets, or all three without it. */
    void addNoteExpressions (uint32_t time, const juce::MidiMessage* changed, int16_t channel, int16_t key) noexcept
    {
        namespace ne = dsp::noteExpression;
        const auto& v = noteExpressions.get (channel + 1);

        if (changed == nullptr || changed->isPitchWheel())
            inputEvents.addExpression (time, CLAP_NOTE_EXPRESSION_TUNING, channel, key, v.cents / 100.0);
        if (changed == nullptr || changed->isControllerOfType (ne::panController))
            inputEvents.addExpression (time, CLAP_NOTE_EXPRESSION_PAN, channel, key, (v.pan + 1.0) / 2.0);
        if (changed == nullptr || changed->isControllerOfType (ne::volumeController))
            inputEvents.addExpression (time, CLAP_NOTE_EXPRESSION_VOLUME, channel, key, v.volume);
        // From MPE controllers: only once one has been sent, so a note's sound is not changed otherwise.
        if ((changed == nullptr && v.hasPressure) || (changed != nullptr && changed->isChannelPressure()))
            inputEvents.addExpression (time, CLAP_NOTE_EXPRESSION_PRESSURE, channel, key, v.pressure);
        if ((changed == nullptr && v.hasTimbre) ||
            (changed != nullptr && changed->isControllerOfType (ne::timbreController)))
            inputEvents.addExpression (time, CLAP_NOTE_EXPRESSION_BRIGHTNESS, channel, key, v.timbre);
    }

    bool fillTransport (clap_event_transport_t& t) const noexcept
    {
        auto* currentPlayHead = getPlayHead();

        if (currentPlayHead == nullptr)
            return false;

        const auto pos = currentPlayHead->getPosition();

        if (! pos.hasValue())
            return false;

        t.header = { sizeof (clap_event_transport_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };

        if (pos->getIsPlaying())
            t.flags |= CLAP_TRANSPORT_IS_PLAYING;

        if (pos->getIsRecording())
            t.flags |= CLAP_TRANSPORT_IS_RECORDING;

        if (auto bpm = pos->getBpm())
        {
            t.flags |= CLAP_TRANSPORT_HAS_TEMPO;
            t.tempo = *bpm;
        }

        if (auto ppq = pos->getPpqPosition())
        {
            t.flags |= CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
            t.song_pos_beats = (clap_beattime)std::llround (*ppq * (double)CLAP_BEATTIME_FACTOR);

            if (auto barStart = pos->getPpqPositionOfLastBarStart())
                t.bar_start = (clap_beattime)std::llround (*barStart * (double)CLAP_BEATTIME_FACTOR);

            if (auto bar = pos->getBarCount())
                t.bar_number = (int32_t)*bar;
        }

        if (auto seconds = pos->getTimeInSeconds())
        {
            t.flags |= CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
            t.song_pos_seconds = (clap_sectime)std::llround (*seconds * (double)CLAP_SECTIME_FACTOR);
        }

        if (auto sig = pos->getTimeSignature())
        {
            t.flags |= CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
            t.tsig_num = (uint16_t)sig->numerator;
            t.tsig_denom = (uint16_t)sig->denominator;
        }

        return true;
    }

    void handleOutputEvent (const clap_event_header_t& e)
    {
        if (e.space_id != CLAP_CORE_EVENT_SPACE_ID || e.type != CLAP_EVENT_PARAM_VALUE)
            return;

        const auto& pv = reinterpret_cast<const clap_event_param_value_t&> (e);

        if (auto it = parametersById.find (pv.param_id); it != parametersById.end())
            it->second->updateFromPlugin (pv.value);
    }

    /** Runs a callback on the message thread, unless this instance has been deleted by then. */
    void onMainThread (std::function<void (ClapPluginInstance&)> fn)
    {
        juce::MessageManager::callAsync (
            [this, token = alive, fn = std::move (fn)]
            {
                if (*token)
                    fn (*this);
            });
    }

    //==============================================================================
    const void* getHostExtension (const char* id)
    {
        if (std::strcmp (id, CLAP_EXT_THREAD_CHECK) == 0)
            return &threadCheckExt;

        if (std::strcmp (id, CLAP_EXT_LOG) == 0)
            return &logExt;

        if (std::strcmp (id, CLAP_EXT_PARAMS) == 0)
            return &hostParamsExt;

        if (std::strcmp (id, CLAP_EXT_STATE) == 0)
            return &hostStateExt;

        if (std::strcmp (id, CLAP_EXT_LATENCY) == 0)
            return &hostLatencyExt;

        if (std::strcmp (id, CLAP_EXT_GUI) == 0)
            return &hostGuiExt;

        if (std::strcmp (id, CLAP_EXT_TIMER_SUPPORT) == 0)
            return &hostTimerExt;

#if ! JUCE_WINDOWS
        if (std::strcmp (id, CLAP_EXT_POSIX_FD_SUPPORT) == 0)
            return &hostFdExt;
#endif

        if (std::strcmp (id, CLAP_EXT_PRESET_LOAD) == 0 || std::strcmp (id, CLAP_EXT_PRESET_LOAD_COMPAT) == 0)
            return &hostPresetLoadExt;

        return nullptr;
    }

    bool isAudioThread() const noexcept
    {
        return stoppingOnThisThread ||
               ((processing || startingOnThisThread) && std::this_thread::get_id() == audioThreadId.load());
    }

    clap_host_thread_check_t threadCheckExt { [] (const clap_host_t*)
                                              { return juce::MessageManager::existsAndIsCurrentThread(); },
                                              [] (const clap_host_t* h) { return self (h)->isAudioThread(); } };

    clap_host_log_t logExt { [] (const clap_host_t*, clap_log_severity severity, const char* msg)
                             {
                                 juce::ignoreUnused (severity, msg);
                                 DBG ("CLAP [" << (int)severity << "] " << msg);
                             } };

    clap_host_params_t hostParamsExt {
        [] (const clap_host_t* h, clap_param_rescan_flags flags)
        {
            if ((flags & CLAP_PARAM_RESCAN_VALUES) != 0)
                self (h)->refreshParameterValues();
            if ((flags & (uint32_t)(CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_TEXT)) != 0)
                self (h)->onMainThread ([] (ClapPluginInstance& p) { p.refreshParameterInfo(); });
        },
        [] (const clap_host_t*, clap_id, clap_param_clear_flags) {},
        [] (const clap_host_t* h)
        {
            self (h)->onMainThread (
                [] (ClapPluginInstance& p)
                {
                    if (! p.active && p.paramsExt != nullptr)
                        p.flushParametersWhileInactive();
                });
        }
    };

    juce::String presetError; // set by the plugin while a preset loads

    clap_host_preset_load_t hostPresetLoadExt {
        [] (const clap_host_t* h, uint32_t, const char*, const char*, int32_t, const char* message)
        { self (h)->presetError = juce::String::fromUTF8 (message != nullptr ? message : "unknown error"); },
        [] (const clap_host_t*, uint32_t, const char*, const char*) {}
    };

    clap_host_state_t hostStateExt { [] (const clap_host_t* h) { self (h)->updateHostDisplay(); } };

    clap_host_latency_t hostLatencyExt {
        [] (const clap_host_t* h) { self (h)->onMainThread ([] (ClapPluginInstance& p) { p.refreshLatency(); }); }
    };

    //==============================================================================
    // Plugin windows (clap.gui) and the main-thread services most Linux GUIs need: timers and file
    // descriptor callbacks (e.g. for X11 connections), both run on the message thread.

    struct PluginTimer final : juce::Timer
    {
        PluginTimer (ClapPluginInstance& p, clap_id i) : owner (p), id (i) {}
        void timerCallback() override
        {
            if (owner.timerExt != nullptr)
                owner.timerExt->on_timer (owner.plugin, id);
        }
        ClapPluginInstance& owner;
        clap_id id;
    };

    struct FdPoller final : juce::Timer
    {
        explicit FdPoller (ClapPluginInstance& p) : owner (p) { startTimer (10); }
        void timerCallback() override
        {
#if ! JUCE_WINDOWS // file descriptors are a Linux (POSIX) service; not offered on Windows
            if (owner.fdExt == nullptr || owner.fds.empty())
                return;

            std::vector<pollfd> polls;
            polls.reserve (owner.fds.size());
            for (auto& [fd, flags] : owner.fds)
            {
                unsigned events = 0;
                if ((flags & (uint32_t)CLAP_POSIX_FD_READ) != 0)
                    events |= (unsigned)POLLIN;
                if ((flags & (uint32_t)CLAP_POSIX_FD_WRITE) != 0)
                    events |= (unsigned)POLLOUT;
                polls.push_back ({ fd, (short)events, 0 });
            }

            if (::poll (polls.data(), (nfds_t)polls.size(), 0) <= 0)
                return;

            for (auto& p : polls)
            {
                const auto revents = (unsigned)p.revents;
                clap_posix_fd_flags_t ready = 0;
                if ((revents & (unsigned)POLLIN) != 0)
                    ready |= (uint32_t)CLAP_POSIX_FD_READ;
                if ((revents & (unsigned)POLLOUT) != 0)
                    ready |= (uint32_t)CLAP_POSIX_FD_WRITE;
                if ((revents & (unsigned)(POLLERR | POLLHUP)) != 0)
                    ready |= (uint32_t)CLAP_POSIX_FD_ERROR;
                if (ready != 0)
                    owner.fdExt->on_fd (owner.plugin, p.fd, ready);
            }
#endif
        }
        ClapPluginInstance& owner;
    };

    clap_host_gui_t hostGuiExt {
        [] (const clap_host_t*) {},
        [] (const clap_host_t* h, uint32_t width, uint32_t height)
        {
            self (h)->onMainThread (
                [width, height] (ClapPluginInstance& p)
                {
                    if (auto* editor = p.getActiveEditor())
                        editor->setSize ((int)width, (int)height);
                });
            return true;
        },
        [] (const clap_host_t*) { return false; },
        [] (const clap_host_t*) { return false; },
        [] (const clap_host_t*, bool) {},
    };

    clap_host_timer_support_t hostTimerExt {
        [] (const clap_host_t* h, uint32_t periodMs, clap_id* timerId)
        {
            auto& p = *self (h);
            const auto id = p.nextTimerId++;
            auto timer = std::make_unique<PluginTimer> (p, id);
            timer->startTimer ((int)juce::jmax (1u, periodMs));
            p.timers[id] = std::move (timer);
            *timerId = id;
            return true;
        },
        [] (const clap_host_t* h, clap_id timerId) { return self (h)->timers.erase (timerId) > 0; },
    };

    clap_host_posix_fd_support_t hostFdExt {
        [] (const clap_host_t* h, int fd, clap_posix_fd_flags_t flags)
        {
            auto& p = *self (h);
            p.fds[fd] = flags;
            if (p.fdPoller == nullptr)
                p.fdPoller = std::make_unique<FdPoller> (p);
            return true;
        },
        [] (const clap_host_t* h, int fd, clap_posix_fd_flags_t flags)
        {
            auto& p = *self (h);
            if (! p.fds.contains (fd))
                return false;
            p.fds[fd] = flags;
            return true;
        },
        [] (const clap_host_t* h, int fd) { return self (h)->fds.erase (fd) > 0; },
    };

    //==============================================================================
    struct ParamChange
    {
        clap_id id;
        void* cookie;
        double value;
    };

    std::shared_ptr<ClapModule> module;
    const clap_plugin_descriptor_t& descriptor;
    clap_host_t host {};
    const clap_plugin_t* plugin = nullptr;

    const clap_plugin_params_t* paramsExt = nullptr;
    const clap_plugin_state_t* stateExt = nullptr;
    const clap_plugin_latency_t* latencyExt = nullptr;
    const clap_plugin_gui_t* guiExt = nullptr;
    const clap_plugin_timer_support_t* timerExt = nullptr;
    const clap_plugin_posix_fd_support_t* fdExt = nullptr;
    std::map<clap_id, std::unique_ptr<PluginTimer>> timers;
    clap_id nextTimerId = 1;
    std::map<int, clap_posix_fd_flags_t> fds;
    std::unique_ptr<FdPoller> fdPoller;
    bool hasNotePorts = false;
    bool clapNoteDialect = false;                 // takes CLAP note events (and note expressions)
    dsp::noteExpression::Tracker noteExpressions; // per-note values from Spinus
    uint32_t numInputChannels = 0, numOutputChannels = 0, sidechainChannels = 0;
    std::vector<uint32_t> inputPortChannels, outputPortChannels; // channels per port; port 0 is the main one
    BusesLayout supportedLayout;

    std::map<clap_id, ClapParameter*> parametersById;
    juce::AbstractFifo paramFifo { 1024 };
    std::vector<ParamChange> paramQueue;

    InputEventList inputEvents;
    clap_output_events_t outputEvents {};
    juce::AudioBuffer<float> inputScratch, outputScratch;
    int maxBlockSize = 512;
    std::vector<float*> inputPointers, outputPointers;
    juce::AudioBuffer<float> extraScratch; // non-main ports: inputs first (kept silent), then outputs
    std::vector<float*> extraPointers;
    uint32_t extraInputChannels = 0;
    std::vector<clap_audio_buffer_t> inputBuffers, outputBuffers;

    bool active = false;
    std::atomic<bool> processing { false };
    std::atomic<bool> stoppingOnThisThread { false };
    std::atomic<bool> startingOnThisThread { false };
    std::atomic<std::thread::id> audioThreadId {};
    int64_t steadyTime = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClapPluginInstance)
};

//==============================================================================
void ClapParameter::setValue (float newValue)
{
    const auto plain = toPlain (newValue);
    current = plain;
    owner.queueParameterChange (*this, plain);
}

juce::String ClapParameter::getText (float normalisedValue, int maximumLength) const
{
    return owner.parameterValueToText (info.id, toPlain (normalisedValue)).substring (0, maximumLength);
}

float ClapParameter::getValueForText (const juce::String& text) const
{
    if (auto plain = owner.parameterTextToValue (info.id, text))
        return toNormalised (*plain);

    return toNormalised (text.getDoubleValue());
}

//==============================================================================
#if JUCE_WINDOWS
/** Where a plugin's window goes (Windows): a child window of Spinus's, made once the editor is on the
    desktop and kept in place by JUCE's HWNDComponent. */
class EmbeddedWindow final : public juce::HWNDComponent
{
public:
    ~EmbeddedWindow() override { removeClient(); }

    bool describe (clap_window_t& window)
    {
        if (child == nullptr)
        {
            auto* peer = getPeer();
            if (peer == nullptr)
                return false;
            child = CreateWindowExW (0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0,
                                     juce::jmax (1, getWidth()), juce::jmax (1, getHeight()),
                                     static_cast<HWND> (peer->getNativeHandle()), nullptr, GetModuleHandleW (nullptr),
                                     nullptr);
            if (child == nullptr)
                return false;
            setHWND (child);
        }
        window.api = CLAP_WINDOW_API_WIN32;
        window.win32 = child;
        return true;
    }

    void removeClient()
    {
        if (child == nullptr)
            return;
        setHWND (nullptr);
        DestroyWindow (child);
        child = nullptr;
    }

private:
    HWND child = nullptr;
};
#else
/** Where a plugin's window goes (Linux): an X window inside Spinus's (XEmbed). */
class EmbeddedWindow final : public juce::XEmbedComponent
{
public:
    EmbeddedWindow()
        : juce::XEmbedComponent (juce::XEmbedComponentOptions {}
                                     .withWantsKeyboardFocus (true)
                                     .withAllowForeignWidgetToResizeComponent (false)
                                     .withIgnoreXembedMapped())
    {
    }

    bool describe (clap_window_t& window)
    {
        window.api = CLAP_WINDOW_API_X11;
        window.x11 = (clap_xwnd)getHostWindowID();
        return window.x11 != 0;
    }
};
#endif

/** A CLAP plugin's own window, embedded in a JUCE component, like JUCE's VST3 editors. */
class ClapEditor final : public juce::AudioProcessorEditor
{
public:
    explicit ClapEditor (ClapPluginInstance& p) : juce::AudioProcessorEditor (p), instance (p)
    {
        uint32_t width = 400, height = 300;
        created = gui().create (plugin(), windowApi, false);
        if (created)
            gui().get_size (plugin(), &width, &height);

        setResizable (created && gui().can_resize (plugin()), false);
        addAndMakeVisible (embedded);
        setSize ((int)juce::jmax (40u, width), (int)juce::jmax (40u, height));
    }

    ~ClapEditor() override
    {
        retry.stopTimer();
        detach();
    }

    bool isAttached() const noexcept { return attached; }

    void resized() override
    {
        embedded.setBounds (getLocalBounds());
        if (attached && gui().can_resize (plugin()))
        {
            auto w = (uint32_t)getWidth(), h = (uint32_t)getHeight();
            if (gui().adjust_size (plugin(), &w, &h))
                gui().set_size (plugin(), w, h);
        }
    }

    // JUCE tells this component about hierarchy changes before its XEmbedComponent child, so the plugin's
    // window is taken down while its X parent still exists (otherwise the plugin later touches a destroyed
    // window and Xlib aborts with BadWindow). The watcher also catches the window being shown later.
    void parentHierarchyChanged() override { update(); }
    void visibilityChanged() override { update(); }

private:
    const clap_plugin_gui_t& gui() const { return *instance.guiExt; }
    const clap_plugin_t* plugin() const { return instance.plugin; }

    /** Detaching happens at once (the X parent is about to go). Attaching waits for the retry timer:
        the XEmbedComponent's X window may not have reached the X server yet when this component joins
        a window that is already showing (e.g. swapped in), and the plugin, on its own X connection,
        would then make its window a child of an unknown one (BadWindow). */
    void update (bool fromTimer = false)
    {
        if (! isInVisibleWindow())
            detach();
        else if (fromTimer)
            attach();

        // X11 maps windows asynchronously and JUCE does not report when that finishes: while in a window
        // but not attached yet, keep checking.
        if (! attached && getPeer() != nullptr)
            retry.startTimer (50);
        else
            retry.stopTimer();
    }

    /** Visible inside a window. Not isShowing(): JUCE reports a window that X has not mapped yet as
        minimised, and embedding only needs the window to exist. */
    bool isInVisibleWindow() const
    {
        if (getPeer() == nullptr)
            return false;
        for (const juce::Component* c = this; c != nullptr; c = c->getParentComponent())
            if (! c->isVisible())
                return false;
        return true;
    }

    void attach()
    {
        if (attached)
            return;

        if (! created)
            created = gui().create (plugin(), windowApi, false);

        // The plugin's window becomes a child of the X window the XEmbedComponent provides, which only
        // exists once this component is on the desktop.
        clap_window_t window {};
        if (! created || ! embedded.describe (window) || ! gui().set_parent (plugin(), &window))
            return;

        attached = true;
        gui().show (plugin());
    }

    void detach()
    {
#if JUCE_WINDOWS
        // The plugin takes its window down first; destroying the parent would destroy its window with it.
        if (attached)
            gui().hide (plugin());
        if (created)
            gui().destroy (plugin());
        embedded.removeClient();
#else
        // Like JUCE's VST3 host: release the embedded client window first, then let the plugin remove it.
        if (attached)
            embedded.removeClient();
        if (attached)
            gui().hide (plugin());
        if (created)
            gui().destroy (plugin());
#endif
        attached = created = false;
    }

    struct ScreenWatcher final : juce::ComponentMovementWatcher
    {
        explicit ScreenWatcher (ClapEditor& e) : juce::ComponentMovementWatcher (&e), editor (e) {}
        using juce::ComponentMovementWatcher::componentMovedOrResized;
        using juce::ComponentMovementWatcher::componentVisibilityChanged;
        void componentMovedOrResized (bool, bool) override {}
        void componentPeerChanged() override { editor.update(); }
        void componentVisibilityChanged() override { editor.update(); }
        ClapEditor& editor;
    };

    ClapPluginInstance& instance;
    EmbeddedWindow embedded;
    struct Retry final : juce::Timer
    {
        explicit Retry (ClapEditor& e) : editor (e) {}
        void timerCallback() override { editor.update (true); }
        ClapEditor& editor;
    };

    bool created = false, attached = false;
    Retry retry { *this };
    ScreenWatcher watcher { *this };
};

juce::AudioProcessorEditor* ClapPluginInstance::createEditor()
{
    return guiExt != nullptr ? new ClapEditor (*this) : nullptr;
}

} // namespace

//==============================================================================
void ClapPluginFormat::findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results,
                                            const juce::String& fileOrIdentifier)
{
    if (! fileMightContainThisPluginType (fileOrIdentifier))
        return;

    juce::String error;
    auto module = ClapModule::load (juce::File (fileOrIdentifier), error);

    if (module == nullptr)
        return;

    const auto& factory = module->getFactory();

    for (uint32_t i = 0; i < factory.get_plugin_count (&factory); ++i)
    {
        auto* descriptor = factory.get_plugin_descriptor (&factory, i);

        if (descriptor == nullptr)
            continue;

        // Instantiating gives accurate channel counts; scanning runs out of process.
        if (auto instance = ClapPluginInstance::create (module, *descriptor, error))
        {
            auto desc = std::make_unique<juce::PluginDescription>();
            instance->fillInPluginDescription (*desc);
            results.add (desc.release());
        }
    }
}

bool ClapPluginFormat::fileMightContainThisPluginType (const juce::String& fileOrIdentifier)
{
    const juce::File f (fileOrIdentifier);
    return f.hasFileExtension (".clap") && f.existsAsFile();
}

juce::String ClapPluginFormat::getNameOfPluginFromIdentifier (const juce::String& fileOrIdentifier)
{
    return juce::File (fileOrIdentifier).getFileNameWithoutExtension();
}

bool ClapPluginFormat::pluginNeedsRescanning (const juce::PluginDescription& desc)
{
    return juce::File (desc.fileOrIdentifier).getLastModificationTime() != desc.lastFileModTime;
}

bool ClapPluginFormat::doesPluginStillExist (const juce::PluginDescription& desc)
{
    return juce::File (desc.fileOrIdentifier).existsAsFile();
}

juce::StringArray ClapPluginFormat::searchPathsForPlugins (const juce::FileSearchPath& directories, bool recursive,
                                                           bool /*allowAsync*/)
{
    juce::StringArray results;

    for (int i = 0; i < directories.getNumPaths(); ++i)
        for (const auto& entry :
             juce::RangedDirectoryIterator (directories[i], recursive, "*.clap", juce::File::findFiles))
            results.add (entry.getFile().getFullPathName());

    return results;
}

juce::FileSearchPath ClapPluginFormat::getDefaultLocationsToSearch()
{
    // CLAP spec (entry.h): ~/.clap and /usr/lib/clap on Linux, %COMMONPROGRAMFILES%\CLAP and
    // %LOCALAPPDATA%\Programs\Common\CLAP on Windows, plus anything in $CLAP_PATH (";" between folders on
    // Windows, ":" elsewhere).
    juce::FileSearchPath paths;
#if JUCE_WINDOWS
    paths.add (
        juce::File (juce::SystemStats::getEnvironmentVariable ("COMMONPROGRAMFILES", "C:\\Program Files\\Common Files"))
            .getChildFile ("CLAP"));
    paths.add (juce::File::getSpecialLocation (juce::File::windowsLocalAppData).getChildFile ("Programs/Common/CLAP"));
    const auto separator = ";";
#else
    paths.add (juce::File ("~/.clap"));
    paths.add (juce::File ("/usr/lib/clap"));
    paths.add (juce::File ("/usr/local/lib/clap"));
    const auto separator = ":";
#endif

    for (auto& p :
         juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("CLAP_PATH", {}), separator, {}))
        if (p.isNotEmpty())
            paths.add (juce::File (p));

    paths.removeNonExistentPaths();
    return paths;
}

bool ClapPluginFormat::isEditorAttached (const juce::AudioProcessorEditor* editor)
{
    auto* clap = dynamic_cast<const ClapEditor*> (editor);
    return clap != nullptr && clap->isAttached();
}

bool ClapPluginFormat::canLoadPresets (const juce::AudioPluginInstance& instance)
{
    auto* clap = dynamic_cast<const ClapPluginInstance*> (&instance);
    return clap != nullptr && clap->getPresetLoad() != nullptr;
}

juce::StringArray ClapPluginFormat::getLoadedFiles() { return ClapModule::getLoadedFiles(); }

juce::Result ClapPluginFormat::loadPreset (juce::AudioPluginInstance& instance, const juce::File& file)
{
    auto* clap = dynamic_cast<ClapPluginInstance*> (&instance);
    if (clap == nullptr)
        return juce::Result::fail ("Not a CLAP plugin.");
    return clap->loadPresetFile (file);
}

std::vector<ClapPluginFormat::DiscoveredPreset> ClapPluginFormat::discoverPresets (juce::AudioPluginInstance& instance,
                                                                                   int most)
{
    auto* clap = dynamic_cast<ClapPluginInstance*> (&instance);
    return clap != nullptr && most > 0 ? clap->discoverPresets (most) : std::vector<DiscoveredPreset> {};
}

juce::Result ClapPluginFormat::loadPreset (juce::AudioPluginInstance& instance, const DiscoveredPreset& preset)
{
    auto* clap = dynamic_cast<ClapPluginInstance*> (&instance);
    if (clap == nullptr)
        return juce::Result::fail ("Not a CLAP plugin.");
    return clap->loadPresetFrom (preset.kind == DiscoveredPreset::inPlugin ? CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN
                                                                           : CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                 preset.location, preset.loadKey);
}

void ClapPluginFormat::createPluginInstance (const juce::PluginDescription& desc, double initialSampleRate,
                                             int initialBufferSize, PluginCreationCallback callback)
{
    juce::String error;
    auto module = ClapModule::load (juce::File (desc.fileOrIdentifier), error);

    if (module == nullptr)
    {
        callback (nullptr, error);
        return;
    }

    auto* descriptor = module->findDescriptor (desc.uniqueId);

    if (descriptor == nullptr)
    {
        callback (nullptr, "CLAP plugin not found in " + desc.fileOrIdentifier + ": " + desc.name);
        return;
    }

    auto instance = ClapPluginInstance::create (module, *descriptor, error);

    if (instance == nullptr)
    {
        callback (nullptr, error);
        return;
    }

    instance->setRateAndBufferSizeDetails (initialSampleRate, initialBufferSize);
    callback (std::move (instance), {});
}

} // namespace spinus::plugins
