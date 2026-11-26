#pragma once

#include "InstrumentProfiles.h"

#include <juce_core/juce_core.h>

namespace spinus::model
{
struct Channel;
}

namespace spinus
{
class Session;
}

/** Which sample bank an instrument plays (InstrumentBank), for instruments whose keys depend on it
    (InstrumentProfiles::Profile::byBank): a sample player's .sfz file, a drum kit and its key map. Found the same
    way for every plugin, with no plugin's name in it: the files the plugin's saved sound names (its state, read
    through the wrappings hosts and plugins put around it: a host's binary XML, base64 text, file:// addresses),
    else the plugin's own name for its current program, else the name the user gave the channel's bank (Bank, in
    the Channel Rack's menu), which always wins. Message thread for the Session one. */
namespace spinus::instrumentbanks
{

/** The files a plugin's saved state names, in the order they first appear: absolute paths (or file://
    addresses) of files with an extension, as written in it, with / between folders. */
juce::StringArray filesNamedIn (const juce::MemoryBlock& state);

/** The bank a state names (filesNamedIn): every file in its id, the first one's name without its extension as
    its name. Not known when the state names no file. */
InstrumentBank fromState (const juce::MemoryBlock& state);
/** The bank a state file names (a saved sound's, a preset's). */
InstrumentBank fromStateFile (const juce::File& state);
/** The bank the user named (Bank › Name this bank...). */
InstrumentBank named (const juce::String& name);
/** The bank a program name stands for, when the state names no file. */
InstrumentBank fromProgram (const juce::String& programName);

/** The bank the channel's instrument plays now: the one its user named (model::Channel::bank), else the one its
    plugin's state names, else its program's (when the plugin has more than one). Not known for Spinus's own
    instruments and plugins that say nothing of a bank. */
InstrumentBank of (Session&, const model::Channel&);

/** What agents are told of an instrument's keys when it plays `bank` (InstrumentProfiles::describeForAgents of
    InstrumentProfiles::get), or, when the loaded bank decides its keys and this bank isn't learned yet (or isn't
    known), {bank, learned: false, note} saying so, so an agent doesn't take another bank's keys for these. Void
    when there is nothing to tell. */
juce::var keysForAgents (const InstrumentProfiles&, const juce::String& instrument, const InstrumentBank&);
/** keysForAgents for a channel's instrument, asking for its bank (of) only when the bank decides its keys. */
juce::var keysForAgents (const InstrumentProfiles&, Session&, const model::Channel&);

} // namespace spinus::instrumentbanks
