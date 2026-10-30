#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ai/Proposal.h"
#include "commands/CommandHistory.h"
#include "session/PartSounds.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <iostream>
#include <thread>

using namespace spinus;

namespace
{
/** A folder of its own for an index a test never saves (or saves only for itself): nothing left for the next
    test or run to find. */
struct UnusedFolder
{
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getNonexistentChildFile ("spinus-unused-index", {}, false);
    ~UnusedFolder() { folder.deleteRecursively(); }
};
} // namespace

using namespace spinus::parts;
using Catch::Approx;

namespace
{
constexpr double rate = 48000.0;
constexpr double pi = 3.14159265358979323846;

juce::var object (std::initializer_list<std::pair<const char*, juce::var>> items)
{
    auto* o = new juce::DynamicObject();
    for (auto& [key, value] : items)
        o->setProperty (key, value);
    return juce::var (o);
}

double frequencyOf (int note) { return 440.0 * std::pow (2.0, (note - 69) / 12.0); }
double saw (double phase) { return 2.0 * (phase - std::floor (phase)) - 1.0; }

/** A synthetic instrument: one wave with an attack, an optional decay and a release. */
struct Voice
{
    double attack = 0.005, decayTime = 0.0, release = 0.05;
    std::function<double (double)> wave = saw;
    bool mono = false;
    int lowest = 0, highest = 127;
};

/** Plays the notes as the voice would (mono, at `rate`). */
juce::AudioBuffer<float> play (const Voice& v, const std::vector<sounds::Note>& notes, double tail)
{
    double end = 0.0;
    for (auto& n : notes)
        end = std::max (end, n.start + n.length);
    juce::AudioBuffer<float> b (1, (int)((end + tail) * rate));
    b.clear();
    std::vector<sounds::Note> playing = notes;
    if (v.mono && playing.size() > 1 && std::abs (playing.front().start - playing.back().start) < 1.0e-9)
        playing = { playing.back() };
    for (auto& n : playing)
    {
        if (n.note < v.lowest || n.note > v.highest)
            continue;
        const auto f = frequencyOf (n.note);
        double releasedAt = -1.0;
        for (int i = (int)(n.start * rate); i < b.getNumSamples(); ++i)
        {
            const auto t = i / rate - n.start;
            double g = t < v.attack ? t / v.attack : 1.0;
            if (v.decayTime > 0.0)
                g *= std::exp (-std::max (0.0, t - v.attack) / v.decayTime);
            if (t >= n.length)
            {
                if (releasedAt < 0)
                    releasedAt = g;
                g = releasedAt * std::max (0.0, 1.0 - (t - n.length) / v.release);
                if (g <= 0.0)
                    break;
            }
            b.addSample (0, i, (float)(0.3 * v.wave (f * t) * g));
        }
    }
    return b;
}

juce::var presetOf (const Voice& v, const juce::String& name, const juce::String& category)
{
    const sounds::Player player = [v] (const std::vector<sounds::Note>& notes, double tail)
    { return play (v, notes, tail); };
    auto sound = sounds::toVar (sounds::describeInstrument (player, rate, name, category));
    sound.getDynamicObject()->setProperty ("id", name);
    sound.getDynamicObject()->setProperty ("name", name);
    sound.getDynamicObject()->setProperty ("category", category);
    return sound;
}

/** A bass line on a mono saw: eighths in E1-A2, as a stem would hold it, and its notes. */
std::pair<juce::AudioBuffer<float>, std::vector<StemNote>> bassStem (const Voice& v)
{
    std::vector<StemNote> notes;
    std::vector<sounds::Note> played;
    const int line[] = { 28, 28, 40, 31, 33, 45, 35, 28 };
    for (int i = 0; i < 32; ++i)
    {
        const auto start = 0.5 + i * 0.5;
        notes.push_back ({ line[i % 8], start, start + 0.4 });
        played.push_back ({ line[i % 8], start, 0.4, 0.8f });
    }
    juce::AudioBuffer<float> audio (1, (int)(17.5 * rate));
    audio.clear();
    for (const auto& n : played)
    {
        auto one = play (v, { { n.note, 0.0, n.length, 0.8f } }, 0.3);
        audio.addFrom (0, (int)(n.start * rate), one, 0, 0,
                       juce::jmin (one.getNumSamples(), audio.getNumSamples() - (int)(n.start * rate)));
    }
    return { audio, notes };
}

SoundSource sourceNamed (const juce::String& key, const juce::String& name, const juce::String& stockType = {})
{
    SoundSource s;
    s.key = key;
    s.name = name;
    s.stockType = stockType;
    s.format = stockType.isNotEmpty() ? "Spinus" : "CLAP";
    s.file = "/plugins/" + name + ".clap";
    s.pluginName = name;
    return s;
}

juce::var summaryWith (const juce::String& parts)
{
    return juce::JSON::parse (
        R"({"kind": "spinus.style-summary", "tempo": {"median": 128.0}, "keys": [{"name": "E minor", "count": 2}],
        "loudness": {"lufs": -9.0},
        "drums": {"lanes": {"kick": [0.9,0,0,0, 0.9,0,0,0, 0.9,0,0,0, 0.9,0,0,0],
                            "snare": [0,0,0,0, 0.8,0,0,0, 0,0,0,0, 0.8,0,0,0],
                            "hats": [0.7,0,0.7,0, 0.7,0,0.7,0, 0.7,0,0.7,0, 0.7,0,0.7,0]}},
        "parts": )" +
        parts + "}");
}
} // namespace

