#include "PartSounds.h"

#include "PresetPreviews.h"
#include "ProgramFolder.h"
#include "StemSamples.h"
#include "UserRoles.h"
#include "dsp/sampler/SampleStorage.h"
#include "engine/EngineAdapter.h"
#include "model/Model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <tuple>

namespace spinus::parts
{

namespace
{
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [key, value] : items)
        o->setProperty (key, value);
    return juce::var (o);
}

double noteFrequency (double note) { return 440.0 * std::pow (2.0, (note - 69.0) / 12.0); }

juce::String noteName (int note)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (names[((note % 12) + 12) % 12]) + juce::String (note / 12 - 1);
}

juce::String timeText (double seconds)
{
    return seconds < 1.0 ? juce::String (juce::roundToInt (seconds * 1000.0)) + " ms"
                         : juce::String (seconds, 1) + " s";
}

double median (std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort (v.begin(), v.end());
    const auto n = v.size();
    return n % 2 == 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double logTime (double seconds) { return std::log10 (juce::jmax (0.0, seconds) + 0.003); }

/** A note's attack on a log scale where everything under about 20 ms counts as nearly instant (a stem's notes
    measure a few milliseconds slower than they are: the note before still rings as the next starts). */
double logAttack (double seconds) { return std::log10 (juce::jmax (0.0, seconds) + 0.02); }

/** A preset's brightness in harmonics at a note: its test notes' (C1 to C7), between the two nearest that
    sounded; else what the held C3 measured. */
double presetHarmonicsAt (const juce::var& sound, int note)
{
    const auto* levels = sound["registerBrightness"].getArray();
    if (levels != nullptr && levels->size() == (int)sounds::TestNotes::probes.size())
    {
        const auto x = juce::jlimit (0.0, (double)levels->size() - 1.0,
                                     ((double)note - (double)sounds::TestNotes::probes.front()) / 12.0);
        const auto i0 = (int)std::floor (x), i1 = juce::jmin (levels->size() - 1, i0 + 1);
        const auto v0 = (double)(*levels)[i0], v1 = (double)(*levels)[i1];
        const auto t = x - i0;
        if (v0 > 0.0 && v1 > 0.0)
            return std::exp ((1.0 - t) * std::log (v0) + t * std::log (v1));
        if (v0 > 0.0 || v1 > 0.0)
            return juce::jmax (v0, v1);
    }
    return (double)sound["sound"]["harmonicBrightness"];
}

bool isRole (const juce::String& role) { return sounds::roles().contains (role); }

juce::String article (const juce::String& word)
{
    return juce::String ("aeiou").containsChar (word[0]) ? "an " + word : "a " + word;
}
} // namespace

//==============================================================================
juce::String displayName (const juce::String& part)
{
    if (part == "bass")
        return "Bass";
    if (part == "other")
        return "Chords and melody";
    if (part == "vocals")
        return "Lead (the vocals' tune)";
    if (part == "drums")
        return "Drums";
    return part.substring (0, 1).toUpperCase() + part.substring (1);
}

const juce::StringArray& songRoles()
{
    static const juce::StringArray roles { "drums",           "percussion",   "bass",        "sub",     "rhythm-guitar",
                                           "acoustic-guitar", "clean-guitar", "lead-guitar", "chords",  "piano",
                                           "organ",           "pad",          "strings",     "choir",   "brass",
                                           "woodwinds",       "arp",          "lead",        "counter", "riser",
                                           "impact" };
    return roles;
}

juce::String roleName (const juce::String& role)
{
    static const std::map<juce::String, juce::String> names {
        { "drums", "Drums" },
        { "percussion", "Percussion" },
        { "bass", "Bass" },
        { "sub", "Sub bass" },
        { "rhythm-guitar", "Rhythm guitar" },
        { "acoustic-guitar", "Acoustic guitar" },
        { "clean-guitar", "Clean guitar (picked)" },
        { "lead-guitar", "Lead guitar" },
        { "chords", "Synth chords" },
        { "piano", "Piano / keys" },
        { "organ", "Organ" },
        { "pad", "Pad" },
        { "strings", "Strings" },
        { "choir", "Choir / vocal pad" },
        { "brass", "Brass" },
        { "woodwinds", "Woodwinds" },
        { "arp", "Arpeggio" },
        { "lead", "Lead melody" },
        { "counter", "Counter-melody" },
        { "riser", "Riser (effect)" },
        { "impact", "Impacts (effect)" },
    };
    const auto found = names.find (role);
    return found != names.end() ? found->second : role;
}

namespace
{
juce::String defaultName (const juce::String& role)
{
    static const std::map<juce::String, juce::String> names { { "chords", "Chords" },
                                                              { "piano", "Piano" },
                                                              { "lead", "Lead" },
                                                              { "riser", "Riser" },
                                                              { "impact", "Impacts" },
                                                              { "choir", "Choir" },
                                                              { "clean-guitar", "Clean guitar" } };
    const auto found = names.find (role);
    return found != names.end() ? found->second : roleName (role);
}

/** Words a sound's name has when it is an instrument of the role (a guitar part wants "Guitar", "Metal"...). */
juce::StringArray nameWords (const juce::String& role)
{
    if (role.endsWith ("guitar"))
        return role == "acoustic-guitar"
                   ? juce::StringArray { "acoustic", "nylon", "steel", "folk", "guitar", "uke" }
                   : juce::StringArray { "guitar", "gtr", "riff", "distort", "overdrive", "electric" };
    if (role == "piano")
        return { "piano", "grand", "upright", "keys", "e.piano", "electric piano" };
    if (role == "organ")
        return { "organ", "drawbar", "church" };
    if (role == "strings")
        return { "string", "violin", "viola", "cello", "ensemble", "orchestra", "spiccato", "pizz" };
    if (role == "brass")
        return { "brass", "horn", "trumpet", "trombone", "tuba", "section" };
    if (role == "woodwinds")
        return { "flute", "clarinet", "oboe", "bassoon", "sax", "wood", "recorder" };
    if (role == "choir")
        return { "choir", "vox", "voice", "vocal", "aah", "ooh", "chant" };
    if (role == "percussion")
        return { "perc", "conga", "bongo", "shaker", "tamb", "hand" };
    return {};
}
} // namespace

juce::String roleFromName (const juce::String& name)
{
    const auto n = name.toLowerCase();
    auto any = [&n] (std::initializer_list<const char*> words)
    { return std::any_of (words.begin(), words.end(), [&n] (const char* w) { return n.contains (w); }); };
    if (any ({ "bass drum", "kick" }))
        return "drums";
    if (any ({ "bass" }) && ! any ({ "bassoon" }))
        return n.contains ("sub") ? "sub" : "bass";
    if (any ({ "drum", "kit", "beat", "808 kit", "trap kit" }))
        return "drums";
    if (any ({ "perc", "conga", "bongo", "shaker", "tambourine" }))
        return "percussion";
    if (any ({ "acoustic", "nylon", "steel string", "ukulele", "banjo" }))
        return "acoustic-guitar";
    if (any ({ "guitar", "gtr", "riff" }))
        return n.contains ("lead") || n.contains ("solo")    ? "lead-guitar"
               : n.contains ("clean") || n.contains ("pick") ? "clean-guitar"
                                                             : "rhythm-guitar";
    if (any ({ "organ", "drawbar" })) // before "keys": an organ preset is often filed under Keys
        return "organ";
    if (any ({ "piano", "grand", "upright", "e.piano", "keys" }))
        return "piano";
    if (any ({ "string", "violin", "viola", "cello", "ensemble", "orchestra" }))
        return "strings";
    if (any ({ "brass", "horn", "trumpet", "trombone", "tuba" }))
        return "brass";
    if (any ({ "flute", "clarinet", "oboe", "bassoon", "sax", "woodwind" }))
        return "woodwinds";
    if (any ({ "choir", "vox", "voice", "vocal", "aah", "ooh" }))
        return "choir";
    if (any ({ "riser", "sweep", "fx", "noise" }))
        return "riser";
    if (any ({ "pad" }))
        return "pad";
    if (any ({ "pluck", "arp", "bell", "mallet", "marimba" }))
        return "arp";
    if (any ({ "lead" }))
        return "lead";
    return {};
}

