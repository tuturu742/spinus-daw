#include "Exporter.h"
#include "core/ChildProcess.h"
#include "core/Log.h"
#include "Beats.h"

#include "EngineAdapter.h"
#include "StemCapturePlugin.h"
#include "model/Freeze.h"
#include "model/StockDevices.h"

#include <set>
#include <thread>

namespace spinus::engine
{

namespace te = tracktion;
using namespace spinus::model;

namespace
{
int bitDepthFor (ExportSettings::Format f)
{
    switch (f)
    {
    case ExportSettings::Format::wav16:
        return 16;
    case ExportSettings::Format::wav32Float:
        return 32;
    case ExportSettings::Format::wav24:
    case ExportSettings::Format::mp3: // the others are rendered to a 24-bit WAV first
    case ExportSettings::Format::flac:
    case ExportSettings::Format::opus:
    case ExportSettings::Format::aac:
        return 24;
    }
    return 24;
}

bool isEncoded (ExportSettings::Format f)
{
    using F = ExportSettings::Format;
    return f != F::wav16 && f != F::wav24 && f != F::wav32Float;
}

/** FLAC and Opus: read the WAV, write the format, with the song's title and author. */
juce::String encodeWithJuce (juce::AudioFormat& format, const juce::File& wav, const juce::File& target, int bits,
                             int qualityIndex, const std::unordered_map<juce::String, juce::String>& metadata)
{
    std::unique_ptr<juce::AudioFormatReader> reader (
        juce::WavAudioFormat().createReaderFor (new juce::FileInputStream (wav), true));
    if (reader == nullptr)
        return "Could not read the rendered audio.";
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (target);
    auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                      .withSampleRate (reader->sampleRate)
                                                      .withNumChannels ((int)reader->numChannels)
                                                      .withBitsPerSample (bits)
                                                      .withQualityOptionIndex (qualityIndex)
                                                      .withMetadataValues (metadata));
    if (writer == nullptr)
        return "Could not write " + target.getFileName() + ".";
    if (! writer->writeFromAudioReader (*reader, 0, reader->lengthInSamples))
        return "Could not encode " + target.getFileName() + ".";
    return {};
}

/** The project as the stems play it: as the song, without solos (a stem is the insert's sound in the whole song). */
Project withoutSolos (Project p)
{
    for (auto& channel : p.channels)
        channel.solo = false;
    for (auto& insert : p.mixer.inserts)
        insert.solo = false;
    p.mixer.master.solo = false;
    return p;
}

bool hasSolo (const Project& p)
{
    return std::any_of (p.channels.begin(), p.channels.end(), [] (auto& c) { return c.solo; }) ||
           std::any_of (p.mixer.inserts.begin(), p.mixer.inserts.end(), [] (auto& i) { return i.solo; });
}
} // namespace

//==============================================================================
ExportJob::ExportJob (te::Engine& e, DeviceFactory& d, const Project& project, std::map<DeviceId, juce::MemoryBlock> s,
                      const juce::File& destination, ExportSettings options,
                      std::function<void (const Result&)> callback)
    : engine (e), devices (d), states (std::move (s)), settings (std::move (options)), onFinished (std::move (callback))
{
    const auto extension = ExportSettings::extensionFor (settings.format);
    const auto mix = destination.withFileExtension (extension);
    steps.push_back ({ project, mix, {}, settings.unheardInserts, {} });

    if (settings.stems)
    {
        const auto folder = mix.getSiblingFile (mix.getFileNameWithoutExtension() + " stems");
        std::vector<Stem> stems;
        for (auto insert : getStemInserts (project))
        {
            const auto& inserts = project.mixer.inserts;
            const auto index = std::find_if (inserts.begin(), inserts.end(), [&] (auto& i) { return i.id == insert; });
            const auto number = juce::String ((int)(index - inserts.begin()) + 1).paddedLeft ('0', 2);
            const auto name = juce::File::createLegalFileName (number + " " + index->name);
            stems.push_back ({ insert, folder.getChildFile (name + extension), {}, nullptr });
        }

        // One render for the mix and the stems; a second for the stems when solos change what the mix plays.
        if (hasSolo (project))
            steps.push_back ({ withoutSolos (project), {}, {}, {}, std::move (stems) });
        else
            steps.front().stems = std::move (stems);
    }

    for (auto& step : steps)
    {
        if (step.file != juce::File())
            plannedFiles.push_back (step.file);
        for (auto& stem : step.stems)
            plannedFiles.push_back (stem.file);
    }
}