//==============================================================================
TEST_CASE ("Sounds per part: the parts of a summary and the roles that fit them", "[sounds][parts]")
{
    const auto summary =
        summaryWith (R"({"bass": {"songs": 3, "low": 28, "median": 36, "high": 45, "notesPerBar": 6, "voices": 1.05},
        "other": {"songs": 3, "low": 48, "median": 60, "high": 76, "notesPerBar": 4, "voices": 2.6},
        "vocals": {"songs": 2, "low": 55, "median": 64, "high": 76, "notesPerBar": 3, "voices": 1.1},
        "drums": {"songs": 3, "notesPerBar": 12}, "piano": {"songs": 0}})");
    const auto targets = targetsFromSummary (summary);
    REQUIRE (targets.size() == 4);
    CHECK (targets[0].part == "bass");
    CHECK (targets[0].roles == juce::StringArray { "bass" });
    CHECK_FALSE (targets[0].chords);
    CHECK (targets[1].part == "other");
    CHECK (targets[1].chords);
    CHECK (targets[1].roles[0] == "pad"); // chords, few notes per bar
    CHECK (targets[2].roles == juce::StringArray { "lead" });
    CHECK (targets[3].roles == juce::StringArray { "drums" });
    CHECK (targets[3].tempo == Approx (128.0));

    // A busier part without chords: a pluck or a lead.
    const auto busy = targetsFromSummary (summaryWith (
        R"({"other": {"songs": 1, "low": 60, "median": 70, "high": 80, "notesPerBar": 12, "voices": 1.1}})"));
    REQUIRE (busy.size() == 1);
    CHECK (busy[0].roles[0] == "pluck");
}

TEST_CASE ("Sounds per part: guitars in the reference songs make guitar parts that follow their guitar stem",
           "[sounds][parts]")
{
    auto summary = summaryWith (R"({"bass": {"songs": 4, "low": 28, "median": 36, "high": 45, "notesPerBar": 6},
        "guitar": {"songs": 3, "low": 40, "median": 47, "high": 59, "notesPerBar": 10, "voices": 2.4},
        "drums": {"songs": 4, "notesPerBar": 12}})");
    // No guitars measured (an older analysis): the usual parts.
    CHECK (guitarRoles (summary).isEmpty());
    auto roles = [] (const std::vector<SongPart>& list)
    {
        juce::StringArray r;
        for (const auto& p : list)
            r.add (p.role);
        return r;
    };
    CHECK (roles (defaultParts ("sketch", summary)) == juce::StringArray { "drums", "bass", "chords", "lead" });

    auto* o = summary.getDynamicObject();
    o->setProperty ("songs", 4);
    o->setProperty ("guitars",
                    juce::JSON::parse (R"({"measured": 4, "songs": 3, "distorted": 3, "clean": 0, "lead": 2})"));
    CHECK (guitarRoles (summary) == juce::StringArray { "rhythm-guitar", "lead-guitar" });
    CHECK (roles (defaultParts ("sketch", summary)) ==
           juce::StringArray { "drums", "bass", "rhythm-guitar", "lead", "lead-guitar" });
    const auto full = roles (defaultParts ("full", summary));
    CHECK (full.contains ("rhythm-guitar"));
    CHECK_FALSE (full.contains ("chords"));

    // The guitar stem is a target of its own, and a guitar part follows its measured range.
    const auto targets = targetsFromSummary (summary);
    const auto stem =
        std::find_if (targets.begin(), targets.end(), [] (const Target& t) { return t.part == "guitar"; });
    REQUIRE (stem != targets.end());
    CHECK (stem->chords);
    CHECK (stem->names.contains ("guitar"));
    const auto rhythm = targetFor ({ "rhythm-guitar", "Rhythm guitar", "rhythm-guitar" }, 0, summary);
    CHECK (rhythm.part == "guitar");
    CHECK (rhythm.low == 40);
    CHECK (rhythm.median == 47);
    CHECK (rhythm.measure);
    const auto lead = targetFor ({ "lead-guitar", "Lead guitar", "lead-guitar" }, 0, summary);
    CHECK (lead.median > rhythm.median);
    CHECK_FALSE (lead.measure);

    // Without a guitar stem in the songs, a guitar part keeps its usual range and is not measured.
    const auto none = targetFor ({ "rhythm-guitar", "Rhythm guitar", "rhythm-guitar" }, 0,
                                 summaryWith (R"({"bass": {"songs": 1, "low": 28, "median": 36, "high": 45}})"));
    CHECK (none.part == "other");
    CHECK (none.median == 50);
    CHECK_FALSE (none.measure);
}

