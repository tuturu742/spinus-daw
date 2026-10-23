#include "SynthMatch.h"

#include "ProgramFolder.h"
#include "model/StockDevices.h"

#include <set>

namespace spinus
{

//==============================================================================
namespace
{
// Built-in profiles: the settings that shape a sound most, leaving out pitch, routing, effects and modulation.
// The search tries each setting alone first, in this order, so the newer ones start with the filter.
// Envelope times stop short of their longest (Odin 2's attack at 0.45 is about 0.7 s, OB-Xf's at 0.4 about
// 1 s), and the first oscillator never goes quiet.
constexpr const char* builtInProfiles[] = {
    R"({ "name": "3x Osc", "stockType": "spinus.3x-osc", "start": "default", "tries": 500, "settings": [
        { "id": "osc1_shape", "steps": 5 }, { "id": "osc1_level" },
        { "id": "osc2_shape", "steps": 5 }, { "id": "osc2_level" }, { "id": "osc2_fine", "min": 0.4, "max": 0.6 },
        { "id": "osc3_shape", "steps": 5 }, { "id": "osc3_level" }, { "id": "osc3_fine", "min": 0.4, "max": 0.6 },
        { "id": "attack", "max": 0.3 }, { "id": "decay", "max": 0.5 }, { "id": "sustain" }, { "id": "release", "max": 0.3 },
        { "id": "filter", "steps": 4 }, { "id": "cutoff" }, { "id": "resonance", "max": 0.9 }, { "id": "envelope" } ] })",
    R"({ "name": "Surge XT", "stockType": "bundled.surge-xt", "start": "closestPreset", "tries": 800, "settings": [
        { "name": "A Osc 1 Shape" }, { "name": "A Osc 1 Width 1" }, { "name": "A Osc 1 Volume" },
        { "name": "A Osc 2 Shape" }, { "name": "A Osc 2 Width 1" }, { "name": "A Osc 2 Volume" },
        { "name": "A Osc 3 Volume" }, { "name": "A Noise Volume" },
        { "name": "A Filter 1 Cutoff" }, { "name": "A Filter 1 Resonance", "max": 0.9 },
        { "name": "A Filter 1 FEG Mod Amount" }, { "name": "A Filter 2 Cutoff" },
        { "name": "A Amp EG Attack", "max": 0.6 }, { "name": "A Amp EG Decay" }, { "name": "A Amp EG Sustain" },
        { "name": "A Amp EG Release", "max": 0.7 },
        { "name": "A Filter EG Attack", "max": 0.6 }, { "name": "A Filter EG Decay" },
        { "name": "A Filter EG Sustain" } ] })",
    // Odin 2's oscillator and filter types are not parameters: its default sound (an analog saw through a
    // 24 dB low pass) stays, with its levels, pulse width, filter and envelopes tuned. Osc1 Volume 0.833 is
    // 0 dB.
    R"({ "name": "Odin 2", "stockType": "bundled.odin2", "plugin": "Odin2", "start": "default", "tries": 600,
        "settings": [
        { "name": "Filter1 Frequency", "min": 0.2 }, { "name": "Filter1 Resonance", "max": 0.9 },
        { "name": "Filter1 Envelope Amount" }, { "name": "Filter1 Saturation", "max": 0.6 },
        { "name": "Filter1 Keyboard Follow" },
        { "name": "AmpEnv Attack", "max": 0.45 }, { "name": "AmpEnv Decay", "max": 0.6 }, { "name": "AmpEnv Sustain" },
        { "name": "AmpEnv Release", "max": 0.5 },
        { "name": "FilterEnv Attack", "max": 0.45 }, { "name": "FilterEnv Decay", "max": 0.6 },
        { "name": "FilterEnv Sustain" }, { "name": "FilterEnv Release", "max": 0.5 },
        { "name": "Osc1 Volume", "min": 0.55, "max": 0.9 }, { "name": "Osc1 PulseWidth", "min": 0.05, "max": 0.95 },
        { "name": "Osc1 Wavetble Position" }, { "name": "Osc1 Drift", "max": 0.5 } ] })",
    // OB-Xf: two oscillators (saw and pulse, each on or off), noise, its filter and two envelopes. Osc 1
    // Volume 0.5 is -18 dB.
    R"({ "name": "OB-Xf", "stockType": "bundled.ob-xf", "plugin": "OB-Xf", "start": "default", "tries": 600,
        "settings": [
        { "name": "Filter Cutoff", "min": 0.15 }, { "name": "Filter Resonance", "max": 0.9 },
        { "name": "Filter Env Amount" }, { "name": "Filter 4-Pole Mode", "steps": 2 },
        { "name": "Amp Env Attack", "max": 0.4 }, { "name": "Amp Env Decay", "max": 0.55 }, { "name": "Amp Env Sustain" },
        { "name": "Amp Env Release", "max": 0.45 },
        { "name": "Filter Env Attack", "max": 0.4 }, { "name": "Filter Env Decay", "max": 0.55 },
        { "name": "Filter Env Sustain" }, { "name": "Filter Env Release", "max": 0.45 },
        { "name": "Osc 1 Saw Wave", "steps": 2 }, { "name": "Osc 1 Pulse Wave", "steps": 2 },
        { "name": "Osc 2 Saw Wave", "steps": 2 }, { "name": "Osc Pulsewidth", "max": 0.9 },
        { "name": "Osc 1 Volume", "min": 0.5 }, { "name": "Osc 2 Volume" }, { "name": "Osc 2 Detune", "max": 0.3 },
        { "name": "Noise Volume", "max": 0.75 } ] })",
};