ExportJob::~ExportJob()
{
    *alive = false;
    handle.reset(); // cancels and waits for the render thread
    adapter.reset();
    edit.reset();
    for (auto& step : steps)
        deleteScratchFiles (step);
}

void ExportJob::start()
{
    jassert (! running);
    startedMs = juce::Time::getMillisecondCounterHiRes();
    noteStart (settings.stems ? "Export with stems" : "Export",
               plannedFiles.empty() ? juce::File() : plannedFiles.front(), settings);
    running = true;
    cancelRequested = false;
    current = 0;
    renderStep();
}

void ExportJob::cancel()
{
    cancelRequested = true;
    if (handle != nullptr)
        handle->cancel();
}

float ExportJob::getProgress() const
{
    if (steps.empty())
        return 1.0f;
    const auto withinStep = handle != nullptr ? handle->getProgress() : 0.0f;
    return ((float)current + withinStep) / (float)steps.size();
}

//==============================================================================
void ExportJob::renderStep()
{
    auto& step = steps[current];

    edit = te::Edit::createSingleTrackEdit (engine);
    adapter = std::make_unique<EngineAdapter> (*edit, devices);
    adapter->setSavedDeviceStates (states);
    adapter->setAudioFolder (settings.audioFolder);
    adapter->setUnheardInserts (step.unheard);
    adapter->setTunedAudio (settings.tunedAudio);
    if (settings.pattern)
        adapter->setPlayMode (EngineAdapter::PlayMode::pattern, *settings.pattern);
    adapter->sync (step.project);

    if (settings.failOnMissingDevices && ! adapter->getMissingDevices().empty())
    {
        juce::StringArray names;
        for (auto id : adapter->getMissingDevices())
            if (const auto* device = step.project.findDevice (id))
                names.addIfNotAlreadyThere (model::getDeviceDisplayName (*device));
        return finish ({ false, false, "Missing plugin: " + names.joinIntoString (", "), {} });
    }

    waitForClipRenders (0);
}

void ExportJob::waitForClipRenders (int attempt)
{
    // Reversed audio clips play from a copy Tracktion writes in the background (its RenderManager): wait for
    // those, up to a minute, before rendering.
    bool waiting = engine.getRenderManager().getNumJobs() > 0;
    for (auto* track : te::getAudioTracks (*edit))
        for (auto* clip : track->getClips())
            if (auto* wave = dynamic_cast<te::WaveAudioClip*> (clip))
                waiting = waiting || (wave->needsRender() && ! wave->getAudioFile().getFile().existsAsFile());
    if (waiting && attempt < 1200)
    {
        juce::Timer::callAfterDelay (50,
                                     [this, isAlive = alive, attempt]
                                     {
                                         if (*isAlive && ! cancelRequested)
                                             waitForClipRenders (attempt + 1);
                                         else if (*isAlive)
                                             stepFinished ({});
                                     });
        return;
    }
    startRender();
}

void ExportJob::startRender()
{
    auto& step = steps[current];

    te::TimeRange range;
    if (settings.pattern)
    {
        range = adapter->getPatternLoopRange();
        if (range.isEmpty())
            return finish ({ false, false, "That pattern does not exist or is empty.", {} });
    }
    else
    {
        const auto end = getSongEnd (step.project);
        if (end <= 0)
            return finish ({ false,
                             false,
                             "The playlist is empty: place a pattern or an audio clip in it, or export a pattern.",
                             {} });
        range = { te::TimePosition(), ticksToTime (*edit, (double)end) };
    }
    range = range.withEnd (range.getEnd() + te::TimeDuration::fromSeconds (settings.tailSeconds));

    if (step.file != juce::File())
    {
        if (! step.file.getParentDirectory().createDirectory())
            return finish (
                { false, false, "Cannot create the folder " + step.file.getParentDirectory().getFullPathName(), {} });
        if (step.file.existsAsFile())
            step.file.deleteFile();
    }

    // What the render writes: the mix, a WAV to encode the mix from, or (a render only for the stems) a scratch
    // file thrown away.
    const bool encoded = isEncoded (settings.format);
    step.renderTo = step.file == juce::File() || encoded ? juce::File::createTempFile (".wav") : step.file;

    if (const auto error = prepareStems (step, range); error.isNotEmpty())
        return finish ({ false, false, error, {} });

    static juce::WavAudioFormat wav;
    te::Renderer::Parameters params (*edit);
    params.destFile = step.renderTo;
    params.tracksToDo = te::toBitSet (te::getAllTracks (*edit));
    params.audioFormat = &wav;
    params.bitDepth = bitDepthFor (settings.format);
    params.ditheringEnabled = settings.format == ExportSettings::Format::wav16;
    params.sampleRateForAudio = settings.sampleRate;
    params.blockSizeForAudio = 512;
    params.time = range;
    params.usePlugins = true;
    params.useMasterPlugins = true;
    params.canRenderInMono = false;
    params.checkNodesForAudio = false; // a silent stem is still a file

    // The callback runs on the render thread once the render is over: close and encode the files there, then
    // continue on the message thread.
    handle = te::EditRenderer::render (params,
                                       [this, isAlive = alive, &step] (tl::expected<juce::File, std::string> result)
                                       {
                                           juce::String error;
                                           if (! result)
                                               error = result.error();
                                           else if (*isAlive)
                                               error = finishFiles (step);

                                           juce::MessageManager::callAsync (
                                               [this, isAlive, error]
                                               {
                                                   if (*isAlive)
                                                       stepFinished (error);
                                               });
                                       });
}

