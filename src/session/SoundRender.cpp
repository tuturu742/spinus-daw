#include "SoundRender.h"

#include "InstrumentPresets.h"
#include "OfflineInstrument.h"
#include "SoundDescriptors.h"
#include "SynthMatch.h"
#include "dsp/SpinusPluginFormat.h"
#include "dsp/sampler/SampleStorage.h"
#include "plugins/PluginCatalog.h"

#include <set>
#include <utility>

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

/** Some instruments load their samples in the background after opening (big sampled guitars, basses and
    drum libraries), silent until then: the test notes are played again, a second apart, until something
    sounds or `seconds` pass (each wait is a line, so Spinus doesn't take the helper for hung). */
bool waitUntilAudible (OfflineInstrument& player, Lines& out, double seconds)
{
    const auto until = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;
    for (;;)
    {
        const auto audio = player.render ({ { 48, 0.0, 0.4, 0.8f }, { 60, 0.0, 0.4, 0.8f } }, 0.3);
        if (audio.getMagnitude (0, audio.getNumSamples()) > 1.0e-4f)
            return true;
        if (juce::Time::getMillisecondCounterHiRes() > until)
            return false;
        out.write ("waiting", "its sounds to load");
        juce::Thread::sleep (1000);
    }
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
    // "defaultOnly": a second try for an instrument that crashed after loading (while its presets were
    // found or right after): only its sound as it opens.
    auto presets = (bool)request["defaultOnly"] ? std::vector<InstrumentPreset> {}
                                                : InstrumentPresets::find (*instance, deviceOf (request));
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
        return player.renderStereo (converted, tail); // the width is measured too
    };

    double firstWait = 40.0; // seconds an instrument may take to load its sounds, then for each preset
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
        {
            auto described =
                sounds::describeInstrument (play, OfflineInstrument::sampleRate, preset.name, preset.category);
            // Silent: perhaps still loading (the first sound, or a preset that loads other samples).
            if (described.silent && waitUntilAudible (player, out, std::exchange (firstWait, 10.0)))
                described =
                    sounds::describeInstrument (play, OfflineInstrument::sampleRate, preset.name, preset.category);
            sound = sounds::toVar (described);
        }
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
                const auto count = (int)std::min<juce::int64> (total, (juce::int64)(61.0 * rate));
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
bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio, double rate)
{
    file.deleteFile();
    file.getParentDirectory().createDirectory();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                                      .withSampleRate (rate)
                                                                      .withNumChannels (audio.getNumChannels())
                                                                      .withBitsPerSample (24));
    return writer != nullptr && writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}

std::vector<OfflineInstrument::Note> notesOf (const juce::var& list)
{
    std::vector<OfflineInstrument::Note> notes;
    if (const auto* a = list.getArray())
        for (const auto& n : *a)
            notes.push_back ({ juce::jlimit (0, 127, (int)n["note"]), juce::jmax (0.0, (double)n["start"]),
                               juce::jmax (0.01, (double)n["length"]),
                               n.hasProperty ("velocity") ? (float)(double)n["velocity"] : 0.8f });
    return notes;
}

/** An audio file read whole, at OfflineInstrument::sampleRate, in stereo. */
juce::AudioBuffer<float> readStereo (const juce::File& file, double from = 0.0, double length = -1.0)
{
    std::unique_ptr<juce::AudioFormatReader> reader (dsp::getAudioFormats().createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0)
        return {};
    const auto start = std::clamp<juce::int64> ((juce::int64)(from * reader->sampleRate), 0, reader->lengthInSamples);
    const auto available = reader->lengthInSamples - start;
    const auto count =
        (int)std::min<juce::int64> (available, length < 0.0 ? (juce::int64)(120.0 * reader->sampleRate)
                                                            : (juce::int64)(length * reader->sampleRate));
    if (count <= 0)
        return {};
    juce::AudioBuffer<float> source (2, count);
    reader->read (&source, 0, count, start, true, true);
    if (reader->numChannels < 2)
        source.copyFrom (1, 0, source, 0, 0, count);
    const auto ratio = reader->sampleRate / OfflineInstrument::sampleRate;
    if (std::abs (ratio - 1.0) < 1.0e-9)
        return source;
    juce::AudioBuffer<float> result (2, (int)((double)count / ratio));
    for (int c = 0; c < 2; ++c)
    {
        juce::LagrangeInterpolator resampler;
        resampler.process (ratio, source.getReadPointer (c), result.getWritePointer (c), result.getNumSamples(), count,
                           0);
    }
    return result;
}