bool isBuiltInStockType (const juce::String& stockType)
{
    for (auto* text : builtInProfiles)
        if (juce::JSON::parse (text)["stockType"].toString() == stockType)
            return true;
    return false;
}

/** The plugin's own name inside a downloaded device's file ("Odin2" for "bundled.odin2"). */
juce::String bundledPluginName (const juce::String& stockType)
{
    for (auto& d : model::getBundledDevices())
        if (stockType == d.stockType)
            return d.pluginName;
    return {};
}
} // namespace

std::optional<SynthProfile> SynthProfile::fromJson (const juce::var& json)
{
    SynthProfile profile;
    profile.name = json["name"].toString();
    profile.stockType = json["stockType"].toString();
    profile.plugin = json["plugin"].toString();
    profile.manufacturer = json["manufacturer"].toString();
    profile.format = json["format"].toString();
    profile.tries = juce::jlimit (0, 100000, (int)json["tries"]);
    if (profile.name.isEmpty())
        profile.name = profile.plugin;
    if (json.hasProperty ("start"))
        profile.start = json["start"].toString();
    if ((profile.stockType.isEmpty() && profile.plugin.isEmpty()) ||
        (profile.start != "default" && profile.start != "closestPreset"))
        return std::nullopt;
    if (auto* list = json["settings"].getArray())
        for (auto& s : *list)
        {
            Setting setting;
            setting.id = s["id"].toString();
            setting.name = s["name"].toString();
            setting.min = s.hasProperty ("min") ? juce::jlimit (0.0f, 1.0f, (float)s["min"]) : 0.0f;
            setting.max = s.hasProperty ("max") ? juce::jlimit (0.0f, 1.0f, (float)s["max"]) : 1.0f;
            setting.steps = juce::jmax (0, (int)s["steps"]);
            if ((setting.id.isEmpty() && setting.name.isEmpty()) || setting.max < setting.min)
                return std::nullopt;
            profile.settings.push_back (setting);
        }
    if (profile.settings.empty())
        return std::nullopt;
    return profile;
}

bool SynthProfile::isFor (const model::Device& device) const
{
    if (device.kind == model::Device::Kind::stock)
    {
        if (stockType.isNotEmpty())
            return stockType == device.stockType;
        // A downloaded plugin, by its own name (it is a CLAP file).
        const auto own = bundledPluginName (device.stockType);
        return own.isNotEmpty() && plugin.equalsIgnoreCase (own) && manufacturer.isEmpty() &&
               (format.isEmpty() || format.equalsIgnoreCase ("CLAP"));
    }
    return plugin.isNotEmpty() && plugin.equalsIgnoreCase (device.plugin.name) &&
           (manufacturer.isEmpty() || manufacturer.equalsIgnoreCase (device.plugin.manufacturer)) &&
           (format.isEmpty() || format.equalsIgnoreCase (device.plugin.format));
}

juce::File SynthProfile::getUserFolder() { return getUserDataRoot().getChildFile ("spinus/synth-profiles"); }

