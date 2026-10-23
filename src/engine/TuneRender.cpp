#include "TuneRender.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <rubberband/RubberBandStretcher.h>

namespace spinus::engine
{

namespace
{
constexpr double slowWindowSeconds = 0.2; // averaging over a vibrato cycle or so
constexpr double edgeSeconds = 0.015;
constexpr int blockSize = 256;

float midiOf (float hz) { return 69.0f + 12.0f * std::log2 (hz / 440.0f); }
} // namespace

PitchTrack PitchTrack::fromJson (const juce::var& track)
{
    PitchTrack result;
    const auto* times = track["times"].getArray();
    if (times != nullptr && times->size() > 1)
        result.frameSeconds = (double)times->getReference (1) - (double)times->getReference (0);
    if (auto* hz = track["hz"].getArray())
        for (auto& v : *hz)
            result.hz.push_back ((float)(double)v);
    return result;
}

std::optional<float> PitchTrack::pitchAt (double seconds) const
{
    const auto frame = seconds / frameSeconds;
    const auto i = (int)std::floor (frame);
    if (i < 0 || i + 1 >= (int)hz.size())
        return std::nullopt;
    const auto a = hz[(size_t)i], b = hz[(size_t)i + 1];
    if (a <= 0.0f && b <= 0.0f)
        return std::nullopt;
    if (a <= 0.0f || b <= 0.0f)
        return midiOf (a > 0.0f ? a : b);
    const auto t = (float)(frame - i);
    return midiOf (a) + t * (midiOf (b) - midiOf (a));
}

//==============================================================================
std::vector<float> TuneRender::corrections (const PitchTrack& track, const std::vector<model::TuneNote>& notes,
                                            double seconds)
{
    const auto step = track.frameSeconds;
    std::vector<float> result ((size_t)std::ceil (seconds / step) + 1, 0.0f);
    const auto half = juce::jmax (1, (int)std::round (slowWindowSeconds / step / 2.0));

    for (auto& note : notes)
    {
        if (note.isUnchanged())
            continue;

        const auto first = juce::jmax (0, (int)std::floor (note.start / step));
        const auto last = juce::jmin ((int)result.size() - 1, (int)std::ceil (note.end / step));

        // The curve inside the note (the note's centre where there is no pitch), and its slow part.
        std::vector<float> curve;
        for (int f = first; f <= last; ++f)
            curve.push_back (track.pitchAt (f * step).value_or (note.pitch));
        std::vector<float> slow (curve.size());
        for (int i = 0; i < (int)curve.size(); ++i)
        {
            const auto from = juce::jmax (0, i - half), to = juce::jmin ((int)curve.size() - 1, i + half);
            float sum = 0.0f;
            for (int k = from; k <= to; ++k)
                sum += curve[(size_t)k];
            slow[(size_t)i] = sum / (float)(to - from + 1);
        }

        for (int f = first; f <= last; ++f)
        {
            const auto i = (size_t)(f - first);
            const auto t = f * step;
            const auto edge = (float)juce::jlimit (0.0, 1.0, juce::jmin (t - note.start, note.end - t) / edgeSeconds);
            const auto correction = note.shift + (note.drift - 1.0f) * (slow[i] - note.pitch) +
                                    (note.vibrato - 1.0f) * (curve[i] - slow[i]);
            result[(size_t)f] = edge * correction;
        }
    }
    return result;
}

juce::Result TuneRender::render (const juce::File& source, const PitchTrack& track,
                                 const std::vector<model::TuneNote>& notes, const juce::File& destination,
                                 std::atomic<float>* progress, const std::atomic<bool>* cancel)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source));
    if (reader == nullptr)
        return juce::Result::fail ("Cannot read " + source.getFullPathName());

    const auto rate = reader->sampleRate;
    const auto channels = (int)juce::jlimit (1u, 2u, reader->numChannels);
    const auto length = (int64_t)reader->lengthInSamples;
    const auto correction = corrections (track, notes, (double)length / rate);

    using Stretcher = RubberBand::RubberBandStretcher;
    // Rubber Band's options are bit flags in a plain int.
    const auto options = (unsigned)Stretcher::OptionProcessRealTime | (unsigned)Stretcher::OptionEngineFiner |
                         (unsigned)Stretcher::OptionFormantPreserved | (unsigned)Stretcher::OptionPitchHighConsistency |
                         (unsigned)Stretcher::OptionChannelsTogether;
    Stretcher stretcher ((size_t)rate, (size_t)channels, (Stretcher::Options)options);
    stretcher.setMaxProcessSize (blockSize);

    if (! destination.getParentDirectory().createDirectory())
        return juce::Result::fail ("Cannot create " + destination.getParentDirectory().getFullPathName());
    destination.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (destination);
    auto writer = wav.createWriterFor (
        stream, juce::AudioFormatWriterOptions()
                    .withSampleRate (rate)
                    .withNumChannels (channels)
                    .withBitsPerSample (32)
                    .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (writer == nullptr)
        return juce::Result::fail ("Cannot write " + destination.getFullPathName());

    juce::AudioBuffer<float> in (channels, blockSize), out (channels, blockSize * 4);
    int64_t toSkip = (int64_t)stretcher.getStartDelay(); // the stretcher's delay, dropped from the output
    int64_t written = 0;

    auto drain = [&]
    {
        while (stretcher.available() > 0)
        {
            const auto count = juce::jmin (stretcher.available(), out.getNumSamples());
            stretcher.retrieve (out.getArrayOfWritePointers(), (size_t)count);
            int offset = 0;
            if (toSkip > 0)
            {
                offset = (int)juce::jmin<int64_t> (toSkip, count);
                toSkip -= offset;
            }
            const auto keep = (int)juce::jmin<int64_t> (count - offset, length - written);
            if (keep > 0)
            {
                writer->writeFromAudioSampleBuffer (out, offset, keep);
                written += keep;
            }
        }
    };

    // Silence first, so the stretcher's output starts with the file's start.
    for (auto pad = (int)stretcher.getPreferredStartPad(); pad > 0; pad -= blockSize)
    {
        const auto count = juce::jmin (pad, blockSize);
        in.clear();
        stretcher.process (in.getArrayOfReadPointers(), (size_t)count, false);
        drain();
    }

    for (int64_t position = 0; position < length; position += blockSize)
    {
        if (cancel != nullptr && cancel->load())
        {
            writer.reset();
            destination.deleteFile();
            return juce::Result::fail ("Cancelled.");
        }

        const auto count = (int)juce::jmin<int64_t> (blockSize, length - position);
        in.clear();
        reader->read (&in, 0, count, position, true, channels > 1);
        const auto frame = (size_t)juce::jlimit<int64_t> (
            0, (int64_t)correction.size() - 1,
            (int64_t)std::llround (((double)position + count / 2.0) / rate / track.frameSeconds));
        stretcher.setPitchScale (std::pow (2.0, (double)correction[frame] / 12.0));
        stretcher.process (in.getArrayOfReadPointers(), (size_t)count, position + count >= length);
        drain();

        if (progress != nullptr)
            *progress = (float)((double)(position + count) / (double)juce::jmax<int64_t> (1, length));
    }

    // What the stretcher still holds, then silence for anything missing at the end.
    drain();
    if (written < length)
    {
        out.clear();
        while (written < length)
        {
            const auto count = (int)juce::jmin<int64_t> (out.getNumSamples(), length - written);
            writer->writeFromAudioSampleBuffer (out, 0, count);
            written += count;
        }
    }
    return juce::Result::ok();
}

} // namespace spinus::engine