juce::String ExportJob::prepareStems (Step& step, te::TimeRange range)
{
    if (step.stems.empty())
        return {};

    const auto silent = getSilentStems (step.project);
    const bool encoded = isEncoded (settings.format);
    const auto bits = bitDepthFor (settings.format);
    using SampleFormat = juce::AudioFormatWriterOptions::SampleFormat;

    for (auto& stem : step.stems)
    {
        auto* track = adapter->getInsertTrack (stem.insert);
        if (track == nullptr)
            return "The insert of " + stem.file.getFileName() + " is missing.";

        const auto folder = stem.file.getParentDirectory();
        if (! folder.createDirectory())
            return "Cannot create the folder " + folder.getFullPathName();
        if (stem.file.existsAsFile())
            stem.file.deleteFile();
        // An encoded stem is captured to a WAV beside it first (not in the temporary folder, often in memory:
        // there can be many).
        stem.wav = encoded ? folder.getNonexistentChildFile ("." + stem.file.getFileNameWithoutExtension(), ".wav.part")
                           : stem.file;

        auto capture = std::make_shared<StemCapture>();
        auto file = std::make_unique<juce::FileOutputStream> (stem.wav);
        if (! file->openedOk())
            return "Cannot write " + stem.wav.getFullPathName();
        std::unique_ptr<juce::OutputStream> stream = std::move (file);
        capture->writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions()
                        .withSampleRate (settings.sampleRate)
                        .withNumChannels (2)
                        .withBitsPerSample (bits)
                        .withSampleFormat (bits == 32 ? SampleFormat::floatingPoint : SampleFormat::integral));
        if (capture->writer == nullptr)
            return "Cannot write " + stem.wav.getFullPathName();
        capture->start = range.getStart();
        capture->lengthSamples = te::toSamples (range.getLength(), settings.sampleRate);
        capture->sampleRate = settings.sampleRate;
        capture->dither = settings.format == ExportSettings::Format::wav16;
        capture->silent = silent.contains (stem.insert);

        // Last on the insert: after its effects, fader and sends, so it hears what the insert puts out.
        auto plugin = edit->getPluginCache().createNewPlugin (StemCapturePlugin::xmlTypeName, {});
        auto* capturer = dynamic_cast<StemCapturePlugin*> (plugin.get());
        if (capturer == nullptr)
            return "Cannot capture the stems.";
        capturer->setCapture (capture);
        track->pluginList.insertPlugin (plugin, -1, nullptr);
        stem.capture = std::move (capture);
    }
    return {};
}

juce::String ExportJob::finishFiles (Step& step)
{
    // The render is over (its graph is gone): nothing writes to the stems any more.
    juce::String error;
    for (auto& stem : step.stems)
        if (stem.capture != nullptr && ! stem.capture->finish() && error.isEmpty())
            error = "Could not write " + stem.file.getFileName() + ".";
    if (error.isNotEmpty() || ! isEncoded (settings.format))
        return error;

    // Encode the mix and the stems, several at once.
    std::vector<std::pair<juce::File, juce::File>> jobs;
    if (step.file != juce::File())
        jobs.emplace_back (step.renderTo, step.file);
    for (auto& stem : step.stems)
        jobs.emplace_back (stem.wav, stem.file);

    std::vector<juce::String> errors (jobs.size());
    std::atomic<size_t> next { 0 };
    auto work = [&]
    {
        for (auto i = next++; i < jobs.size() && *alive; i = next++)
            errors[i] = encodeFile (jobs[i].first, jobs[i].second, settings, steps.front().project.settings);
    };
    std::vector<std::thread> workers;
    const auto threads = (size_t)juce::jlimit (1, 8, juce::SystemStats::getNumCpus() - 1);
    for (size_t i = 1; i < juce::jmin (threads, jobs.size()); ++i)
        workers.emplace_back (work);
    work();
    for (auto& w : workers)
        w.join();

    for (auto& e : errors)
        if (e.isNotEmpty())
            return e;
    return {};
}

