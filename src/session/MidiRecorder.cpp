#include "MidiRecorder.h"
#include "engine/Beats.h"

#include "Session.h"
#include "commands/CommandRegistry.h"

namespace spinus
{

namespace te = tracktion;

namespace
{
constexpr model::Ticks stepTicks = model::ticksPerQuarterNote / 4; // a sixteenth
constexpr model::Ticks minimumLength = stepTicks / 4;

juce::var noteToJson (const model::Note& n)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("start", (juce::int64)n.start);
    o->setProperty ("length", (juce::int64)n.length);
    o->setProperty ("pitch", n.pitch);
    o->setProperty ("velocity", n.velocity);
    o->setProperty ("releaseVelocity", n.releaseVelocity);
    o->setProperty ("pan", n.pan);
    o->setProperty ("finePitchCents", n.finePitchCents);
    return o;
}
} // namespace

MidiRecorder::MidiRecorder (Session& s) : session (s)
{
    auto& devices = session.getEdit().engine.getDeviceManager();
    const auto& all = devices.getMidiInDevices();
    if (std::none_of (all.begin(), all.end(),
                      [] (auto& d) { return d != nullptr && d->getName() == computerKeyboardName; })) [[maybe_unused]]
        auto created = devices.createVirtualMidiDevice (computerKeyboardName);
    attachToInputs();
    session.getEdit().engine.getDeviceManager().addChangeListener (this);
    updateRouting();
}

MidiRecorder::~MidiRecorder()
{
    session.getEdit().engine.getDeviceManager().removeChangeListener (this);
    detachFromInputs();
}

void MidiRecorder::attachToInputs()
{
    detachFromInputs();
    const juce::ScopedLock sl (lock);
    for (auto& device : session.getEdit().engine.getDeviceManager().getMidiInDevices())
    {
        // Virtual inputs ("All MIDI Ins") merge the real ones: using both would double every note. The
        // computer keyboard is a virtual input of its own, with no real ones in it.
        if (device == nullptr)
            continue;
        if (dynamic_cast<te::VirtualMidiInputDevice*> (device.get()) != nullptr)
        {
            if (device->getName() != computerKeyboardName)
                continue;
            device->setEnabled (true);
            computerKeyboard = device;
        }
        device->keyboardState.addListener (this);
        device->setMonitorMode (te::InputDevice::MonitorMode::on);
        inputs.push_back (device);
    }
}

void MidiRecorder::detachFromInputs()
{
    const juce::ScopedLock sl (lock);
    for (auto& device : inputs)
        device->keyboardState.removeListener (this);
    inputs.clear();
    computerKeyboard = nullptr;
}

void MidiRecorder::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // A keyboard was plugged in or out.
    const auto before = getInputNames();
    const bool hadComputerKeyboard = hasComputerKeyboard();
    attachToInputs();
    if (getInputNames() != before || hasComputerKeyboard() != hadComputerKeyboard)
    {
        routedTo = nullptr;
        updateRouting();
    }
}

juce::StringArray MidiRecorder::getInputNames() const
{
    juce::StringArray names;
    for (auto& device : inputs)
        if (device != computerKeyboard)
            names.add (device->getName());
    return names;
}

void MidiRecorder::playComputerKey (int note, bool down, float velocity)
{
    if (computerKeyboard == nullptr || ! juce::isPositiveAndBelow (note, 128))
        return;
    if (down)
        computerKeyboard->keyboardState.noteOn (1, note, velocity);
    else
        computerKeyboard->keyboardState.noteOff (1, note, 0.0f);
}

void MidiRecorder::updateRouting()
{
    auto& edit = session.getEdit();
    auto* track = session.getAdapter().getChannelTrack (session.getSelectedChannel());
    if (track == routedTo && track != nullptr)
        return;

    edit.getTransport().ensureContextAllocated();
    auto* context = edit.getCurrentPlaybackContext();
    if (context == nullptr)
        return;

    for (auto* instance : context->getAllInputs())
    {
        const bool ours = std::any_of (inputs.begin(), inputs.end(),
                                       [instance] (auto& device) { return &instance->owner == device.get(); });
        if (! ours)
            continue;

        for (auto target : instance->getTargets()) [[maybe_unused]]
            auto removed = instance->removeTarget (target, nullptr);

        if (track != nullptr) [[maybe_unused]]
            auto added = instance->setTarget (track->itemID, false, nullptr);
    }
    routedTo = track;
}

