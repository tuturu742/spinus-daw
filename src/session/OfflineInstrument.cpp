#include "OfflineInstrument.h"

namespace spinus
{

OfflineInstrument::OfflineInstrument (juce::AudioPluginInstance& i) : instance (i)
{
    instance.setPlayConfigDetails (0, 2, sampleRate, blockSize);
    instance.prepareToPlay (sampleRate, blockSize);
}

OfflineInstrument::~OfflineInstrument() { instance.releaseResources(); }

void OfflineInstrument::play (int samples, const juce::MidiBuffer& midi, juce::AudioBuffer<float>* into)
{
    const auto channels = juce::jmax (2, instance.getTotalNumOutputChannels(), instance.getTotalNumInputChannels());
    juce::AudioBuffer<float> block (channels, blockSize);
    for (int done = 0; done < samples; done += blockSize)
    {
        const auto count = juce::jmin (blockSize, samples - done);
        block.setSize (block.getNumChannels(), count, false, false, true);
        block.clear();
        juce::MidiBuffer events;
        events.addEvents (midi, done, count, -done);
        instance.processBlock (block, events);
        if (into != nullptr && into->getNumChannels() >= 2)
            for (int c = 0; c < 2; ++c)
                into->addFrom (c, done, block, c, 0, count);
        else if (into != nullptr)
            for (int c = 0; c < 2; ++c)
                into->addFrom (0, done, block, c, 0, count, 0.5f);
    }
}

void OfflineInstrument::silence()
{
    // All sound off cuts what still rings (all notes off only releases the notes, and a long release would
    // carry into the next render); then 0.1 s for settings changed just before to take effect.
    juce::MidiBuffer off;
    off.addEvent (juce::MidiMessage::allNotesOff (1), 0);
    off.addEvent (juce::MidiMessage::allSoundOff (1), 0);
    play ((int)(0.1 * sampleRate), off, nullptr);
}

juce::Result OfflineInstrument::loadPreset (const InstrumentPreset& preset)
{
    // Surge XT loads a preset on a thread of its own once it has played: it fades out, stays silent while the
    // thread loads, then plays the new patch. Its state must not be read meanwhile (saving it while the thread
    // clears the patch crashes Surge), so the end of the load is heard instead: first long enough without notes
    // for the fade (20 of Surge's 32-sample blocks) to finish and the loading to start...
    if (auto loaded = InstrumentPresets::load (instance, preset); loaded.failed())
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
    juce::MidiBuffer release;
    release.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
    play (blockSize, release, nullptr);
    silence();
    return juce::Result::ok();
}

juce::AudioBuffer<float> OfflineInstrument::render (const std::vector<Note>& notes, double tail)
{
    return renderChannels (notes, tail, 1);
}

juce::AudioBuffer<float> OfflineInstrument::renderStereo (const std::vector<Note>& notes, double tail)
{
    return renderChannels (notes, tail, 2);
}

juce::AudioBuffer<float> OfflineInstrument::renderChannels (const std::vector<Note>& notes, double tail, int channels)
{
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
    juce::AudioBuffer<float> sound (channels, juce::jmax (1, samples));
    sound.clear();
    play (samples, midi, &sound);
    return sound;
}

} // namespace spinus
