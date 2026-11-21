#pragma once

#include <juce_core/juce_core.h>

#include <map>

namespace spinus::drumkeys
{

/** The keys a drum kit plays each drum on, by the names the Analyzer's drum hits have ("kick", "snare", "clap",
    "closed_hat", "open_hat", "low_tom", "mid_tom", "high_tom", "rim", "crash", "ride"): General MIDI's key
    where the kit's sound there is that drum (or one like it: a clap for a snare, a cymbal for an open hat), else
    its first key with that drum, else General MIDI's. `drumMap` is the sound index's measure of the kit
    ([{note, class}]); without one, General MIDI's keys. */
std::map<juce::String, int> fromMap (const juce::var& drumMap);

/** The kits the sound index measured an instrument play: its sounds (presets, or sounds the user saved) with a
    kick, a snare and a hat on their keys, as the index keeps them ({id, name, drumMap, ...}), from the
    instrument's file there (`instruments/<key>.json`, `key` as InstrumentPresets::libraryOf names it). Kept
    until the file changes (a big instrument's file takes a moment to read): message thread. */
juce::Array<juce::var> measuredKits (const juce::File& indexFolder, const juce::String& key);

/** What the Drum Machine's pad (as the Analyzer's drum hits come: the Synth Kit's layout from 60, 69 and 71 up
    for a second sound of a drum) plays: its drum's name, or empty. `extra`: the pads of the second sounds, with
    their drum. */
juce::String drumOfPad (int pad, const std::map<int, juce::String>& extra = {});

} // namespace spinus::drumkeys
