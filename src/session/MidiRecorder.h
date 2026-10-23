#pragma once

#include "model/Model.h"

#include <tracktion_engine/tracktion_engine.h>

namespace spinus
{

class Session;

/** MIDI keyboards in Spinus: plays the selected channel live, and records into the current
    pattern while recording (FL: the selected channel follows the keyboard).

    Every MIDI input except Tracktion's virtual ones is routed to the selected channel's track with
    monitoring on, so the keyboard is heard at once. While recording, each note is timestamped as it
    arrives and placed with the same clock mapping Tracktion uses for its own recordings (including
    the pattern loop and the output latency). When recording stops, the notes are added to the
    channel's notes in the current pattern as one undo step ("Record notes"), snapped to sixteenths
    when quantizing is on.

    Message thread, except the note callbacks (MIDI thread), which only append to a locked list.
*/
class MidiRecorder : private juce::MidiKeyboardState::Listener, private juce::ChangeListener
{
public:
    explicit MidiRecorder (Session&);
    ~MidiRecorder() override;

    /** Routes the keyboards to the session's selected channel (call when it or the project changes). */
    void updateRouting();

    void start();
    /** Ends recording and writes the notes. Returns how many notes were added. */
    int stop();
    bool isRecording() const noexcept { return recording; }

    bool isQuantizing() const noexcept { return quantize; }
    void setQuantizing (bool shouldQuantize) noexcept { quantize = shouldQuantize; }

    /** The inputs used (the MIDI keyboards found), for the settings and tests. Without the computer
        keyboard. */
    juce::StringArray getInputNames() const;

    /** The computer keyboard as a MIDI keyboard (FL: "Typing keyboard to piano"): a Tracktion virtual
        input fed by the user interface, played and recorded like any MIDI keyboard. */
    static constexpr const char* computerKeyboardName = "Spinus computer keyboard";
    bool hasComputerKeyboard() const noexcept { return computerKeyboard != nullptr; }
    /** Presses or releases a note on the computer keyboard input (message thread). */
    void playComputerKey (int note, bool down, float velocity = 0.8f);

private:
    struct Event
    {
        int note;
        float velocity; // 0 for note-off
        double editSeconds;
    };

    void handleNoteOn (juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void handleNoteOff (juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the device list changed
    void attachToInputs();
    void detachFromInputs();
    void add (juce::MidiKeyboardState*, int note, float velocity);
    double editSecondsNow (const tracktion::MidiInputDevice&) const;

    Session& session;
    std::vector<std::shared_ptr<tracktion::MidiInputDevice>> inputs;
    std::shared_ptr<tracktion::MidiInputDevice> computerKeyboard; // also in `inputs`
    tracktion::AudioTrack* routedTo = nullptr;

    juce::CriticalSection lock; // `inputs` and `events`: the MIDI thread (add) and the message thread
    std::vector<Event> events;
    std::atomic<bool> recording { false };
    double recordStartSeconds = 0.0;
    bool quantize = true;
};

} // namespace spinus