TEST_CASE ("Sounds per part: a stem is measured over its own notes", "[sounds][parts]")
{
    Voice v;
    v.mono = true;
    v.attack = 0.02;
    v.highest = 60;
    const auto [stem, notes] = bassStem (v);
    const auto measured = measureNotes (stem.getReadPointer (0), stem.getNumSamples(), rate, notes);
    CHECK (measured.count() >= 20);
    const auto m = measured.median();
    CHECK (m.attack == Approx (0.022).margin (0.01)); // to 90 % of the loudest 10 ms
    CHECK (m.sustain > 0.6);                          // held at its level
    CHECK (measured.knowsSustain());
    CHECK (measured.medianPitch() >= 28);
    CHECK (measured.medianPitch() <= 45);
    // A naive saw's centroid sits a few harmonics up.
    CHECK (measured.medianHarmonics() > 2.0);

    // Notes that ring into each other are not clear: a held chord under the next one is left out.
    const std::vector<StemNote> overlapping { { 40, 0.5, 3.0 }, { 43, 1.0, 1.5 }, { 47, 2.0, 2.5 } };
    CHECK (measureNotes (stem.getReadPointer (0), stem.getNumSamples(), rate, overlapping).count() == 1);
}

TEST_CASE ("Sounds per part: the closest presets of the right role and register come first", "[sounds][parts]")
{
    Voice close;
    close.mono = true;
    close.attack = 0.02;
    close.highest = 72;
    Voice slow = close;
    slow.attack = 0.4;
    Voice high = close;
    high.lowest = 60;
    high.highest = 127;
    Voice pad;
    pad.attack = 0.5;
    pad.release = 0.8;
    pad.wave = [] (double p) { return std::sin (2 * pi * p); };

    const UnusedFolder unused;
    SoundIndex index (unused.folder);
    auto& synth = index.instruments["plugin-synth"];
    synth.source = sourceNamed ("plugin-synth", "Synth");
    synth.sounds.add (presetOf (slow, "Slow Bass", "Basses"));
    synth.sounds.add (presetOf (close, "Close Bass", "Basses"));
    synth.sounds.add (presetOf (high, "High Bass", "Basses")); // does not reach the part's low notes
    synth.sounds.add (presetOf (pad, "Warm Pad", "Pads"));

    const auto [stem, notes] = bassStem (close);
    Reference reference;
    reference.notes = measureNotes (stem.getReadPointer (0), stem.getNumSamples(), rate, notes);
    Target bass;
    bass.part = "bass";
    bass.roles = { "bass" };
    bass.low = 28;
    bass.median = 36;
    bass.high = 45;

    const auto found = choose (bass, reference, index, 5);
    REQUIRE (found.size() == 3); // the pad is no bass
    CHECK (found[0].name == "Close Bass");
    CHECK (found[0].compared);
    CHECK (found[0].reasons().contains ("attack"));
    CHECK (found[0].reasons().contains ("its name says"));
    CHECK (found[1].name == "Slow Bass"); // fits every rule, but its attack is far off
    CHECK (found[2].name == "High Bass"); // closer sound, but it does not cover the register
    CHECK (found[2].reasons().contains ("not all of it"));
    CHECK (found[0].distance < found[1].distance);
    CHECK (found[0].title() == "Synth: Close Bass");
    CHECK (found[0].deviceJson()["plugin"]["name"].toString() == "Synth");

    // Chords: only polyphonic presets fit first.
    Target chords;
    chords.part = "other";
    chords.roles = { "pad", "keys" };
    chords.chords = true;
    chords.low = 48;
    chords.high = 72;
    const auto forChords = choose (chords, {}, index, 5);
    REQUIRE_FALSE (forChords.empty());
    CHECK (forChords[0].name == "Warm Pad");
    CHECK_FALSE (forChords[0].compared); // no stem to compare with: by the rules alone

    // No preset of that role at all: the closest of another, saying so.
    Target lead;
    lead.part = "vocals";
    lead.roles = { "lead" };
    lead.low = 55;
    lead.high = 76;
    const auto forLead = choose (lead, {}, index, 2);
    REQUIRE (forLead.size() == 2);
    CHECK (forLead[0].reasons().contains ("no lead sound in the index"));
}

