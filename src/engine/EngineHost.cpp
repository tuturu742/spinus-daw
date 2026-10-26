#include "EngineHost.h"
#include "PipeWireInput.h"

#include <SpinusVersion.h>

#include "AudioGuardPlugin.h"
#include "OutputCapturePlugin.h"
#include "MutePlugin.h"
#include "StemCapturePlugin.h"
#include "AudioSetup.h"
#include "ExpressiveMidi.h"
#include "dsp/SpinusPluginFormat.h"
#include "plugins/clap/ClapPluginFormat.h"
#include "plugins/sandbox/SandboxPluginFormat.h"
#include "session/ProgramFolder.h"

namespace spinus
{

namespace
{
class SpinusEngineBehaviour : public tracktion::EngineBehaviour
{
public:
    explicit SpinusEngineBehaviour (bool shouldOpenDevice) : openDevice (shouldOpenDevice) {}

    bool autoInitialiseDeviceManager() override { return openDevice; }

    /** Tracktion's defaults (400 tracks, 1500 clips per track, 16 plugins per track) are too low
        for Spinus: every channel and every mixer insert is a track, the mixer must
        handle 500+ inserts, and an insert holds any number of effects plus sends and utilities. */
    tracktion::EditLimits getEditLimits() override
    {
        tracktion::EditLimits limits;
        limits.maxNumTracks = 4096;
        limits.maxClipsInTrack = 100000;
        limits.maxPluginsOnTrack = std::numeric_limits<int>::max();   // no limit on effects per insert
        limits.maxNumMasterPlugins = std::numeric_limits<int>::max(); // nor on the master's
        return limits;
    }

    /** Every sound card input in use is one recording input, so that several tracks record at once, each
        picking its channels (multi-track recording; recording::activeInputChannels says which is which).
        Outputs are stereo pairs, as Tracktion's default. */
    bool isDescriptionOfWaveDevicesSupported() override { return true; }
    void describeWaveDevices (std::vector<tracktion::WaveDeviceDescription>& list, juce::AudioIODevice& device,
                              bool isInput) override
    {
        list.clear();
        if (isInput)
        {
            // The sound card hands over only the inputs in use, one after another.
            if (! device.getInputChannelNames().isEmpty())
                list.push_back (tracktion::WaveDeviceDescription::withNumChannels (
                    {}, 0, (uint32_t)juce::jmax (1, device.getActiveInputChannels().countNumberOfSetBits()), true));
            return;
        }
        const auto outputs = device.getOutputChannelNames().size();
        for (int i = 0; i < outputs; i += 2)
            list.push_back (
                tracktion::WaveDeviceDescription::withNumChannels ({}, (uint32_t)i, i + 1 < outputs ? 2u : 1u, true));
    }

    /** Notes with per-note fine pitch, pan or volume go out on channels of their own. */
    juce::MidiMessageSequence createPlaybackMidiSequence (const tracktion::MidiList& list, tracktion::MidiClip& clip,
                                                          tracktion::MidiList::TimeBase timeBase,
                                                          bool generateMPE) override
    {
        if (generateMPE && engine::expressiveMidi::hasNoteExpressions (list))
            return engine::expressiveMidi::createSequence (list, clip, timeBase);
        return EngineBehaviour::createPlaybackMidiSequence (list, clip, timeBase, generateMPE);
    }

private:
    bool openDevice;
};

/** Tracktion hands long tasks (rendering, exporting) to the UI to run with a progress bar; its default
    does nothing. Until Spinus has a progress dialog, run them on the calling thread. */
class SpinusUIBehaviour : public tracktion::UIBehaviour
{
public:
    void runTaskWithProgressBar (tracktion::ThreadPoolJobWithProgress& task) override
    {
        while (task.runJob() == juce::ThreadPoolJob::jobNeedsRunningAgain)
        {
        }
    }

    std::unique_ptr<juce::Component> createPluginWindow (tracktion::PluginWindowState& state) override
    {
        auto* pluginState = dynamic_cast<tracktion::Plugin::WindowState*> (&state);
        if (pluginState != nullptr && windowFactory)
            return windowFactory (pluginState->plugin);
        return {};
    }