/** Plays an instrument's presets (with settings) or puts audio files together, into WAV files:
    `{"mode": "render", <the instrument, as for "instrument">, "items": [...]}`, each item one of
    - `{id, preset (its id; "" or none: as the instrument is), settings {parameter ID: 0..1}, notes [{note,
      start, length, velocity}], tail, output}`: the instrument playing the notes;
    - `{id, cut: {file, from, length}, output}`: part of an audio file;
    - `{id, mix: [{file, at, gain}], length, output}`: audio files played at those times (a beat of drum samples).
    Writes `{"begin": id}` and `{"rendered": {id, output, peak}}` (or `{rendered: {id, problem}}`) per item. An
    item that comes out silent is played again once the instrument sounds (it may load its samples late), as in
    "instrument" mode; presets are found only when an item names one. */
int runRender (const juce::var& request, Lines& out)
{
    const auto* items = request["items"].getArray();
    const bool needsInstrument = items != nullptr && std::any_of (items->begin(), items->end(), [] (const juce::var& i)
                                                                  { return i.hasProperty ("notes"); });
    std::unique_ptr<juce::AudioPluginInstance> instance;
    std::unique_ptr<OfflineInstrument> player;
    std::vector<InstrumentPreset> presets;
    if (needsInstrument)
    {
        juce::String error;
        instance = loadInstrument (request, error);
        if (instance == nullptr)
        {
            out.write ("error", error);
            out.write ("done", true);
            return 0;
        }
        player = std::make_unique<OfflineInstrument> (*instance);
        // Presets are found only when an item asks for one: finding them crashes some instruments.
        if (std::any_of (items->begin(), items->end(),
                         [] (const juce::var& i) { return i["preset"].toString().isNotEmpty(); }))
            presets = InstrumentPresets::find (*instance, deviceOf (request));
    }
    double wait = 40.0;               // seconds an instrument may take to load its sounds (as runInstrument), then 10
    juce::String loadedPreset = "\n"; // none yet
    bool changed = false;             // settings were set over the loaded preset
    if (items != nullptr)
        for (const auto& item : *items)
        {
            const auto id = item["id"].toString();
            out.write ("begin", id);
            auto* o = new juce::DynamicObject();
            juce::var rendered (o);
            o->setProperty ("id", id);
            juce::AudioBuffer<float> audio;
            juce::String problem;
            if (item.hasProperty ("notes") && player != nullptr)
            {
                const auto presetId = item["preset"].toString();
                if ((presetId != loadedPreset || changed) && presetId.isNotEmpty())
                {
                    const auto found = std::find_if (presets.begin(), presets.end(),
                                                     [&] (const InstrumentPreset& p) { return p.id == presetId; });
                    if (found == presets.end())
                        problem = "The instrument has no preset " + presetId + ".";
                    else if (const auto loaded = player->loadPreset (*found); loaded.failed())
                        problem = loaded.getErrorMessage();
                }
                loadedPreset = presetId;
                changed = item["settings"].getDynamicObject() != nullptr;
                if (problem.isEmpty())
                {
                    if (const auto* settings = item["settings"].getDynamicObject())
                        for (const auto& [key, value] : settings->getProperties())
                            if (auto* parameter = SynthMatcher::findParameter (*instance, key.toString()))
                                parameter->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, (float)(double)value));
                    const auto notes = notesOf (item["notes"]);
                    const auto tail = item.hasProperty ("tail") ? (double)item["tail"] : 0.5;
                    audio = player->renderStereo (notes, tail);
                    // Silent: perhaps still loading its samples; played again once it sounds.
                    if (! notes.empty() && audio.getMagnitude (0, audio.getNumSamples()) <= 1.0e-4f &&
                        waitUntilAudible (*player, out, std::exchange (wait, 10.0)))
                        audio = player->renderStereo (notes, tail);
                }
            }
            else if (const auto& cut = item["cut"]; cut.isObject())
                audio = readStereo (juce::File (cut["file"].toString()), (double)cut["from"], (double)cut["length"]);
            else if (const auto* mix = item["mix"].getArray())
            {
                audio.setSize (2, juce::jmax (1, (int)((double)item["length"] * OfflineInstrument::sampleRate)));
                audio.clear();
                std::map<juce::String, juce::AudioBuffer<float>> files;
                for (const auto& hit : *mix)
                {
                    const auto path = hit["file"].toString();
                    if (! files.contains (path))
                        files[path] = readStereo (juce::File (path), 0.0, 4.0);
                    const auto& sound = files[path];
                    const auto at = (int)((double)hit["at"] * OfflineInstrument::sampleRate);
                    const auto count = juce::jmin (sound.getNumSamples(), audio.getNumSamples() - at);
                    const auto gain = hit.hasProperty ("gain") ? (float)(double)hit["gain"] : 1.0f;
                    for (int c = 0; c < 2 && count > 0 && at >= 0; ++c)
                        audio.addFrom (c, at, sound, c, 0, count, gain);
                }
            }
            else
                problem = "Nothing to render.";
            for (int c = 0; c < audio.getNumChannels(); ++c) // a plugin blowing up: silence there
            {
                auto* v = audio.getWritePointer (c);
                for (int i = 0; i < audio.getNumSamples(); ++i)
                    if (! std::isfinite (v[i]) || std::abs (v[i]) > 100.0f)
                        v[i] = 0.0f;
            }
            if (problem.isEmpty() && audio.getNumSamples() == 0)
                problem = "There is no sound.";
            if (problem.isEmpty())
            {
                const juce::File output (item["output"].toString());
                if (! writeWav (output, audio, OfflineInstrument::sampleRate))
                    problem = "Cannot write " + output.getFullPathName() + ".";
                o->setProperty ("output", output.getFullPathName());
                o->setProperty ("peak", audio.getMagnitude (0, audio.getNumSamples()));
            }
            if (problem.isNotEmpty())
                o->setProperty ("problem", problem);
            out.write ("rendered", rendered);
        }
    out.write ("done", true);
    return 0;
}