std::vector<SynthProfile> SynthProfile::getAll()
{
    std::vector<SynthProfile> all;
    auto sameSynth = [] (const SynthProfile& a, const SynthProfile& b)
    {
        if (a.stockType.isNotEmpty() || b.stockType.isNotEmpty())
            return a.stockType == b.stockType;
        return a.plugin.equalsIgnoreCase (b.plugin) && a.manufacturer.equalsIgnoreCase (b.manufacturer) &&
               a.format.equalsIgnoreCase (b.format);
    };
    auto add = [&all, &sameSynth] (const SynthProfile& p)
    {
        auto same = std::find_if (all.begin(), all.end(), [&] (auto& q) { return sameSynth (p, q); });
        if (same != all.end())
            *same = p;
        else
            all.push_back (p);
    };
    for (auto* text : builtInProfiles)
        if (auto p = fromJson (juce::JSON::parse (text)))
            add (*p);
    auto files = getUserFolder().findChildFiles (juce::File::findFiles, false, "*.json");
    files.sort();
    for (auto& file : files)
        if (auto p = fromJson (juce::JSON::parse (file)))
            add (*p);
    return all;
}

std::optional<SynthProfile> SynthProfile::find (const juce::String& stockType)
{
    model::Device device;
    device.kind = model::Device::Kind::stock;
    device.stockType = stockType;
    return find (device);
}

std::optional<SynthProfile> SynthProfile::find (const model::Device& device)
{
    // The user's come after Spinus's: the last one that fits is the user's own when there is one.
    std::optional<SynthProfile> found;
    for (auto& p : getAll())
        if (p.isFor (device))
            found = p;
    return found;
}

bool SynthProfile::isTested (const model::Device& device)
{
    return device.kind == model::Device::Kind::stock && isBuiltInStockType (device.stockType);
}

//==============================================================================
SynthProfile::Parameter SynthProfile::describe (const juce::AudioProcessorParameter& p)
{
    Parameter d;
    d.id = SynthMatcher::getParameterId (p);
    d.name = p.getName (200);
    d.numSteps = p.getNumSteps();
    d.isBoolean = p.isBoolean();
    d.isAutomatable = p.isAutomatable();
    d.isMeta = p.isMetaParameter();
    return d;
}

