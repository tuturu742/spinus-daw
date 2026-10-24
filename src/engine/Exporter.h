#pragma once

#include <set>

#include "DeviceFactory.h"
#include "model/Model.h"

#include <tracktion_engine/tracktion_engine.h>

namespace spinus::engine
{

/** What to export (WAV and MP3, full mix and stems). */
struct ExportSettings
{
    enum class Format
    {
        wav16,
        wav24,
        wav32Float,
        mp3,
        flac, // lossless, 24-bit
        opus, // 160 kbit/s
        aac   // in an .m4a file, 256 kbit/s (needs ffmpeg)
    };

    Format format = Format::wav24;
    int mp3Kbps = 320;

    /** The file extension for the format, with the dot (".wav", ".flac", ...). */
    static juce::String extensionFor (Format);
    double sampleRate = 48000.0;

    /** Also write one file per mixer insert that has sound routed into it. */
    bool stems = false;

    /** Export one pattern (what pattern mode plays) instead of the song (the playlist). */
    std::optional<model::PatternId> pattern;

    /** Where audio clips with relative paths are (a saved project's audio/ folder). */
    juce::File audioFolder;

    /** Time after the end for reverbs and releases to ring out. */
    double tailSeconds = 2.0;

    /** Inserts in the mix that play only to feed sidechains (see EngineAdapter::setUnheardInserts). */
    std::set<model::InsertId> unheardInserts;

    /** Fail instead of rendering silence for devices whose plugin is missing here (Freeze). */
    bool failOnMissingDevices = false;

    /** Audio clips that play another file: Spinus Tune's renderings (Session fills this in). */
    std::map<model::ClipId, juce::File> tunedAudio;
};

/** Renders a project to audio files, offline and on a background thread, without touching playback:
    it builds its own Edit from the model with the devices' current settings. WAV is written directly; the
    other formats are rendered to a WAV first and encoded from it: FLAC and Opus by Spinus, MP3 with the `lame`
    program, AAC with `ffmpeg`.

    A stem is one insert's own output, as FL Studio's "split mixer tracks": what the insert puts out in the
    song (after its effects and fader, before whatever it plays into), without the master's effects and fader.
    A send-return insert's stem is its wet sound, fed by the sends; a muted insert's stem is silence, and so is
    an insert's that plays only into a muted one (it is not heard either). The stems add up to the mix before
    the master (inserts on a bus are in their own stem and in the bus's). Solo is ignored for stems.

    The mix and all stems come from one render of the song: a StemCapturePlugin last on each stem's
    insert writes its file while the mix is rendered. When something is soloed, the mix plays the solo and the
    stems do not, so the stems get a render of their own, without the solos.

    Create, start() and destroy on the message thread; onFinished is called there. Destroying the job
    cancels it.
*/
class ExportJob
{
public:
    struct Result
    {
        bool ok = false;
        bool cancelled = false;
        juce::String error;
        std::vector<juce::File> files;
    };

    ExportJob (tracktion::Engine&, DeviceFactory&, const model::Project&,
               std::map<model::DeviceId, juce::MemoryBlock> states, const juce::File& destination, ExportSettings,
               std::function<void (const Result&)> onFinished);
    ~ExportJob();

    void start();
    void cancel();

    /** 0..1 over all files. */
    float getProgress() const;
    bool isRunning() const noexcept { return running; }

    /** The files this job writes, in order: the mix first, then the stems. */
    const std::vector<juce::File>& getPlannedFiles() const noexcept { return plannedFiles; }

    //==============================================================================
    /** The inserts that get a stem: those with at least one channel playing into them. */
    static std::vector<model::InsertId> getStemInserts (const model::Project&);

    /** The inserts whose stem is silence: muted ones, and those that play into a muted one (directly or through
        others), as Tracktion mutes those too. */
    static std::set<model::InsertId> getSilentStems (const model::Project&);

    /** Where the song ends: the end of the last pattern or audio clip (0 when there is none). */
    static model::Ticks getSongEnd (const model::Project&);

    /** Whether MP3 export is possible (the `lame` program is installed). */
    static bool isMp3Available();

    /** Encodes a WAV into the settings' format (FLAC, Opus, MP3 or AAC, with the song's title and author);
        nothing to do for a WAV format. Returns an error, or nothing. Any thread. */
    static juce::String encodeFile (const juce::File& wav, const juce::File& target, const ExportSettings&,
                                    const model::ProjectSettings& song);
    /** Whether the format is encoded from a WAV (not written directly). */
    static bool isEncodedFormat (ExportSettings::Format);
    /** Whether AAC export is possible (the `ffmpeg` program is installed). */
    static bool isAacAvailable();

    /** Notes a render's start, or its end (with `result`), in the session log: `what` ("Export", "Live
        export"), the file, the format, and at the end how it went and how long it took. Message thread. */
    static void noteStart (const juce::String& what, const juce::File&, const ExportSettings&);
    static void noteEnd (const juce::String& what, const juce::File&, const Result&, double startedMs);

private:
    struct Stem
    {
        model::InsertId insert;
        juce::File file;
        juce::File wav; // what the capture writes: the file, or a WAV to encode it from
        std::shared_ptr<struct StemCapture> capture;
    };

    /** One render of the song: the mix (or a render only for its stems), and the stems captured in it. */
    struct Step
    {
        model::Project project;
        juce::File file;     // the mix; empty when this render is only for the stems
        juce::File renderTo; // where the render writes: the file, a WAV to encode it from, or a scratch file
        std::set<model::InsertId> unheard; // playing only to feed sidechains
        std::vector<Stem> stems;
    };

    void renderStep();
    void waitForClipRenders (int attempt);
    void startRender();
    /** Places a StemCapturePlugin on each stem's insert and opens its file; an error, or empty. */
    juce::String prepareStems (Step&, tracktion::TimeRange);
    /** On the render thread, after the render: closes the stems' files and encodes what needs it. */
    juce::String finishFiles (Step&);
    void stepFinished (const juce::String& error);
    void finish (const Result&);
    /** Deletes the step's scratch files (the WAVs encoded from). */
    void deleteScratchFiles (Step&);

    tracktion::Engine& engine;
    DeviceFactory& devices;
    std::map<model::DeviceId, juce::MemoryBlock> states;
    ExportSettings settings;
    std::function<void (const Result&)> onFinished;

    std::vector<Step> steps;
    std::vector<juce::File> plannedFiles;
    size_t current = 0;
    bool running = false, cancelRequested = false;
    double startedMs = 0.0;

    std::unique_ptr<tracktion::Edit> edit;
    std::unique_ptr<class EngineAdapter> adapter;
    std::shared_ptr<tracktion::EditRenderer::Handle> handle;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    JUCE_DECLARE_NON_COPYABLE (ExportJob)
};

} // namespace spinus::engine
