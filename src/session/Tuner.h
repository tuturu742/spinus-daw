#pragma once

#include "engine/TuneRender.h"

#include <juce_events/juce_events.h>

namespace spinus
{

class Session;

/** Keeps tuned audio clips (Spinus Tune's graphical mode, model::TuneNote) playing their rendering.

    Before every sync of the Engine Adapter, update() works out the file each tuned clip plays: a
    rendering in the cache folder (the project's cache/tune/, or a temporary folder while untitled),
    named by a hash of the audio file and the clip's notes. Missing ones are rendered in the background,
    one at a time; meanwhile a clip plays its previous rendering, or the recording itself.

    Rendering needs the vocal's pitch as sung, which Spinus Tune's window gets from the AI service and
    stores here (storeTrack); it is kept in the cache folder, so a saved project renders again without
    the AI service.

    Message thread only (renders run on a thread of their own).
*/
class Tuner : public juce::ChangeBroadcaster, private juce::AsyncUpdater
{
public:
    explicit Tuner (Session&);
    ~Tuner() override;

    void setProjectFolder (const juce::File&);
    void clear();

    /** Call before every sync of the adapter. */
    void update();

    /** Keeps an audio file's pitch as sung (ai.v1.listen.vocal's result). */
    void storeAnalysis (const juce::File& audio, const juce::var& analysis);

    /** The stored analysis of an audio file (track and notes), or void. */
    juce::var getAnalysis (const juce::File& audio) const;

    enum class State
    {
        none,          // not tuned
        needsAnalysis, // tuned, but its pitch is not known here: open it in Spinus Tune
        rendering,
        ready,
        failed
    };

    struct Status
    {
        State state = State::none;
        float progress = 0.0f;
        juce::String message;
    };

    Status getStatus (model::ClipId) const;
    bool isRendering() const noexcept { return worker != nullptr || ! pending.empty(); }
    int getRenderCount() const noexcept { return renderCount; }
    juce::File getCacheFolder() const;

    /** The rendering each tuned clip plays now (for exports). */
    std::map<model::ClipId, juce::File> getRenderedFiles() const;

private:
    struct Render
    {
        model::ClipId clip;
        juce::File source, destination;
        engine::PitchTrack track;
        std::vector<model::TuneNote> notes;
    };

    class Worker;

    void handleAsyncUpdate() override;
    void startNext();
    juce::File analysisFileFor (const juce::File& audio) const;
    juce::String identify (const juce::File& audio) const;

    Session& session;
    juce::File projectFolder, tempFolder;
    std::map<model::ClipId, juce::File> playing; // rendering each clip plays (maybe stale while re-rendering)
    std::map<model::ClipId, Render> pending;
    std::map<model::ClipId, juce::String> failures;
    std::map<juce::String, juce::var> analyses;              // by audio file identity
    mutable std::map<juce::String, juce::String> identities; // path, size and date -> content hash
    std::unique_ptr<Worker> worker;
    int renderCount = 0;

    JUCE_DECLARE_NON_COPYABLE (Tuner)
};

} // namespace spinus
