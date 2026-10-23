#pragma once

#include "engine/Exporter.h"
#include "model/Freeze.h"

namespace spinus
{

class Session;

/** Keeps frozen channels and inserts playing their rendered audio (model/Freeze.h).

    Before every sync of the Engine Adapter, update() works out the audio each frozen target plays: a
    file in the cache folder named by freezeKey() (the project's cache/freeze/, or a temporary folder
    while the project is untitled). When the file is not there yet (just frozen, or what it plays was
    edited), it is rendered in the background, one at a time, with the export renderer. Meanwhile the
    target keeps playing what it played before: its previous audio, or its live plugins when it was
    just frozen. Unfreezing deletes the target's files.

    Pattern mode renders the current pattern on its own; song mode renders the song.

    Message thread only. Sends a change message when a render starts or ends.
*/
class Freezer : public juce::ChangeBroadcaster, private juce::AsyncUpdater
{
public:
    explicit Freezer (Session&);
    ~Freezer() override;

    /** Where the project is saved (invalid while untitled). The audio in use moves along. */
    void setProjectFolder (const juce::File&);

    /** Forgets the open project's audio before another one is loaded (its files stay). */
    void clear();

    /** A different project was loaded: nothing was just frozen or unfrozen. Also removes cache files the
        project no longer uses. */
    void projectLoaded();

    /** Call before every sync of the adapter. */
    void update();

    enum class State
    {
        live,         // not frozen
        rendering,    // frozen, its audio is being rendered
        frozen,       // playing its rendered audio
        failed,       // the render failed (e.g. a plugin is missing): plays live
        cannotFreeze, // frozen, but the routing changed so that it cannot be: plays live
    };

    struct Status
    {
        State state = State::live;
        float progress = 0.0f; // while rendering, 0..1
        juce::String message;  // why it failed or cannot be frozen
    };

    Status getStatus (model::FreezeTarget) const;

    /** True while any target is being rendered or waits to be. */
    bool isRendering() const noexcept { return job != nullptr || ! pending.empty(); }

    /** Renders finished since this Freezer was created. */
    int getRenderCount() const noexcept { return renderCount; }

    juce::File getCacheFolder() const;

private:
    struct Slot
    {
        model::FreezeTarget target;
        std::optional<model::PatternId> pattern; // pattern mode's pattern, or the song

        auto operator<=> (const Slot&) const = default;
    };

    struct Render
    {
        juce::String key;
        model::FreezePlan plan;
    };

    void handleAsyncUpdate() override;
    void startNextRender();
    void renderDone (const engine::ExportJob::Result&);
    void forget (const model::FreezeTarget&);
    void play (const Slot&, const juce::File&);
    void deleteIfOurs (const juce::File&);
    /** Deletes the audio files that could not be deleted earlier (Windows keeps a file the engine still
        reads; Linux deletes it at once). */
    void deleteLeftOvers();
    std::set<juce::File> leftOvers;
    double getSampleRate() const;
    std::set<model::FreezeTarget> getFrozenFlags() const;

    Session& session;
    juce::File projectFolder, tempFolder;

    std::map<Slot, juce::File> playing; // the audio each slot plays (current, or stale while re-rendering)
    std::map<Slot, Render> pending;
    std::map<Slot, std::pair<juce::String, juce::String>> failures; // key that failed, and why
    std::set<model::FreezeTarget> lastFrozen;

    std::unique_ptr<engine::ExportJob> job;
    Slot jobSlot;
    juce::String jobKey;
    std::optional<engine::ExportJob::Result> jobResult;
    int renderCount = 0;

    JUCE_DECLARE_NON_COPYABLE (Freezer)
};

} // namespace spinus
