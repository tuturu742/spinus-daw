#pragma once

#include "InstrumentProfiles.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace spinus
{

/** The plugin's own report of its keys (the instrument learning design's third source): the names a plugin gives
    its keys (VST2 key names, a VST3 instrument's pitch names, CLAP note names) and a VST3 instrument's key switches.
    Few plugins report them; those that do mostly name a drum map's keys ("Kick") or their key switches ("Palm
    mute"). Runs in the sound index's helper (SoundRender's "keynames" mode), never in Spinus. Message thread. */
namespace pluginkeys
{
struct Named
{
    int key = 0;
    juce::String name;
    bool switches = false; // the plugin reports it as a key switch, or its name says so
};

/** What the plugin reports for keys 0 to 127, in key order; empty when it reports nothing. A name that is only
    the key's note name ("C3", "C#-1") says nothing and is left out. */
std::vector<Named> read (juce::AudioPluginInstance&);

/** The names as a profile's keys (Source::plugin): a key switch switches, any other named key plays and keeps the
    name as what it does. The other keys stay unknown. */
std::array<InstrumentProfiles::Key, 128> asKeys (const std::vector<Named>&);

/** True for a name that is only a note's name, in any octave numbering ("C3", "Db-1", "F#5"). */
bool isNoteName (const juce::String&);
/** True for a name that says the key is a switch, in generic words ("KS Legato", "Keyswitch: palm mute",
    "Articulation 2"). */
bool namesASwitch (const juce::String&);
} // namespace pluginkeys

} // namespace spinus