std::vector<SongPart> describedParts (const juce::var& summary)
{
    std::vector<SongPart> list;
    if (const auto* named = summary["described"]["parts"].getArray())
        for (const auto& p : *named)
            if (songRoles().contains (p["role"].toString()))
            {
                auto part = newPart (list, p["role"].toString());
                if (const auto name = p["name"].toString();
                    name.isNotEmpty() &&
                    std::none_of (list.begin(), list.end(), [&] (const SongPart& o) { return o.name == name; }))
                    part.name = name;
                list.push_back (part);
            }
    return list;
}

std::vector<SongPart> suggestedParts (const juce::String& which, const juce::var& summary)
{
    if (which != "genre")
        return defaultParts (which, summary);
    std::vector<SongPart> list;
    if (const auto* roles = summary["described"]["suggested"].getArray())
        for (const auto& role : *roles)
            if (songRoles().contains (role.toString()))
                list.push_back (newPart (list, role.toString()));
    return list;
}

std::vector<Choice> vary (std::vector<Choice> candidates, const std::set<juce::String>& avoid, juce::uint32 seed,
                          int most)
{
    auto keyOf = [] (const Choice& c)
    {
        return c.kind == Choice::Kind::drumSamples ? juce::String ("samples")
               : c.kind == Choice::Kind::sample    ? juce::String ("sampler")
               : c.kind == Choice::Kind::loop      ? juce::String ("loop")
               : c.instrument.isNotEmpty()         ? c.instrument
                                                   : c.source.key + c.source.name;
    };
    struct Scored
    {
        Choice choice;
        double score;
    };
    std::vector<Scored> scored;
    const bool measured =
        std::any_of (candidates.begin(), candidates.end(), [] (const Choice& c) { return c.compared; });
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        const auto& c = candidates[i];
        const auto key = keyOf (c);
        // The order it came in (closest first), the other parts' instruments later, near ones shuffled by the seed;
        // compared with the songs, the closest stays first and the sounds not compared come after.
        juce::Random random ((juce::int64)(seed ^ (juce::uint32)(key + c.name).hashCode()));
        auto score = (double)i * 0.1 + (avoid.contains (key) ? 0.6 : 0.0) + random.nextDouble() * 0.35;
        if (measured)
            score = i == 0 && c.compared ? -1.0 : score + (c.compared ? c.distance : 2.0);
        scored.push_back ({ c, score });
    }
    std::stable_sort (scored.begin(), scored.end(),
                      [] (const Scored& a, const Scored& b) { return a.score < b.score; });
    // The first few from different instruments, then different preset families, then the rest.
    std::vector<Choice> result;
    std::set<juce::String> instruments, families;
    for (int pass = 0; pass < 3 && (int)result.size() < most; ++pass)
        for (const auto& s : scored)
        {
            if ((int)result.size() >= most)
                break;
            const auto key = keyOf (s.choice);
            const auto family = key + "/" + s.choice.category;
            const bool taken =
                std::any_of (result.begin(), result.end(), [&] (const Choice& r)
                             { return keyOf (r) == key && r.name == s.choice.name && r.preset == s.choice.preset; });
            if (taken || (pass == 0 && instruments.contains (key)) || (pass == 1 && families.contains (family)))
                continue;
            instruments.insert (key);
            families.insert (family);
            result.push_back (s.choice);
        }
    return result;
}

std::vector<SongPart> defaultParts (const juce::String& arrangement, const juce::var& summary)
{
    std::vector<SongPart> list;
    juce::StringArray roles { "drums", "bass", "chords", "lead" };
    if (arrangement == "full")
    {
        roles = { "drums", "bass", "chords", "pad", "arp", "lead", "counter", "riser", "impact" };
        // A sub bass when the bass sits high enough to leave room under it (as spinus_ai.songwriter.has_sub).
        if ((int)summary["parts"]["bass"]["median"] >= 40)
            roles.insert (2, "sub");
    }
    for (const auto& role : roles)
        list.push_back (newPart (list, role));
    return list;
}

SongPart newPart (const std::vector<SongPart>& list, const juce::String& role)
{
    auto taken = [&list] (const juce::String& id, const juce::String& name)
    {
        return std::any_of (list.begin(), list.end(), [&] (const SongPart& p) { return p.id == id || p.name == name; });
    };
    SongPart part { role, defaultName (role), role };
    for (int n = 2; taken (part.id, part.name); ++n)
        part = { role + "-" + juce::String (n), defaultName (role) + " " + juce::String (n), role };
    return part;
}

Target targetFor (const SongPart& part, int instance, const juce::var& summary)
{
    const auto& role = part.role;
    static const juce::StringArray drums { "drums", "impact", "percussion" }, bass { "bass", "sub" },
        lead { "lead", "counter", "lead-guitar", "woodwinds" };
    const auto stem = drums.contains (role)  ? juce::String ("drums")
                      : bass.contains (role) ? juce::String ("bass")
                      : lead.contains (role) ? juce::String ("vocals")
                      : role == "riser"      ? juce::String ("fx")
                                             : juce::String ("other");
    Target t;
    for (auto& found : targetsFromSummary (summary))
        if (found.part == stem)
            t = found;
    if (t.part.isEmpty()) // the summary has no such part: a usual one
    {
        t.part = stem;
        t.tempo = (double)summary["tempo"]["median"];
        t.low = stem == "bass" ? 28 : stem == "vocals" ? 55 : 48;
        t.median = stem == "bass" ? 38 : stem == "vocals" ? 64 : 60;
        t.high = stem == "bass" ? 50 : stem == "vocals" ? 76 : 72;
        t.notesPerBar = 4.0;
        t.roles = { stem == "drums" ? "drums" : stem == "bass" ? "bass" : stem == "vocals" ? "lead" : "keys" };
    }
    t.id = part.id;
    t.name = part.name;
    t.role = role;
    t.measure = instance == 0 && (role == "drums" || role == "bass" || role == "chords" || role == "lead");
    const auto up = instance % 2 == 1 && role != "drums" && role != "impact" && role != "sub" ? 12 : 0;
    if (role == "sub")
    {
        t.low = 24, t.median = 31, t.high = 40;
        t.roles = { "bass" };
    }
    else if (role == "pad")
    {
        t.median = juce::jlimit (55, 67, t.median + 3);
        t.low = t.median - 9, t.high = t.median + 12;
        t.roles = { "pad", "keys" };
        t.chords = true, t.voices = 3.0;
    }
    else if (role == "arp")
    {
        t.low = juce::jlimit (52, 72, t.median + 5), t.median = t.low + 9, t.high = t.low + 18;
        t.roles = { "pluck", "keys", "lead" };
        t.chords = false, t.voices = 1.0;
    }
    else if (role == "counter")
    {
        t.median = juce::jmax (52, t.median - 7), t.low = t.median - 7, t.high = t.median + 7;
        t.roles = { "lead", "keys", "pad" };
    }
    else if (role == "riser")
    {
        t.low = 60, t.median = 66, t.high = 71;
        t.roles = { "fx", "pad" };
        t.measure = false;
    }
    else if (role == "impact" || role == "percussion")
        t.roles = { "drums" };
    else if (role.endsWith ("guitar") || role == "piano" || role == "organ" || role == "strings" || role == "brass" ||
             role == "woodwinds" || role == "choir")
    {
        // Their ranges (as spinus_ai.songwriter.layer_register), and the roles of sound that fit.
        static const std::map<juce::String, std::array<int, 3>> ranges {
            { "rhythm-guitar", { 40, 50, 64 } },   { "lead-guitar", { 55, 67, 79 } },
            { "acoustic-guitar", { 40, 55, 76 } }, { "clean-guitar", { 45, 57, 76 } },
            { "piano", { 33, 60, 76 } },           { "organ", { 40, 62, 74 } },
            { "strings", { 45, 65, 79 } },         { "brass", { 55, 63, 72 } },
            { "woodwinds", { 60, 72, 84 } },       { "choir", { 52, 62, 74 } }
        };
        const auto& r = ranges.at (role);
        t.low = r[0], t.median = r[1], t.high = r[2];
        t.roles = role == "lead-guitar" || role == "woodwinds" || role == "brass"
                      ? juce::StringArray { "lead", "keys", "pluck" }
                  : role == "strings" || role == "choir"                ? juce::StringArray { "pad", "keys" }
                  : role == "acoustic-guitar" || role == "clean-guitar" ? juce::StringArray { "pluck", "keys", "lead" }
                                                                        : juce::StringArray { "keys", "lead", "pluck" };
        t.chords = role != "lead-guitar" && role != "woodwinds";
        t.voices = t.chords ? 3.0 : 1.0;
        t.measure = false;
    }
    t.names = nameWords (role);
    t.low += up, t.median += up, t.high += up;
    return t;
}

