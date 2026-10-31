#include "AgentCommandLine.h"

#include "commands/CommandRegistry.h"
#include "dsp/drums/DrumMachineProcessor.h"
#include "dsp/drums/StarterPack.h"
#include "dsp/loudness/LoudnessEngine.h"
#include "dsp/sampler/SamplerProcessor.h"
#include "dsp/ParameterText.h"
#include "dsp/threeosc/ThreeOscPresets.h"
#include "engine/EngineAdapter.h"
#include "engine/Exporter.h"
#include "engine/ParameterIds.h"
#include "model/ModelJson.h"
#include "model/StockDevices.h"
#include "session/Session.h"

#include <tracktion_engine/tracktion_engine.h>

#include <map>
#include <optional>
#include <set>

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

/** Automation points given as `text` ("-6 dB", "30% L", "1.2 kHz") instead of `value`, read for the clip's
    target as the control shows it (Session::parseTargetValue). The params as they were when nothing needs it. */
juce::var readPointTexts (Session& session, const juce::String& command, const juce::var& params, juce::String& error)
{
    if (command != "clip.addAutomation" && command != "clip.setPoints")
        return params;
    const auto* points = params["points"].getArray();
    if (points == nullptr)
        return params;
    bool any = false;
    for (auto& point : *points)
        any = any || point.hasProperty ("text");
    if (! any)
        return params;

    model::ParameterTarget target;
    if (command == "clip.addAutomation")
    {
        if (auto e = model::targetFromJson (params["target"], target); e.isNotEmpty())
            return params; // the command says what is wrong with it
    }
    else
    {
        const auto* clip = session.getProject().findClip (model::ClipId { (uint32_t)(int)params["clip"] });
        const auto* automation = clip != nullptr ? std::get_if<model::AutomationClip> (&clip->content) : nullptr;
        if (automation == nullptr)
            return params;
        target = automation->target;
    }

    // A pan as "30% L", "20% R", "C" (or "-30" to "30", left negative): 0 is left, 0.5 the middle, 1 right.
    const auto readPan = [] (const juce::String& text) -> std::optional<float>
    {
        const auto t = text.trim().toUpperCase();
        if (t == "C" || t == "CENTER" || t == "CENTRE" || t == "0" || t == "0%")
            return 0.5f;
        if (! t.containsAnyOf ("0123456789"))
            return std::nullopt;
        auto amount = t.retainCharacters ("0123456789.").getFloatValue();
        if (t.endsWith ("L") || t.startsWith ("-"))
            amount = -amount;
        else if (! t.endsWith ("R") && ! t.startsWith ("+"))
            return std::nullopt; // "30%": which side?
        return juce::jlimit (0.0f, 1.0f, 0.5f + amount / 200.0f);
    };
    const bool pan = target.kind == model::ParameterTarget::Kind::channelPan ||
                     target.kind == model::ParameterTarget::Kind::insertPan;

    auto copy = juce::JSON::parse (juce::JSON::toString (params));
    for (auto& point : *copy["points"].getArray())
        if (auto* o = point.getDynamicObject(); o != nullptr && o->hasProperty ("text"))
        {
            const auto text = o->getProperty ("text").toString();
            const auto value = pan ? readPan (text) : session.parseTargetValue (target, text);
            if (! value)
            {
                juce::StringArray shown;
                for (auto v : { 0.0f, 0.5f, 1.0f })
                    shown.add ("\"" + session.describeTargetValue (target, v) + "\" at " + juce::String (v));
                error = "cannot read point text \"" + text + "\" for " + session.describeTarget (target) +
                        " (it shows " + shown.joinIntoString (", ") + "; or give 'value' from 0 to 1)";
                return params;
            }
            o->removeProperty ("text");
            o->setProperty ("value", *value);
        }
    return copy;
}

} // namespace

juce::var readStepTexts (Session& session, const juce::var& steps, juce::String& error)
{
    const auto* list = steps.getArray();
    if (list == nullptr)
        return steps;
    juce::Array<juce::var> out;
    for (auto& step : *list)
    {
        const auto key = step.hasProperty ("params") ? "params" : "parameters";
        const auto read = readPointTexts (session, step["command"].toString(), step[key], error);
        if (read == step[key])
        {
            out.add (step);
            continue;
        }
        auto copy = step.clone();
        if (auto* o = copy.getDynamicObject())
            o->setProperty (key, read);
        out.add (copy);
    }
    return out;
}

namespace
{
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
        else if (params = readPointTexts (session, command, params, error); error.isNotEmpty())
        {
        }
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

