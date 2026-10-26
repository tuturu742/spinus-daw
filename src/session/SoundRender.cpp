#include "SoundRender.h"

#include "InstrumentPresets.h"
#include "OfflineInstrument.h"
#include "SoundDescriptors.h"
#include "dsp/SpinusPluginFormat.h"
#include "dsp/sampler/SampleStorage.h"
#include "plugins/PluginCatalog.h"

#include <set>

namespace spinus
{

namespace
{
/** One JSON object per line, written at once (the parent reads the file as it grows). */
class Lines
{
public:
    explicit Lines (const juce::File& file) : stream (file)
    {
        if (stream.openedOk())
        {
            stream.setPosition (0);
            stream.truncate();
        }
    }
    bool isOk() const { return stream.openedOk(); }
    void write (const juce::String& key, const juce::var& value)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty (key, value);
        stream.writeText (
            juce::JSON::toString (juce::var (o), juce::JSON::FormatOptions {}.withSpacing (juce::JSON::Spacing::none)) +
                "\n",
            false, false, nullptr);
        stream.flush();
    }

private:
    juce::FileOutputStream stream;
};

std::unique_ptr<juce::AudioPluginInstance> loadInstrument (const juce::var& request, juce::String& error)
{
    const auto format = request["format"].toString(), file = request["file"].toString(),
               name = request["name"].toString();
    const auto uniqueId = (int)request["uniqueId"];
    if (format == dsp::SpinusPluginFormat::formatName)
    {
        auto instance = dsp::SpinusPluginFormat::create (file);
        if (instance == nullptr)
            error = "Spinus has no instrument " + file + ".";
        return instance;
    }
    juce::AudioPluginFormatManager formats;
    plugins::PluginCatalog::addFormats (formats);
    for (auto* f : formats.getFormats())
    {
        if (f->getName() != format)
            continue;
        juce::OwnedArray<juce::PluginDescription> found;
        f->findAllTypesForFile (found, file);
        const juce::PluginDescription* chosen = nullptr;
        for (auto* d : found)
            if (d->name == name && (uniqueId == 0 || d->uniqueId == uniqueId))
                chosen = d;
        for (auto* d : found)
            if (chosen == nullptr && d->name == name)
                chosen = d;
        for (auto* d : found)
            if (chosen == nullptr && d->isInstrument)
                chosen = d;
        if (chosen == nullptr)
        {
            error = "Spinus did not find " + name + " in " + file + ".";
            return nullptr;
        }
        auto instance =
            formats.createPluginInstance (*chosen, OfflineInstrument::sampleRate, OfflineInstrument::blockSize, error);
        if (instance == nullptr && error.isEmpty())
            error = name + " did not load.";
        return instance;
    }
    error = "Spinus does not host " + format + " plugins here.";
    return nullptr;
}

model::Device deviceOf (const juce::var& request)
{
    model::Device device;
    if (const auto stockType = request["stockType"].toString(); stockType.isNotEmpty())
    {
        device.kind = model::Device::Kind::stock;
        device.stockType = stockType;
    }
    else
    {
        device.kind = model::Device::Kind::plugin;
        device.plugin.format = request["format"].toString();
        device.plugin.name = request["name"].toString();
        device.plugin.fileOrIdentifier = request["file"].toString();
        device.plugin.uniqueId = (int)request["uniqueId"];
    }
    return device;
}

int runInstrument (const juce::var& request, Lines& out)
{
    juce::String error;
    auto instance = loadInstrument (request, error);
    if (instance == nullptr)
    {
        out.write ("error", error);
        out.write ("done", true);
        return 0;
    }
    OfflineInstrument player (*instance);
    auto presets = InstrumentPresets::find (*instance, deviceOf (request));
    const bool hasPresets = ! presets.empty();
    if (! hasPresets)
    {
        InstrumentPreset asItIs;
        asItIs.name = "Default sound";
        presets.push_back (asItIs);
    }

    juce::Array<juce::var> listed;
    bool presetFiles = false;
    for (const auto& p : presets)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", p.id);
        o->setProperty ("version", p.version);
        listed.add (juce::var (o));
        presetFiles = presetFiles || p.file != juce::File();
    }
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("list", listed);
        o->setProperty ("presetFiles", presetFiles);
        out.write ("presets", juce::var (o));
    }

    const auto* known = request["known"].getDynamicObject();
    std::set<juce::String> skip;
    if (const auto* list = request["skip"].getArray())
        for (const auto& s : *list)
            skip.insert (s.toString());

    const sounds::Player play = [&player] (const std::vector<sounds::Note>& notes, double tail)
    {
        std::vector<OfflineInstrument::Note> converted;
        converted.reserve (notes.size());
        for (const auto& n : notes)
            converted.push_back ({ n.note, n.start, n.length, n.velocity });
        return player.render (converted, tail);
    };

    for (const auto& preset : presets)
    {
        if (skip.contains (preset.id))
            continue;
        if (known != nullptr && known->hasProperty (preset.id) &&
            known->getProperty (preset.id).toString() == preset.version)
            continue;
        out.write ("begin", preset.id);
        juce::var sound;
        juce::String problem;
        if (hasPresets)
            if (const auto loaded = player.loadPreset (preset); loaded.failed())
                problem = loaded.getErrorMessage();
        if (problem.isEmpty())
            sound = sounds::toVar (
                sounds::describeInstrument (play, OfflineInstrument::sampleRate, preset.name, preset.category));
        else
        {
            sound = juce::var (new juce::DynamicObject());
            sound.getDynamicObject()->setProperty ("problem", problem);
        }
        auto* o = sound.getDynamicObject();
        o->setProperty ("id", preset.id);
        o->setProperty ("version", preset.version);
        o->setProperty ("name", preset.name);
        if (preset.category.isNotEmpty())
            o->setProperty ("category", preset.category);
        out.write ("sound", sound);
    }
    out.write ("done", true);
    return 0;
}