std::vector<Target> targetsFromSummary (const juce::var& summary)
{
    std::vector<Target> targets;
    const auto& parts = summary["parts"];
    const auto tempo = (double)summary["tempo"]["median"];
    for (const auto* name : { "bass", "other", "vocals", "drums" })
    {
        const auto& p = parts[name];
        if (! p.isObject() || (int)p["songs"] <= 0)
            continue;
        Target t;
        t.part = name;
        t.id = name;
        t.name = displayName (name);
        t.role = juce::String (name) == "other" ? "chords" : juce::String (name) == "vocals" ? "lead" : name;
        t.songs = (int)p["songs"];
        t.low = (int)p["low"];
        t.median = (int)p["median"];
        t.high = (int)p["high"];
        t.notesPerBar = (double)p["notesPerBar"];
        t.voices = p.hasProperty ("voices") ? (double)p["voices"] : 1.0;
        // Chords are the other instruments' (Basic Pitch hears a bass's or a voice's overlapping notes as
        // more than one voice now and then).
        t.chords = t.part == "other" && t.voices > 1.3;
        t.tempo = tempo;
        if (t.part == "bass")
            t.roles = { "bass" };
        else if (t.part == "vocals")
            t.roles = { "lead" };
        else if (t.part == "drums")
            t.roles = { "drums" };
        else if (t.chords) // the fewer notes per bar, the more a pad
            t.roles = t.notesPerBar < 6.0 ? juce::StringArray { "pad", "keys" } : juce::StringArray { "keys", "pad" };
        else
            t.roles = t.notesPerBar >= 8.0 ? juce::StringArray { "pluck", "lead", "keys" }
                                           : juce::StringArray { "lead", "pluck", "keys" };
        targets.push_back (std::move (t));
    }
    return targets;
}

//==============================================================================
std::vector<StemNote> stemNotesFromVar (const juce::var& list)
{
    std::vector<StemNote> notes;
    if (const auto* a = list.getArray())
        for (const auto& n : *a)
            if ((double)n["end"] > (double)n["start"])
                notes.push_back ({ juce::jlimit (0, 127, (int)n["pitch"]), (double)n["start"], (double)n["end"] });
    std::sort (notes.begin(), notes.end(), [] (const StemNote& a, const StemNote& b) { return a.start < b.start; });
    return notes;
}