void ExportJob::deleteScratchFiles (Step& step)
{
    for (auto& stem : step.stems)
    {
        stem.capture = nullptr; // closes its file, if the render did not
        if (stem.wav != juce::File() && stem.wav != stem.file)
            stem.wav.deleteFile();
    }
    if (step.renderTo != juce::File() && step.renderTo != step.file)
        step.renderTo.deleteFile();
}

void ExportJob::stepFinished (const juce::String& error)
{
    handle.reset();
    adapter.reset();
    edit.reset();
    deleteScratchFiles (steps[current]);

    if (cancelRequested)
        return finish ({ false, true, "Export cancelled.", {} });
    if (error.isNotEmpty())
        return finish ({ false, false, error, {} });

    if (++current < steps.size())
        return renderStep();

    finish ({ true, false, {}, plannedFiles });
}

void ExportJob::finish (const Result& result)
{
    running = false;
    handle.reset();
    adapter.reset();
    edit.reset();

    // A failed or cancelled export leaves no half-written files behind.
    if (! result.ok)
        for (size_t i = 0; i <= current && i < steps.size(); ++i)
        {
            deleteScratchFiles (steps[i]);
            steps[i].file.deleteFile();
            for (auto& stem : steps[i].stems)
                stem.file.deleteFile();
        }

    noteEnd ("Export", plannedFiles.empty() ? juce::File() : plannedFiles.front(), result, startedMs);
    if (onFinished)
        onFinished (result);
}

void ExportJob::noteStart (const juce::String& what, const juce::File& file, const ExportSettings& s)
{
    core::Logs::info (
        "export", what + ": " + file.getFullPathName() + " (" + ExportSettings::extensionFor (s.format).substring (1) +
                      (s.format == ExportSettings::Format::wav16        ? " 16-bit"
                       : s.format == ExportSettings::Format::wav24      ? " 24-bit"
                       : s.format == ExportSettings::Format::wav32Float ? " 32-bit float"
                       : s.format == ExportSettings::Format::mp3        ? " " + juce::String (s.mp3Kbps) + " kbit/s"
                                                                        : juce::String()) +
                      ", " + juce::String (s.sampleRate / 1000.0, 1) + " kHz" + (s.pattern ? ", one pattern" : "") +
                      ")");
}

void ExportJob::noteEnd (const juce::String& what, const juce::File& file, const Result& r, double startedMs)
{
    const auto took = juce::String ((juce::Time::getMillisecondCounterHiRes() - startedMs) / 1000.0, 1) + " s";
    if (r.ok)
        core::Logs::info ("export", what + " done: " + juce::String ((int)r.files.size()) +
                                        (r.files.size() == 1 ? " file" : " files") + " in " + took);
    else if (r.cancelled)
        core::Logs::info ("export", what + " stopped after " + took + ": " + r.error);
    else
        core::Logs::warning ("export",
                             what + " failed after " + took + " (" + file.getFullPathName() + "): " + r.error);
}

namespace
{
juce::String encodeAac (const juce::File& wavFile, const juce::File& m4aFile, const ProjectSettings& song)
{
    juce::StringArray args { "ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", wavFile.getFullPathName(),
                             "-c:a",   "aac",      "-b:a",      "256k" };
    if (song.title.isNotEmpty())
        args.addArray ({ "-metadata", "title=" + song.title });
    if (song.author.isNotEmpty())
        args.addArray ({ "-metadata", "artist=" + song.author });
    args.add (m4aFile.getFullPathName());

    core::ChildProcess ffmpeg;
    if (! ffmpeg.start (args, core::ChildProcess::wantStdErr))
        return "Could not run the AAC encoder: install the 'ffmpeg' package.";

    const auto output = ffmpeg.readAllProcessOutput();
    if (ffmpeg.getExitCode() != 0 || ! m4aFile.existsAsFile())
        return "The AAC encoder failed: " + output.trim();
    return {};
}

juce::String encodeMp3 (const juce::File& wavFile, const juce::File& mp3File, int kbps, const ProjectSettings& song)
{
    juce::StringArray args { "lame", "--quiet", "-b", juce::String (kbps), "--cbr", "-q", "2" };
    if (song.title.isNotEmpty())
        args.addArray ({ "--tt", song.title });
    if (song.author.isNotEmpty())
        args.addArray ({ "--ta", song.author });
    args.addArray ({ wavFile.getFullPathName(), mp3File.getFullPathName() });

    core::ChildProcess lame;
    if (! lame.start (args, core::ChildProcess::wantStdErr))
        return "Could not run the MP3 encoder: install the 'lame' package.";

    const auto output = lame.readAllProcessOutput();
    if (lame.getExitCode() != 0 || ! mp3File.existsAsFile())
        return "The MP3 encoder failed: " + output.trim();
    return {};
}
} // namespace

