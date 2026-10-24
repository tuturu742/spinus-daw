#pragma once

#include "engine/Exporter.h"
#include "engine/OutputCapturePlugin.h"
#include "session/Session.h"

namespace spinus
{

/** Exports the song while it plays, so it can be heard (real time): the song plays once
    from the start through the sound card, and what the Master puts out (after its effects and fader) is
    written to the file, with the tail after the end. Changes made while it plays are in the file. The
    file has the sound card's sample rate; no stems.

    The sound is taken by an OutputCapturePlugin placed last on the Master while the export runs, and
    written by a thread of its own; other formats are encoded from that WAV as ExportJob does.

    Create, start() and destroy on the message thread; onFinished is called there. Destroying it cancels. */
class LiveExport final : private juce::Timer, private juce::Thread
{
public:
    using Result = engine::ExportJob::Result;

    LiveExport (Session&, const juce::File& destination, engine::ExportSettings,
                std::function<void (const Result&)> onFinished);
    ~LiveExport() override;

    void start();
    /** Stops playback and the export; no file is left behind. */
    void cancel();

    bool isRunning() const noexcept { return running; }
    double getProgress() const noexcept;
    /** The file it writes (the destination with the format's extension). */
    const juce::File& getFile() const noexcept { return file; }

private:
    void timerCallback() override;
    void run() override;
    void finish (const Result&);
    double startedMs = 0.0;
    void restorePlayback();

    Session& session;
    juce::File file, wavFile;
    engine::ExportSettings settings;
    std::function<void (const Result&)> onFinished;

    std::shared_ptr<engine::OutputCapture> capture;
    tracktion::Plugin::Ptr plugin;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    juce::int64 songSamples = 0;
    std::atomic<juce::int64> written { 0 };
    std::atomic<bool> writerDone { false };
    juce::String writerError; // set by the thread before writerDone
    bool running = false, stoppedTransport = false;
    double startedAt = 0.0;

    // Playback as it was, to put back afterwards.
    Session::Mode previousMode = Session::Mode::song;
    std::optional<Session::LoopRegion> songLoop;
};

} // namespace spinus