TEST_CASE ("Sounds per part: drums get the closest kick, snare and hat samples", "[sounds][parts]")
{
    // One-shots with known measurements, as the index keeps them.
    auto sample =
        [] (const juce::String& path, const juce::String& cls, double attack, double length, double brightness)
    {
        sounds::Descriptors d;
        d.attack = attack;
        d.length = length;
        d.brightness = brightness;
        d.noisiness = cls == "kick" ? 0.05 : 0.6;
        d.low = cls == "kick" ? 0.8 : 0.05;
        d.high = cls == "hat" ? 0.7 : 0.1;
        SoundIndex::Sample s;
        s.path = path;
        s.info = object (
            { { "kind", "one-shot" }, { "class", cls }, { "sound", sounds::toVar (d) }, { "duration", length } });
        return s;
    };
    const UnusedFolder unused;
    SoundIndex index (unused.folder);
    index.samples = { sample ("/s/Kick Long.wav", "kick", 0.003, 0.9, 90.0),
                      sample ("/s/Kick Short.wav", "kick", 0.003, 0.25, 120.0),
                      sample ("/s/Snare A.wav", "snare", 0.002, 0.3, 2500.0),
                      sample ("/s/Snare B.wav", "snare", 0.002, 0.6, 1200.0),
                      sample ("/s/Hat A.wav", "hat", 0.001, 0.08, 9000.0),
                      sample ("/s/Hat B.wav", "hat", 0.001, 0.4, 6000.0) };
    auto loop = sample ("/s/Loop 127 bpm.wav", "drums", 0.003, 7.56, 3000.0);
    loop.info.getDynamicObject()->setProperty ("kind", "loop");
    loop.info.getDynamicObject()->setProperty ("tempo", 127.0);
    index.samples.push_back (loop);

    Reference reference;
    auto hits = [] (double length, double brightness, double noisiness, double low, double high)
    {
        Measured m;
        for (int i = 0; i < 5; ++i)
        {
            sounds::Descriptors d;
            d.attack = 0.003;
            d.length = length;
            d.brightness = brightness;
            d.noisiness = noisiness;
            d.low = low;
            d.high = high;
            m.sounds.push_back (d);
            m.held.push_back (0.0);
        }
        return m;
    };
    reference.hits["kick"] = hits (0.25, 115.0, 0.05, 0.8, 0.1);
    reference.hits["snare"] = hits (0.6, 1300.0, 0.6, 0.05, 0.1);
    reference.hits["hat"] = hits (0.08, 9000.0, 0.6, 0.05, 0.7);
    Target drums;
    drums.part = "drums";
    drums.roles = { "drums" };
    drums.tempo = 128.0;

    const auto found = choose (drums, reference, index, 5);
    REQUIRE (found.size() == 4); // two sets of samples, Spinus's own Drum Machine and the loop
    CHECK (found[0].kind == Choice::Kind::drumSamples);
    CHECK (found[0].drums.at ("kick") == "/s/Kick Short.wav");
    CHECK (found[0].drums.at ("snare") == "/s/Snare B.wav");
    CHECK (found[0].drums.at ("hat") == "/s/Hat A.wav");
    CHECK (found[0].compared);
    CHECK (found[1].drums.at ("kick") == "/s/Kick Long.wav");
    const auto ownKit =
        std::find_if (found.begin(), found.end(), [] (const Choice& c)
                      { return c.kind == Choice::Kind::kit && c.source.stockType == "spinus.drum-machine"; });
    REQUIRE (ownKit != found.end());
    CHECK (ownKit->drumNotes.at ("kick") == 60);
    const auto drumLoop =
        std::find_if (found.begin(), found.end(), [] (const Choice& c) { return c.kind == Choice::Kind::loop; });
    REQUIRE (drumLoop != found.end());
    CHECK (drumLoop->reasons().contains ("127.0 BPM"));
    CHECK (found[0].deviceJson()["stockType"].toString() == "spinus.drum-machine");
}

