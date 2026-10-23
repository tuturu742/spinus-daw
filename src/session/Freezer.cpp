#include "Freezer.h"

#include "Session.h"

namespace spinus
{

using namespace spinus::model;

namespace
{
const char* const renderingSuffix = "-rendering";
} // namespace

Freezer::Freezer (Session& s) : session (s) {}

Freezer::~Freezer()
{
    cancelPendingUpdate();
    job.reset();
    if (tempFolder != juce::File())
        tempFolder.deleteRecursively();
}

juce::File Freezer::getCacheFolder() const
{
    if (projectFolder != juce::File())
        return projectFolder.getChildFile ("cache").getChildFile ("freeze");

    if (tempFolder == juce::File())
        const_cast<Freezer*> (this)->tempFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                                      .getChildFile ("spinus-freeze")
                                                      .getChildFile (juce::Uuid().toString());
    return tempFolder;
}

double Freezer::getSampleRate() const
{
    // The rate playback runs at, so the audio plays without resampling.
    const auto rate = session.getEdit().engine.getDeviceManager().getSampleRate();
    return rate > 0.0 ? rate : 48000.0;
}

std::set<FreezeTarget> Freezer::getFrozenFlags() const
{
    std::set<FreezeTarget> result;
    const auto& p = session.getProject();
    for (auto& c : p.channels)
        if (c.frozen)
            result.insert (c.id);
    for (auto& i : p.mixer.inserts)
        if (i.frozen)
            result.insert (i.id);
    return result;
}

namespace
{
/** Closes the engine's cached readers of `file`, then deletes it. Windows refuses to delete a file that is
    still open (Linux does not mind), and Tracktion keeps readers of the files it played. */
bool releaseAndDelete (tracktion::Engine& engine, const juce::File& file)
{
    engine.getAudioFileManager().releaseFile (tracktion::AudioFile (engine, file));
    return file.deleteFile();
}
} // namespace

void Freezer::deleteIfOurs (const juce::File& file)
{
    if (file.isAChildOf (getCacheFolder()) && ! releaseAndDelete (session.getEdit().engine, file))
        leftOvers.insert (file); // still playing: once the engine lets go (update())
}

void Freezer::deleteLeftOvers()
{
    auto& engine = session.getEdit().engine;
    std::erase_if (leftOvers, [&engine] (const juce::File& f) { return releaseAndDelete (engine, f); });
}

//==============================================================================
void Freezer::setProjectFolder (const juce::File& folder)
{
    if (folder == projectFolder)
        return;

    projectFolder = folder;
    const auto cache = getCacheFolder();

    // The audio in use comes along (e.g. the first save of an untitled project).
    for (auto& [slot, file] : playing)
    {
        if (file.isAChildOf (cache))
            continue;
        const auto copy = cache.getChildFile (file.getFileName());
        if (copy.existsAsFile() || (cache.createDirectory() && file.copyFileTo (copy)))
            file = copy;
    }

    if (folder != juce::File() && tempFolder != juce::File())
    {
        // Still playing from the temporary folder until the adapter's next sync: keep it until then.
        auto old = std::exchange (tempFolder, juce::File());
        juce::MessageManager::callAsync ([old] { old.deleteRecursively(); });
    }
}

void Freezer::clear()
{
    job.reset();
    jobResult.reset();
    pending.clear();
    playing.clear();
    failures.clear();
    lastFrozen.clear();
    session.getAdapter().setFrozenAudio ({});
}

void Freezer::projectLoaded()
{
    clear();
    lastFrozen = getFrozenFlags();
    update();

    // Files the project no longer plays (in song mode or any pattern), and renders a crash left behind.
    if (projectFolder != juce::File())
    {
        const auto& p = session.getProject();
        std::set<juce::String> keep;
        for (auto& target : getEffectiveFreezeTargets (p))
        {
            const auto tuned = session.getTuner().getRenderedFiles();
            auto renderingsOf = [&] (const FreezePlan& plan)
            {
                juce::String renderings;
                for (auto& clip : plan.project.playlist.clips)
                    if (auto it = tuned.find (clip.id); it != tuned.end())
                        renderings << it->second.getFileName() << ";";
                return renderings;
            };
            const auto song = makeFreezePlan (p, target, std::nullopt);
            keep.insert (freezeKey (song, getSampleRate(), renderingsOf (song)));
            for (auto& pattern : p.patterns)
            {
                const auto plan = makeFreezePlan (p, target, pattern.id);
                keep.insert (freezeKey (plan, getSampleRate(), renderingsOf (plan)));
            }
        }
        for (auto& file : getCacheFolder().findChildFiles (juce::File::findFiles, false, "*.wav"))
            if (! keep.contains (file.getFileNameWithoutExtension()))
                file.deleteFile();
    }
}

void Freezer::forget (const FreezeTarget& target)
{
    for (auto it = playing.begin(); it != playing.end();)
    {
        if (it->first.target != target)
        {
            ++it;
            continue;
        }
        deleteIfOurs (it->second);
        it = playing.erase (it);
    }
    std::erase_if (pending, [&] (auto& entry) { return entry.first.target == target; });
    std::erase_if (failures, [&] (auto& entry) { return entry.first.target == target; });
}

void Freezer::play (const Slot& slot, const juce::File& file)
{
    auto it = playing.find (slot);
    if (it != playing.end() && it->second != file)
        deleteIfOurs (it->second); // replaced by newer audio
    playing[slot] = file;
}

void Freezer::update()
{
    const auto& p = session.getProject();

    // Unfrozen or removed since last time: their audio goes (an undo freezes and renders again).
    const auto flags = getFrozenFlags();
    for (auto& target : lastFrozen)
        if (! flags.contains (target))
            forget (target);

    // Just frozen: close the plugin windows, whose settings are about to be rendered.
    for (auto& target : flags)
        if (! lastFrozen.contains (target))
            for (auto device : getFrozenParts (p, { target }).devices)
                session.closeDeviceWindow (device);
    lastFrozen = flags;

    std::optional<PatternId> pattern;
    if (session.getMode() == Session::Mode::pattern)
        pattern = session.getCurrentPattern();

    const auto cache = getCacheFolder();
    const auto rate = getSampleRate();
    std::map<FreezeTarget, juce::File> audio;
    std::map<Slot, Render> wanted;

    for (auto& target : getEffectiveFreezeTargets (p))
    {
        auto plan = makeFreezePlan (p, target, pattern);
        const Slot slot { target, pattern };
        if (plan.silent)
        {
            audio[target] = juce::File(); // nothing to play
            continue;
        }

        // Tuned audio clips (Spinus Tune) are rendered with their tuned audio: a new rendering refreezes.
        const auto tuned = session.getTuner().getRenderedFiles();
        juce::String renderings;
        for (auto& clip : plan.project.playlist.clips)
            if (auto it = tuned.find (clip.id); it != tuned.end())
                renderings << it->second.getFileName() << ";";
        const auto key = freezeKey (plan, rate, renderings);
        const auto file = cache.getChildFile (key + ".wav");
        if (file.existsAsFile())
        {
            play (slot, file);
            audio[target] = file;
            continue;
        }

        if (auto it = playing.find (slot); it != playing.end())
            audio[target] = it->second; // the previous audio until the new one is ready

        auto failed = failures.find (slot);
        if (failed == failures.end() || failed->second.first != key)
            wanted[slot] = { key, std::move (plan) };
    }

    pending = std::move (wanted);

    // A render that is no longer wanted stops; one that still is need not start again.
    if (job != nullptr)
    {
        auto it = pending.find (jobSlot);
        if (it != pending.end() && it->second.key == jobKey)
            pending.erase (it);
        else
            job->cancel();
    }

    session.getAdapter().setFrozenAudio (std::move (audio));
    deleteLeftOvers(); // the engine has let go of the audio it no longer plays

    if (! pending.empty())
        triggerAsyncUpdate();
}

//==============================================================================
void Freezer::handleAsyncUpdate()
{
    if (jobResult)
    {
        const auto result = *jobResult;
        jobResult.reset();
        job.reset();
        renderDone (result);
    }

    if (job == nullptr)
        startNextRender();

    sendChangeMessage();
}

void Freezer::startNextRender()
{
    if (pending.empty())
        return;

    auto next = pending.begin();
    jobSlot = next->first;
    jobKey = next->second.key;
    const auto plan = std::move (next->second.plan);
    pending.erase (next);

    engine::ExportSettings settings;
    settings.format = engine::ExportSettings::Format::wav32Float;
    settings.sampleRate = getSampleRate();
    settings.pattern = plan.pattern;
    settings.unheardInserts = plan.unheard;
    settings.failOnMissingDevices = true;
    settings.tunedAudio = session.getTuner().getRenderedFiles();
    settings.tailSeconds = 2.0;
    if (session.getProjectFolder() != juce::File())
        settings.audioFolder = session.getProjectFolder().getChildFile ("audio");

    const auto cache = getCacheFolder();
    if (! cache.createDirectory())
    {
        failures[jobSlot] = { jobKey, "Cannot create " + cache.getFullPathName() };
        return startNextRender();
    }

    // The job calls back from inside itself: finish on the next message loop turn.
    job = std::make_unique<engine::ExportJob> (session.getEdit().engine, session.getDeviceFactory(), plan.project,
                                               session.getAdapter().captureDeviceStates(),
                                               cache.getChildFile (jobKey + renderingSuffix), settings,
                                               [this] (const engine::ExportJob::Result& r)
                                               {
                                                   jobResult = r;
                                                   triggerAsyncUpdate();
                                               });
    job->start();
}

void Freezer::renderDone (const engine::ExportJob::Result& result)
{
    const auto cache = getCacheFolder();
    const auto rendered = cache.getChildFile (jobKey + renderingSuffix + ".wav");

    if (result.cancelled)
    {
        rendered.deleteFile();
        return;
    }

    if (! result.ok)
    {
        rendered.deleteFile();
        failures[jobSlot] = { jobKey, result.error };
        return;
    }

    ++renderCount;
    if (! rendered.moveFileTo (cache.getChildFile (jobKey + ".wav")))
    {
        failures[jobSlot] = { jobKey, "Cannot write into " + cache.getFullPathName() };
        return;
    }

    session.refreshRenderedAudio(); // calls update(), which finds the new file
}

//==============================================================================
Freezer::Status Freezer::getStatus (FreezeTarget target) const
{
    const auto& p = session.getProject();
    const bool flagged = std::holds_alternative<ChannelId> (target)
                             ? (p.findChannel (std::get<ChannelId> (target)) != nullptr &&
                                p.findChannel (std::get<ChannelId> (target))->frozen)
                             : (p.findInsert (std::get<InsertId> (target)) != nullptr &&
                                p.findInsert (std::get<InsertId> (target))->frozen);
    if (! flagged)
        return {};

    if (auto* insert = std::get_if<InsertId> (&target))
        if (auto why = whyCannotFreeze (p, *insert); why.isNotEmpty())
            return { State::cannotFreeze, 0.0f, why };

    std::optional<PatternId> pattern;
    if (session.getMode() == Session::Mode::pattern)
        pattern = session.getCurrentPattern();
    const Slot slot { target, pattern };

    if (job != nullptr && jobSlot == slot)
        return { State::rendering, job->getProgress(), {} };
    if (pending.contains (slot))
        return { State::rendering, 0.0f, {} };
    if (auto it = failures.find (slot); it != failures.end())
        return { State::failed, 0.0f, it->second.second };
    return { State::frozen, 1.0f, {} };
}

} // namespace spinus
