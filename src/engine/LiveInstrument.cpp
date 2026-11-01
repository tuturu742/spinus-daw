#include "LiveInstrument.h"

namespace spinus::engine
{

LiveInstrument::LiveInstrument (juce::AudioPluginInstance& i) : instance (i) {}

void LiveInstrument::prepare (double rate)
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    instance.prepareToPlay (sampleRate, maxBlockSize);
    // The main output's channels (the first ones); a mono one plays on both sides.
    right = instance.getBusCount (false) > 0 && instance.getChannelCountOfBus (false, 0) == 1 ? 0 : 1;
    const auto channels = juce::jmax (2, instance.getTotalNumOutputChannels(), instance.getTotalNumInputChannels());
    block.setSize (channels, maxBlockSize);
    midi.ensureSize ((size_t)maxEvents * 16);
}

bool LiveInstrument::play (const std::vector<Note>& notes)
{
    std::vector<Event> events { Event { 0, -1, 0 } }; // what still plays is cut first
    events.reserve (1 + 2 * notes.size());
    for (const auto& n : notes)
    {
        const auto velocity = (juce::uint8)juce::jlimit (1, 127, juce::roundToInt (n.velocity * 127.0f));
        const auto on = (juce::int64)(juce::jmax (0.0, n.start) * sampleRate);
        events.push_back ({ on, juce::jlimit (0, 127, n.note), velocity });
        events.push_back ({ juce::jmax (on + 1, (juce::int64)((n.start + n.length) * sampleRate)),
                            juce::jlimit (0, 127, n.note), 0 });
    }
    return push (events);
}

void LiveInstrument::stopNotes() { push ({ Event { 0, -1, 0 } }); }

bool LiveInstrument::push (const std::vector<Event>& events)
{
    // In one write: the audio thread takes all of them in the same block, or none.
    if (fifo.getFreeSpace() < (int)events.size())
        return false;
    const auto scope = fifo.write ((int)events.size());
    size_t n = 0;
    scope.forEach ([&] (int index) { queued[(size_t)index] = events[n++]; });
    return true;
}

void LiveInstrument::mix (float* const* outputs, int numChannels, int numSamples) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    // Notes just played: their times from now on.
    const auto scope = fifo.read (fifo.getNumReady());
    scope.forEach (
        [&] (int index)
        {
            const auto e = queued[(size_t)index];
            if (e.note < 0)
            {
                waiting[0] = { now, -1, 0 };
                numWaiting = 1;
            }
            else if (numWaiting < maxEvents)
                waiting[(size_t)numWaiting++] = { now + e.at, e.note, e.velocity };
        });

    for (int done = 0; done < numSamples;)
    {
        const auto count = juce::jmin (maxBlockSize, numSamples - done);
        midi.clear();
        int kept = 0;
        for (int i = 0; i < numWaiting; ++i)
        {
            const auto e = waiting[(size_t)i];
            if (e.at < now + count)
            {
                const auto offset = (int)juce::jmax ((juce::int64)0, e.at - now);
                if (e.note < 0)
                    midi.addEvent (juce::MidiMessage::allNotesOff (1), offset);
                else if (e.velocity > 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, e.note, e.velocity), offset);
                else
                    midi.addEvent (juce::MidiMessage::noteOff (1, e.note), offset);
            }
            else
                waiting[(size_t)kept++] = e;
        }
        numWaiting = kept;

        block.setSize (block.getNumChannels(), count, false, false, true);
        block.clear();
        if (! instance.isSuspended())
            instance.processBlock (block, midi);

        float loudest = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            if (outputs[ch] != nullptr)
            {
                const auto source = ch == 0 ? 0 : right;
                juce::FloatVectorOperations::add (outputs[ch] + done, block.getReadPointer (source), count);
                loudest = juce::jmax (loudest, block.getMagnitude (source, 0, count));
            }
        for (auto p = peak.load(); loudest > p && ! peak.compare_exchange_weak (p, loudest);)
        {
        }
        now += count;
        done += count;
    }
}

} // namespace spinus::engine