TEST_CASE ("Sounds per part: the role guess learnt from presets that name their role", "[sounds][parts]")
{
    // Two kinds of preset: dark one-note sounds named basses, bright slow chords named pads; then unnamed
    // ones of each kind.
    juce::Random random (3); // the same every time
    auto jitter = [&random] (auto&) { return 0.9 + 0.2 * random.nextDouble(); };
    const UnusedFolder unused;
    SoundIndex index (unused.folder);
    auto& synth = index.instruments["plugin-synth"];
    synth.source = sourceNamed ("plugin-synth", "Synth");
    auto make = [&] (const juce::String& name, const juce::String& role, bool bass)
    {
        juce::DynamicObject::Ptr sound = new juce::DynamicObject();
        sounds::Descriptors d;
        d.attack = (bass ? 0.005 : 0.3) * jitter (random);
        d.sustain = bass ? 0.9 : 0.7;
        d.release = (bass ? 0.05 : 0.8) * jitter (random);
        d.brightness = (bass ? 300.0 : 3000.0) * jitter (random);
        d.harmonicBrightness = bass ? 2.0 : 12.0;
        d.fundamental = bass ? 65.0 : 130.0;
        sound->setProperty ("name", name);
        sound->setProperty ("id", name);
        sound->setProperty ("role", role);
        sound->setProperty ("roleFrom", role.isNotEmpty() ? "name" : "sound");
        sound->setProperty ("soundRole", "lead");
        sound->setProperty ("tracksPitch", true);
        sound->setProperty ("polyphonic", ! bass);
        sound->setProperty ("low", 24);
        sound->setProperty ("high", bass ? 60 : 96);
        sound->setProperty ("sound", sounds::toVar (d));
        synth.sounds.add (juce::var (sound.get()));
    };
    for (int i = 0; i < 40; ++i)
    {
        make ("Bass " + juce::String (i), "bass", true);
        make ("Pad " + juce::String (i), "pad", false);
    }
    // Too few roles to learn from (two): the rules decide.
    make ("Mystery Low", "", true);
    index.learnRoles();
    CHECK (index.roleExamples == 0);
    CHECK (synth.sounds.getLast()["soundRoleFrom"].toString() == "rules");

    // With four roles named, the guess is learnt.
    for (int i = 0; i < 10; ++i)
    {
        make ("Lead " + juce::String (i), "lead", true);
        make ("Keys " + juce::String (i), "keys", false);
    }
    make ("Mystery High", "", false);
    index.learnRoles();
    CHECK (index.roleExamples == 100);
    juce::var low, high;
    for (const auto& s : synth.sounds)
    {
        if (s["name"].toString() == "Mystery Low")
            low = s;
        if (s["name"].toString() == "Mystery High")
            high = s;
    }
    CHECK (low["soundRoleFrom"].toString() == "learnt");
    CHECK (juce::StringArray { "bass", "lead" }.contains (low["role"].toString()));
    CHECK (juce::StringArray { "pad", "keys" }.contains (high["role"].toString()));
    // A preset its name decides keeps its role; the sound's guess is kept beside it.
    CHECK (synth.sounds[0]["role"].toString() == "bass");
    CHECK (synth.sounds[0]["soundRoleFrom"].toString() == "learnt");
    CHECK (synth.sounds[0].hasProperty ("ruleRole"));
}

TEST_CASE ("Sounds per part: the proposal adds every part in one undo step", "[sounds][parts]")
{
    const auto summary =
        summaryWith (R"({"bass": {"songs": 1, "low": 28, "median": 36, "high": 45, "notesPerBar": 6, "voices": 1.0},
        "other": {"songs": 1, "low": 48, "median": 60, "high": 72, "notesPerBar": 4, "voices": 2.5},
        "drums": {"songs": 1, "notesPerBar": 12}})");
    const auto targets = targetsFromSummary (summary);
    REQUIRE (targets.size() == 3);
    Plan plan;
    plan.summary = summary;
    plan.tempo = 128.0;

    Plan::Part bass;
    bass.target = targets[0];
    bass.choice.kind = Choice::Kind::preset;
    bass.choice.source = sourceNamed ("stock-spinus.3x-osc", "3x Osc", "spinus.3x-osc");
    bass.choice.preset = "program:3:Reese Bass";
    bass.choice.name = "Reese Bass";
    bass.tuned = SynthMatchResult { { { "cutoff", 0.4f }, { "osc1_shape", 0.25f } }, 3.0, 2.0, 50 };
    bass.effects.add (
        object ({ { "kind", "eq" },
                  { "insert", "bass" },
                  { "bands", juce::JSON::parse (R"([{"type": "Bell", "freq": 250, "gain": -3.0, "q": 0.9}])") } }));
    bass.effects.add (object ({ { "kind", "compressor" },
                                { "insert", "bass" },
                                { "threshold", -20.0 },
                                { "ratio", 3.0 },
                                { "attack", 10.0 },
                                { "release", 120.0 },
                                { "makeup", 0.0 } }));
    bass.volumeDb = -4.5;
    plan.parts.push_back (bass);

    Plan::Part other;
    other.target = targets[1];
    other.choice.kind = Choice::Kind::sample;
    other.choice.file = "/samples/Keys C3.wav";
    plan.parts.push_back (other);

    Plan::Part drums;
    drums.target = targets[2];
    drums.choice.kind = Choice::Kind::drumSamples;
    drums.choice.drums = { { "kick", "/s/k.wav" }, { "snare", "/s/s.wav" }, { "hat", "/s/h.wav" } };
    plan.parts.push_back (drums);
    plan.master.add (
        object ({ { "kind", "limiter" }, { "insert", "Master" }, { "threshold", -1.0 }, { "gain", 6.0 } }));

    commands::CommandHistory history { model::Project {} };
    const auto before = history.getProject();
    auto proposal = ai::Proposal::prepare (makeProposal (plan), history);
    INFO (proposal.error);
    REQUIRE (proposal.canApply());
    proposal.undoName = "Track creator: sounds";
    REQUIRE (proposal.apply (history).wasOk());

    const auto& project = history.getProject();
    REQUIRE (project.channels.size() == before.channels.size() + 3);
    const auto& made = project.channels[before.channels.size()];
    CHECK (made.name == "Bass (Track creator)");
    CHECK (made.group == "Track creator");
    REQUIRE (made.instrument.has_value());
    CHECK (made.instrument.value().stockType == "spinus.3x-osc");
    CHECK (made.instrument.value().parameters.at ("cutoff") == Approx (0.4f));
    const auto* insert = project.findInsert (made.targetInsert);
    REQUIRE (insert != nullptr);
    CHECK (insert->volumeDb == Approx (-4.5));
    CHECK (insert->effects.size() == 2);
    CHECK (project.channels[before.channels.size() + 1].instrument.value().stockType == "spinus.sampler");
    CHECK (project.channels[before.channels.size() + 2].instrument.value().stockType == "spinus.drum-machine");
    const auto* master = project.findInsert (model::masterInsertId);
    REQUIRE (master != nullptr);
    CHECK (master->effects.size() == 1);
    REQUIRE (project.patterns.size() == before.patterns.size() + 1);
    CHECK (project.patterns.back().name == "Track creator: sounds");

    // One undo step takes it all away.
    REQUIRE (history.undo());
    CHECK (history.getProject().channels.size() == before.channels.size());
    CHECK (history.getProject().patterns.size() == before.patterns.size());
    CHECK (history.getProject().findInsert (model::masterInsertId)->effects.empty());
}

