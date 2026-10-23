#pragma once

#include <juce_core/juce_core.h>

namespace spinus::project
{

/*  Audio files from outside, dropped on the Playlist, the Launcher or the Channel Rack.

    The engine plays WAV, AIFF and FLAC as they are. Everything else (MP3, Ogg, Opus, M4A, ...) is decoded
    once into a FLAC file, which the engine plays without trouble: it cannot play Opus or M4A at all, and
    some MP3s (with large cover pictures) came out silent. Spinus reads MP3, Ogg and Opus itself; the
    other formats need the system's ffmpeg (not linked or shipped). */

/** Extensions Spinus reads itself, and the ones it reads with ffmpeg. */
inline constexpr const char* ownAudioFormats = "wav;aif;aiff;flac;ogg;mp3;opus";
inline constexpr const char* ffmpegAudioFormats = "m4a;aac;mp4;webm;mka;wma;alac;ape;wv";

/** Whether the file looks like audio Spinus can bring in (by its extension). */
bool isImportableAudio (const juce::File&);

/** Brings an audio file into a project and returns the path to store in an audio clip: inside the saved
    project's audio/ folder (relative), or an absolute path while the project is untitled (`projectFolder`
    empty), which the first save copies in. Files the engine cannot play as they are arrive as FLAC. */
juce::Result importAudio (const juce::File& source, const juce::File& projectFolder, juce::String& path);

/** A file with the same sound that the engine and the Sampler can read: the source itself, or a FLAC
    decoded from it into `folder` (reused when it is already there). */
juce::Result makePlayable (const juce::File& source, const juce::File& folder, juce::File& playable);

/** Where decoded files go while a project is untitled. */
juce::File getDecodedAudioFolder();

} // namespace spinus::project