juce::AudioBuffer<float> readMono (const juce::File& file, double rate, double longest)
{
    std::unique_ptr<juce::AudioFormatReader> reader (dsp::getAudioFormats().createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return {};
    const auto count =
        (int)std::min<juce::int64> (reader->lengthInSamples, (juce::int64)(longest * reader->sampleRate));
    juce::AudioBuffer<float> source (2, count);
    reader->read (&source, 0, count, 0, true, true);
    if (reader->numChannels > 1)
    {
        source.addFrom (0, 0, source, 1, 0, count);
        source.applyGain (0, 0, count, 0.5f);
    }
    const auto ratio = reader->sampleRate / rate;
    if (std::abs (ratio - 1.0) < 1.0e-9)
    {
        source.setSize (1, count, true);
        return source;
    }
    juce::AudioBuffer<float> result (1, (int)((double)count / ratio));
    juce::LagrangeInterpolator resampler;
    resampler.process (ratio, source.getReadPointer (0), result.getWritePointer (0), result.getNumSamples(), count, 0);
    return result;
}

std::vector<StemHit> stemHitsFromVar (const juce::var& list)
{
    std::vector<StemHit> hits;
    if (const auto* a = list.getArray())
        for (const auto& h : *a)
            hits.push_back ({ (double)h["time"], h["kind"].toString() });
    std::sort (hits.begin(), hits.end(), [] (const StemHit& a, const StemHit& b) { return a.time < b.time; });
    return hits;
}

juce::String drumOf (const juce::String& kind)
{
    if (kind == "kick")
        return "kick";
    if (kind == "snare" || kind == "clap" || kind == "rim")
        return "snare";
    if (kind.contains ("hat"))
        return "hat";
    return {};
}

void Measured::add (const Measured& other)
{
    sounds.insert (sounds.end(), other.sounds.begin(), other.sounds.end());
    harmonics.insert (harmonics.end(), other.harmonics.begin(), other.harmonics.end());
    pitches.insert (pitches.end(), other.pitches.begin(), other.pitches.end());
    held.insert (held.end(), other.held.begin(), other.held.end());
}

bool Measured::knowsSustain() const
{
    return std::count_if (held.begin(), held.end(), [] (double h) { return h >= 0.3; }) >= 3;
}

sounds::Descriptors Measured::median() const
{
    sounds::Descriptors d;
    if (sounds.empty())
        return d;
    const bool longOnly = knowsSustain();
    auto of = [&] (auto field, bool needsHold = false)
    {
        std::vector<double> v;
        for (size_t i = 0; i < sounds.size(); ++i)
            if (! needsHold || ! longOnly || (i < held.size() && held[i] >= 0.3))
                v.push_back (field (sounds[i]));
        return spinus::parts::median (std::move (v));
    };
    d.attack = of ([] (const sounds::Descriptors& s) { return s.attack; });
    d.onset = of ([] (const sounds::Descriptors& s) { return s.onset; });
    d.decay = of ([] (const sounds::Descriptors& s) { return s.decay; }, true);
    d.sustain = of ([] (const sounds::Descriptors& s) { return s.sustain; }, true);
    d.release = of ([] (const sounds::Descriptors& s) { return s.release; });
    d.length = of ([] (const sounds::Descriptors& s) { return s.length; });
    d.brightness = of ([] (const sounds::Descriptors& s) { return s.brightness; });
    d.harmonicBrightness = of ([] (const sounds::Descriptors& s) { return s.harmonicBrightness; });
    d.noisiness = of ([] (const sounds::Descriptors& s) { return s.noisiness; });
    d.pitched = of ([] (const sounds::Descriptors& s) { return s.pitched; });
    d.fundamental = of ([] (const sounds::Descriptors& s) { return s.fundamental; });
    d.low = of ([] (const sounds::Descriptors& s) { return s.low; });
    d.high = of ([] (const sounds::Descriptors& s) { return s.high; });
    d.peakDb = of ([] (const sounds::Descriptors& s) { return s.peakDb; });
    return d;
}

double Measured::medianHarmonics() const { return spinus::parts::median (harmonics); }

int Measured::medianPitch() const
{
    std::vector<double> v (pitches.begin(), pitches.end());
    return juce::roundToInt (spinus::parts::median (std::move (v)));
}

Measured measureNotes (const float* mono, int count, double rate, const std::vector<StemNote>& notes, int most)
{
    Measured result;
    if (mono == nullptr || count <= 0 || rate <= 0.0 || notes.empty())
        return result;
    // Notes starting together are one event (a chord), heard at its lowest note.
    struct Event
    {
        double start, end;
        int pitch;
    };
    std::vector<Event> events;
    for (const auto& n : notes)
    {
        if (! events.empty() && n.start - events.back().start < 0.03)
        {
            events.back().end = juce::jmax (events.back().end, n.end);
            events.back().pitch = juce::jmin (events.back().pitch, n.pitch);
        }
        else
            events.push_back ({ n.start, n.end, n.pitch });
    }
    float loudest = 0.0f;
    for (int i = 0; i < count; ++i)
        loudest = juce::jmax (loudest, std::abs (mono[i]));
    if (loudest <= 1.0e-5f)
        return result;

    // Clear notes: nothing still held from before (a note ending, at most, just after this one starts), at least
    // 80 ms long, apart from the next start by as much.
    std::vector<size_t> clear;
    double heldUntil = -1.0;
    for (size_t i = 0; i < events.size(); ++i)
    {
        const auto& e = events[i];
        const auto next = i + 1 < events.size() ? events[i + 1].start : e.end + 1.0;
        if (heldUntil <= e.start + 0.02 && e.end - e.start >= 0.08 && next - e.start >= 0.08)
            clear.push_back (i);
        heldUntil = juce::jmax (heldUntil, e.end);
    }
    // Spread over the song.
    const auto step = juce::jmax (1.0, (double)clear.size() / (double)juce::jmax (1, most));
    for (size_t n = 0; (size_t)((double)n * step) < clear.size() && result.count() < most; ++n)
    {
        const auto i = clear[(size_t)((double)n * step)];
        const auto& e = events[i];
        const auto next = i + 1 < events.size() ? events[i + 1].start : e.end + 0.5;
        const auto from = juce::jmax (0.0, e.start - 0.01);
        const auto to = juce::jmin (next, e.end + 0.5);
        const auto a = (int)(from * rate), b = juce::jmin (count, (int)(to * rate));
        if (b - a < (int)(0.05 * rate))
            continue;
        const auto d = sounds::describe (mono + a, b - a, rate, e.end - from);
        if (d.peakDb < 20.0 * std::log10 (loudest) - 40.0) // too quiet to say anything
            continue;
        result.sounds.push_back (d);
        result.harmonics.push_back (d.brightness / noteFrequency (e.pitch));
        result.pitches.push_back (e.pitch);
        result.held.push_back (e.end - e.start);
    }
    return result;
}

std::map<juce::String, Measured> measureHits (const float* mono, int count, double rate,
                                              const std::vector<StemHit>& hits, int most)
{
    std::map<juce::String, Measured> result;
    if (mono == nullptr || count <= 0 || rate <= 0.0)
        return result;
    std::map<juce::String, std::vector<size_t>> byDrum;
    for (size_t i = 0; i < hits.size(); ++i)
        if (const auto drum = drumOf (hits[i].kind); drum.isNotEmpty())
            byDrum[drum].push_back (i);
    for (auto& [drum, list] : byDrum)
    {
        auto& m = result[drum];
        const auto step = juce::jmax (1.0, (double)list.size() / (double)juce::jmax (1, most));
        for (size_t n = 0; (size_t)((double)n * step) < list.size() && m.count() < most; ++n)
        {
            const auto i = list[(size_t)((double)n * step)];
            const auto next = i + 1 < hits.size() ? hits[i + 1].time : hits[i].time + 0.5;
            const auto from = juce::jmax (0.0, hits[i].time - 0.005);
            const auto to = juce::jmin (next - 0.002, hits[i].time + 0.5);
            const auto a = (int)(from * rate), b = juce::jmin (count, (int)(to * rate));
            if (b - a < (int)(0.03 * rate))
                continue;
            const auto d = sounds::describe (mono + a, b - a, rate, 0.0);
            if (d.peakDb <= -80.0)
                continue;
            m.sounds.push_back (d);
            m.held.push_back (0.0);
        }
    }
    return result;
}

//==============================================================================
double presetDistance (const juce::var& sound, const Measured& stem, juce::StringArray* why)
{
    if (stem.count() == 0)
        return 0.0;
    const auto s = sounds::descriptorsFromVar (sound["sound"]);
    const auto m = stem.median();
    const auto pitch = stem.medianPitch();
    const auto mine = presetHarmonicsAt (sound, pitch), theirs = stem.medianHarmonics();
    std::vector<double> d;
    d.push_back ((logAttack (s.attack) - logAttack (m.attack)) / 0.35);
    if (mine > 0.0 && theirs > 0.0)
        d.push_back (std::log2 (mine / theirs) / 1.0);
    d.push_back ((s.noisiness - m.noisiness) / 0.15);
    if (stem.knowsSustain())
    {
        d.push_back ((s.sustain - m.sustain) / 0.3);
        d.push_back ((std::log10 (s.decay + 0.01) - std::log10 (m.decay + 0.01)) / 0.6);
    }
    double sum = 0.0;
    for (auto x : d)
        sum += x * x;
    if (why != nullptr)
    {
        why->add ("attack " + timeText (s.attack) + " (the songs' " + timeText (m.attack) + ")");
        if (mine > 0.0 && theirs > 0.0)
            why->add ("brightness " + juce::String (mine, 1) + " harmonics at " + noteName (pitch) + " (the songs' " +
                      juce::String (theirs, 1) + ")");
        if (stem.knowsSustain())
            why->add ((s.sustain >= 0.5 ? juce::String ("holds its level") : "fades while held") +
                      " (the songs' notes " + (m.sustain >= 0.5 ? juce::String ("hold") : juce::String ("fade")) + ")");
        if (std::abs (s.noisiness - m.noisiness) > 0.1)
            why->add (s.noisiness > m.noisiness ? "noisier than the songs'" : "purer than the songs'");
    }
    return std::sqrt (sum / (double)d.size());
}

double hitDistance (const sounds::Descriptors& a, const sounds::Descriptors& b)
{
    const double d[] = { (logTime (a.attack) - logTime (b.attack)) / 0.4,
                         (std::log10 (a.length + 0.01) - std::log10 (b.length + 0.01)) / 0.4,
                         std::log2 (juce::jmax (20.0, a.brightness) / juce::jmax (20.0, b.brightness)) / 1.0,
                         (a.noisiness - b.noisiness) / 0.2,
                         (a.low - b.low) / 0.25,
                         (a.high - b.high) / 0.25 };
    double sum = 0.0;
    for (auto x : d)
        sum += x * x;
    return std::sqrt (sum / 6.0);
}

//==============================================================================
juce::String Choice::title() const
{
    switch (kind)
    {
    case Kind::preset:
    case Kind::kit:
        return name.isEmpty() || name == "Default sound" ? source.name : source.name + ": " + name;
    case Kind::sample:
        return "Sampler: " + juce::File (file).getFileNameWithoutExtension();
    case Kind::drumSamples:
    {
        juce::StringArray names;
        for (const auto* drum : { "kick", "snare", "hat" })
            if (drums.contains (drum))
                names.add (juce::File (drums.at (drum)).getFileNameWithoutExtension());
        return "Drum Machine: " + names.joinIntoString (", ");
    }
    case Kind::loop:
        return "Loop: " + juce::File (file).getFileNameWithoutExtension() +
               (tempo > 0.0 ? " (" + juce::String (tempo, 0) + " BPM)" : juce::String());
    }
    return name;
}

juce::var Choice::deviceJson() const
{
    if (kind == Kind::sample)
        return object ({ { "kind", "stock" }, { "stockType", "spinus.sampler" } });
    if (kind == Kind::drumSamples)
        return object ({ { "kind", "stock" }, { "stockType", "spinus.drum-machine" } });
    if (source.stockType.isNotEmpty())
        return object ({ { "kind", "stock" }, { "stockType", source.stockType } });
    return object ({ { "kind", "plugin" },
                     { "plugin", object ({ { "format", source.format },
                                           { "name", source.pluginName.isNotEmpty() ? source.pluginName : source.name },
                                           { "fileOrIdentifier", toPortablePluginPath (source.file) },
                                           { "uniqueId", source.uniqueId } }) } });
}

//==============================================================================
namespace
{
std::vector<Choice> chooseMelodic (const Target& target, const Reference& reference, const SoundIndex& index, int most)
{
    struct Ranked
    {
        Choice choice;
        int tier = 0; // 0: fits every rule; more: fits fewer
        double score = 0.0;
    };
    std::vector<Ranked> ranked;
    const bool compare = reference.notes.count() >= 3;
    UserRoles marked (index.getFolder());
    marked.load();
    for (const auto& [key, instrument] : index.instruments)
        for (const auto& sound : instrument.sounds)
        {
            // What the user marked it as comes first: a sound marked for the part's role is offered even when it
            // could not be measured.
            const auto mine = marked.get (key, sound["id"].toString());
            const bool forThis = mine.isNotEmpty() && mine == target.role;
            if (! forThis && (sound.hasProperty ("problem") || (bool)sound["silent"] || (bool)sound["invalid"] ||
                              ! (bool)sound["tracksPitch"]))
                continue;
            const auto role = sound["role"].toString();
            const auto rank = target.roles.indexOf (role);
            const auto low = (int)sound["low"], high = (int)sound["high"];
            // The test notes are an octave apart: within half an octave of the part's ends is covered.
            const bool covers = target.low <= 0 || (low <= target.low + 6 && high >= target.high - 6);
            const bool chords = ! target.chords || (bool)sound["polyphonic"];
            Ranked r;
            auto& c = r.choice;
            c.kind = Choice::Kind::preset;
            c.instrument = key;
            c.source = instrument.source;
            c.preset = sound["id"].toString();
            c.version = sound["version"].toString();
            c.name = sound["name"].toString();
            c.category = sound["category"].toString();
            c.role = role;
            c.roleFrom = sound["roleFrom"].toString();
            c.low = low;
            c.high = high;
            c.polyphonic = (bool)sound["polyphonic"];
            r.tier = (rank < 0 ? 4 : 0) + (chords ? 0 : 2) + (covers ? 0 : 1);
            if (isRole (role))
                c.why.add (c.roleFrom == "name" ? article (role) + " (its name says)"
                                                : "sounds like " + article (role));
            c.why.add ("plays " + noteName (low) + "-" + noteName (high) +
                       (target.low > 0 ? " (the part: " + noteName (target.low) + "-" + noteName (target.high) + ")"
                                       : juce::String()) +
                       (covers ? juce::String() : ", not all of it"));
            if (target.chords)
                c.why.add (c.polyphonic ? "plays chords" : "plays one note at a time; the part has chords");
            if (compare)
            {
                c.distance = presetDistance (sound, reference.notes, &c.why);
                c.compared = true;
            }
            r.score = c.distance + 0.25 * juce::jmax (0, rank) + (c.roleFrom == "name" ? 0.0 : 0.15);
            if (mine.isNotEmpty())
            {
                c.why.insert (0, "you marked it as " + roleName (mine).toLowerCase());
                r.tier = forThis ? 0 : r.tier + 3;
                r.score += forThis ? -3.0 : 1.0;
            }
            // A part of an instrument (a guitar, strings...) wants a sound whose name says so: first, whatever
            // role its sound was guessed.
            else if (! target.names.isEmpty())
            {
                const auto text = (c.name + " " + c.category + " " + c.source.name).toLowerCase();
                const auto word = std::find_if (target.names.begin(), target.names.end(),
                                                [&text] (const juce::String& w) { return text.contains (w); });
                if (word != target.names.end())
                {
                    r.tier = juce::jmax (0, r.tier - 4);
                    r.score -= 1.0;
                    c.why.insert (0, "its name says " + word->trim());
                }
                else
                    r.tier += 1;
            }
            ranked.push_back (std::move (r));
        }

    // An 808 or other bass one-shot can play a bass line.
    if (target.part == "bass")
        for (const auto& sample : index.samples)
        {
            const auto& info = sample.info;
            if (info["kind"].toString() != "one-shot" || info["class"].toString() != "bass")
                continue;
            Ranked r;
            auto& c = r.choice;
            c.kind = Choice::Kind::sample;
            c.file = sample.path;
            c.name = juce::File (sample.path).getFileName();
            c.role = "bass";
            c.roleFrom = info["classFrom"].toString() == "name" ? "name" : "sound";
            c.duration = (double)info["duration"];
            c.fundamental = (double)info["sound"]["fundamental"];
            c.why.add ("a bass sample (its " + juce::String (c.roleFrom == "name" ? "name" : "sound") + " says)");
            c.why.add ("played on every key from its own pitch");
            if (compare)
            {
                const auto s = sounds::descriptorsFromVar (info["sound"]);
                const auto m = reference.notes.median();
                const auto harmonicsDiff = s.harmonicBrightness > 0.0 && reference.notes.medianHarmonics() > 0.0
                                               ? std::log2 (s.harmonicBrightness / reference.notes.medianHarmonics())
                                               : 0.0;
                const double d[] = { (logAttack (s.attack) - logAttack (m.attack)) / 0.35, harmonicsDiff,
                                     (s.noisiness - m.noisiness) / 0.15 };
                c.distance = std::sqrt ((d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / 3.0);
                c.compared = true;
                c.why.add ("attack " + timeText (s.attack) + " (the songs' " + timeText (m.attack) + ")");
            }
            r.tier = 1; // a sample lacks a synth's control
            r.score = c.distance + 0.3;
            ranked.push_back (std::move (r));
        }

    std::stable_sort (ranked.begin(), ranked.end(), [] (const Ranked& a, const Ranked& b)
                      { return a.tier != b.tier ? a.tier < b.tier : a.score < b.score; });
    // Nothing of a fitting role at all: the closest of any role, saying so.
    const bool anyFits = std::any_of (ranked.begin(), ranked.end(), [] (const Ranked& r) { return r.tier < 4; });
    std::vector<Choice> result;
    std::map<juce::String, int> perInstrument;
    const auto take = [&] (bool limitEach)
    {
        for (auto& r : ranked)
        {
            if ((int)result.size() >= most || (anyFits && r.tier >= 4))
                continue;
            const auto source = r.choice.kind == Choice::Kind::sample ? juce::String ("samples") : r.choice.instrument;
            const bool already = std::any_of (result.begin(), result.end(),
                                              [&] (const Choice& c)
                                              {
                                                  return c.instrument == r.choice.instrument &&
                                                         c.preset == r.choice.preset && c.file == r.choice.file;
                                              });
            // At most three of one instrument while others fit too, so the alternatives differ.
            if (already || (limitEach && perInstrument[source] >= 3))
                continue;
            ++perInstrument[source];
            if (! anyFits && r.choice.why.size() > 0)
                r.choice.why.insert (0, "no " + target.roles[0] + " sound in the index; the closest of another role");
            result.push_back (r.choice);
        }
    };
    take (true);
    take (false);
    return result;
}

std::vector<Choice> chooseDrums (const Target& target, const Reference& reference, const SoundIndex& index, int most)
{
    std::vector<Choice> result;
    // Drum samples: the closest kick, snare and hat (the second closest set next, and so on).
    std::map<juce::String, std::vector<std::pair<double, const SoundIndex::Sample*>>> candidates;
    for (const auto& sample : index.samples)
    {
        const auto& info = sample.info;
        if (info["kind"].toString() != "one-shot")
            continue;
        const auto cls = info["class"].toString();
        const auto drum = cls == "kick"                     ? juce::String ("kick")
                          : cls == "snare" || cls == "clap" ? juce::String ("snare")
                          : cls == "hat"                    ? juce::String ("hat")
                                                            : juce::String();
        if (drum.isEmpty())
            continue;
        const auto found = reference.hits.find (drum);
        const bool compare = found != reference.hits.end() && found->second.count() >= 3;
        auto distance =
            compare ? hitDistance (sounds::descriptorsFromVar (info["sound"]), found->second.median()) : 0.0;
        if (drum == "snare" && cls == "clap")
            distance += 0.3; // a snare's part is a snare first
        candidates[drum].emplace_back (distance, &sample);
    }
    if (candidates.size() == 3)
    {
        for (auto& [drum, list] : candidates)
            std::stable_sort (list.begin(), list.end(), [] (const auto& a, const auto& b)
                              { return std::tie (a.first, a.second->path) < std::tie (b.first, b.second->path); });
        const auto sets =
            juce::jmin ((size_t)3, candidates["kick"].size(), candidates["snare"].size(), candidates["hat"].size());
        for (size_t k = 0; k < sets; ++k)
        {
            Choice c;
            c.kind = Choice::Kind::drumSamples;
            c.role = "drums";
            double total = 0.0;
            for (const auto* drum : { "kick", "snare", "hat" })
            {
                const auto& [distance, sample] = candidates[drum][k];
                c.drums[drum] = sample->path;
                total += distance;
                const auto d = sounds::descriptorsFromVar (sample->info["sound"]);
                juce::String text = juce::String (drum) + ": " + juce::File (sample->path).getFileName() + ", " +
                                    timeText (d.length) + " long";
                if (const auto found = reference.hits.find (drum);
                    found != reference.hits.end() && found->second.count() >= 3)
                {
                    text << " (the songs' " << timeText (found->second.median().length) << "), brightness "
                         << juce::String (juce::roundToInt (d.brightness)) << " Hz (theirs "
                         << juce::String (juce::roundToInt (found->second.median().brightness)) << " Hz)";
                    c.compared = true;
                }
                c.why.add (text);
            }
            c.distance = total / 3.0;
            c.name = c.title();
            result.push_back (std::move (c));
        }
    }

    // Instruments with a kit on their keys (the Drum Machine's Synth Kit).
    for (const auto& [key, instrument] : index.instruments)
        for (const auto& sound : instrument.sounds)
        {
            const auto* map = sound["drumMap"].getArray();
            if (map == nullptr || sound.hasProperty ("problem"))
                continue;
            std::map<juce::String, int> notes;
            for (const auto& entry : *map)
            {
                const auto cls = entry["class"].toString();
                const auto drum = cls == "kick"                     ? juce::String ("kick")
                                  : cls == "snare" || cls == "clap" ? juce::String ("snare")
                                  : cls == "hat"                    ? juce::String ("hat")
                                                                    : juce::String();
                if (drum.isNotEmpty() && ! notes.contains (drum))
                    notes[drum] = (int)entry["note"];
            }
            if (notes.size() < 3)
                continue;
            Choice c;
            c.kind = Choice::Kind::kit;
            c.instrument = key;
            c.source = instrument.source;
            c.preset = sound["id"].toString();
            c.name = sound["name"].toString();
            c.role = "drums";
            c.drumNotes = notes;
            c.why.add ("a drum kit with a kick, a snare and hats on its keys");
            c.why.add ("its sounds are not compared with the songs' drums");
            c.distance = 1.0;
            result.push_back (std::move (c));
            break; // one kit per instrument
        }

    // Instruments the user marked as drums (or percussion): a kit on General MIDI's notes, unless measured above.
    UserRoles marked (index.getFolder());
    marked.load();
    for (const auto& [key, instrument] : index.instruments)
    {
        const auto mine = marked.get (key);
        if ((mine != "drums" && mine != "percussion") ||
            std::any_of (result.begin(), result.end(), [&key] (const Choice& c) { return c.instrument == key; }))
            continue;
        Choice c;
        c.kind = Choice::Kind::kit;
        c.instrument = key;
        c.source = instrument.source;
        c.role = "drums";
        c.drumNotes = { { "kick", 36 },   { "snare", 38 },  { "hat", 42 },     { "openHat", 46 },
                        { "tomLow", 45 }, { "tomMid", 47 }, { "tomHigh", 50 }, { "crash", 49 } };
        c.why.add ("you marked it as " + roleName (mine).toLowerCase() + "; General MIDI's drum notes");
        c.distance = -0.5;
        result.push_back (std::move (c));
    }

    // Spinus's own Drum Machine with its own kit, always there (unless the index has a kit of it already).
    if (std::none_of (result.begin(), result.end(),
                      [] (const Choice& c) { return c.source.stockType == "spinus.drum-machine"; }))
    {
        Choice c;
        c.kind = Choice::Kind::kit;
        c.instrument = "stock-spinus.drum-machine";
        c.source.key = c.instrument;
        c.source.name = "Drum Machine";
        c.source.format = "Spinus";
        c.source.stockType = "spinus.drum-machine";
        c.role = "drums";
        c.drumNotes = { { "kick", 60 },   { "snare", 61 },  { "hat", 63 },     { "openHat", 64 },
                        { "tomLow", 65 }, { "tomMid", 66 }, { "tomHigh", 67 }, { "crash", 70 } };
        c.why.add ("Spinus's own Drum Machine, with its own kit");
        c.distance = reference.hits.empty() ? -1.0 : 1.0; // first when there are no songs' drums to compare
        result.push_back (std::move (c));
    }

    // Drum loops near the songs' tempo (or twice or half of it).
    for (const auto& sample : index.samples)
    {
        const auto& info = sample.info;
        if (info["kind"].toString() != "loop" || info["class"].toString() != "drums" || target.tempo <= 0.0)
            continue;
        const auto tempo = (double)info["tempo"];
        if (tempo <= 0.0)
            continue;
        double off = 1.0e9;
        for (double factor : { 1.0, 2.0, 0.5 })
            off = juce::jmin (off, std::abs (std::log2 (tempo * factor / target.tempo)));
        if (off > std::log2 (1.05))
            continue;
        Choice c;
        c.kind = Choice::Kind::loop;
        c.file = sample.path;
        c.name = juce::File (sample.path).getFileName();
        c.role = "drums";
        c.tempo = tempo;
        c.duration = (double)info["duration"];
        c.distance = 1.2 + off * 10.0;
        c.why.add ("a drum loop at " + juce::String (tempo, 1) + " BPM (the songs: " + juce::String (target.tempo, 1) +
                   ")");
        c.why.add ("plays as audio on a Playlist track");
        result.push_back (std::move (c));
    }

    std::stable_sort (result.begin(), result.end(),
                      [] (const Choice& a, const Choice& b) { return a.distance < b.distance; });
    // Each kind there is among the first (a sample set, a kit, a loop), so the user can pick any of them.
    std::vector<Choice> kept;
    for (const auto& c : result)
        if (std::none_of (kept.begin(), kept.end(), [&] (const Choice& k) { return k.kind == c.kind; }))
            kept.push_back (c);
    for (const auto& c : result)
        if ((int)kept.size() < most && std::none_of (kept.begin(), kept.end(), [&] (const Choice& k)
                                                     { return k.title() == c.title() && k.kind == c.kind; }))
            kept.push_back (c);
    std::stable_sort (kept.begin(), kept.end(),
                      [] (const Choice& a, const Choice& b) { return a.distance < b.distance; });
    if ((int)kept.size() > most)
        kept.resize ((size_t)most);
    return kept;
}
} // namespace

std::vector<Choice> choose (const Target& target, const Reference& reference, const SoundIndex& index, int most)
{
    return target.part == "drums" ? chooseDrums (target, reference, index, most)
                                  : chooseMelodic (target, reference, index, most);
}

//==============================================================================
namespace
{
/** The key's tonic (pitch class) and whether it is minor, from the summary's commonest key ("D minor", "Bb minor"
    or, from older analyses, "A# minor"). */
std::pair<int, bool> keyOf (const juce::var& summary)
{
    const auto* keys = summary["keys"].getArray();
    const auto name = keys != nullptr && ! keys->isEmpty() ? keys->getReference (0)["name"].toString() : juce::String();
    static const juce::StringArray sharps { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const juce::StringArray flats { "C", "Db", "D", "Eb", "Fb", "F", "Gb", "G", "Ab", "A", "Bb", "Cb" };
    const auto note = name.upToFirstOccurrenceOf (" ", false, false);
    const auto tonic = sharps.contains (note) ? sharps.indexOf (note) : flats.indexOf (note);
    return { tonic < 0 ? 0 : tonic, name.containsIgnoreCase ("minor") };
}
} // namespace

std::vector<OfflineInstrument::Note> auditionPhrase (const Target& target, const Choice& choice, double tempo,
                                                     const juce::var& summary)
{
    std::vector<OfflineInstrument::Note> notes;
    if (tempo <= 0.0)
        tempo = 120.0;
    const auto beat = 60.0 / tempo;
    if (target.part == "drums")
    {
        if (choice.kind == Choice::Kind::loop)
            return notes;
        auto noteOf = [&] (const char* drum)
        {
            if (choice.kind == Choice::Kind::kit && choice.drumNotes.contains (drum))
                return choice.drumNotes.at (drum);
            return juce::String (drum) == "kick" ? 60 : juce::String (drum) == "snare" ? 61 : 63; // Drum Machine pads
        };
        const auto& lanes = summary["drums"]["lanes"];
        for (int bar = 0; bar < 2; ++bar)
            for (int step = 0; step < 16; ++step)
            {
                const auto at = (bar * 16 + step) * beat / 4.0;
                auto on = [&] (const char* lane, bool fallback)
                {
                    const auto* values = lanes[lane].getArray();
                    return values != nullptr && values->size() == 16 ? (double)values->getReference (step) >= 0.5
                                                                     : fallback;
                };
                if (on ("kick", step % 4 == 0))
                    notes.push_back ({ noteOf ("kick"), at, beat / 4.0, 0.9f });
                if (on ("snare", step == 4 || step == 12))
                    notes.push_back ({ noteOf ("snare"), at, beat / 4.0, 0.85f });
                if (on ("hats", step % 2 == 0))
                    notes.push_back ({ noteOf ("hat"), at, beat / 4.0, step % 4 == 0 ? 0.7f : 0.55f });
            }
        return notes;
    }
    const auto [tonic, minor] = keyOf (summary);
    static const int majorScale[] = { 0, 2, 4, 5, 7, 9, 11 }, minorScale[] = { 0, 2, 3, 5, 7, 8, 10 };
    const auto* scale = minor ? minorScale : majorScale;
    const auto centre = target.median > 0 ? target.median : (target.part == "bass" ? 36 : 60);
    // The tonic nearest the part's middle.
    auto root = tonic + 12 * ((centre - tonic) / 12);
    if (centre - root > 6)
        root += 12;
    auto degree = [&] (int d) { return root + 12 * (d / 7) + scale[d % 7]; };
    if (target.part == "bass")
    {
        const double rhythm[][2] = { { 0.0, 1.0 }, { 1.5, 0.5 }, { 2.0, 1.0 }, { 3.0, 0.75 } };
        for (int bar = 0; bar < 2; ++bar)
            for (const auto& r : rhythm)
                notes.push_back ({ bar == 0 ? degree (0) : degree (4) - 12 + (degree (4) - 12 < target.low ? 12 : 0),
                                   (bar * 4 + r[0]) * beat, r[1] * beat * 0.9, 0.8f });
    }
    else if (target.part == "other" && target.chords)
    {
        for (int bar = 0; bar < 2; ++bar)
        {
            const auto base = bar == 0 ? 0 : 3; // the tonic chord, then the subdominant
            for (int half = 0; half < 2; ++half)
                for (int k = 0; k < 3; ++k)
                    notes.push_back ({ degree (base + 2 * k), (bar * 4 + half * 2) * beat, 1.9 * beat, 0.7f });
        }
    }
    else
    {
        const int line[] = { 0, 1, 2, 4, 2, 1, 0, 4 };
        for (int i = 0; i < 8; ++i)
            notes.push_back ({ degree (line[i]), i * beat, (i == 7 ? 1.5 : 0.9) * beat, 0.8f });
    }
    return notes;
}

//==============================================================================
void addItemSteps (juce::Array<juce::var>& steps, const juce::var& item, const juce::var& insert)
{
    const auto kind = item["kind"].toString();
    auto number = [] (double v, int decimals = 1) { return juce::String (v, decimals); };
    auto addEffect = [&] (const char* stockType, juce::var text)
    {
        steps.add (
            object ({ { "command", "insert.addEffect" },
                      { "parameters",
                        object ({ { "insert", insert },
                                  { "device", object ({ { "kind", "stock" }, { "stockType", stockType } }) } }) } }));
        steps.add (object ({ { "command", "device.setParameters" },
                             { "parameters", object ({ { "device", "$" + juce::String (steps.size()) + ".device" },
                                                       { "text", text } }) } }));
    };
    if (kind == "volume")
        steps.add (object (
            { { "command", "insert.set" },
              { "parameters", object ({ { "insert", insert },
                                        { "volumeDb", juce::jlimit (-100.0, 12.0, (double)item["db"]) } }) } }));
    else if (kind == "eq")
    {
        static const std::map<juce::String, juce::String> types {
            { "Bell", "Bell" }, { "Hi-pass", "Low cut" }, { "Lo-shelf", "Low shelf" }, { "Hi-shelf", "High shelf" }
        };
        auto* text = new juce::DynamicObject();
        int band = 0;
        if (const auto* bands = item["bands"].getArray())
            for (const auto& b : *bands)
            {
                const auto type = types.find (b["type"].toString());
                if (type == types.end() || band >= 8)
                    continue;
                const auto n = "Band " + juce::String (++band);
                text->setProperty (n + " on", "On");
                text->setProperty (n + " type", type->second);
                text->setProperty (n + " frequency", number ((double)b["freq"], 0) + " Hz");
                text->setProperty (n + " gain", number ((double)b["gain"]) + " dB");
                text->setProperty (n + " Q", number ((double)b["q"], 2));
            }
        if (band > 0)
            addEffect ("spinus.eq", juce::var (text));
        else
            delete text;
    }
    else if (kind == "compressor")
        addEffect ("spinus.compressor", object ({ { "Threshold", number ((double)item["threshold"]) + " dB" },
                                                  { "Ratio", number ((double)item["ratio"], 2) },
                                                  { "Attack", number ((double)item["attack"]) + " ms" },
                                                  { "Release", number ((double)item["release"]) + " ms" },
                                                  { "Makeup", number ((double)item["makeup"]) + " dB" } }));
    else if (kind == "limiter")
        addEffect ("spinus.limiter", object ({ { "Input gain", number ((double)item["gain"]) + " dB" },
                                               { "Ceiling", number ((double)item["threshold"]) + " dB" } }));
    else if (kind == "gate")
        addEffect ("spinus.gate", object ({ { "Threshold", number ((double)item["threshold"]) + " dB" },
                                            { "Range", number ((double)item["range"]) + " dB" },
                                            { "Release", number ((double)item["release"]) + " ms" } }));
}

juce::var makeProposal (const Plan& plan)
{
    juce::Array<juce::var> steps;
    auto step = [&steps] (const char* command, const juce::var& parameters)
    {
        steps.add (object ({ { "command", command }, { "parameters", parameters } }));
        return "$" + juce::String (steps.size());
    };
    const auto tempo = plan.tempo > 0.0 ? plan.tempo : 120.0;
    const auto ticks = [tempo] (double seconds)
    { return (juce::int64)std::llround (seconds * tempo / 60.0 * (double)model::ticksPerQuarterNote); };
    const auto pattern = step ("pattern.add", object ({ { "name", "Track creator: sounds" },
                                                        { "length", (juce::int64)(8 * model::ticksPerQuarterNote) } }));
    int count = 0;
    for (const auto& part : plan.parts)
    {
        const auto& c = part.choice;
        const auto name = (part.target.name.isNotEmpty() ? part.target.name : displayName (part.target.part))
                              .upToFirstOccurrenceOf (" (", false, false) +
                          " (Track creator)";
        if (c.kind == Choice::Kind::loop)
        {
            const auto lane = step ("lane.add", object ({ { "name", name } }));
            step ("clip.addAudio", object ({ { "lane", lane + ".lane" },
                                             { "file", c.file },
                                             { "start", 0 },
                                             { "length", std::max<juce::int64> (1, ticks (c.duration)) },
                                             { "name", name } }));
            ++count;
            continue;
        }
        const auto channel = step ("channel.add", object ({ { "name", name }, { "instrument", c.deviceJson() } }));
        step ("channel.set", object ({ { "channel", channel + ".channel" }, { "group", "Track creator" } }));
        if (c.kind == Choice::Kind::preset && c.source.stockType == "spinus.3x-osc" && c.preset.startsWith ("program:"))
            step ("device.loadPreset", object ({ { "device", channel + ".device" },
                                                 { "preset", c.preset.fromLastOccurrenceOf (":", false, false) } }));
        if (part.tuned && ! part.tuned->settings.empty())
        {
            auto* values = new juce::DynamicObject();
            for (const auto& [id, value] : part.tuned->settings)
                values->setProperty (id, juce::jlimit (0.0f, 1.0f, value));
            step ("device.setParameters",
                  object ({ { "device", channel + ".device" }, { "values", juce::var (values) } }));
        }
        for (const auto& item : part.effects)
            addItemSteps (steps, item, channel + ".insert");
        if (std::abs (part.volumeDb) >= 0.05)
            step ("insert.set", object ({ { "insert", channel + ".insert" },
                                          { "volumeDb", juce::jlimit (-100.0, 12.0, part.volumeDb) } }));
        juce::Array<juce::var> notes;
        for (const auto& n : auditionPhrase (part.target, c, tempo, plan.summary))
            notes.add (object ({ { "start", ticks (n.start) },
                                 { "length", std::max<juce::int64> (1, ticks (n.length)) },
                                 { "pitch", n.note },
                                 { "velocity", (double)n.velocity } }));
        if (! notes.isEmpty())
            step ("pattern.setNotes", object ({ { "pattern", pattern + ".pattern" },
                                                { "channel", channel + ".channel" },
                                                { "notes", notes } }));
        ++count;
    }
    for (const auto& item : plan.master)
        addItemSteps (steps, item, (int)model::masterInsertId.value);
    return object ({ { "summary", "sounds for " + juce::String (count) + (count == 1 ? " part" : " parts") },
                     { "steps", steps } });
}

//==============================================================================
juce::var instrumentRequest (const SoundSource& source)
{
    return object ({ { "format", source.format },
                     { "file", source.file },
                     { "name", source.pluginName.isNotEmpty() ? source.pluginName : source.name },
                     { "uniqueId", source.uniqueId },
                     { "stockType", source.stockType } });
}

namespace
{
juce::String helperProblem (const SoundIndexer::Run& run, const juce::String& what)
{
    return run.exitCode == -1 ? juce::String ("Spinus's helper program is missing.")
           : run.hung         ? "It stopped answering while " + what + "; it was stopped."
                              : "It crashed while " + what + " (in Spinus's helper: Spinus carries on).";
}

juce::var notesVar (const std::vector<OfflineInstrument::Note>& notes)
{
    juce::Array<juce::var> list;
    for (const auto& n : notes)
        list.add (object (
            { { "note", n.note }, { "start", n.start }, { "length", n.length }, { "velocity", (double)n.velocity } }));
    return list;
}
} // namespace

juce::Array<juce::var> renderInHelper (const SoundIndexer::Options& options, const SoundSource& source,
                                       const juce::Array<juce::var>& items, const std::function<bool()>& shouldStop,
                                       juce::String& problem)
{
    juce::Array<juce::var> rendered;
    auto request = instrumentRequest (source);
    request.getDynamicObject()->setProperty ("mode", "render");
    request.getDynamicObject()->setProperty ("items", items);
    const auto run = SoundIndexer::runHelper (
        options, request, source.hostLibraries,
        [&] (const juce::var& line)
        {
            if (line["rendered"].isObject())
                rendered.add (line["rendered"]);
            if (line.hasProperty ("error"))
                problem = line["error"].toString();
        },
        shouldStop);
    if (! run.finished && problem.isEmpty())
        problem = shouldStop && shouldStop() ? juce::String ("Stopped.") : helperProblem (run, "playing");
    return rendered;
}

std::optional<SynthMatchResult> tuneInHelper (const SoundIndexer::Options& options, const SoundSource& source,
                                              const juce::String& preset, const juce::File& target, double start,
                                              double length, const std::vector<OfflineInstrument::Note>& notes,
                                              int tries, const std::function<bool()>& shouldStop,
                                              const std::function<void (int, double)>& progress, juce::String& problem)
{
    auto request = instrumentRequest (source);
    auto* o = request.getDynamicObject();
    o->setProperty ("mode", "tune");
    o->setProperty ("preset", preset);
    o->setProperty ("target",
                    object ({ { "file", target.getFullPathName() }, { "start", start }, { "length", length } }));
    o->setProperty ("notes", notesVar (notes));
    o->setProperty ("tries", tries);
    std::optional<SynthMatchResult> found;
    const auto run = SoundIndexer::runHelper (
        options, request, source.hostLibraries,
        [&] (const juce::var& line)
        {
            if (const auto& p = line["progress"]; p.isObject() && progress)
                progress ((int)p["tries"], (double)p["best"]);
            if (line.hasProperty ("error"))
                problem = line["error"].toString();
            if (const auto& r = line["result"]; r.isObject())
            {
                SynthMatchResult result;
                if (const auto* settings = r["settings"].getDynamicObject())
                    for (const auto& [id, value] : settings->getProperties())
                        result.settings.emplace_back (id.toString(), (float)(double)value);
                result.startDistance = (double)r["startDistance"];
                result.distance = (double)r["distance"];
                result.tries = (int)r["tries"];
                found = std::move (result);
            }
        },
        shouldStop);
    if (! run.finished && problem.isEmpty())
        problem = shouldStop && shouldStop() ? juce::String ("Stopped.") : helperProblem (run, "being tuned");
    if (! run.finished)
        found.reset();
    return found;
}

juce::Result loadSounds (Session& session, const Plan& plan, const std::vector<model::ChannelId>& channels)
{
    juce::StringArray problems;
    for (size_t i = 0; i < plan.parts.size() && i < channels.size(); ++i)
    {
        const auto& part = plan.parts[i];
        const auto& c = part.choice;
        const auto channel = channels[i];
        if (channel == model::ChannelId {})
            continue;
        juce::Result result = juce::Result::ok();
        if (c.kind == Choice::Kind::sample)
            result = loadSamplerFile (session, channel, juce::File (c.file), c.rootNote());
        else if (c.kind == Choice::Kind::drumSamples)
        {
            static const std::map<juce::String, int> pads { { "kick", 60 }, { "snare", 61 }, { "hat", 63 } };
            for (const auto& [drum, file] : c.drums)
                if (const auto pad = pads.find (drum); pad != pads.end() && result.wasOk())
                    result = loadDrumPadFile (session, channel, pad->second, juce::File (file));
        }
        else if ((c.kind == Choice::Kind::preset || c.kind == Choice::Kind::kit) && c.preset.isNotEmpty() &&
                 ! (c.source.stockType == "spinus.3x-osc" && c.preset.startsWith ("program:")))
        {
            // As closest preset loads one: through the plugin's own way of loading presets.
            const auto* project = session.getProject().findChannel (channel);
            const auto device = project != nullptr ? project->instrument : std::nullopt;
            auto* plugin =
                device ? dynamic_cast<tracktion::ExternalPlugin*> (session.getAdapter().getDevicePlugin (device->id))
                       : nullptr;
            auto* instance = plugin != nullptr ? plugin->getAudioPluginInstance() : nullptr;
            if (instance == nullptr || ! device)
                result = juce::Result::fail (c.source.name + " is not loaded.");
            else
            {
                const auto presets = InstrumentPresets::find (*instance, *device);
                const auto found = std::find_if (presets.begin(), presets.end(),
                                                 [&] (const InstrumentPreset& p) { return p.id == c.preset; });
                result = found == presets.end() ? juce::Result::fail (c.source.name + " no longer has " + c.name + ".")
                                                : loadInstrumentPreset (session, channel, *found);
            }
        }
        // The tuned settings on top of the preset (they are also in the project, set by the proposal).
        if (result.wasOk() && part.tuned)
            SynthMatcher::apply (session, channel, *part.tuned);
        if (result.failed())
            problems.add ((part.target.name.isNotEmpty() ? part.target.name : displayName (part.target.part)) + ": " +
                          result.getErrorMessage());
    }
    return problems.isEmpty() ? juce::Result::ok() : juce::Result::fail (problems.joinIntoString (" "));
}

int Choice::rootNote() const
{
    if (fundamental > 0.0)
        return juce::jlimit (0, 127, juce::roundToInt (69.0 + 12.0 * std::log2 (fundamental / 440.0)));
    return 36;
}

} // namespace spinus::parts
