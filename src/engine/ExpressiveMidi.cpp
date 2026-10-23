#include "ExpressiveMidi.h"

#include "dsp/NoteExpression.h"

namespace spinus::engine::expressiveMidi
{

namespace te = tracktion;
namespace ne = dsp::noteExpression;

namespace
{
const juce::Identifier& centsId()
{
    static const juce::Identifier id ("spinusCents");
    return id;
}

const juce::Identifier& panId()
{
    static const juce::Identifier id ("spinusPan");
    return id;
}

const juce::Identifier& volumeId()
{
    static const juce::Identifier id ("spinusVolume");
    return id;
}

ne::Values valuesOf (const te::MidiNote& note)
{
    return { (float)note.state.getProperty (centsId(), 0.0f), (float)note.state.getProperty (panId(), 0.0f),
             (float)note.state.getProperty (volumeId(), 1.0f) };
}

double edgeTime (const te::MidiNote& note, te::MidiClip& clip, te::MidiList::TimeBase timeBase, bool start)
{
    // Spinus does not use groove templates.
    const auto edge = start ? te::MidiNote::startEdge : te::MidiNote::endEdge;
    switch (timeBase)
    {
    case te::MidiList::TimeBase::beatsRaw:
        return (start ? note.getStartBeat() : note.getEndBeat()).inBeats();
    case te::MidiList::TimeBase::beats:
        return note.getPlaybackBeats (edge, clip, nullptr).inBeats();
    case te::MidiList::TimeBase::seconds:
        break;
    }
    return note.getPlaybackTime (edge, clip, nullptr).inSeconds();
}
} // namespace

bool canPlay (const model::Device& instrument)
{
    if (instrument.kind == model::Device::Kind::stock)
        return instrument.stockType.startsWith ("spinus.") || instrument.stockType.startsWith ("bundled.");
    return instrument.plugin.format == "CLAP";
}

bool setNoteExpressions (te::MidiNote& note, const model::Note& n) { return setNoteExpressions (note.state, n); }

bool setNoteExpressions (juce::ValueTree& state, const model::Note& n)
{
    const ne::Values values { n.finePitchCents, n.pan, n.getVolume() };
    if (values.isDefault())
        return false;

    state.setProperty (centsId(), values.cents, nullptr);
    state.setProperty (panId(), values.pan, nullptr);
    state.setProperty (volumeId(), values.volume, nullptr);
    return true;
}

bool hasNoteExpressions (const te::MidiList& list)
{
    for (auto* note : list.getNotes())
        if (note->state.hasProperty (centsId()))
            return true;
    return false;
}

juce::MidiMessageSequence createSequence (const te::MidiList& list, te::MidiClip& clip, te::MidiList::TimeBase timeBase)
{
    // Pattern clips hold notes only (no controller or SysEx events). Each note gets a member channel
    // of its own while it sounds; when all are busy, the one that frees up first is shared.
    constexpr int numChannels = ne::lastChannel - ne::firstChannel + 1;
    std::array<double, numChannels> busyUntil;
    busyUntil.fill (std::numeric_limits<double>::lowest());
    int last = numChannels - 1;

    juce::MidiMessageSequence sequence;
    for (auto* note : list.getNotes())
    {
        if (note->isMute() || note->getLengthBeats() <= te::BeatDuration::fromBeats (0.00001))
            continue;

        const auto down = std::max (0.0, edgeTime (*note, clip, timeBase, true));
        const auto up = edgeTime (*note, clip, timeBase, false);
        if (up <= down)
            continue;

        int slot = -1;
        for (int i = 1; i <= numChannels && slot < 0; ++i)
            if (const auto candidate = (last + i) % numChannels; busyUntil[(size_t)candidate] <= down)
                slot = candidate;
        if (slot < 0)
            slot = (int)(std::min_element (busyUntil.begin(), busyUntil.end()) - busyUntil.begin());
        last = slot;
        busyUntil[(size_t)slot] = std::max (busyUntil[(size_t)slot], up);

        const auto channel = ne::firstChannel + slot;
        for (auto& m : ne::encode (channel, valuesOf (*note)))
            sequence.addEvent (m, down);
        sequence.addEvent (juce::MidiMessage::noteOn (channel, note->getNoteNumber(), (juce::uint8)note->getVelocity()),
                           down);
        sequence.addEvent (
            juce::MidiMessage::noteOff (channel, note->getNoteNumber(), (juce::uint8)note->getNoteOffVelocity()), up);
    }

    return sequence;
}

} // namespace spinus::engine::expressiveMidi