juce::String ExportJob::encodeFile (const juce::File& wav, const juce::File& target, const ExportSettings& settings,
                                    const ProjectSettings& song)
{
    std::unordered_map<juce::String, juce::String> metadata;
    using F = ExportSettings::Format;
    switch (settings.format)
    {
    case F::mp3:
        return encodeMp3 (wav, target, settings.mp3Kbps, song);
    case F::aac:
        return encodeAac (wav, target, song);
    case F::flac:
    {
        juce::FlacAudioFormat flac;
        return encodeWithJuce (flac, wav, target, 24, 0, metadata);
    }
    case F::opus:
    {
        // The title and author, as the MP3 and AAC exports have them.
        metadata[juce::OpusAudioFormat::id3title] = song.title;
        metadata[juce::OpusAudioFormat::id3artist] = song.author;
        juce::OpusAudioFormat opus;
        const auto quality = opus.getQualityOptions().indexOf ("160 kbps");
        return encodeWithJuce (opus, wav, target, 32, juce::jmax (0, quality), metadata);
    }
    case F::wav16:
    case F::wav24:
    case F::wav32Float:
        break;
    }
    return {};
}

bool ExportJob::isEncodedFormat (ExportSettings::Format f) { return isEncoded (f); }

//==============================================================================
std::vector<InsertId> ExportJob::getStemInserts (const Project& p)
{
    // Those with a channel playing into them, through outputs or sends, or into what feeds their sidechains.
    std::vector<InsertId> result;
    for (auto& insert : p.mixer.inserts)
    {
        auto feeds = insertsReaching (p, { insert.id });
        const auto keyers = sidechainFeedsOf (p, feeds);
        feeds.insert (keyers.begin(), keyers.end());
        if (std::any_of (p.channels.begin(), p.channels.end(),
                         [&] (auto& c) { return ! c.muted && feeds.contains (c.targetInsert); }))
            result.push_back (insert.id);
    }
    return result;
}

std::set<InsertId> ExportJob::getSilentStems (const Project& p)
{
    std::set<InsertId> silent;
    for (auto& insert : p.mixer.inserts)
    {
        // Follow where it plays into, as Tracktion does ("muted because the destination is muted").
        std::set<InsertId> seen;
        for (const auto* i = &insert; i != nullptr && seen.insert (i->id).second;
             i = i->output == masterInsertId ? nullptr : p.findInsert (i->output))
            if (i->muted)
            {
                silent.insert (insert.id);
                break;
            }
    }
    return silent;
}

Ticks ExportJob::getSongEnd (const Project& p)
{
    Ticks end = 0;
    for (auto& clip : p.playlist.clips)
        // Clips that make sound; automation alone does not lengthen the song.
        if (! clip.muted && ! std::holds_alternative<AutomationClip> (clip.content))
            end = std::max (end, clip.start + clip.length);
    return end;
}

juce::String ExportSettings::extensionFor (Format f)
{
    switch (f)
    {
    case Format::mp3:
        return ".mp3";
    case Format::flac:
        return ".flac";
    case Format::opus:
        return ".opus";
    case Format::aac:
        return ".m4a";
    case Format::wav16:
    case Format::wav24:
    case Format::wav32Float:
        break;
    }
    return ".wav";
}

bool ExportJob::isAacAvailable()
{
    core::ChildProcess ffmpeg;
    return ffmpeg.start (juce::StringArray { "ffmpeg", "-hide_banner", "-encoders" }) &&
           ffmpeg.readAllProcessOutput().contains (" aac ") && ffmpeg.getExitCode() == 0;
}

bool ExportJob::isMp3Available()
{
    core::ChildProcess lame;
    return lame.start (juce::StringArray { "lame", "--version" }, 0) && lame.waitForProcessToFinish (5000) &&
           lame.getExitCode() == 0;
}

} // namespace spinus::engine
