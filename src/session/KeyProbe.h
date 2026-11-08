#pragma once

#include "InstrumentProfiles.h"
#include "SoundDescriptors.h"

#include <functional>

namespace spinus
{

/** Listen to every key: what each key of an instrument does in one mode, found by ear (the instrument learning
    design's probing). Runs in the sound index's helper (SoundRender's "probe" mode), never in Spinus.

    Every key is played on its own, after the mode's entry key when it has one, and heard:
    - a sound at the key's pitch (an octave off counts): it plays;
    - a sound at another pitch, several pitches, or no pitch: a special key (a strummer's chord key, a noise);
    - silence, after which a reference note (one that plays, heard before the probing starts) sounds different:
      a switch key; the instrument is then put back as it was (`reset`) before the next key;
    - silence that changes nothing: unused.
    The reference note is heard a few times first, so a sampler's own variation (round robin) isn't taken for a
    switch. Message thread or one other thread, as the player allows. */
namespace keyprobe
{
using Player = sounds::Player;

struct Options
{
    int enterKey = -1;             // the key that enters the mode, played before every note (-1: none)
    int from = 0, to = 127;        // the keys heard
    double hold = 0.4, tail = 0.4; // seconds each key is held, and heard after
};

struct Result
{
    std::array<InstrumentProfiles::Key, 128> keys {}; // the keys heard (Source::probing); the others unknown
    int reference = -1;                               // the reference note (-1: nothing sounded)
    juce::String problem;                             // why nothing was found ("it makes no sound")
};

/** Hears keys `from` to `to`. `progress` (keys done, keys in all) returns false to stop: what was heard so far is
    kept. `reset` puts the instrument back as it was (its preset again), after a switch key. */
Result probe (const Player&, const std::function<void()>& reset, const Options& = {},
              const std::function<bool (int, int)>& progress = {});

/** The key's frequency in Hz (A4 = 69 = 440 Hz). */
double frequencyOf (int note);

/** Merges a probing's keys into a mode: the keys the user set stay theirs (the user's word wins), every other key
    heard takes what was heard. Returns how many keys changed. */
int merge (InstrumentProfiles::Mode&, const std::array<InstrumentProfiles::Key, 128>& heard);
} // namespace keyprobe

} // namespace spinus