    EngineHost::PluginWindowFactory windowFactory;
    PipeWireInput* pipeWireInput = nullptr; // the EngineHost's, while it lives
};
} // namespace

namespace
{
juce::String& settingsName()
{
    static juce::String name = productName;
    return name;
}

/** The metronome's two clicks, made by Spinus: a high one on the first beat of a bar and a lower one on
    the others (the engine's own two differ only in loudness). Short sine blips, written once into the
    settings folder, where the engine's click reads them. */
void useSpinusClicks (tracktion::Engine& engine)
{
    const auto folder = engine.getPropertyStorage().getAppPrefsFolder().getChildFile ("metronome");
    auto make = [&] (const juce::String& name, double frequency, float level) -> juce::File
    {
        const auto file = folder.getChildFile (name);
        if (file.existsAsFile() || ! folder.createDirectory())
            return file;
        constexpr double rate = 48000.0;
        juce::AudioBuffer<float> click (1, (int)(0.04 * rate));
        for (int i = 0; i < click.getNumSamples(); ++i)
        {
            const auto t = i / rate;
            const auto envelope = juce::jmin (1.0, t / 0.001) * std::exp (-t / 0.008);
            click.setSample (0, i,
                             level * (float)(envelope * std::sin (juce::MathConstants<double>::twoPi * frequency * t)));
        }
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        if (auto writer = juce::WavAudioFormat().createWriterFor (
                stream,
                juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (24)))
            writer->writeFromAudioSampleBuffer (click, 0, click.getNumSamples());
        return file;
    };
    const auto bar = make ("bar-v1.wav", 1760.0, 1.0f);   // A6
    const auto beat = make ("beat-v1.wav", 1175.0, 0.8f); // D6
    auto& storage = engine.getPropertyStorage();
    storage.setProperty (tracktion::SettingID::clickTrackSampleBig, bar.getFullPathName());
    storage.setProperty (tracktion::SettingID::clickTrackSampleSmall, beat.getFullPathName());
}

} // namespace

void EngineHost::setSettingsName (const juce::String& name) { settingsName() = name; }

void EngineHost::setPluginWindowFactory (tracktion::Engine& engine, PluginWindowFactory factory)
{
    auto* behaviour = dynamic_cast<SpinusUIBehaviour*> (&engine.getUIBehaviour());
    jassert (behaviour != nullptr);
    if (behaviour != nullptr)
        behaviour->windowFactory = std::move (factory);
}

EngineHost::EngineHost (const Options& options)
    : engine (std::make_unique<tracktion::Engine> (std::make_unique<tracktion::PropertyStorage> (settingsName()),
                                                   std::make_unique<SpinusUIBehaviour>(),
                                                   std::make_unique<SpinusEngineBehaviour> (options.openAudioDevice)))
{
    // JUCE and Tracktion don't host CLAP; Spinus provides its own format.
    engine->getPluginManager().pluginFormatManager.addFormat (std::make_unique<plugins::ClapPluginFormat>());
    // Spinus's own devices (Sampler, ...) are plugins of an internal format.
    engine->getPluginManager().pluginFormatManager.addFormat (std::make_unique<dsp::SpinusPluginFormat>());
    // Plugins in a separate process: the real plugin is one the engine knows.
    const auto host = options.pluginHost != juce::File() ? options.pluginHost : getHelperProgram ("spinus-plugin-host");
    engine->getPluginManager().pluginFormatManager.addFormat (std::make_unique<plugins::sandbox::SandboxPluginFormat> (
        host,
        [e = engine.get()] (const juce::String& format, const juce::String& file,
                            int uniqueId) -> std::optional<juce::PluginDescription>
        {
            for (auto& type : e->getPluginManager().knownPluginList.getTypes())
                if (type.pluginFormatName == format && type.fileOrIdentifier == file && type.uniqueId == uniqueId)
                    return type;
            return std::nullopt;
        }));

    useSpinusClicks (*engine);

    // Follows every device the Engine Adapter creates.
    engine->getPluginManager().createBuiltInType<engine::AudioGuardPlugin>();
    engine->getPluginManager().createBuiltInType<engine::OutputCapturePlugin>();
    engine->getPluginManager().createBuiltInType<engine::MutePlugin>();
    engine->getPluginManager().createBuiltInType<engine::StemCapturePlugin>();

    // First start (no saved audio setup): pick the device as AudioSetup::applyDefaults does instead of JUCE's first
    // choice. Tracktion saves the setup whenever it changes, so later starts reopen the user's choice.
    if (options.openAudioDevice &&
        engine->getPropertyStorage().getXmlProperty (tracktion::SettingID::audio_device_setup) == nullptr)
    {
        firstStartAudioChoice = engine::AudioSetup::applyDefaults (engine->getDeviceManager().deviceManager);
        engine->getDeviceManager().saveSettings();
    }
    // The microphone chosen by name, when recording through PipeWire.
    if (options.openAudioDevice)
    {
        pipeWireInput = std::make_unique<PipeWireInput> (engine->getDeviceManager().deviceManager,
                                                         &engine->getPropertyStorage().getPropertiesFile());
        if (auto* behaviour = dynamic_cast<SpinusUIBehaviour*> (&engine->getUIBehaviour()))
            behaviour->pipeWireInput = pipeWireInput.get();

        // First start through JACK: record from a microphone, not from whatever program plays (JUCE's choice).
        auto& manager = engine->getDeviceManager().deviceManager;
        if (firstStartAudioChoice.isNotEmpty() && manager.getCurrentAudioDeviceType() == "JACK")
            if (auto* type = manager.getCurrentDeviceTypeObject())
            {
                juce::StringArray microphones;
                for (const auto& source : pipeWireInput->listSources())
                {
                    microphones.add (source.description);
                    microphones.add (source.name);
                }
                auto setup = manager.getAudioDeviceSetup();
                const auto input = engine::AudioSetup::pickJackInput (type->getDeviceNames (true), microphones);
                if (input != setup.inputDeviceName)
                {
                    setup.inputDeviceName = input;
                    setup.useDefaultInputChannels = false;
                    setup.inputChannels.clear(); // switched on when something records (recording::InputUse)
                    manager.setAudioDeviceSetup (setup, true);
                    engine->getDeviceManager().saveSettings();
                }
            }
    }
}

EngineHost::~EngineHost()
{
    if (auto* behaviour = dynamic_cast<SpinusUIBehaviour*> (&engine->getUIBehaviour()))
        behaviour->pipeWireInput = nullptr;
    pipeWireInput = nullptr;
}

PipeWireInput* EngineHost::getPipeWireInput (tracktion::Engine& e)
{
    auto* behaviour = dynamic_cast<SpinusUIBehaviour*> (&e.getUIBehaviour());
    return behaviour != nullptr ? behaviour->pipeWireInput : nullptr;
}

juce::String EngineHost::describeAudioDevice() const
{
    auto* device = engine->getDeviceManager().deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return "No audio device open";

    return device->getTypeName() + ": " + device->getName() + " @ " + juce::String (device->getCurrentSampleRate(), 0) +
           " Hz, " + juce::String (device->getCurrentBufferSizeSamples()) + " samples";
}

} // namespace spinus