//==============================================================================
double MidiRecorder::editSecondsNow (const te::MidiInputDevice& device) const
{
    auto* context = session.getEdit().getCurrentPlaybackContext();
    if (context == nullptr)
        return 0.0;

    // As Tracktion does for its own recordings: the device's stream time, mapped to the timeline
    // (pattern loop included), less the latency of the playback graph.
    const auto streamTime = juce::Time::getMillisecondCounterHiRes() * 0.001 + device.getAdjustSecs();
    const auto sampleRate = session.getEdit().engine.getDeviceManager().getSampleRate();
    const auto latency = sampleRate > 0.0 ? (double)context->getLatencySamples() / sampleRate : 0.0;
    return context->globalStreamTimeToEditTime (streamTime).inSeconds() - latency;
}

void MidiRecorder::add (juce::MidiKeyboardState* state, int note, float velocity)
{
    if (! recording)
        return;

    const juce::ScopedLock sl (lock);
    for (auto& device : inputs)
        if (&device->keyboardState == state)
        {
            events.push_back ({ note, velocity, editSecondsNow (*device) });
            return;
        }
}

void MidiRecorder::handleNoteOn (juce::MidiKeyboardState* state, int, int note, float velocity)
{
    add (state, note, juce::jmax (0.01f, velocity));
}

void MidiRecorder::handleNoteOff (juce::MidiKeyboardState* state, int, int note, float) { add (state, note, 0.0f); }

//==============================================================================
void MidiRecorder::start()
{
    const juce::ScopedLock sl (lock);
    events.clear();
    recordStartSeconds = session.getEdit().getTransport().getPosition().inSeconds();
    recording = true;
}

int MidiRecorder::stop()
{
    if (! recording)
        return 0;

    std::vector<Event> taken;
    double stopSeconds = 0.0;
    {
        const juce::ScopedLock sl (lock);
        recording = false;
        taken.swap (events);
        if (! inputs.empty())
            stopSeconds = editSecondsNow (*inputs.front());
    }

    const auto* pattern = session.getProject().findPattern (session.getCurrentPattern());
    const auto channel = session.getSelectedChannel();
    if (pattern == nullptr || ! channel.isValid() || taken.empty())
        return 0;

    const auto patternLength = juce::jmax<model::Ticks> (1, pattern->length);
    auto ticksAt = [&] (double seconds)
    {
        return (model::Ticks)std::llround (
            engine::timeToTicks (session.getEdit(), te::TimePosition::fromSeconds (seconds)));
    };

    // Pair note-ons with their note-offs (or the end of recording). The pattern loops, so a note-off
    // may come "before" its note-on.
    std::vector<model::Note> recorded;
    std::map<int, std::pair<model::Ticks, float>> held;
    auto finish = [&] (int pitch, model::Ticks end)
    {
        auto it = held.find (pitch);
        if (it == held.end())
            return;
        auto [start, velocity] = it->second;
        held.erase (it);

        auto length = end - start;
        while (length <= 0)
            length += patternLength;

        model::Note n;
        n.pitch = pitch;
        n.start = ((start % patternLength) + patternLength) % patternLength;
        if (quantize)
            n.start = (n.start + stepTicks / 2) / stepTicks * stepTicks % patternLength;
        n.length = juce::jmax (minimumLength, juce::jmin (length, patternLength));
        n.velocity = velocity;
        recorded.push_back (n);
    };

    for (auto& e : taken)
    {
        const auto at = ticksAt (e.editSeconds);
        if (e.velocity > 0.0f)
        {
            finish (e.note, at); // a repeated note-on ends the previous one
            held[e.note] = { at, e.velocity };
        }
        else
        {
            finish (e.note, at);
        }
    }
    const auto end = ticksAt (stopSeconds);
    while (! held.empty())
        finish (held.begin()->first, end);

    if (recorded.empty())
        return 0;

    // Added to what the pattern already has for this channel (overdub, as in FL).
    std::vector<model::Note> all;
    if (auto it = pattern->notes.find (channel); it != pattern->notes.end())
        all = it->second;
    all.insert (all.end(), recorded.begin(), recorded.end());
    std::sort (all.begin(), all.end(),
               [] (auto& a, auto& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });

    juce::Array<juce::var> list;
    for (auto& n : all)
        list.add (noteToJson (n));

    auto* params = new juce::DynamicObject();
    params->setProperty ("pattern", (int)pattern->id.value);
    params->setProperty ("channel", (int)channel.value);
    params->setProperty ("notes", list);
    params->setProperty ("description", "Record notes");

    juce::String error;
    auto command = commands::CommandRegistry::get().create ("pattern.setNotes", juce::var (params), error);
    if (command == nullptr || session.getHistory().perform (std::move (command)).failed())
        return 0;
    return (int)recorded.size();
}

} // namespace spinus
