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
    (by default the first in use). */
int getDefaultInput (tracktion::Engine&);
void setDefaultInput (tracktion::Engine&, int input);
/** Switches these inputs on as well, so they can be recorded (the sound card restarts if one was off). */
juce::Result useInputs (tracktion::Engine&, const std::set<int>& inputs);
/** After the sound card changed: the recording input holds every input in use (EngineHost calls it). */
void refreshInputLayout (tracktion::Engine&);

} // namespace spinus::recording
