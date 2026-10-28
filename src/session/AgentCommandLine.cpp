#include "AgentCommandLine.h"

#include "commands/CommandRegistry.h"
#include "dsp/drums/DrumMachineProcessor.h"
#include "dsp/drums/StarterPack.h"
#include "dsp/loudness/LoudnessEngine.h"
#include "dsp/sampler/SamplerProcessor.h"
#include "dsp/threeosc/ThreeOscPresets.h"
#include "engine/EngineAdapter.h"
#include "model/StockDevices.h"
#include "session/Session.h"

#include <tracktion_engine/tracktion_engine.h>

#include <optional>

namespace spinus::cli
{

namespace
{
namespace te = tracktion;

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> properties)
{
    auto* o = new juce::DynamicObject();
    for (auto& [name, value] : properties)
        o->setProperty (name, value);
    return juce::var (o);
}

juce::String line (const juce::var& v) { return juce::JSON::toString (v, true); }

void settle (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

const char* const noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

/** FL's note names, as the Piano Roll shows them: MIDI 60 is C5. */
juce::String noteName (int pitch) { return juce::String (noteNames[pitch % 12]) + juce::String (pitch / 12); }

/** A relative file in the script's folder, if it is there; otherwise the text as given. */
juce::String resolvePath (const juce::String& path, const juce::File& folder)
{
    if (path.isEmpty() || juce::File::isAbsolutePath (path) || folder == juce::File())
        return path;
    const auto file = folder.getChildFile (path);
    return file.exists() ? file.getFullPathName() : path;
}

/** The parameters that name files: an audio clip's (commands), and the actions' kits and samples. */
juce::var resolvePaths (const juce::var& params, const juce::File& folder)
{
    auto* o = params.getDynamicObject();
    if (o == nullptr)
        return params;
    for (const auto* key : { "file", "kit" })
        if (o->hasProperty (key) && o->getProperty (key).isString())
        {
            const auto given = o->getProperty (key).toString();
            if (juce::String (key) == "kit" && ! given.endsWithIgnoreCase (dsp::DrumMachineProcessor::kitExtension))
                continue; // a factory kit, by name
            o->setProperty (key, resolvePath (given, folder));
        }
    if (auto* files = o->getProperty ("files").getArray())
    {
        juce::Array<juce::var> resolved;
        for (auto& f : *files)
            resolved.add (resolvePath (f.toString(), folder));
        o->setProperty ("files", resolved);
    }
    return params;
}

/** The instance a device plays through, once the engine has made it (a moment after its command). */
juce::AudioPluginInstance* instanceOf (Session& session, model::DeviceId device)
{
    for (int i = 0; i < 100; ++i)
    {
        session.getEdit().dispatchPendingUpdatesSynchronously();
        if (auto* external = dynamic_cast<te::ExternalPlugin*> (session.getAdapter().getDevicePlugin (device)))
            if (auto* instance = external->getAudioPluginInstance())
                return instance;
        settle (20);
    }
    return nullptr;
}

juce::Result loadKit (Session& session, const juce::var& params)
{
    const auto device = model::DeviceId { (uint32_t)(int)params["device"] };
    const auto kit = params["kit"].toString();
    auto* drums = dynamic_cast<dsp::DrumMachineProcessor*> (instanceOf (session, device));
    if (drums == nullptr)
        return juce::Result::fail ("device " + params["device"].toString() + " is not a Drum Machine");
    if (kit.equalsIgnoreCase ("Spinus Synth Kit"))
    {
        drums->loadSynthKit();
        return juce::Result::ok();
    }
    if (kit.endsWithIgnoreCase (dsp::DrumMachineProcessor::kitExtension))
        return drums->loadKit (juce::File (kit));
    const auto pack = dsp::starter::findPack();
    juce::StringArray names { "Spinus Synth Kit" };
    for (auto& k : dsp::starter::readKits (pack))
    {
        if (k.name.equalsIgnoreCase (kit))
            return drums->loadFactoryKit (k, pack);
        names.add (k.name);
    }
    return juce::Result::fail ("no kit \"" + kit + "\"; the kits are " + names.joinIntoString (", ") +
                               ", or a .spinuskit file");
}

juce::Result loadSamples (Session& session, const juce::var& params)
{
    const auto device = model::DeviceId { (uint32_t)(int)params["device"] };
    juce::Array<juce::File> files;
    if (auto* list = params["files"].getArray())
        for (auto& f : *list)
            files.add (juce::File (f.toString()));
    else if (params["file"].isString())
        files.add (juce::File (params["file"].toString()));
    if (files.isEmpty())
        return juce::Result::fail ("'files' must list audio files");
    for (auto& f : files)
        if (! f.existsAsFile())
            return juce::Result::fail ("no file " + f.getFullPathName());

    auto* instance = instanceOf (session, device);
    if (auto* sampler = dynamic_cast<dsp::SamplerProcessor*> (instance))
        return sampler->loadFiles (files, ! params.hasProperty ("replace") || (bool)params["replace"]);
    if (auto* drums = dynamic_cast<dsp::DrumMachineProcessor*> (instance))
    {
        const int pad = params.hasProperty ("pad") ? (int)params["pad"] : 1;
        if (pad < 1 || pad + files.size() - 1 > dsp::DrumMachineProcessor::numPads)
            return juce::Result::fail ("'pad' must be 1 to 16 (one file per pad from there)");
        for (int i = 0; i < files.size(); ++i)
            if (auto r = drums->loadPadFile (pad - 1 + i, files[i]); r.failed())
                return r;
        return juce::Result::ok();
    }
    return juce::Result::fail ("device " + params["device"].toString() + " is not a Sampler or a Drum Machine");
}

juce::String keyName (const model::Key& key)
{
    return juce::String (noteNames[((key.tonic % 12) + 12) % 12]) + " " + key.scale;
}

juce::var deviceSummary (const model::Device& d)
{
    return object ({ { "device", (int)d.id.value },
                     { "type", d.kind == model::Device::Kind::stock ? d.stockType : "plugin" },
                     { "name", model::getDeviceDisplayName (d) },
                     { "bypassed", d.bypassed } });
}
} // namespace

//==============================================================================
juce::var resolveReferences (const juce::var& value, const std::map<juce::String, juce::var>& results,
                             juce::String& error)
{
    if (value.isString())
    {
        // Only "$name" or "$name.field" (letters, digits, '_' and '-') is a reference; other text stays as it is.
        const auto text = value.toString();
        const auto label = text.substring (1).upToFirstOccurrenceOf (".", false, false);
        const auto field = text.fromFirstOccurrenceOf (".", false, false);
        const auto plain = [] (const juce::String& s, bool mayBeEmpty)
        {
            return (mayBeEmpty || s.isNotEmpty()) &&
                   s.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");
        };
        if (! text.startsWithChar ('$') || ! plain (label, false) || ! plain (field, ! text.containsChar ('.')))
            return value;
        const auto found = results.find (label);
        if (found == results.end() || (field.isNotEmpty() && ! found->second.hasProperty (field)))
        {
            if (error.isEmpty())
                error = "the reference " + text + " names no earlier result";
            return value;
        }
        return field.isEmpty() ? found->second : found->second[juce::Identifier (field)];
    }
    if (auto* array = value.getArray())
    {
        juce::Array<juce::var> copy;
        for (auto& item : *array)
            copy.add (resolveReferences (item, results, error));
        return copy;
    }
    if (auto* o = value.getDynamicObject())
    {
        auto* copy = new juce::DynamicObject();
        for (auto& property : o->getProperties())
            copy->setProperty (property.name, resolveReferences (property.value, results, error));
        return juce::var (copy);
    }
    return value;
}

int runSteps (Session& session, const juce::var& steps, const RunOptions& options, const Say& say)
{
    const auto* list = steps.getArray();
    if (list == nullptr)
    {
        say (line (object ({ { "ok", false }, { "error", "the script must be a JSON list of steps" } })));
        return 1;
    }
    std::map<juce::String, juce::var> results;
    bool failed = false;
    for (int i = 0; i < list->size(); ++i)
    {
        const auto& step = list->getReference (i);
        const auto command = step["command"].toString(), action = step["action"].toString();
        auto report = object ({ { "step", i + 1 } });
        report.getDynamicObject()->setProperty (action.isNotEmpty() ? "action" : "command",
                                                action.isNotEmpty() ? action : command);
        const auto fail = [&] (const juce::String& error)
        {
            report.getDynamicObject()->setProperty ("ok", false);
            report.getDynamicObject()->setProperty ("error", error);
            say (line (report));
            failed = true;
        };

        juce::String error;
        auto params =
            resolveReferences (step.hasProperty ("params") ? step["params"] : step["parameters"], results, error);
        if (error.isNotEmpty())
        {
            fail (error);
            if (! options.keepGoing)
                break;
            continue;
        }
        if (params.isVoid())
            params = juce::var (new juce::DynamicObject());
        params = resolvePaths (juce::JSON::parse (juce::JSON::toString (params)), options.scriptFolder);

        juce::var result;
        if (action.isNotEmpty())
        {
            const auto done = action == "loadKit" ? loadKit (session, params)
                              : action == "loadSamples"
                                  ? loadSamples (session, params)
                                  : juce::Result::fail ("unknown action \"" + action + "\" (loadKit or loadSamples)");
            if (done.failed())
                error = done.getErrorMessage();
            result = object ({ { "device", params["device"] } });
        }
        else if (command.isEmpty())
            error = "a step needs \"command\" (or \"action\")";
        else if (auto made = commands::CommandRegistry::get().create (command, params, error))
        {
            auto* raw = made.get();
            if (const auto done = session.getHistory().perform (std::move (made)); done.failed())
                error = done.getErrorMessage();
            else
                result = raw->getResult();
        }
        if (error.isNotEmpty())
        {
            fail (error);
            if (! options.keepGoing)
                break;
            continue;
        }

        results[juce::String (i + 1)] = result;
        if (const auto label = step["as"].toString(); label.isNotEmpty())
            results[label] = result;
        report.getDynamicObject()->setProperty ("ok", true);
        report.getDynamicObject()->setProperty ("result", result);
        say (line (report));
        if ((i + 1) % 50 == 0)
            settle (1);
    }
    session.getEdit().dispatchPendingUpdatesSynchronously();
    settle (50);
    return failed ? 2 : 0;
}

//==============================================================================
juce::var summarize (Session& session, const juce::File& projectFolder)
{
    const auto& p = session.getProject();
    const auto barTicks = p.settings.timeSignature.barTicks();
    model::Ticks end = 0;
    juce::Array<juce::var> problems;

    juce::Array<juce::var> channels;
    for (auto& c : p.channels)
    {
        auto instrument = c.instrument ? deviceSummary (*c.instrument) : juce::var();
        if (c.instrument)
        {
            auto* plugin = dynamic_cast<te::ExternalPlugin*> (session.getAdapter().getDevicePlugin (c.instrument->id));
            auto* instance = plugin != nullptr ? plugin->getAudioPluginInstance() : nullptr;
            if (auto* drums = dynamic_cast<dsp::DrumMachineProcessor*> (instance))
                instrument.getDynamicObject()->setProperty ("kit", drums->getKitDisplayName());
            if (auto* sampler = dynamic_cast<dsp::SamplerProcessor*> (instance))
                instrument.getDynamicObject()->setProperty ("samples", (int)sampler->getZones().size());
        }
        channels.add (object ({ { "channel", (int)c.id.value },
                                { "name", c.name },
                                { "group", c.group },
                                { "instrument", instrument },
                                { "audioTrack", c.audioTrack },
                                { "insert", (int)c.targetInsert.value },
                                { "volumeDb", c.volumeDb } }));
    }

    juce::Array<juce::var> inserts;
    const auto insertSummary = [&] (const model::Insert& in)
    {
        juce::Array<juce::var> effects, sends, sidechains;
        for (auto& e : in.effects)
            effects.add (deviceSummary (e.device));
        for (auto& s : in.sends)
            sends.add (object ({ { "to", (int)s.target.value }, { "levelDb", s.levelDb } }));
        for (auto& s : in.sidechainSources)
            sidechains.add ((int)s.value);
        return object ({ { "insert", (int)in.id.value },
                         { "name", in.name },
                         { "volumeDb", in.volumeDb },
                         { "output", (int)in.output.value },
                         { "effects", effects },
                         { "sends", sends },
                         { "sidechainFrom", sidechains } });
    };
    inserts.add (insertSummary (p.mixer.master));
    for (auto& in : p.mixer.inserts)
        inserts.add (insertSummary (in));

    juce::Array<juce::var> patterns;
    for (auto& pat : p.patterns)
    {
        int notes = 0;
        for (auto& [channel, list] : pat.notes)
            notes += (int)list.size();
        patterns.add (object ({ { "pattern", (int)pat.id.value },
                                { "name", pat.name },
                                { "bars", (double)pat.length / (double)barTicks },
                                { "notes", notes } }));
    }

    juce::Array<juce::var> lanes;
    for (auto& lane : p.playlist.lanes)
        lanes.add (object ({ { "lane", (int)lane.id.value }, { "name", lane.name }, { "muted", lane.muted } }));

    int patternClips = 0, audioClips = 0, automationClips = 0;
    const auto audioFolder = projectFolder == juce::File() ? juce::File() : projectFolder.getChildFile ("audio");
    for (auto& clip : p.playlist.clips)
    {
        end = std::max (end, clip.start + clip.length);
        if (std::holds_alternative<model::PatternClip> (clip.content))
            ++patternClips;
        else if (const auto* audio = std::get_if<model::AudioClip> (&clip.content))
        {
            ++audioClips;
            const auto file = juce::File::isAbsolutePath (audio->file) ? juce::File (audio->file)
                              : audioFolder != juce::File()            ? audioFolder.getChildFile (audio->file)
                                                                       : juce::File();
            if (! file.existsAsFile())
                problems.add ("audio file missing: " + audio->file + " (clip " + juce::String (clip.id.value) + ")");
        }
        else
            ++automationClips;
    }

    juce::Array<juce::var> markers;
    for (auto& m : p.playlist.markers)
        markers.add (object ({ { "name", m.name }, { "bar", 1.0 + (double)m.at / (double)barTicks } }));

    for (auto& missing : session.getAdapter().getMissingDevices())
        problems.add ("device " + juce::String (missing.value) + " does not load here (its plugin is missing)");

    return object (
        { { "tempoBpm", p.settings.tempoBpm },
          { "timeSignature", juce::String (p.settings.timeSignature.numerator) + "/" +
                                 juce::String (p.settings.timeSignature.denominator) },
          { "key", p.settings.key ? juce::var (keyName (*p.settings.key)) : juce::var() },
          { "bars", (double)end / (double)barTicks },
          { "seconds", (double)end / (double)model::ticksPerQuarterNote * 60.0 / p.settings.tempoBpm },
          { "channels", channels },
          { "inserts", inserts },
          { "patterns", patterns },
          { "lanes", lanes },
          { "clips",
            object ({ { "pattern", patternClips }, { "audio", audioClips }, { "automation", automationClips } }) },
          { "markers", markers },
          { "problems", problems } });
}

//==============================================================================
juce::StringArray describableTypes (Session& session)
{
    juce::StringArray types;
    for (auto& e : session.getDeviceList().getEntries())
        if (e.origin != plugins::DeviceList::Origin::installed && e.available && e.device.stockType.isNotEmpty())
            types.addIfNotAlreadyThere (e.device.stockType);
    return types;
}

juce::var describe (Session& session, const juce::String& stockType)
{
    const plugins::DeviceList::Entry* entry = nullptr;
    for (auto& e : session.getDeviceList().getEntries())
        if (e.device.stockType == stockType && e.origin != plugins::DeviceList::Origin::installed)
            entry = &e;
    if (entry == nullptr)
        return object ({ { "type", stockType },
                         { "error", "no such device; --describe all lists them: " +
                                        describableTypes (session).joinIntoString (", ") } });
    if (! entry->available)
        return object ({ { "type", stockType },
                         { "error", entry->name + " is not installed here (Options > Get free plugins)" } });

    // Made for a moment, so its parameters can be read, then undone.
    auto device = object ({ { "kind", "stock" }, { "stockType", stockType } });
    juce::String error;
    auto command = entry->instrument
                       ? commands::CommandRegistry::get().create (
                             "channel.add", object ({ { "name", "Describe" }, { "instrument", device } }), error)
                       : commands::CommandRegistry::get().create (
                             "insert.addEffect", object ({ { "insert", 1 }, { "device", device } }), error);
    if (command == nullptr)
        return object ({ { "type", stockType }, { "error", error } });
    auto* raw = command.get();
    if (const auto done = session.getHistory().perform (std::move (command)); done.failed())
        return object ({ { "type", stockType }, { "error", done.getErrorMessage() } });
    const auto id = model::DeviceId { (uint32_t)(int)raw->getResult()["device"] };
    auto* instance = instanceOf (session, id);

    juce::Array<juce::var> parameters;
    for (auto& [pid, name] : session.getDeviceParameters (id))
    {
        model::ParameterTarget target;
        target.kind = model::ParameterTarget::Kind::deviceParameter;
        target.device = id;
        target.parameterId = pid;
        auto* values = new juce::DynamicObject();
        for (const auto at : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            values->setProperty (juce::String (at, 2), session.describeTargetValue (target, at));
        parameters.add (object ({ { "id", pid }, { "name", name }, { "at", juce::var (values) } }));
    }

    auto result = object ({ { "type", stockType },
                            { "name", entry->name },
                            { "maker", entry->maker },
                            { "instrument", entry->instrument },
                            { "parameters", parameters } });
    auto* out = result.getDynamicObject();

    if (stockType == "spinus.3x-osc")
    {
        juce::Array<juce::var> presets;
        for (auto& preset : dsp::threeOscPresets::all())
            presets.add (object (
                { { "name", preset.name }, { "category", preset.category }, { "description", preset.description } }));
        out->setProperty ("presets", presets);
        out->setProperty ("loadPreset", "{\"command\": \"device.loadPreset\", \"params\": {\"device\": <id>, "
                                        "\"preset\": \"Reese Bass\"}}");
    }
    if (auto* drums = dynamic_cast<dsp::DrumMachineProcessor*> (instance))
    {
        juce::Array<juce::var> pads;
        int number = 1;
        for (auto& pad : drums->getPads())
            pads.add (object ({ { "pad", number++ },
                                { "note", pad.note },
                                { "noteName", noteName (pad.note) },
                                { "name", pad.name },
                                { "chokeGroup", pad.chokeGroup } }));
        out->setProperty ("pads", pads);
        juce::Array<juce::var> kits;
        kits.add (object ({ { "name", "Spinus Synth Kit" }, { "description", "Built-in electronic drums" } }));
        for (auto& kit : dsp::starter::readKits (dsp::starter::findPack()))
            kits.add (object ({ { "name", kit.name }, { "description", kit.description } }));
        out->setProperty ("kits", kits);
        out->setProperty ("loadKit", "{\"action\": \"loadKit\", \"params\": {\"device\": <id>, \"kit\": \"Magpie\"}}");
    }
    if (dynamic_cast<dsp::SamplerProcessor*> (instance) != nullptr)
        out->setProperty ("loadSamples", "{\"action\": \"loadSamples\", \"params\": {\"device\": <id>, "
                                         "\"files\": [\"Piano C4.wav\"]}}");
    session.getHistory().undo();
    session.getEdit().dispatchPendingUpdatesSynchronously();
    return result;
}

//==============================================================================
juce::var measureFile (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
        return object ({ { "file", file.getFullPathName() } });

    dsp::LoudnessEngine meter;
    meter.prepare (reader->sampleRate);
    const int block = 4096;
    juce::AudioBuffer<float> buffer ((int)juce::jmin (2u, reader->numChannels), block);
    for (juce::int64 at = 0; at < reader->lengthInSamples; at += block)
    {
        const auto n = (int)juce::jmin ((juce::int64)block, reader->lengthInSamples - at);
        buffer.setSize (buffer.getNumChannels(), n, false, false, true);
        reader->read (&buffer, 0, n, at, true, true);
        meter.process (buffer);
    }
    const auto round1 = [] (float v) { return std::round ((double)v * 10.0) / 10.0; };
    return object ({ { "file", file.getFullPathName() },
                     { "seconds", std::round ((double)reader->lengthInSamples / reader->sampleRate * 100.0) / 100.0 },
                     { "lufs", round1 (meter.getIntegrated()) },
                     { "truePeakDb", round1 (meter.getTruePeakDb()) } });
}

int render (Session& session, const juce::File& audioFolder, const juce::File& out, const juce::File& stems,
            double tailSeconds, const Say& say)
{
    const auto exportTo = [&] (const juce::File& destination, bool asStems) -> std::optional<engine::ExportJob::Result>
    {
        engine::ExportSettings settings;
        const auto extension = destination.getFileExtension().toLowerCase();
        settings.format = extension == ".flac"   ? engine::ExportSettings::Format::flac
                          : extension == ".opus" ? engine::ExportSettings::Format::opus
                          : extension == ".mp3"  ? engine::ExportSettings::Format::mp3
                                                 : engine::ExportSettings::Format::wav24;
        settings.stems = asStems;
        settings.tailSeconds = tailSeconds;
        settings.audioFolder = audioFolder;
        destination.getParentDirectory().createDirectory();
        std::optional<engine::ExportJob::Result> result;
        auto job =
            session.createExportJob (destination, settings, [&] (const engine::ExportJob::Result& r) { result = r; });
        if (job == nullptr)
            return engine::ExportJob::Result { .error = "nothing to export" };
        job->start();
        while (! result)
            settle (20);
        return result;
    };

    const auto mix = exportTo (out, false);
    if (! mix || ! mix->ok)
    {
        say (line (object ({ { "ok", false }, { "error", mix ? mix->error : juce::String ("export failed") } })));
        return 1;
    }
    auto report = object ({ { "ok", true }, { "mix", measureFile (out) } });
    if (stems != juce::File())
    {
        const auto parts = exportTo (stems.getChildFile ("Mix" + out.getFileExtension()), true);
        if (! parts || ! parts->ok)
        {
            say (
                line (object ({ { "ok", false }, { "error", parts ? parts->error : juce::String ("stems failed") } })));
            return 1;
        }
        juce::Array<juce::var> files;
        for (auto& f : parts->files)
            files.add (measureFile (f));
        report.getDynamicObject()->setProperty ("stems", files);
    }
    say (line (report));
    return 0;
}

} // namespace spinus::cli
