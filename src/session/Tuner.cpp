#include "Tuner.h"

#include "Session.h"
#include "model/ModelJson.h"

#include <juce_cryptography/juce_cryptography.h>

namespace spinus
{

using namespace spinus::model;

/** Renders one clip on its own thread; tells the Tuner when done. */
class Tuner::Worker final : public juce::Thread
{
public:
    Worker (Tuner& t, Render r) : juce::Thread ("Spinus Tune render"), tuner (t), render (std::move (r))
    {
        startThread (juce::Thread::Priority::low);
    }

    ~Worker() override
    {
        cancel = true;
        stopThread (10000);
    }

    void run() override
    {
        const auto partial = render.destination.withFileExtension ("rendering.wav");
        result = engine::TuneRender::render (render.source, render.track, render.notes, partial, &progress, &cancel);
        if (result.wasOk() && ! partial.moveFileTo (render.destination))
            result = juce::Result::fail ("Cannot write " + render.destination.getFullPathName());
        partial.deleteFile();
        finished = true;
        tuner.triggerAsyncUpdate();
    }

    Tuner& tuner;
    Render render;
    juce::Result result = juce::Result::ok();
    std::atomic<float> progress { 0.0f };
    std::atomic<bool> cancel { false }, finished { false };
};

//==============================================================================
Tuner::Tuner (Session& s) : session (s) {}

Tuner::~Tuner()
{
    cancelPendingUpdate();
    worker.reset();
    if (tempFolder != juce::File())
        tempFolder.deleteRecursively();
}

juce::File Tuner::getCacheFolder() const
{
    if (projectFolder != juce::File())
        return projectFolder.getChildFile ("cache").getChildFile ("tune");
    if (tempFolder == juce::File())
        const_cast<Tuner*> (this)->tempFolder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                                    .getChildFile ("spinus-tune")
                                                    .getChildFile (juce::Uuid().toString());
    return tempFolder;
}

void Tuner::setProjectFolder (const juce::File& folder)
{
    if (folder == projectFolder)
        return;

    // What was analysed and rendered so far comes along (e.g. the first save of an untitled project).
    const auto old = getCacheFolder();
    projectFolder = folder;
    const auto cache = getCacheFolder();
    if (old.isDirectory() && cache.createDirectory())
        for (auto& file : old.findChildFiles (juce::File::findFiles, false))
            if (! cache.getChildFile (file.getFileName()).exists())
                file.copyFileTo (cache.getChildFile (file.getFileName()));
    for (auto& [clip, file] : playing)
        if (file.isAChildOf (old))
            file = cache.getChildFile (file.getFileName());
}

void Tuner::clear()
{
    worker.reset();
    pending.clear();
    playing.clear();
    failures.clear();
    session.getAdapter().setTunedAudio ({});
}

juce::String Tuner::identify (const juce::File& audio) const
{
    // By content, so a copy (Save As collects audio into the project) is the same recording. Hashed
    // once per file and version.
    const auto where = audio.getFullPathName() + "|" + juce::String (audio.getSize()) + "|" +
                       juce::String (audio.getLastModificationTime().toMilliseconds());
    if (auto it = identities.find (where); it != identities.end())
        return it->second;
    juce::FileInputStream in (audio);
    const auto hash = in.openedOk() ? juce::SHA256 (in).toHexString().substring (0, 24) : juce::String();
    identities[where] = hash;
    return hash;
}

juce::File Tuner::analysisFileFor (const juce::File& audio) const
{
    return getCacheFolder().getChildFile (identify (audio) + "-analysis.json");
}

void Tuner::storeAnalysis (const juce::File& audio, const juce::var& analysis)
{
    analyses[identify (audio)] = analysis;
    const auto file = analysisFileFor (audio);
    if (file.getParentDirectory().createDirectory())
        file.replaceWithText (juce::JSON::toString (analysis, true));
    triggerAsyncUpdate(); // clips waiting for it can render now
}

juce::var Tuner::getAnalysis (const juce::File& audio) const
{
    const auto id = identify (audio);
    if (auto it = analyses.find (id); it != analyses.end())
        return it->second;
    const auto file = analysisFileFor (audio);
    if (! file.existsAsFile())
        return {};
    auto loaded = juce::JSON::parse (file);
    const_cast<Tuner*> (this)->analyses[id] = loaded;
    return loaded;
}

//==============================================================================
void Tuner::update()
{
    const auto& project = session.getProject();
    std::map<ClipId, juce::File> files;
    std::map<ClipId, Render> wanted;
    std::set<ClipId> tuned;

    for (auto& clip : project.playlist.clips)
    {
        const auto* audio = std::get_if<AudioClip> (&clip.content);
        if (audio == nullptr ||
            std::all_of (audio->tune.begin(), audio->tune.end(), [] (auto& n) { return n.isUnchanged(); }))
            continue;
        tuned.insert (clip.id);

        const auto source = session.resolveAudioFile (audio->file);
        const auto analysis = getAnalysis (source);
        if (! source.existsAsFile() || ! analysis.isObject())
            continue; // plays as recorded (needs analysis)

        juce::Array<juce::var> notes;
        for (auto& n : audio->tune)
            notes.add (tuneNoteToJson (n));
        const auto key =
            juce::SHA256 (("tune 1|" + identify (source) + "|" + juce::JSON::toString (notes, true)).toUTF8())
                .toHexString()
                .substring (0, 24);
        const auto destination = getCacheFolder().getChildFile (key + ".wav");

        if (destination.existsAsFile())
        {
            if (auto it = playing.find (clip.id);
                it != playing.end() && it->second != destination && it->second.isAChildOf (getCacheFolder()))
                it->second.deleteFile(); // replaced
            playing[clip.id] = destination;
            files[clip.id] = destination;
            failures.erase (clip.id);
            continue;
        }

        if (auto it = playing.find (clip.id); it != playing.end() && it->second.existsAsFile())
            files[clip.id] = it->second; // the previous rendering until the new one is ready
        if (auto failed = failures.find (clip.id); failed == failures.end() || failed->second != key)
            wanted[clip.id] = { clip.id, source, destination, engine::PitchTrack::fromJson (analysis["track"]),
                                audio->tune };
    }

    for (auto it = playing.begin(); it != playing.end();)
        it = tuned.contains (it->first) ? std::next (it) : playing.erase (it);

    pending = std::move (wanted);
    if (worker != nullptr)
        pending.erase (worker->render.clip); // being rendered (its result is kept by its own name)
    session.getAdapter().setTunedAudio (std::move (files));
    if (! pending.empty())
        triggerAsyncUpdate();
}

void Tuner::handleAsyncUpdate()
{
    if (worker != nullptr && worker->finished)
    {
        const auto clip = worker->render.clip;
        const auto result = worker->result;
        const auto destination = worker->render.destination;
        worker.reset();
        if (result.wasOk())
        {
            ++renderCount;
            session.refreshRenderedAudio(); // syncs the adapter: update() finds the new rendering
        }
        else if (result.getErrorMessage() != "Cancelled.")
            failures[clip] = destination.getFileNameWithoutExtension();
    }

    if (worker == nullptr)
    {
        if (pending.empty())
            session.refreshRenderedAudio(); // an analysis arrived: render what waited for it
        startNext();
    }
    sendChangeMessage();
}

void Tuner::startNext()
{
    if (pending.empty() || worker != nullptr)
        return;
    auto next = pending.begin();
    auto render = std::move (next->second);
    pending.erase (next);
    if (! render.destination.getParentDirectory().createDirectory())
    {
        failures[render.clip] = render.destination.getFileNameWithoutExtension();
        return startNext();
    }
    worker = std::make_unique<Worker> (*this, std::move (render));
}

std::map<ClipId, juce::File> Tuner::getRenderedFiles() const
{
    std::map<ClipId, juce::File> result;
    for (auto& [clip, file] : playing)
        if (file.existsAsFile())
            result[clip] = file;
    return result;
}

Tuner::Status Tuner::getStatus (ClipId id) const
{
    const auto* clip = session.getProject().findClip (id);
    const auto* audio = clip != nullptr ? std::get_if<AudioClip> (&clip->content) : nullptr;
    if (audio == nullptr ||
        std::all_of (audio->tune.begin(), audio->tune.end(), [] (auto& n) { return n.isUnchanged(); }))
        return {};
    if (! getAnalysis (session.resolveAudioFile (audio->file)).isObject())
        return { State::needsAnalysis, 0.0f, "Open it in Spinus Tune (with AI on) to analyse it." };
    if (worker != nullptr && worker->render.clip == id)
        return { State::rendering, worker->progress.load(), {} };
    if (pending.contains (id))
        return { State::rendering, 0.0f, {} };
    if (failures.contains (id))
        return { State::failed, 0.0f, "Could not render the tuned clip." };
    return { State::ready, 1.0f, {} };
}

} // namespace spinus