TEST_CASE ("Sounds per part: an audition phrase in the part's key and register", "[sounds][parts]")
{
    const auto summary =
        summaryWith (R"({"bass": {"songs": 1, "low": 28, "median": 36, "high": 45, "notesPerBar": 6, "voices": 1.0},
        "other": {"songs": 1, "low": 48, "median": 60, "high": 72, "notesPerBar": 4, "voices": 2.5},
        "drums": {"songs": 1, "notesPerBar": 12}})");
    const auto targets = targetsFromSummary (summary);
    Choice choice;
    const auto bass = auditionPhrase (targets[0], choice, 128.0, summary);
    REQUIRE_FALSE (bass.empty());
    for (const auto& n : bass)
    {
        CHECK (n.note >= 28 - 6);
        CHECK (n.note <= 45 + 6);
    }
    CHECK (bass[0].note % 12 == 4); // E minor's tonic
    const auto chords = auditionPhrase (targets[1], choice, 128.0, summary);
    CHECK (chords.size() == 12);                  // triads, two per bar, two bars
    CHECK (chords[1].note - chords[0].note == 3); // E G B: a minor third up
    choice.kind = Choice::Kind::drumSamples;
    const auto beat = auditionPhrase (targets[2], choice, 120.0, summary);
    CHECK (std::count_if (beat.begin(), beat.end(), [] (const auto& n) { return n.note == 60; }) == 8);
    CHECK (std::count_if (beat.begin(), beat.end(), [] (const auto& n) { return n.note == 61; }) == 4);
    CHECK (std::count_if (beat.begin(), beat.end(), [] (const auto& n) { return n.note == 63; }) == 16);
}

//==============================================================================
#if defined(SPINUS_PLUGIN_SCANNER) && defined(SPINUS_BROKEN_PLUGINS_DIR)
namespace
{
SoundSource testSynth (const juce::String& file = "test-synth.clap", const juce::String& name = "Spinus Test Synth")
{
    auto s = sourceNamed ("plugin-" + file, name);
    s.file = juce::File (SPINUS_BROKEN_PLUGINS_DIR).getChildFile (file).getFullPathName();
    s.fingerprint = "1";
    return s;
}

SoundIndexer::Options helper (int timeoutMs = 60000)
{
    SoundIndexer::Options o;
    o.helper = juce::File (SPINUS_PLUGIN_SCANNER);
    o.soundTimeoutMs = o.loadTimeoutMs = timeoutMs;
    return o;
}

juce::String presetId (const SoundIndex& index, const SoundSource& source, const juce::String& name)
{
    for (const auto& s : index.instruments.at (source.key).sounds)
        if (s["name"].toString() == name)
            return s["id"].toString();
    return {};
}

struct Temp
{
    Temp() { folder.createDirectory(); }
    ~Temp() { folder.deleteRecursively(); }
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("spinus-part-sounds-" + juce::Uuid().toString());
};
} // namespace

