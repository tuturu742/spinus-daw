#include "HiddenInstrument.h"

#include "engine/PluginCrashGuard.h"
#include "plugins/SandboxList.h"

namespace spinus
{

namespace te = tracktion;

namespace
{
model::Device stockDevice (const juce::String& stockType)
{
    model::Device device;
    device.kind = model::Device::Kind::stock;
    device.stockType = stockType;
    return device;
}
} // namespace

HiddenInstrument::HiddenInstrument (Session& session, const juce::String& stockType)
    : HiddenInstrument (session, stockDevice (stockType))
{
}

HiddenInstrument::HiddenInstrument (Session& session, const model::Device& device)
{
    edit = te::Edit::createSingleTrackEdit (session.getEdit().engine);
    // A plugin's libraries are noted, as for the song's own devices, so a crash in them names it.
    const engine::PluginCrashGuard::ModuleWatch watch;
    plugin = session.getDeviceFactory().create (*edit, device);
    if (plugin != nullptr && device.kind == model::Device::Kind::plugin)
        watch.addNewModules (plugins::SandboxList::keyOf (device.plugin.format, device.plugin.fileOrIdentifier),
                             device.plugin.name, device.plugin.fileOrIdentifier);
    if (auto* external = dynamic_cast<te::ExternalPlugin*> (plugin.get()))
        instance = external->getAudioPluginInstance();
    if (instance == nullptr)
    {
        problem = "The instrument (" +
                  (device.kind == model::Device::Kind::stock ? device.stockType : device.plugin.name) +
                  ") is not installed.";
        return;
    }
    instance->setPlayConfigDetails (0, 2, sampleRate, blockSize);
    instance->prepareToPlay (sampleRate, blockSize);
}

HiddenInstrument::~HiddenInstrument()
{
    if (instance != nullptr)
        instance->releaseResources();
    plugin = nullptr;
    edit = nullptr;
}

juce::File HiddenInstrument::getPluginFile() const
{
    return instance != nullptr ? juce::File (instance->getPluginDescription().fileOrIdentifier) : juce::File();
}

void HiddenInstrument::play (int samples, const juce::MidiBuffer& midi, juce::AudioBuffer<float>* into)
{
    juce::AudioBuffer<float> block (juce::jmax (2, instance->getTotalNumOutputChannels()), blockSize);
    for (int done = 0; done < samples; done += blockSize)
    {
        const auto count = juce::jmin (blockSize, samples - done);
        block.setSize (block.getNumChannels(), count, false, false, true);
        block.clear();
        juce::MidiBuffer events;
        events.addEvents (midi, done, count, -done);
        instance->processBlock (block, events);
        if (into != nullptr)
            for (int c = 0; c < 2; ++c)
                into->addFrom (0, done, block, c, 0, count, 0.5f);
    }
}

void HiddenInstrument::silence()
{
    // All sound off cuts what still rings (all notes off only releases the notes, and a long release would
    // carry into the next render); then 0.1 s for settings changed just before to take effect.
    juce::MidiBuffer off;
    off.addEvent (juce::MidiMessage::allNotesOff (1), 0);
    off.addEvent (juce::MidiMessage::allSoundOff (1), 0);
    play ((int)(0.1 * sampleRate), off, nullptr);
}

juce::Result HiddenInstrument::loadPreset (const juce::File& file)
{
    InstrumentPreset preset;
    preset.kind = InstrumentPreset::Kind::file;
    preset.name = file.getFileNameWithoutExtension();
    preset.file = file;
    return loadPreset (preset);
}

juce::Result HiddenInstrument::loadPreset (const InstrumentPreset& preset)
{
    if (instance == nullptr)
        return juce::Result::fail (problem);
    // Surge XT loads a preset on a thread of its own once it has played: it fades out, stays silent while the
    // thread loads, then plays the new patch. Its state must not be read meanwhile (saving it while the thread
    // clears the patch crashes Surge), so the end of the load is heard instead: first long enough without notes
    // for the fade (20 of Surge's 32-sample blocks) to finish and the loading to start...
    if (auto loaded = InstrumentPresets::load (*instance, preset); loaded.failed())
        return loaded;
    play (4 * blockSize, {}, nullptr);
    // ...then a note until it sounds. A preset that makes no sound from a note at all is waited for at most a
    // few seconds (a file may still be loading; a state, a program or a discovered preset is in place at once,
    // or nearly, so a second is plenty).
    const auto longest = preset.kind == InstrumentPreset::Kind::file ? 6000.0 : 1000.0;
    juce::MidiBuffer probe;
    probe.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8)100), 0);
    juce::AudioBuffer<float> heard (1, blockSize);
    const auto started = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - started < longest)
    {
        heard.clear();
        play (blockSize, probe, &heard);
        if (heard.getMagnitude (0, 0, blockSize) > 1.0e-4f)
            break;
        juce::Thread::sleep (1);
    }
    silence();
    return juce::Result::ok();
}

juce::AudioBuffer<float> HiddenInstrument::render (const std::vector<Note>& notes, double tail)
{
    if (instance == nullptr)
        return {};
    silence();

    juce::MidiBuffer midi;
    double end = 0.0;
    for (auto& n : notes)
    {
        const auto velocity = (juce::uint8)juce::jlimit (1, 127, juce::roundToInt (n.velocity * 127.0f));
        midi.addEvent (juce::MidiMessage::noteOn (1, n.note, velocity), (int)(n.start * sampleRate));
        midi.addEvent (juce::MidiMessage::noteOff (1, n.note), (int)((n.start + n.length) * sampleRate));
        end = juce::jmax (end, n.start + n.length);
    }
    const auto samples = (int)((end + tail) * sampleRate);
    juce::AudioBuffer<float> mono (1, juce::jmax (1, samples));
    mono.clear();
    play (samples, midi, &mono);
    return mono;
}

} // namespace spinus