int runSamples (const juce::var& request, Lines& out)
{
    auto& formats = dsp::getAudioFormats();
    if (const auto* files = request["files"].getArray())
        for (const auto& path : *files)
        {
            const juce::File file (path.toString());
            out.write ("begin", file.getFullPathName());
            auto* o = new juce::DynamicObject();
            juce::var sample (o);
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
            // A file whose extension is not its format (AIFF named .wav, WAV named .mp3, as some packs have):
            // every format is tried on its contents.
            if (reader == nullptr)
                if (auto in = file.createInputStream())
                    reader.reset (formats.createReaderFor (std::move (in)));
            if (reader == nullptr || reader->sampleRate <= 0.0)
                o->setProperty ("problem", "Spinus cannot read it.");
            else if (reader->lengthInSamples <= 0)
                o->setProperty ("problem", "It holds no sound.");
            else
            {
                // At most a minute and a bit is read: anything longer is a song, not a sample.
                const auto rate = reader->sampleRate;
                const auto total = reader->lengthInSamples;
                const auto count = (int)juce::jmin<juce::int64> (total, (juce::int64)(61.0 * rate));
                juce::AudioBuffer<float> audio ((int)juce::jlimit (1u, 2u, reader->numChannels), count);
                reader->read (&audio, 0, count, 0, true, audio.getNumChannels() > 1);
                juce::AudioBuffer<float> mono (1, count);
                mono.copyFrom (0, 0, audio, 0, 0, count);
                if (audio.getNumChannels() > 1)
                {
                    mono.addFrom (0, 0, audio, 1, 0, count);
                    mono.applyGain (0.5f);
                }
                auto info = sounds::describeSample (mono, rate, file);
                info.duration = (double)total / rate;
                if (info.duration > 60.0)
                    info.kind = "long";
                sample = sounds::toVar (info);
                o = sample.getDynamicObject();
            }
            o->setProperty ("path", file.getFullPathName());
            out.write ("sample", sample);
        }
    out.write ("done", true);
    return 0;
}
} // namespace

int runSoundIndexProcess (const juce::File& request, const juce::File& output)
{
    const auto parsed = juce::JSON::parse (request);
    Lines out (output);
    if (! out.isOk())
        return 3;
    const auto mode = parsed["mode"].toString();
    if (mode == "instrument")
        return runInstrument (parsed, out);
    if (mode == "samples")
        return runSamples (parsed, out);
    out.write ("error", "Unknown request.");
    return 2;
}

} // namespace spinus