TEST_CASE ("Sounds per part: playing and tuning a synth in the helper, out of Spinus", "[sounds][parts][helper]")
{
    Temp temp;
    SoundIndex index (temp.folder.getChildFile ("index"));
    const auto synth = testSynth();
    SoundIndexer::update (index, helper(), { synth }, {}, {}, {});
    const auto dark = presetId (index, synth, "Dark Square"), saw = presetId (index, synth, "Resonant Saw");
    REQUIRE (dark.isNotEmpty());
    REQUIRE (saw.isNotEmpty());

    // The target: Dark Square playing a phrase, rendered in the helper.
    const std::vector<OfflineInstrument::Note> notes { { 36, 0.0, 0.4, 0.8f },
                                                       { 43, 0.5, 0.4, 0.8f },
                                                       { 36, 1.0, 0.8, 0.8f } };
    juce::Array<juce::var> items;
    juce::Array<juce::var> list;
    for (const auto& n : notes)
        list.add (object ({ { "note", n.note }, { "start", n.start }, { "length", n.length } }));
    const auto target = temp.folder.getChildFile ("target.wav");
    items.add (object ({ { "id", "target" },
                         { "preset", dark },
                         { "notes", list },
                         { "tail", 0.3 },
                         { "output", target.getFullPathName() } }));
    items.add (object ({ { "id", "missing" },
                         { "preset", "no such preset" },
                         { "notes", list },
                         { "output", temp.folder.getChildFile ("x.wav").getFullPathName() } }));
    items.add (
        object ({ { "id", "cut" },
                  { "cut", object ({ { "file", target.getFullPathName() }, { "from", 0.5 }, { "length", 0.5 } }) },
                  { "output", temp.folder.getChildFile ("cut.wav").getFullPathName() } }));
    juce::String problem;
    const auto rendered = renderInHelper (helper(), synth, items, {}, problem);
    INFO (problem);
    CHECK (problem.isEmpty());
    REQUIRE (rendered.size() == 3);
    CHECK (rendered[0]["output"].toString() == target.getFullPathName());
    CHECK ((double)rendered[0]["peak"] > 0.01);
    CHECK (rendered[1]["problem"].toString().contains ("no preset"));
    CHECK (temp.folder.getChildFile ("cut.wav").existsAsFile());
    std::unique_ptr<juce::AudioFormatReader> reader (
        juce::WavAudioFormat().createReaderFor (target.createInputStream().release(), true));
    REQUIRE (reader != nullptr);
    CHECK (reader->numChannels == 2);

    // Tuned from Resonant Saw toward it: closer than where it started.
    int reports = 0;
    const auto tuned =
        tuneInHelper (helper(), synth, saw, target, 0.0, 1.8, notes, 60, {}, [&] (int, double) { ++reports; }, problem);
    INFO (problem);
    REQUIRE (tuned.has_value());
    CHECK (reports > 0);
    CHECK (tuned.value().tries >= 60); // the last generation is finished
    CHECK (tuned.value().distance < tuned.value().startDistance);
    CHECK_FALSE (tuned.value().settings.empty());

    // An instrument with nothing to tune says so.
    const auto organ = testSynth ("test-synth.clap", "Spinus Test Organ");
    problem.clear();
    CHECK_FALSE (tuneInHelper (helper(), organ, {}, target, 0.0, 1.8, notes, 20, {}, {}, problem).has_value());
    CHECK (problem.contains ("no settings"));
}

TEST_CASE ("Sounds per part: an instrument that loads its sounds late is played once it sounds", "[sounds][helper]")
{
    // Silent for its first seconds, and finding its presets crashes it: the part's notes are still heard (presets
    // are not looked for when none is asked for), as when the parts of a written song are measured for the mix.
    Temp temp;
    const auto synth = testSynth ("difficult-synth.clap", "Spinus Difficult Synth");
    juce::Array<juce::var> list;
    list.add (object ({ { "note", 48 }, { "start", 0.0 }, { "length", 0.5 } }));
    juce::Array<juce::var> items;
    items.add (object ({ { "id", "part" },
                         { "notes", list },
                         { "output", temp.folder.getChildFile ("part.wav").getFullPathName() } }));
    juce::String problem;
    const auto rendered = renderInHelper (helper(), synth, items, {}, problem);
    INFO (problem);
    CHECK (problem.isEmpty());
    REQUIRE (rendered.size() == 1);
    CHECK ((double)rendered[0]["peak"] > 0.01);
}