namespace
{
/** A parameter's name as lower-case words: "FilterEnv Attack" -> filter, env, attack; "Osc1 PulseWidth" ->
    osc, 1, pulse, width; "VCFCutoff" -> vcf, cutoff. */
juce::StringArray toWords (const juce::String& name)
{
    juce::StringArray result;
    juce::String word;
    auto flush = [&]
    {
        if (word.isNotEmpty())
            result.add (word.toLowerCase());
        word.clear();
    };
    const auto text = name.toUTF32();
    const auto length = (int)text.length();
    using C = juce::CharacterFunctions;
    for (int i = 0; i < length; ++i)
    {
        const auto c = text[i];
        if (! C::isLetterOrDigit (c))
        {
            flush();
            continue;
        }
        if (word.isNotEmpty())
        {
            const auto last = word.getLastCharacter();
            const bool digits = C::isDigit (c) != C::isDigit (last);
            const bool camel = C::isUpperCase (c) && C::isLowerCase (last);
            const bool acronymEnds =
                C::isUpperCase (c) && C::isUpperCase (last) && i + 1 < length && C::isLowerCase (text[i + 1]);
            if (digits || camel || acronymEnds)
                flush();
        }
        word += juce::String::charToString (c);
    }
    flush();
    return result;
}

/** What a setting does, most important first (the order an automatic profile takes them in). */
enum class Kind
{
    cutoff,
    resonance,
    envelopeAmount,
    ampEnvelope,
    filterEnvelope,
    shape,
    level,
    pulseWidth,
    detune,
    drive,
    otherEnvelope,
    fm,
    filterType,
    lfoRate, // only when there is nothing else
    none
};

struct Kinded
{
    Kind kind = Kind::none;
    float min = 0.0f, max = 1.0f;
};

Kinded classify (const juce::String& name)
{
    const auto words = toWords (name);
    const auto joined = words.joinIntoString ({});
    auto has = [&words] (std::initializer_list<const char*> list)
    { return std::any_of (list.begin(), list.end(), [&] (auto* x) { return words.contains (x); }); };
    auto contains = [&joined] (std::initializer_list<const char*> list)
    { return std::any_of (list.begin(), list.end(), [&] (auto* x) { return joined.contains (x); }); };

    // Not the sound's shape: the level of the whole, tuning, voices and MIDI, switches, effects, routing and
    // modulation, arpeggiators and sequencers, presets, stereo, and the plugin's own housekeeping.
    if (has ({ "master",     "output",     "out",       "main",        "tune",       "tuning",      "fine",
               "finetune",   "coarse",     "transpose", "octave",      "oct",        "semi",        "semis",
               "semitone",   "semitones",  "pitch",     "bend",        "pb",         "glide",       "porta",
               "portamento", "midi",       "channel",   "voice",       "voices",     "poly",        "polyphony",
               "mono",       "legato",     "priority",  "bypass",      "enable",     "enabled",     "on",
               "off",        "active",     "power",     "mute",        "solo",       "reverb",      "delay",
               "chorus",     "flanger",    "phaser",    "echo",        "fx",         "effect",      "effects",
               "send",       "dry",        "wet",       "drywet",      "eq",         "compressor",  "comp",
               "limiter",    "distortion", "arp",       "arpeggiator", "seq",        "sequencer",   "step",
               "preset",     "program",    "patch",     "bank",        "pan",        "panorama",    "panning",
               "stereo",     "spread",     "reset",     "sync",        "retrigger",  "trigger",     "retrig",
               "phase",      "random",     "drift",     "slop",        "quality",    "hq",          "curve",
               "slope",      "loop",       "velocity",  "vel",         "keytrack",   "key",         "kbd",
               "keyboard",   "follow",     "modwheel",  "wheel",       "aftertouch", "macro",       "xy",
               "vibrato",    "tempo",      "bpm",       "invert",      "inv",        "polarity",    "to",
               "input",      "route",      "routing",   "source",      "dest",       "destination", "target",
               "matrix",     "learn",      "pad" }) ||
        contains ({ "transpose", "octave", "semitone", "polyphon", "portamento", "pitchbend", "bypass", "reverb",
                    "chorus", "flanger", "phaser", "arpeg", "preset", "keytrack", "keyfollow", "modwheel", "velocity",
                    "panorama", "drywet" }))
        return {};
    if (has ({ "unison" }) && ! has ({ "detune" }))
        return {}; // how many voices

    const bool lfo = contains ({ "lfo" });
    const bool filter = has ({ "filter", "flt", "filt", "vcf", "lpf", "hpf", "feg", "fenv", "cutoff" });
    const bool envelope = has ({ "env", "envelope", "eg", "adsr", "feg", "aeg", "fenv", "aenv", "contour" });
    const bool amp = has ({ "amp", "vca", "aeg", "aenv", "amplifier" });
    const bool otherEnvelope = has ({ "mod", "global", "aux", "pitch" }) || contains ({ "env2", "env3", "eg2", "eg3" });
    const bool osc = has ({ "osc", "oscillator", "op", "operator", "sub", "noise", "ring", "wave", "saw", "pulse",
                            "square", "tri", "sine" }) ||
                     contains ({ "osc" });
    const bool amount = has ({ "amount", "amt", "depth", "mod", "int", "intensity" });

    const bool attack = has ({ "attack", "att", "atk" }) || contains ({ "attack" });
    const bool decay = has ({ "decay", "dec", "dcy" }) || contains ({ "decay" });
    const bool sustain = has ({ "sustain", "sus", "sust" }) || contains ({ "sustain" });
    const bool release = has ({ "release", "rel", "rls" }) || contains ({ "release" });
    const bool stage = attack || decay || sustain || release;

    if (lfo)
        return has ({ "rate", "freq", "frequency", "speed" }) ? Kinded { Kind::lfoRate } : Kinded {};
    if (envelope && amount && ! stage)
        return otherEnvelope && ! filter ? Kinded {} : Kinded { Kind::envelopeAmount };

    // Envelope stages: the amp's, then the filter's, then any other's. Times stop short of their longest.
    if (stage)
    {
        const auto max = attack ? 0.5f : decay ? 0.7f : release ? 0.6f : 1.0f;
        if (otherEnvelope && ! amp && ! filter)
            return { Kind::otherEnvelope, 0.0f, max };
        if (filter && ! amp)
            return { Kind::filterEnvelope, 0.0f, max };
        return { Kind::ampEnvelope, 0.0f, max };
    }
    if (has ({ "mod", "modulation" }))
        return has ({ "cross", "fm" }) ? Kinded { Kind::fm, 0.0f, 0.5f } : Kinded {};

    if (has ({ "cutoff", "cut" }) || contains ({ "cutoff" }) ||
        (filter && has ({ "freq", "frequency", "fc", "hz", "brightness" })))
        return { Kind::cutoff, 0.05f, 1.0f };
    if (has ({ "resonance", "reso", "emphasis", "peak" }) || contains ({ "resonan" }) ||
        (filter && has ({ "res", "q" })))
        return { Kind::resonance, 0.0f, 0.85f };
    if (filter && has ({ "type", "kind", "model" }))
        return { Kind::filterType };
    if (has ({ "drive", "saturation", "sat", "overdrive", "warmth" }))
        return { Kind::drive, 0.0f, 0.6f };
    if (has ({ "detune", "detuning" }) || contains ({ "detune" }))
        return { Kind::detune, 0.0f, 0.4f };
    if (has ({ "pw", "pwm", "pulsewidth" }) || contains ({ "pulsewidth" }) ||
        (has ({ "pulse" }) && has ({ "width" })) || (osc && has ({ "width" })))
        return { Kind::pulseWidth, 0.05f, 0.95f };
    if (has ({ "fm", "crossmod", "xmod" }) || contains ({ "crossmod" }))
        return { Kind::fm, 0.0f, 0.5f };
    if (has ({ "wave", "waveform", "shape", "wavetable", "wt", "morph", "timbre", "table" }) ||
        contains ({ "waveform", "wavetable" }) || (osc && has ({ "type", "position", "pos" })))
        return filter ? Kinded {} : Kinded { Kind::shape };
    const bool level = has ({ "level", "lvl", "vol", "volume", "gain", "mix", "amount", "balance" });
    if (has ({ "noise" }) && (level || words.size() == 1))
        return { Kind::level, 0.0f, 0.6f };
    if (osc && level)
        return { Kind::level };
    return {};
}
} // namespace