/** Synth match out of Spinus's process: `{"mode": "tune", <the instrument>, "preset" (its id, optional),
    "profile" (a synth profile's JSON, optional: else its own or an automatic one), "target": {file, start,
    length}, "notes" [{note, start, length, velocity}] (from the target's start), "tries"}`. Writes
    `{"profile": {name, settings, automatic}}`, `{"progress": {tries, best}}` now and then, and
    `{"result": {settings: {parameter ID: value}, startDistance, distance, tries}}` (or `{"error"}`). */
int runTune (const juce::var& request, Lines& out)
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
    const auto device = deviceOf (request);
    if (const auto presetId = request["preset"].toString(); presetId.isNotEmpty())
    {
        const auto presets = InstrumentPresets::find (*instance, device);
        const auto found =
            std::find_if (presets.begin(), presets.end(), [&] (const InstrumentPreset& p) { return p.id == presetId; });
        if (found == presets.end() || player.loadPreset (*found).failed())
        {
            out.write ("error", "The preset could not be loaded.");
            out.write ("done", true);
            return 0;
        }
    }
    waitUntilAudible (player, out, 40.0); // an instrument that loads its sounds late is tuned once it sounds
    auto profile =
        request["profile"].isObject() ? SynthProfile::fromJson (request["profile"]) : std::optional<SynthProfile> {};
    if (! profile)
        profile = SynthSearch::profileFor (*instance, device);
    const auto settings = SynthSearch::findSettings (*instance, *profile);
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", profile->name);
        o->setProperty ("settings", (int)settings.size());
        o->setProperty ("automatic", profile->automatic);
        out.write ("profile", juce::var (o));
    }
    if ((int)settings.size() < SynthProfile::fewestSettings)
    {
        out.write ("error", "It has no settings Spinus can tune.");
        out.write ("done", true);
        return 0;
    }
    const auto& target = request["target"];
    const auto wanted =
        readExcerpt (juce::File (target["file"].toString()), (double)target["start"], (double)target["length"], 0.2);
    const auto notes = notesOf (request["notes"]);
    if (wanted.getNumSamples() == 0 || notes.empty())
    {
        out.write ("error", "There is nothing to tune to.");
        out.write ("done", true);
        return 0;
    }
    const auto tries =
        juce::jlimit (1, 5000, request.hasProperty ("tries") ? (int)request["tries"] : profile->getTries());
    const auto found = SynthSearch::run (
        settings, [&] { return player.render (notes, 0.2); }, wanted, tries, {},
        [&] (int done, double best)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("tries", done);
            o->setProperty ("best", best);
            out.write ("progress", juce::var (o));
        });
    auto* values = new juce::DynamicObject();
    for (const auto& [id, value] : found.settings)
        values->setProperty (id, value);
    auto* o = new juce::DynamicObject();
    o->setProperty ("settings", juce::var (values));
    o->setProperty ("startDistance", found.startDistance);
    o->setProperty ("distance", found.distance);
    o->setProperty ("tries", found.tries);
    out.write ("result", juce::var (o));
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
    if (mode == "render")
        return runRender (parsed, out);
    if (mode == "tune")
        return runTune (parsed, out);
    out.write ("error", "Unknown request.");
    return 2;
}

} // namespace spinus