    // The channels something plays: a pattern clip (Playlist or launcher) with notes for it, or an audio clip.
    std::set<model::ChannelId> used;
    const auto playedBy = [&] (const auto& content)
    {
        if (const auto* clip = std::get_if<model::PatternClip> (&content))
        {
            if (const auto* pattern = p.findPattern (clip->pattern))
                for (auto& [channel, notes] : pattern->notes)
                    if (! notes.empty())
                        used.insert (channel);
        }
        else if (const auto* audio = std::get_if<model::AudioClip> (&content))
            used.insert (audio->channel);
    };
    for (auto& clip : p.playlist.clips)
        playedBy (clip.content);
    for (auto& clip : p.launcher.clips)
        playedBy (clip.content);

    juce::Array<juce::var> channels, unused;
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
                                { "volumeDb", c.volumeDb },
                                { "used", used.count (c.id) > 0 } }));
        if (used.count (c.id) == 0)
            unused.add (c.name);
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
          { "unusedChannels", unused }, // channels no clip plays: not a problem, but often a mistake
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

juce::var notInstalled (Session& session)
{
    juce::Array<juce::var> missing;
    juce::StringArray seen;
    for (auto& e : session.getDeviceList().getEntries())
        if (e.origin != plugins::DeviceList::Origin::installed && ! e.available && e.device.stockType.isNotEmpty() &&
            ! seen.contains (e.device.stockType))
        {
            seen.add (e.device.stockType);
            missing.add (
                object ({ { "type", e.device.stockType }, { "name", e.name }, { "instrument", e.instrument } }));
        }
    return object ({ { "notInstalled", missing },
                     { "hint", "Free plugins Spinus can download: in Spinus, Options > Get free plugins. Spinus's own "
                               "effects have no reverb (its delay is spinus.delay); the Dragonfly reverbs and Surge XT "
                               "Effects (chorus, more delays) are among these." } });
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
    std::optional<engine::ParameterFinder> finder;
    if (instance != nullptr)
        finder.emplace (*instance);
    for (auto& [pid, name] : session.getDeviceParameters (id))
    {
        model::ParameterTarget target;
        target.kind = model::ParameterTarget::Kind::deviceParameter;
        target.device = id;
        target.parameterId = pid;
        auto* values = new juce::DynamicObject();
        for (const auto at : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            values->setProperty (juce::String (at, 2), session.describeTargetValue (target, at));
        auto parameter = object ({ { "id", pid }, { "name", name }, { "at", juce::var (values) } });
        // A choice or switch: every value by name, as device.setParameters' text takes it.
        if (auto* p = finder ? finder->find (pid) : nullptr)
            if (const auto choices = dsp::param::choicesOf (*p); ! choices.isEmpty())
            {
                juce::Array<juce::var> names;
                for (auto& c : choices)
                    names.add (c);
                parameter.getDynamicObject()->setProperty ("choices", names);
            }
        parameters.add (parameter);
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

    // In 100 ms steps: the integrated loudness (gated: only where it sounds), and how long it sounds
    // (momentary loudness above -60 LUFS), so a short hit doesn't read as a loud part.
    dsp::LoudnessEngine meter;
    meter.prepare (reader->sampleRate);
    const int block = juce::jmax (1, (int)std::round (reader->sampleRate / 10.0));
    juce::AudioBuffer<float> buffer ((int)juce::jmin (2u, reader->numChannels), block);
    int steps = 0, sounding = 0;
    for (juce::int64 at = 0; at < reader->lengthInSamples; at += block)
    {
        const auto n = (int)juce::jmin ((juce::int64)block, reader->lengthInSamples - at);
        buffer.setSize (buffer.getNumChannels(), n, false, false, true);
        reader->read (&buffer, 0, n, at, true, true);
        meter.process (buffer);
        ++steps;
        if (meter.getMomentary() > -60.0f)
            ++sounding;
    }
    const auto round1 = [] (double v) { return std::isfinite (v) ? std::round (v * 10.0) / 10.0 : -100.0; };
    const auto seconds = (double)reader->lengthInSamples / reader->sampleRate;
    return object ({ { "file", file.getFullPathName() },
                     { "seconds", std::round (seconds * 100.0) / 100.0 },
                     { "lufs", round1 (meter.getIntegrated()) },
                     { "truePeakDb", round1 (meter.getTruePeakDb()) },
                     { "soundingSeconds", round1 (seconds * (double)sounding / (double)juce::jmax (1, steps)) } });
}

int render (Session& session, const juce::File& audioFolder, const juce::File& out, const juce::File& stems,
            double tailSeconds, const Say& say)
{
    const auto extension = out.getFileExtension().toLowerCase();
    engine::ExportSettings settings;
    settings.format = extension == ".flac"   ? engine::ExportSettings::Format::flac
                      : extension == ".opus" ? engine::ExportSettings::Format::opus
                      : extension == ".mp3"  ? engine::ExportSettings::Format::mp3
                                             : engine::ExportSettings::Format::wav24;
    const bool encoded = engine::ExportJob::isEncodedFormat (settings.format);
    const auto failed = [&] (const juce::String& error)
    {
        say (line (object ({ { "ok", false }, { "error", error } })));
        return 1;
    };

    // One render for the mix and its stems, as WAV into a scratch folder; then each file is encoded (or
    // moved) to where it goes: the mix to `out`, the stems straight into `stems` as "NN <insert>.<ext>".
    const auto scratch = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("spinus-render-" + juce::Uuid().toString());
    struct Cleanup
    {
        juce::File folder;
        ~Cleanup() { folder.deleteRecursively(); }
    } cleanup { scratch };
    scratch.createDirectory();
    const auto mixWav = scratch.getChildFile ("Mix.wav");

    auto wavSettings = settings;
    wavSettings.format = engine::ExportSettings::Format::wav24;
    wavSettings.stems = stems != juce::File();
    wavSettings.tailSeconds = tailSeconds;
    wavSettings.audioFolder = audioFolder;
    std::optional<engine::ExportJob::Result> result;
    auto job = session.createExportJob (mixWav, wavSettings, [&] (const engine::ExportJob::Result& r) { result = r; });
    if (job == nullptr)
        return failed ("nothing to export");
    job->start();
    while (! result)
        settle (20);
    job = nullptr;
    if (! result->ok)
        return failed (result->error);

    const auto place = [&] (const juce::File& wav, const juce::File& target) -> juce::String
    {
        target.getParentDirectory().createDirectory();
        if (! encoded && settings.format == engine::ExportSettings::Format::wav24)
            return target.deleteFile() && wav.moveFileTo (target) ? juce::String()
                                                                  : "could not write " + target.getFullPathName();
        return engine::ExportJob::encodeFile (wav, target, settings, session.getProject().settings);
    };

    // An encoded file's peaks can be about 1 dB above the render's: both are reported.
    const auto unencodedPeak = encoded ? measureFile (mixWav)["truePeakDb"] : juce::var();
    if (const auto e = place (mixWav, out); e.isNotEmpty())
        return failed (e);
    auto mixReport = measureFile (out);
    if (encoded)
        mixReport.getDynamicObject()->setProperty ("unencodedTruePeakDb", unencodedPeak);
    auto report = object ({ { "ok", true }, { "mix", mixReport } });

    if (stems != juce::File())
    {
        juce::Array<juce::var> files;
        for (size_t i = 1; i < result->files.size(); ++i) // the mix first, then the stems
        {
            const auto& wav = result->files[i];
            const auto target = stems.getChildFile (wav.getFileNameWithoutExtension() + extension);
            if (const auto e = place (wav, target); e.isNotEmpty())
                return failed (e);
            files.add (measureFile (target));
        }
        report.getDynamicObject()->setProperty ("stems", files);
    }
    say (line (report));
    return 0;
}

//==============================================================================
namespace
{
/** Spinus's options, and whether each takes a value after it. */
const std::map<juce::String, bool>& knownOptions()
{
    static const std::map<juce::String, bool> options {
        { "--help", false },     { "-h", false },         { "--version", false },    { "--run", true },
        { "--open", true },      { "--save", true },      { "--keep-going", false }, { "--check", true },
        { "--render", true },    { "--out", true },       { "--stems", true },       { "--tail", true },
        { "--describe", true },  { "--commands", false }, { "--self-test", false },  { "--scan-plugins", false },
        { "--no-audio", false }, { "--snapshot", true },  { "--choose-file", true },
    };
    return options;
}
} // namespace

juce::String usage()
{
    return "Spinus DAW, from the command line (no window, no sound card):\n"
           "  spinus [<project.spn or folder>]           Open Spinus (with that project)\n"
           "  spinus --run <script.json> [--open <project>] [--save <folder>] [--keep-going]\n"
           "                                              Run a script of commands; without --save, a dry run\n"
           "  spinus --check <project>                   What a project holds, and what is missing here\n"
           "  spinus --render <project> --out <file.wav|.flac|.opus|.mp3> [--stems <folder>] [--tail <seconds>]\n"
           "                                              Render the song (and one file per mixer insert)\n"
           "  spinus --describe <device>|all             A device's settings, choices, presets, pads and kits\n"
           "  spinus --commands                          Every command and its parameters (JSON Schema)\n"
           "  spinus --self-test [<folder>]              Check that Spinus works on this computer\n"
           "  spinus --version                           The version\n"
           "  spinus --help                              This\n"
           "Each option prints JSON lines. The manual's page \"Spinus from the command line\" (command-line.md)\n"
           "explains scripts, with an example song.\n";
}

juce::String unknownOption (const juce::StringArray& args)
{
    const auto& options = knownOptions();
    for (int i = 0; i < args.size(); ++i)
    {
        const auto arg = args[i].unquoted();
        if (arg == "--" || ! (arg.startsWith ("--") || arg == "-h"))
            continue; // a project, or the value of the option before it
        const auto found = options.find (arg);
        if (found == options.end())
            return arg;
        if (found->second)
            ++i; // its value, whatever it looks like
    }
    return {};
}

} // namespace spinus::cli