std::optional<SynthProfile> SynthProfile::makeAutomatic (const juce::String& synthName,
                                                         const std::vector<Parameter>& parameters)
{
    // How many of each kind at most: two filters' cutoffs, resonances and envelope amounts, the four stages
    // of each envelope, a few oscillators.
    const std::map<Kind, size_t> most {
        { Kind::cutoff, 2 },         { Kind::resonance, 2 }, { Kind::envelopeAmount, 2 }, { Kind::ampEnvelope, 4 },
        { Kind::filterEnvelope, 4 }, { Kind::shape, 3 },     { Kind::level, 4 },          { Kind::pulseWidth, 2 },
        { Kind::detune, 2 },         { Kind::drive, 1 },     { Kind::otherEnvelope, 4 },  { Kind::fm, 2 },
        { Kind::filterType, 1 },     { Kind::lfoRate, 1 }
    };
    std::map<Kind, std::vector<Setting>> byKind;
    std::set<juce::String> seen;
    for (auto& p : parameters)
    {
        // Switches (two steps) and the plugin's own housekeeping are not sound.
        if (! p.isAutomatable || p.isMeta || p.isBoolean || p.numSteps < 3 || p.name.trim().isEmpty())
            continue;
        const auto kinded = classify (p.name);
        if (kinded.kind == Kind::none || ! seen.insert (p.id).second)
            continue;
        auto& list = byKind[kinded.kind];
        if (list.size() >= most.at (kinded.kind))
            continue;
        Setting s;
        s.id = p.id;
        s.name = p.name;
        // A few choices (a wave, a filter type): each one tried. A continuous one: within its range.
        if (p.numSteps <= 24)
            s.steps = p.numSteps;
        else
        {
            s.min = kinded.min;
            s.max = kinded.max;
        }
        list.push_back (s);
    }

    // The first oscillator's level never goes quiet (the sound would be gone).
    if (auto levels = byKind.find (Kind::level); levels != byKind.end())
        for (auto& s : levels->second)
            if (s.steps == 0 && ! s.name.containsIgnoreCase ("noise"))
            {
                s.min = 0.3f;
                break;
            }

    SynthProfile profile;
    profile.name = synthName;
    profile.automatic = true;
    for (int k = 0; k < (int)Kind::lfoRate; ++k)
        if (auto found = byKind.find ((Kind)k); found != byKind.end())
            for (auto& s : found->second)
                if ((int)profile.settings.size() < mostAutomaticSettings)
                    profile.settings.push_back (s);
    // An LFO's speed only when there is nothing else to tune.
    if ((int)profile.settings.size() < fewestSettings)
        if (auto found = byKind.find (Kind::lfoRate); found != byKind.end())
            profile.settings.push_back (found->second.front());
    if ((int)profile.settings.size() < fewestSettings)
        return std::nullopt;
    return profile;
}

} // namespace spinus
