#include "LiveExport.h"

#include "engine/Beats.h"

namespace spinus
{

namespace te = tracktion;

LiveExport::LiveExport (Session& s, const juce::File& destination, engine::ExportSettings exportSettings,
                        std::function<void (const Result&)> finished)
    : juce::Thread ("Spinus live export"), session (s),
      file (destination.withFileExtension (engine::ExportSettings::extensionFor (exportSettings.format))),
      settings (std::move (exportSettings)), onFinished (std::move (finished))
{
}

LiveExport::~LiveExport()
{
    if (running)
        cancel();
    stopThread (5000);
}

double LiveExport::getProgress() const noexcept
{
    if (capture == nullptr)
        return 0.0;
    const auto wanted = (double)juce::jmax ((juce::int64)1, capture->wanted.load());
    return juce::jlimit (0.0, 1.0, (double)written.load() / wanted);
}

void LiveExport::start()
{
    const auto end = engine::ExportJob::getSongEnd (session.getProject());
    if (end <= 0)
        return finish ({ false, false, "The playlist is empty: place a pattern or an audio clip in it first.", {} });

    auto& edit = session.getEdit();
    const auto rate = edit.engine.getDeviceManager().getSampleRate();
    if (rate <= 0.0)
        return finish ({ false, false, "There is no sound card to play through: export without playing instead.", {} });

    if (! file.getParentDirectory().createDirectory())
        return finish ({ false, false, "Cannot create the folder " + file.getParentDirectory().getFullPathName(), {} });

    // The file the thread writes: the target itself, or a WAV to encode from.
    const auto encoded = engine::ExportJob::isEncodedFormat (settings.format);
    wavFile = encoded ? juce::File::createTempFile (".wav") : file;
    file.deleteFile();
    wavFile.deleteFile();
    using F = engine::ExportSettings::Format;
    const auto bits = settings.format == F::wav16 ? 16 : settings.format == F::wav32Float ? 32 : 24;
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (wavFile);
        auto options =
            juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (bits);
        if (bits == 32)
            options = options.withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        juce::WavAudioFormat wav;
        writer = wav.createWriterFor (stream, options);
    }
    if (writer == nullptr)
        return finish ({ false, false, "Cannot write " + wavFile.getFullPathName(), {} });

    // Four seconds of room: the writer thread empties it every few milliseconds.
    capture = std::make_shared<engine::OutputCapture> ((int)(rate * 4.0));
    songSamples = (juce::int64)(engine::ticksToTime (edit, (double)end).inSeconds() * rate);
    capture->wanted.store (songSamples + (juce::int64)(settings.tailSeconds * rate));

    plugin = edit.getPluginCache().createNewPlugin (engine::OutputCapturePlugin::xmlTypeName, {});
    auto* capturer = dynamic_cast<engine::OutputCapturePlugin*> (plugin.get());
    if (capturer == nullptr)
        return finish ({ false, false, "Could not start the export.", {} });
    capturer->setCapture (capture, edit.getMasterVolumePlugin());
    edit.getMasterPluginList().insertPlugin (plugin, -1, nullptr); // last: after the Master's fader

    // The song once, from the start, without a loop.
    previousMode = session.getMode();
    songLoop = session.getLoopRegion (Session::Mode::song);
    session.stop();
    session.setMode (Session::Mode::song);
    session.setLoopRegion (Session::Mode::song, std::nullopt);
    edit.getTransport().setPosition (te::TimePosition());
    edit.dispatchPendingUpdatesSynchronously();

    running = true;
    startedAt = juce::Time::getMillisecondCounterHiRes();
    startThread();
    session.play();
    startTimerHz (20);
}

void LiveExport::cancel()
{
    if (! running)
        return;
    signalThreadShouldExit();
    stopThread (5000);
    finish ({ false, true, "Export cancelled.", {} });
}

void LiveExport::run()
{
    auto& c = *capture;
    const auto wanted = c.wanted.load();
    while (! threadShouldExit() && written.load() < wanted)
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        c.fifo.prepareToRead (c.fifo.getNumReady(), start1, size1, start2, size2);
        if (size1 + size2 == 0)
        {
            juce::Thread::sleep (5);
            continue;
        }
        bool ok = size1 == 0 || writer->writeFromAudioSampleBuffer (c.buffer, start1, size1);
        ok = ok && (size2 == 0 || writer->writeFromAudioSampleBuffer (c.buffer, start2, size2));
        c.fifo.finishedRead (size1 + size2);
        written.fetch_add (size1 + size2);
        if (! ok)
        {
            writerError = "Could not write " + wavFile.getFileName() + ": is the disk full?";
            break;
        }
    }
    writer.reset(); // closes the file

    if (! threadShouldExit() && writerError.isEmpty() && wavFile != file)
        writerError = engine::ExportJob::encodeFile (wavFile, file, settings, session.getProject().settings);
    writerDone.store (true);
}

void LiveExport::timerCallback()
{
    if (! running)
        return;

    // The song is in: stop playing (the tail is still recorded as it rings out).
    if (! stoppedTransport && capture->captured.load() >= songSamples)
    {
        stoppedTransport = true;
        session.stop();
    }
    else if (! stoppedTransport && ! session.isPlaying())
    {
        // Stopped by the user before the end.
        signalThreadShouldExit();
        stopThread (5000);
        return finish ({ false, true, "Export stopped: playback was stopped before the end of the song.", {} });
    }
    else if (! stoppedTransport && capture->captured.load() == 0 &&
             juce::Time::getMillisecondCounterHiRes() - startedAt > 2000.0)
    {
        // Playing, but the capture missed the start (it joined the playback late): play from the start again.
        startedAt = juce::Time::getMillisecondCounterHiRes();
        session.stop();
        session.getEdit().getTransport().setPosition (te::TimePosition());
        session.play();
    }

    if (capture->overflowed.load())
    {
        signalThreadShouldExit();
        stopThread (5000);
        return finish ({ false,
                         false,
                         "The computer could not save the sound as fast as it played. Try again, or export without "
                         "playing.",
                         {} });
    }

    if (writerDone.load())
    {
        stopThread (1000);
        if (writerError.isNotEmpty())
            return finish ({ false, false, writerError, {} });
        finish ({ true, false, {}, { file } });
    }
}

void LiveExport::restorePlayback()
{
    if (session.isPlaying())
        session.stop();
    if (plugin != nullptr)
    {
        plugin->deleteFromParent();
        plugin = nullptr;
    }
    session.setLoopRegion (Session::Mode::song, songLoop);
    session.setMode (previousMode);
}

void LiveExport::finish (const Result& result)
{
    stopTimer();
    const bool wasRunning = running;
    running = false;
    if (wasRunning)
        restorePlayback();
    writer.reset();
    if (wavFile != file)
        wavFile.deleteFile();
    if (! result.ok)
        file.deleteFile();
    if (onFinished)
        onFinished (result);
}

} // namespace spinus
