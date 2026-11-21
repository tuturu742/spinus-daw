#include "InstrumentLearning.h"

#include "InstrumentPresets.h"
#include "KeyProbe.h"
#include "PartSounds.h"
#include "ProgramFolder.h"
#include "SoundIndex.h"
#include "engine/EngineAdapter.h"
#include "model/StockDevices.h"
#include "plugins/HostLibraries.h"

namespace spinus::learning
{

Job::~Job()
{
    if (state != juce::File())
        state.deleteFile();
}

Job prepare (Session& session, model::DeviceId id, const juce::File& indexFolder)
{
    Job job;
    job.indexFolder = indexFolder;
    const auto* device = session.getProject().findDevice (id);
    if (device == nullptr)
    {
        job.problem = "there is no device " + juce::String ((int)id.value) + " in the song.";
        return job;
    }
    job.key = InstrumentPresets::libraryOf (*device);
    job.name = model::getDeviceDisplayName (*device);
    bool found = false;
    for (auto& s : findSoundSources (session.getDeviceList()))
        if (s.key == job.key)
        {
            job.source = s;
            found = true;
        }
    if (! found)
    {
        job.problem = job.name + " can't be listened to here: it isn't an instrument installed on this computer, or "
                                 "it plays only the samples it's given.";
        return job;
    }
    // With the sound it has now, as the Channel Rack's Learn this instrument does.
    if (auto* plugin = dynamic_cast<tracktion::ExternalPlugin*> (session.getAdapter().getDevicePlugin (id)))
        if (auto* instance = plugin->getAudioPluginInstance())
        {
            juce::MemoryBlock block;
            instance->getStateInformation (block);
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("spinus-learn-" + juce::Uuid().toString() + ".state");
            if (block.getSize() > 0 && file.replaceWithData (block.getData(), block.getSize()))
                job.state = file;
        }
    return job;
}

Outcome run (const Job& job, const std::function<bool()>& shouldStop,
             const std::function<void (const juce::String&)>& say)
{
    Outcome outcome;
    if (job.problem.isNotEmpty())
    {
        outcome.problem = job.problem;
        return outcome;
    }
    SoundIndexer::Options helper;
    helper.helper = getHelperProgram ("spinus-plugin-scanner");
    helper.hostLibraries = plugins::getHostLibraries();

    InstrumentProfiles profiles (job.indexFolder);
    profiles.load();
    auto profile = profiles.hasOwn (job.key) ? *profiles.get (job.key) : InstrumentProfiles::Profile {};
    if (profile.modes.empty())
        profile.modes.emplace_back();

    say ("Asking " + job.name + " for its keys' names...");
    juce::String problem;
    if (const auto named = parts::keyNamesInHelper (helper, job.source, {}, job.state, shouldStop, problem))
    {
        for (const auto& k : *named)
            outcome.named += k.job != InstrumentProfiles::Job::unknown ? 1 : 0;
        outcome.changed += keyprobe::merge (profile.modes.front(), *named);
    }

    juce::String listenProblem;
    for (size_t m = 0; m < profile.modes.size(); ++m)
    {
        auto& mode = profile.modes[m];
        if (m > 0 && mode.enterKey < 0)
            continue; // a mode entered another way (a button): only its window can learn it
        if (shouldStop && shouldStop())
            break;
        problem = {};
        int octaves = 0;
        int lastSaid = -100;
        const auto heard = parts::probeInHelper (
            helper, job.source, {}, job.state, mode.enterKey, shouldStop,
            [&] (int done, int total)
            {
                if (done - lastSaid >= 16 || done == total)
                {
                    lastSaid = done;
                    say ("Listening to " + job.name + (m > 0 ? " (" + mode.name + ")" : juce::String()) + ": " +
                         juce::String (done) + " of " + juce::String (total) + " keys");
                }
            },
            problem, &octaves);
        if (! heard)
        {
            if (listenProblem.isEmpty())
                listenProblem = problem.isNotEmpty() ? problem : juce::String ("nothing could be heard.");
            continue;
        }
        for (const auto& k : *heard)
            outcome.heard +=
                k.job != InstrumentProfiles::Job::unknown && k.job != InstrumentProfiles::Job::unused ? 1 : 0;
        outcome.changed += keyprobe::merge (mode, *heard);
        mode.octaves = octaves;
    }
    if (outcome.named == 0 && outcome.heard == 0)
    {
        outcome.problem = listenProblem.isNotEmpty() ? listenProblem : juce::String ("nothing could be heard.");
        return outcome;
    }
    outcome.note = listenProblem; // the keys it named are kept even when listening failed
    profiles.set (job.key, profile);
    if (! profiles.save())
        outcome.problem = "the learned keys could not be saved.";
    outcome.profile = profile;
    return outcome;
}

Outcome learn (Session& session, model::DeviceId id, const juce::File& indexFolder,
               const std::function<bool()>& shouldStop, const std::function<void (const juce::String&)>& say)
{
    return run (prepare (session, id, indexFolder), shouldStop, say);
}

juce::var toVar (const Outcome& outcome, const juce::String& instrumentName)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("ok", outcome.ok());
    o->setProperty ("instrument", instrumentName);
    if (! outcome.ok())
    {
        o->setProperty ("problem", outcome.problem);
        return juce::var (o);
    }
    o->setProperty ("named", outcome.named);
    o->setProperty ("heard", outcome.heard);
    o->setProperty ("changed", outcome.changed);
    if (outcome.note.isNotEmpty())
        o->setProperty ("note", outcome.note);
    if (const auto keys = InstrumentProfiles::describeForAgents (outcome.profile); ! keys.isVoid())
        o->setProperty ("keys", keys);
    else
        o->setProperty ("keys", "it plays every key as a note");
    return juce::var (o);
}

} // namespace spinus::learning
