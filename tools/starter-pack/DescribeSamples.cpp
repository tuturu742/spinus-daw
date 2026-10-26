// spinus_describe_samples <file or folder>...
//
// Prints one JSON line per audio file: what the sound index measures (session/SoundDescriptors'
// describeSample) and the checks the starter pack's files must pass (tools/starter-pack/make_pack.py):
// the peak, the silence before the sound starts, the length, the channels and how different they are.

#include "dsp/sampler/SampleStorage.h"
#include "session/SoundDescriptors.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <iostream>

namespace
{
juce::var measure (const juce::File& file)
{
    auto* object = new juce::DynamicObject();
    juce::var result (object);
    object->setProperty ("file", file.getFullPathName());

    std::unique_ptr<juce::AudioFormatReader> reader (spinus::dsp::getAudioFormats().createReaderFor (file));
    if (reader == nullptr)
    {
        object->setProperty ("problem", "unreadable");
        return result;
    }
    const auto channels = (int)reader->numChannels;
    const auto length = (int)juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64)(reader->sampleRate * 600));
    juce::AudioBuffer<float> audio (juce::jmax (1, channels), length);
    reader->read (&audio, 0, length, 0, true, true);

    float peak = 0.0f;
    int firstLoud = -1;
    const float threshold = std::pow (10.0f, -60.0f / 20.0f);
    double sideSquares = 0, midSquares = 0;
    for (int i = 0; i < length; ++i)
    {
        float m = 0.0f;
        for (int c = 0; c < channels; ++c)
            m = juce::jmax (m, std::abs (audio.getSample (c, i)));
        peak = juce::jmax (peak, m);
        if (firstLoud < 0 && m > threshold)
            firstLoud = i;
        if (channels >= 2)
        {
            const double l = audio.getSample (0, i), r = audio.getSample (1, i);
            sideSquares += (l - r) * (l - r);
            midSquares += (l + r) * (l + r);
        }
    }

    juce::AudioBuffer<float> mono (1, length);
    mono.clear();
    for (int c = 0; c < channels; ++c)
        mono.addFrom (0, 0, audio, c, 0, length, 1.0f / (float)channels);

    const auto sound = spinus::sounds::describeSample (mono, reader->sampleRate, file);
    object->setProperty ("sound", spinus::sounds::toVar (sound));
    object->setProperty ("rate", reader->sampleRate);
    object->setProperty ("bits", (int)reader->bitsPerSample);
    object->setProperty ("float", reader->usesFloatingPointData);
    object->setProperty ("channels", channels);
    object->setProperty ("seconds", length / reader->sampleRate);
    object->setProperty ("peakDb", juce::Decibels::gainToDecibels (peak, -200.0f));
    object->setProperty ("leadingSilence", firstLoud < 0 ? -1.0 : firstLoud / reader->sampleRate);
    // How far the channels are apart: the side's level under the middle's (-100: the same).
    object->setProperty ("sideDb", channels < 2 || midSquares <= 0
                                       ? -100.0
                                       : 10.0 * std::log10 (juce::jmax (1e-10, sideSquares / midSquares)));
    return result;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce; // the audio formats' MP3 reader wants a message manager
    for (int a = 1; a < argc; ++a)
    {
        const juce::File arg (juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[a])));
        juce::Array<juce::File> files;
        if (arg.isDirectory())
            for (const auto& e : juce::RangedDirectoryIterator (arg, true, "*", juce::File::findFiles))
            {
                if (e.getFile().hasFileExtension ("wav;aif;aiff;flac;ogg;mp3"))
                    files.add (e.getFile());
            }
        else
            files.add (arg);
        files.sort();
        for (const auto& f : files)
            std::cout << juce::JSON::toString (measure (f),
                                               juce::JSON::FormatOptions {}.withSpacing (juce::JSON::Spacing::none))
                      << "\n"
                      << std::flush;
    }
    return 0;
}
