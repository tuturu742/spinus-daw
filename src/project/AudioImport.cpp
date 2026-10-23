#include "AudioImport.h"
#include "core/ChildProcess.h"

#include "ProjectFiles.h"
#include "dsp/sampler/SampleStorage.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace spinus::project
{

namespace
{
constexpr const char* engineFormats = "wav;aif;aiff;flac";

/** With `tolerant`, a file that stops decoding part way (some MP3s do) keeps what was read. */
juce::Result decodeWithSpinus (const juce::File& source, const juce::File& flac, bool tolerant)
{
    std::unique_ptr<juce::AudioFormatReader> reader (dsp::getAudioFormats().createReaderFor (source));
    if (reader == nullptr || reader->sampleRate <= 0.0)
        return juce::Result::fail ("Spinus cannot read " + source.getFileName());

    juce::FlacAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (flac);
    auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                      .withSampleRate (reader->sampleRate)
                                                      .withNumChannels ((int)juce::jmin (2u, reader->numChannels))
                                                      .withBitsPerSample (24));
    if (writer == nullptr)
        return juce::Result::fail ("cannot write " + flac.getFullPathName());
    constexpr int block = 65536;
    juce::AudioBuffer<float> buffer ((int)juce::jmin (2u, reader->numChannels), block);
    for (juce::int64 at = 0; at < reader->lengthInSamples; at += block)
    {
        const auto n = (int)juce::jmin<juce::int64> (block, reader->lengthInSamples - at);
        if (! reader->read (&buffer, 0, n, at, true, buffer.getNumChannels() > 1))
        {
            if (tolerant && at > 0)
                break;
            return juce::Result::fail ("Spinus cannot read all of " + source.getFileName());
        }
        if (! writer->writeFromAudioSampleBuffer (buffer, 0, n))
            return juce::Result::fail ("cannot write " + flac.getFullPathName());
    }
    return juce::Result::ok();
}

juce::Result decodeWithFfmpeg (const juce::File& source, const juce::File& flac)
{
    core::ChildProcess ffmpeg;
    if (! ffmpeg.start (juce::StringArray { "ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i",
                                            source.getFullPathName(), "-vn", "-c:a", "flac", "-sample_fmt", "s32", "-f",
                                            "flac", flac.getFullPathName() },
                        core::ChildProcess::wantStdErr))
        return juce::Result::fail ("Spinus cannot read " + source.getFileName() +
                                   " by itself. Install ffmpeg (sudo pacman -S ffmpeg) to use it.");
    const auto output = ffmpeg.readAllProcessOutput();
    ffmpeg.waitForProcessToFinish (-1);
    if (ffmpeg.getExitCode() != 0 || ! flac.existsAsFile())
        return juce::Result::fail ("Spinus cannot read " + source.getFileName() + ": " +
                                   output.trim().upToFirstOccurrenceOf ("\n", false, false));
    return juce::Result::ok();
}
} // namespace

bool isImportableAudio (const juce::File& file)
{
    return file.hasFileExtension (ownAudioFormats) || file.hasFileExtension (ffmpegAudioFormats);
}

juce::File getDecodedAudioFolder()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("spinus-decoded-audio");
}

juce::Result makePlayable (const juce::File& source, const juce::File& folder, juce::File& playable)
{
    if (! source.existsAsFile())
        return juce::Result::fail (source.getFullPathName() + " does not exist");
    if (source.hasFileExtension (engineFormats))
    {
        playable = source;
        return juce::Result::ok();
    }
    if (! folder.createDirectory())
        return juce::Result::fail ("cannot create " + folder.getFullPathName());

    // Named after the source; the same source decoded before (same name, size and time) is reused.
    const auto stamp =
        juce::String (source.getSize()) + "-" + juce::String (source.getLastModificationTime().toMilliseconds());
    const auto marker = folder.getChildFile ("." + source.getFileName() + ".decoded");
    auto target = folder.getChildFile (source.getFileNameWithoutExtension() + ".flac");
    if (target.existsAsFile() && marker.loadFileAsString() == stamp + " " + target.getFileName())
    {
        playable = std::move (target);
        return juce::Result::ok();
    }
    if (target.exists())
        target = target.getNonexistentSibling (true);

    const auto partial = target.getSiblingFile (target.getFileName() + ".part");
    // Spinus's own decoder first; ffmpeg for what it cannot read (all of); and as a last resort, as much
    // as Spinus could read.
    auto result = juce::Result::fail ({});
    if (source.hasFileExtension (ownAudioFormats))
        result = decodeWithSpinus (source, partial, false);
    if (result.failed())
    {
        auto withFfmpeg = decodeWithFfmpeg (source, partial);
        result = withFfmpeg.wasOk() || ! source.hasFileExtension (ownAudioFormats)
                     ? withFfmpeg
                     : decodeWithSpinus (source, partial, true);
    }
    if (result.failed() || ! partial.moveFileTo (target))
    {
        partial.deleteFile();
        return result.failed() ? result : juce::Result::fail ("cannot write " + target.getFullPathName());
    }
    marker.replaceWithText (stamp + " " + target.getFileName());
    playable = std::move (target);
    return juce::Result::ok();
}

juce::Result importAudio (const juce::File& source, const juce::File& projectFolder, juce::String& path)
{
    if (projectFolder == juce::File())
    {
        juce::File playable;
        auto result = makePlayable (source, getDecodedAudioFolder(), playable);
        if (result.wasOk())
            path = playable.getFullPathName();
        return result;
    }

    // Decoded straight into the project's audio/ folder; files that play as they are get copied in.
    const auto audio = projectFolder.getChildFile ("audio");
    juce::File playable;
    if (auto result = makePlayable (source, audio, playable); result.failed())
        return result;
    if (playable.isAChildOf (audio))
    {
        path = playable.getFileName();
        return juce::Result::ok();
    }
    return importAudioFile (projectFolder, playable, path);
}

} // namespace spinus::project
