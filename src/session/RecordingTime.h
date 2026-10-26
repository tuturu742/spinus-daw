#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include <set>

namespace spinus::recording
{

/** The first enabled audio input of the edit's playback context (allocated if needed); nullptr when
    Spinus has none (Options › Audio). */
tracktion::InputDeviceInstance* findAudioInput (tracktion::Edit&);

/** Where in the song (unlooped seconds) a sound that arrived at `streamTime` was played, as Tracktion maps
    its own recordings: the stream time to the song's time, less the time the sound took to arrive and the
    song took to be heard (input, output and playback latency). */
double editSecondsAt (tracktion::Edit&, double streamTime, double sampleRate);

/** Recording from a sound card with many inputs (multi-track recording). Inputs are counted from 0 here (the
    window shows them from 1).

    The sound card's inputs in use, in the order the recording input holds them: findAudioInput()'s channel
    i is input activeInputChannels()[i]. Empty without a sound card. */
std::vector<int> activeInputChannels (tracktion::Engine&);
/** The sound card's inputs by name, in use or not ("Input 1"... or the card's own names). */
juce::StringArray inputChannelNames (tracktion::Engine&);
/** The input Record audio and Record idea use when no track is armed: the one chosen in Options › Audio
    (by default the first in use, else the first). It may be off: useInputs() switches it on. */
int getDefaultInput (tracktion::Engine&);
void setDefaultInput (tracktion::Engine&, int input);
/** Switches these inputs on as well, so they can be recorded (the sound card restarts if one was off). */
juce::Result useInputs (tracktion::Engine&, const std::set<int>& inputs);
/** After the sound card changed: the recording input holds every input in use (EngineHost calls it). */
void refreshInputLayout (tracktion::Engine&);

/** Spinus opens the sound card's inputs only while something records from them or meters them: an armed
    track, a recording, Record idea, Options › Audio. Each holds an InputUse; when the last one goes, the
    inputs are switched off (releaseInputs), the input chosen in Options › Audio staying chosen. So a
    microphone (a webcam's, say) is not opened while nobody records: through PipeWire it would join the
    system's sound and could become its clock, with small buffers for every program.
    Holding one switches nothing on: useInputs() does. Message thread only. */
class InputUse
{
public:
    explicit InputUse (tracktion::Engine&);
    ~InputUse();

    /** How many are held for this engine. */
    static int count (tracktion::Engine&);
    /** Going, it leaves the inputs as they are (Spinus closing: no need to restart the sound card). */
    void leaveInputsOn() noexcept { release = false; }

private:
    tracktion::Engine& engine;
    bool release = true;
    JUCE_DECLARE_NON_COPYABLE (InputUse)
};

/** Switches every input of the sound card off, unless an InputUse is held. */
void releaseInputs (tracktion::Engine&);

} // namespace spinus::recording
