#include "SongPreparation.h"

#include "core/ChildProcess.h"
#include "dsp/sampler/SampleStorage.h"
#include "project/AudioImport.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

namespace spinus
{

namespace
{
/** Decodes a song Spinus cannot read itself with the system's ffmpeg (not linked or shipped). Fails when
    ffmpeg is missing or cannot read it either. */
juce::Result decodeWithFfmpeg (const juce::File& source, const juce::File& wav, const std::function<bool()>& shouldStop)
{
    core::ChildProcess ffmpeg;
    if (! ffmpeg.start (juce::StringArray { "ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i",
                                            source.getFullPathName(), "-vn", "-ac", "2", "-c:a", "pcm_f32le", "-f",
                                            "wav", wav.getFullPathName() },
                        core::ChildProcess::wantStdErr))
        return juce::Result::fail ("Spinus cannot read " + source.getFileName() + " by itself. " +
                                   project::installFfmpeg + " to read it.");
    while (! ffmpeg.waitForProcessToFinish (200))
        if (shouldStop())
        {
            ffmpeg.kill();
            return juce::Result::fail ("Stopped.");
        }
    if (ffmpeg.getExitCode() != 0 || ! wav.existsAsFile())
        return juce::Result::fail ("Spinus cannot read " + source.getFileName() + ": " +
                                   ffmpeg.readAllProcessOutput().trim().upToFirstOccurrenceOf ("\n", false, false));
    return juce::Result::ok();
}
} // namespace

bool isSongFile (const juce::File& file)
{
    return file.hasFileExtension (project::ownAudioFormats) || file.hasFileExtension (project::ffmpegAudioFormats);
}

juce::String songFilePatterns()
{
    return "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.opus;*.m4a;*.aac;*.mp4;*.webm;*.mka;*.wma;*.alac;*.ape;*.wv";
}

juce::String contentDigest (const juce::File& file)
{
    juce::FileInputStream in (file);
    return juce::SHA256 (in).toHexString();
}

juce::File preparedSongFile (const juce::File& folder, const juce::File& song, const juce::String& digest)
{
    return folder.getChildFile (song.getFileNameWithoutExtension().retainCharacters (
                                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_ ") +
                                "-" + digest.substring (0, 16) + ".wav");
}

juce::Result prepareSong (const juce::File& source, const juce::File& destination,
                          const std::function<bool()>& shouldStop, std::atomic<double>& progress)
{
    if (destination.existsAsFile())
        return juce::Result::ok(); // decoded before (the name carries the song's contents)
    destination.getParentDirectory().createDirectory();
    const auto partial = destination.withFileExtension ("partial");
    partial.deleteFile();
    std::unique_ptr<juce::AudioFormatReader> reader (dsp::getAudioFormats().createReaderFor (source));
    if (reader == nullptr && source.hasFileExtension (project::ffmpegAudioFormats))
    {
        auto decoded = decodeWithFfmpeg (source, partial, shouldStop);
        if (decoded.wasOk() && ! partial.moveFileTo (destination))
            decoded = juce::Result::fail ("Spinus cannot write " + destination.getFullPathName());
        partial.deleteFile();
        progress = 1.0;
        return decoded;
    }
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return juce::Result::fail ("Spinus cannot read " + source.getFileName() +
                                   ". It reads WAV, AIFF, FLAC, Ogg and MP3 files, and Opus, M4A and others "
                                   "when ffmpeg is installed.");
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (partial);
    auto writer = wav.createWriterFor (
        stream, juce::AudioFormatWriterOptions()
                    .withSampleRate (reader->sampleRate)
                    .withNumChannels (2)
                    .withBitsPerSample (32)
                    .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (writer == nullptr)
        return juce::Result::fail ("Spinus cannot write " + partial.getFullPathName());
    juce::AudioBuffer<float> buffer (2, 65536);
    for (juce::int64 at = 0; at < reader->lengthInSamples; at += buffer.getNumSamples())
    {
        if (shouldStop())
            return juce::Result::fail ("Stopped.");
        const auto count = (int)std::min<juce::int64> (buffer.getNumSamples(), reader->lengthInSamples - at);
        reader->read (&buffer, 0, count, at, true, true);
        if (reader->numChannels == 1)
            buffer.copyFrom (1, 0, buffer, 0, 0, count);
        writer->writeFromAudioSampleBuffer (buffer, 0, count);
        progress = (double)(at + count) / (double)reader->lengthInSamples;
    }
    writer = nullptr;
    if (! partial.moveFileTo (destination))
        return juce::Result::fail ("Spinus cannot write " + destination.getFullPathName());
    return juce::Result::ok();
}

} // namespace spinus
