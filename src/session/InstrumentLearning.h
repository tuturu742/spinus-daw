#pragma once

#include "InstrumentProfiles.h"
#include "Session.h"

#include <functional>

namespace spinus::learning
{

/** What learning an instrument found and kept. */
struct Outcome
{
    juce::String problem; // why nothing was learned (it isn't a plugin Spinus can load here, it played no note...)
    InstrumentProfiles::Profile profile; // as saved
    int named = 0;                       // keys the plugin named
    int heard = 0;                       // keys listening found something for (plays, a switch, a special key)
    int changed = 0;                     // keys whose job changed in the profile
    juce::String note;                   // what went wrong on the way when something was learned all the same
    bool ok() const { return problem.isEmpty(); }
};

/** An instrument ready to learn: what the helper loads, and with which sound. */
struct Job
{
    juce::String key, name; // the sound index's key of the instrument (InstrumentPresets::libraryOf), its name
    SoundSource source;
    juce::File state; // its state now (a scratch file, deleted with the job), or none
    juce::File indexFolder;
    juce::String problem; // why it can't be learned
    Job() = default;
    Job (const Job&) = delete;
    Job (Job&&) = default;
    ~Job();
};

/** Learn this instrument without its window, for the command line and agents: the device's instrument, with the
    sound it has now (its state), is asked for its keys' names and then listened to key by key in the helper
    program (parts::keyNamesInHelper, parts::probeInHelper), as Ask the instrument and Listen to every key do; what
    they found goes into its first mode (and each mode entered by a key, listened to after that key), the keys the
    user set staying theirs, with the octave its keys sound in, and the profile is saved (InstrumentProfiles, beside
    the sound index's folder). The manual is not read (that needs the AI). Takes a minute or two: not the audio
    thread; `say` gets progress in words, `shouldStop` ends it early (what was found is kept). */
Outcome learn (Session&, model::DeviceId, const juce::File& indexFolder, const std::function<bool()>& shouldStop,
               const std::function<void (const juce::String&)>& say);
/** learn() in two halves, for a caller that listens on another thread: prepare() reads the device (message
    thread), run() does the rest and touches no session (any thread but the audio thread). */
Job prepare (Session&, model::DeviceId, const juce::File& indexFolder);
Outcome run (const Job&, const std::function<bool()>& shouldStop, const std::function<void (const juce::String&)>& say);

/** The outcome as JSON for the command line and agents: {ok, instrument, named, heard, changed, keys
    (InstrumentProfiles::describeForAgents), problem}. */
juce::var toVar (const Outcome&, const juce::String& instrumentName);

} // namespace spinus::learning
