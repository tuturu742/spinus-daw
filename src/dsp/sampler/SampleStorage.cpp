#include "SampleStorage.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace spinus::dsp
{

namespace
{
/** A zone's audio, scaled to fit FLAC (which stores -1..1), as a FLAC file in memory. */
juce::MemoryBlock encodeFlac (const juce::AudioBuffer<float>& audio, double sampleRate, float& scale)
{
    const auto peak = audio.getMagnitude (0, audio.getNumSamples());
    scale = juce::jmax (1.0f, peak);
    juce::AudioBuffer<float> scaled (audio);
    scaled.applyGain (1.0f / scale);

    juce::MemoryBlock block;
    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (block, false);
    auto writer = flac.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                    .withSampleRate (juce::jlimit (1.0, 655350.0, sampleRate))
                                                    .withNumChannels (audio.getNumChannels())
                                                    .withBitsPerSample (24));
    if (writer == nullptr)
        return {};
    writer->writeFromAudioSampleBuffer (scaled, 0, scaled.getNumSamples());
    writer = nullptr; // flushes into the block
    return block;
}

bool decodeFlac (const juce::MemoryBlock& block, float scale, juce::AudioBuffer<float>& result)
{
    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatReader> reader (
        flac.createReaderFor (new juce::MemoryInputStream (block, false), true));
    if (reader == nullptr)
        return false;
    result.setSize ((int)reader->numChannels, (int)reader->lengthInSamples);
    if (! reader->read (&result, 0, (int)reader->lengthInSamples, 0, true, true))
        return false;
    result.applyGain (scale);
    return true;
}

} // namespace

juce::AudioFormatManager& getAudioFormats()
{
    // Built in one go, so it is created after (and destroyed before) JUCE's leak counters for the
    // formats it holds; otherwise they would report the formats as leaked at exit.
    static const auto manager = []
    {
        auto m = std::make_unique<juce::AudioFormatManager>();
        m->registerBasicFormats();
        return m;
    }();
    return *manager;
}

juce::ValueTree zoneToState (const SampleZone& z)
{
    juce::ValueTree zone ("Zone");
    zone.setProperty ("name", z.name, nullptr)
        .setProperty ("sampleRate", z.sampleRate, nullptr)
        .setProperty ("root", z.rootNote, nullptr)
        .setProperty ("lowKey", z.lowKey, nullptr)
        .setProperty ("highKey", z.highKey, nullptr)
        .setProperty ("lowVelocity", z.lowVelocity, nullptr)
        .setProperty ("highVelocity", z.highVelocity, nullptr)
        .setProperty ("start", z.start, nullptr)
        .setProperty ("end", z.end, nullptr)
        .setProperty ("loop", z.loop, nullptr)
        .setProperty ("loopStart", z.loopStart, nullptr)
        .setProperty ("loopEnd", z.loopEnd, nullptr)
        .setProperty ("settings", z.settingsIndex, nullptr)
        .setProperty ("choke", z.chokeGroup, nullptr);

    if (z.audio != nullptr)
    {
        float scale = 1.0f;
        zone.setProperty ("audio", encodeFlac (*z.audio, z.sampleRate, scale), nullptr);
        zone.setProperty ("scale", scale, nullptr);
    }
    return zone;
}

SampleZone zoneFromState (const juce::ValueTree& zone)
{
    SampleZone z;
    z.name = zone["name"].toString();
    z.sampleRate = (double)zone.getProperty ("sampleRate", 44100.0);
    z.rootNote = zone.getProperty ("root", 60);
    z.lowKey = zone.getProperty ("lowKey", 0);
    z.highKey = zone.getProperty ("highKey", 127);
    z.lowVelocity = zone.getProperty ("lowVelocity", 1);
    z.highVelocity = zone.getProperty ("highVelocity", 127);
    z.start = (juce::int64)zone.getProperty ("start", 0);
    z.end = (juce::int64)zone.getProperty ("end", 0);
    z.loop = zone.getProperty ("loop", false);
    z.loopStart = (juce::int64)zone.getProperty ("loopStart", 0);
    z.loopEnd = (juce::int64)zone.getProperty ("loopEnd", 0);
    z.settingsIndex = zone.getProperty ("settings", 0);
    z.chokeGroup = zone.getProperty ("choke", 0);

    if (auto* block = zone["audio"].getBinaryData())
    {
        juce::AudioBuffer<float> audio;
        if (decodeFlac (*block, (float)zone.getProperty ("scale", 1.0f), audio))
            z.audio = std::make_shared<const juce::AudioBuffer<float>> (std::move (audio));
    }
    return z;
}

juce::Result readAudioFile (const juce::File& file, SampleZone& zone, double maxSeconds)
{
    std::unique_ptr<juce::AudioFormatReader> reader (getAudioFormats().createReaderFor (file));
    if (reader == nullptr)
        return juce::Result::fail (file.getFileName() + " is not an audio file Spinus can read");
    if (reader->sampleRate <= 0 || (double)reader->lengthInSamples > maxSeconds * reader->sampleRate)
        return juce::Result::fail (file.getFileName() + " is longer than " +
                                   juce::String (juce::roundToInt (maxSeconds / 60.0)) + " minutes");

    // Stereo at most: Spinus's devices play to a stereo output.
    const auto channels = (int)juce::jmin (2u, reader->numChannels);
    juce::AudioBuffer<float> audio (channels, (int)reader->lengthInSamples);
    reader->read (&audio, 0, (int)reader->lengthInSamples, 0, true, channels > 1);

    zone.name = file.getFileNameWithoutExtension();
    zone.sampleRate = reader->sampleRate;
    zone.start = 0;
    zone.end = audio.getNumSamples();
    zone.loopStart = 0;
    zone.loopEnd = zone.end;
    zone.audio = std::make_shared<const juce::AudioBuffer<float>> (std::move (audio));
    return juce::Result::ok();
}

void SampleSetHolder::publish (std::vector<SampleZone> zones)
{
    for (auto& z : zones)
        z.clampPoints();

    auto set = std::make_unique<SampleSet>();
    set->zones = std::move (zones);
    playing.store (set.get());

    if (current != nullptr)
        retired.emplace_back (std::move (current), blocksDone.load());
    current = std::move (set);

    // A set retired during block k may be read until the end of block k + 1 (voices fade out in the
    // first block that sees the new set).
    const auto done = blocksDone.load();
    std::erase_if (retired, [done] (auto& r) { return done >= r.second + 2; });
}

} // namespace spinus::dsp
