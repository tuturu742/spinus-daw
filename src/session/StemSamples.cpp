#include "StemSamples.h"

#include "dsp/drums/DrumMachineProcessor.h"
#include "dsp/sampler/SampleStorage.h"
#include "dsp/sampler/SamplerProcessor.h"

namespace spinus
{

namespace te = tracktion;

namespace
{
template <typename Processor> Processor* instrumentOf (Session& session, model::ChannelId channel)
{
    const auto* c = session.getProject().findChannel (channel);
    if (c == nullptr || ! c->instrument)
        return nullptr;
    auto* plugin = dynamic_cast<te::ExternalPlugin*> (session.getAdapter().getDevicePlugin (c->instrument->id));
    return plugin != nullptr ? dynamic_cast<Processor*> (plugin->getAudioPluginInstance()) : nullptr;
}

std::unique_ptr<juce::AudioFormatReader> openStem (const juce::File& stem)
{
    return std::unique_ptr<juce::AudioFormatReader> (dsp::getAudioFormats().createReaderFor (stem));
}
} // namespace

juce::AudioBuffer<float> cutPiece (juce::AudioFormatReader& reader, double start, double end)
{
    const auto from = juce::jlimit<juce::int64> (0, reader.lengthInSamples, (juce::int64)(start * reader.sampleRate));
    const auto to = juce::jlimit<juce::int64> (0, reader.lengthInSamples, (juce::int64)(end * reader.sampleRate));
    if (to <= from)
        return {};
    const auto channels = (int)juce::jmin (2u, reader.numChannels);
    juce::AudioBuffer<float> piece (channels, (int)(to - from));
    reader.read (&piece, 0, piece.getNumSamples(), from, true, channels > 1);

    // 1 ms in, 10 ms out (or a fifth of a short piece).
    const auto fadeIn = juce::jmin (piece.getNumSamples() / 5, (int)(0.001 * reader.sampleRate));
    const auto fadeOut = juce::jmin (piece.getNumSamples() / 5, (int)(0.01 * reader.sampleRate));
    if (fadeIn > 0)
        piece.applyGainRamp (0, fadeIn, 0.0f, 1.0f);
    if (fadeOut > 0)
        piece.applyGainRamp (piece.getNumSamples() - fadeOut, fadeOut, 1.0f, 0.0f);
    return piece;
}

juce::Result loadDrumPieces (Session& session, model::ChannelId channel, const juce::File& stem,
                             const std::vector<StemPiece>& pieces)
{
    auto* drums = instrumentOf<dsp::DrumMachineProcessor> (session, channel);
    if (drums == nullptr)
        return juce::Result::fail ("The channel has no Drum Machine.");
    auto reader = openStem (stem);
    if (reader == nullptr)
        return juce::Result::fail ("Spinus could not read " + stem.getFileName() + ".");
    const auto& pads = drums->getPads();
    for (auto& piece : pieces)
    {
        const auto pad = std::find_if (pads.begin(), pads.end(), [&] (auto& p) { return p.note == piece.note; });
        auto audio = cutPiece (*reader, piece.start, piece.end);
        if (pad != pads.end() && audio.getNumSamples() > 0)
            drums->setPadAudio ((int)std::distance (pads.begin(), pad), std::move (audio), reader->sampleRate,
                                piece.name);
    }
    return juce::Result::ok();
}

namespace
{
int padPlaying (const dsp::DrumMachineProcessor& drums, int note)
{
    const auto& pads = drums.getPads();
    const auto found = std::find_if (pads.begin(), pads.end(), [note] (auto& p) { return p.note == note; });
    return found != pads.end() ? (int)std::distance (pads.begin(), found) : -1;
}
} // namespace

juce::Result loadDrumPadFile (Session& session, model::ChannelId channel, int note, const juce::File& file)
{
    auto* drums = instrumentOf<dsp::DrumMachineProcessor> (session, channel);
    if (drums == nullptr)
        return juce::Result::fail ("The channel has no Drum Machine.");
    const auto pad = padPlaying (*drums, note);
    if (pad < 0)
        return juce::Result::fail ("No pad plays that note.");
    return drums->loadPadFile (pad, file);
}

juce::Result copyDrumPad (Session& session, model::ChannelId channel, int from, int to, const juce::String& name)
{
    auto* drums = instrumentOf<dsp::DrumMachineProcessor> (session, channel);
    if (drums == nullptr)
        return juce::Result::fail ("The channel has no Drum Machine.");
    const auto source = padPlaying (*drums, from), target = padPlaying (*drums, to);
    if (source < 0 || target < 0 || drums->getPads()[(size_t)source].audio == nullptr)
        return juce::Result::fail ("No pad plays that note.");
    const auto& pad = drums->getPads()[(size_t)source];
    drums->setPadAudio (target, *pad.audio, pad.sampleRate, name);
    return juce::Result::ok();
}

juce::Result loadSamplerPieces (Session& session, model::ChannelId channel, const juce::File& stem,
                                const std::vector<StemPiece>& pieces)
{
    auto* sampler = instrumentOf<dsp::SamplerProcessor> (session, channel);
    if (sampler == nullptr)
        return juce::Result::fail ("The channel has no Sampler.");
    auto reader = openStem (stem);
    if (reader == nullptr)
        return juce::Result::fail ("Spinus could not read " + stem.getFileName() + ".");
    std::vector<dsp::SampleZone> zones;
    for (auto& piece : pieces)
    {
        auto audio = cutPiece (*reader, piece.start, piece.end);
        if (audio.getNumSamples() == 0)
            continue;
        dsp::SampleZone zone;
        zone.name = piece.name;
        zone.end = audio.getNumSamples();
        zone.audio = std::make_shared<const juce::AudioBuffer<float>> (std::move (audio));
        zone.sampleRate = reader->sampleRate;
        zone.rootNote = zone.lowKey = zone.highKey = piece.note;
        zones.push_back (std::move (zone));
    }
    sampler->setZones (std::move (zones));
    return juce::Result::ok();
}

juce::Result loadSamplerFile (Session& session, model::ChannelId channel, const juce::File& file, int rootNote)
{
    auto* sampler = instrumentOf<dsp::SamplerProcessor> (session, channel);
    if (sampler == nullptr)
        return juce::Result::fail ("The channel has no Sampler.");
    auto reader = openStem (file);
    if (reader == nullptr)
        return juce::Result::fail ("Spinus could not read " + file.getFileName() + ".");
    auto audio = cutPiece (*reader, 0.0, juce::jmin (30.0, (double)reader->lengthInSamples / reader->sampleRate));
    if (audio.getNumSamples() == 0)
        return juce::Result::fail (file.getFileName() + " holds no sound.");
    dsp::SampleZone zone;
    zone.name = file.getFileNameWithoutExtension();
    zone.end = audio.getNumSamples();
    zone.audio = std::make_shared<const juce::AudioBuffer<float>> (std::move (audio));
    zone.sampleRate = reader->sampleRate;
    zone.rootNote = juce::jlimit (0, 127, rootNote);
    zone.lowKey = 0;
    zone.highKey = 127;
    sampler->setZones ({ std::move (zone) });
    return juce::Result::ok();
}

} // namespace spinus