TEST_CASE ("Sounds per part: a synth that crashes or hangs while tuned takes only the helper down",
           "[sounds][parts][helper]")
{
    Temp temp;
    // The misbehaving synth's presets: "Crashes" and "Hangs" (left out of the index, so read from the plugin's list).
    SoundIndex index (temp.folder.getChildFile ("index"));
    const auto synth = testSynth ("misbehaving-synth.clap", "Spinus Misbehaving Synth");
    SoundIndexer::update (index, helper(), { synth }, {}, {}, {});
    const auto& entry = index.instruments.at (synth.key);
    REQUIRE (entry.problemPresets.size() == 2);
    const auto target = temp.folder.getChildFile ("target.wav");
    {
        juce::AudioBuffer<float> b (1, 96000);
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, (float)(0.3 * std::sin (2 * pi * 110.0 * i / rate)));
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (target);
        auto writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (16));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }
    const std::vector<OfflineInstrument::Note> notes { { 45, 0.0, 1.5, 0.8f } };
    for (const auto& id : entry.problemPresets)
    {
        juce::String problem;
        // Only here a short limit: the hang is the point.
        const auto tuned = tuneInHelper (helper (4000), synth, id, target, 0.0, 1.5, notes, 30, {}, {}, problem);
        CHECK_FALSE (tuned.has_value());
        INFO (problem);
        CHECK ((problem.contains ("crashed") || problem.contains ("stopped answering")));
    }

    // Stopped by the user: the helper is killed at once.
    juce::String problem;
    const auto started = juce::Time::getMillisecondCounterHiRes();
    std::atomic<bool> stop { false };
    std::thread stopper (
        [&]
        {
            juce::Thread::sleep (300);
            stop = true;
        });
    const auto tuned = tuneInHelper (
        helper(), testSynth(), {}, target, 0.0, 1.5, notes, 5000, [&] { return stop.load(); }, {}, problem);
    stopper.join();
    CHECK_FALSE (tuned.has_value());
    CHECK (problem == "Stopped.");
    CHECK (juce::Time::getMillisecondCounterHiRes() - started < 30000.0);
}
#endif

//==============================================================================
TEST_CASE ("Role guess on an instrument's measured presets (a file named by SPINUS_ROLE_EVAL)", "[.roles]")
{
    // A check by hand: the sound helper's output for an instrument whose presets' categories name their role
    // (Surge XT's factory presets: spinus-plugin-scanner --sound-index). Each named preset is guessed from the
    // others in 5 folds; prints how often the rules and the learnt guess agree with the name, per role.
    const juce::File file (juce::SystemStats::getEnvironmentVariable ("SPINUS_ROLE_EVAL", {}));
    if (! file.existsAsFile())
        SKIP ("set SPINUS_ROLE_EVAL to the helper's output (one JSON line per preset)");
    juce::StringArray lines;
    file.readLines (lines);
    std::vector<juce::var> named;
    for (const auto& line : lines)
    {
        const auto v = juce::JSON::parse (line);
        const auto& s = v["sound"];
        if (s.isObject() && ! s.hasProperty ("problem") && ! (bool)s["silent"] && s["roleFrom"].toString() == "name")
            named.push_back (s);
    }
    REQUIRE (named.size() > 100);
    auto foldOf = [] (const juce::var& s) { return (int)((juce::uint32)s["id"].toString().hashCode() % 5u); };
    std::map<juce::String, std::pair<int, int>> rules, learnt; // role: right, all
    for (int fold = 0; fold < 5; ++fold)
    {
        sounds::RoleGuess guess;
        for (const auto& s : named)
            if (foldOf (s) != fold)
                guess.add (s, s["role"].toString());
        guess.prepare();
        for (const auto& s : named)
            if (foldOf (s) == fold)
            {
                const auto role = s["role"].toString();
                ++rules[role].second;
                ++learnt[role].second;
                rules[role].first += s["ruleRole"].toString() == role ? 1 : 0;
                learnt[role].first += guess.guess (s) == role ? 1 : 0;
            }
    }
    auto line = [] (const char* name, const std::map<juce::String, std::pair<int, int>>& counts)
    {
        int right = 0, all = 0;
        juce::String text;
        for (const auto& [role, c] : counts)
        {
            right += c.first;
            all += c.second;
            text << " " << role << " " << juce::roundToInt (100.0 * c.first / juce::jmax (1, c.second)) << "% ("
                 << c.second << ")";
        }
        std::cout << name << ": " << juce::String (100.0 * right / juce::jmax (1, all), 1) << "% of " << all << ";"
                  << text << "\n";
        return (double)right / juce::jmax (1, all);
    };
    const auto byRules = line ("rules ", rules);
    const auto byLearning = line ("learnt", learnt);
    CHECK (byLearning > byRules);
}
