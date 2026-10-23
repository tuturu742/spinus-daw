#pragma once

// "Rainy Window", an original lo-fi hip hop piece in D minor at 82 BPM, made through the Command API.
// Included by LofiSong.cpp after its helpers and the showcase song's Song builder.

namespace lofi
{
using Ticks = juce::int64;
using showcase::Song;
using Ch = Song::Channel;
using P = Song::P;

constexpr double bpm = 82.0;
constexpr int songBars = 52;

struct Section
{
    const char* name;
    int startBar, bars;
};
constexpr Section sections[] = { { "Intro", 0, 4 },  { "Verse 1", 4, 16 },  { "Hook", 20, 8 },
                                 { "Break", 28, 4 }, { "Verse 2", 32, 12 }, { "Outro", 44, 8 } };

// The Drum Machine's pads (their notes): the song's own dusty kit on the first pads it uses.
enum Pad
{
    kick = 60,
    snare = 61,
    hat = 63,
    openHat = 64,
    rim = 68,
    shaker = 71
};

/** A note in beats, with what the Piano Roll's lane edits: velocity, pan, fine pitch and per-note volume. */
struct L
{
    double start, length;
    int pitch;
    double velocity = 0.75;
    double pan = 0.0, fine = 0.0, volume = 1.0;
};
using Notes = std::vector<L>;

model::Note toModel (const L& n)
{
    model::Note m;
    m.start = (Ticks)std::llround (n.start * (double)beat);
    m.length = std::max<Ticks> (1, (Ticks)std::llround (n.length * (double)beat));
    m.pitch = juce::jlimit (0, 127, n.pitch);
    m.velocity = (float)juce::jlimit (0.0, 1.0, n.velocity);
    m.pan = (float)juce::jlimit (-1.0, 1.0, n.pan);
    m.finePitchCents = (float)juce::jlimit (-100.0, 100.0, n.fine);
    if (n.volume < 1.0)
        m.expressions[model::Note::volumeExpression] = (float)n.volume;
    return m;
}

std::vector<model::Note> toModel (const Notes& notes)
{
    std::vector<model::Note> out;
    for (auto& n : notes)
        out.push_back (toModel (n));
    return out;
}

//==============================================================================
// Harmony. Progression A (the verses): Bbmaj9 | Am7 | Dm9 | Gm9 C13. Progression B (the hook):
// Gm9 | C13 | Fmaj9 | Bbmaj7 A7(b13). Voiced for the electric piano, bass note first.
struct Chord
{
    double start, length; // beats in a 4-bar loop
    std::vector<int> notes;
};
const std::vector<Chord> progressionA = {
    { 0, 4, { 46, 53, 57, 60, 62 } },  // Bbmaj9
    { 4, 4, { 45, 52, 55, 60, 64 } },  // Am7
    { 8, 4, { 50, 57, 60, 64, 65 } },  // Dm9
    { 12, 2, { 43, 53, 57, 58, 62 } }, // Gm9
    { 14, 2, { 48, 52, 57, 58, 62 } }, // C13
};
const std::vector<Chord> progressionB = {
    { 0, 4, { 43, 53, 57, 58, 62 } }, // Gm9
    { 4, 4, { 48, 52, 57, 58, 62 } }, // C13
    { 8, 4, { 41, 52, 55, 57, 60 } }, // Fmaj9
    { 12, 2, { 46, 53, 57, 62 } },    // Bbmaj7
    { 14, 2, { 45, 55, 61, 65 } },    // A7(b13)
};

/** The Piano Roll's chord stamp: the named chord on `root` (model::getChordTypes, as the stamp's list). */
std::vector<int> stamp (int root, const char* chordName)
{
    for (auto& type : model::getChordTypes())
        if (juce::String (type.name) == chordName)
            return model::chordOn (root, type);
    std::cerr << "No chord type " << chordName << "\n";
    std::exit (3);
}

/** The electric piano's comping: each chord struck where `hits` say (beats into the chord, length,
    velocity), each note a few cents off (fine pitch: an old, slightly detuned piano), then strummed up as
    the Piano Roll's Strum tool does it, and the velocities humanized as its Humanize tool does. */
std::vector<model::Note>
keysPart (const std::vector<Chord>& chords,
          const std::function<std::vector<std::tuple<double, double, double>> (const Chord&)>& hits, int seed,
          bool strumUp = true)
{
    std::mt19937 random ((unsigned)seed);
    std::uniform_real_distribution<double> cents (-7.0, 7.0);
    Notes notes;
    for (auto& chord : chords)
        for (auto [at, length, velocity] : hits (chord))
            for (size_t i = 0; i < chord.notes.size(); ++i)
                notes.push_back ({ chord.start + at, length, chord.notes[i], velocity - (i == 0 ? 0.12 : 0.0), 0.0,
                                   cents (random) });
    auto m = model::noteTools::strum (toModel (notes), ui::PianoRoll::strumSpacing, strumUp);
    juce::Random humanize (seed);
    return model::noteTools::humanizeVelocity (std::move (m), 0.08f, humanize);
}

std::vector<model::Note> keysVerse()
{
    return keysPart (
        progressionA,
        [] (const Chord& c) -> std::vector<std::tuple<double, double, double>>
        {
            if (c.length < 4)
                return { { 0, 1.9, 0.66 } };
            return { { 0, 2.3, 0.68 }, { 2.5, 1.3, 0.5 } };
        },
        11);
}

std::vector<model::Note> keysHook()
{
    return keysPart (
        progressionB,
        [] (const Chord& c) -> std::vector<std::tuple<double, double, double>>
        {
            if (c.length < 4)
                return { { 0, 1.9, 0.64 } };
            return { { 0, 1.4, 0.7 }, { 1.5, 0.4, 0.45 }, { 2.75, 1.15, 0.55 } };
        },
        12);
}

std::vector<model::Note> keysIntro()
{
    return keysPart (
        progressionA, [] (const Chord& c) -> std::vector<std::tuple<double, double, double>>
        { return { { 0, c.length - 0.1, 0.55 } }; }, 13, false);
}

std::vector<model::Note> keysOutro()
{
    auto chords = progressionB;
    chords.resize (3);
    chords.push_back ({ 12, 4, { 38, 53, 57, 60, 64, 69 } }); // Dm9, held to the end
    return keysPart (
        chords, [] (const Chord& c) -> std::vector<std::tuple<double, double, double>>
        { return { { 0, c.length, c.start >= 12 ? 0.62 : 0.55 } }; }, 14);
}

/** The pad: the same changes as plain stamped chords (Major 7, Minor 7, Seventh), held. */
std::vector<model::Note> padPart (int which) // 0: verse, 1: hook, 2: outro
{
    struct S
    {
        double start, length;
        int root;
        const char* type;
    };
    const std::vector<S> a = { { 0, 4, 58, "Major 7" },
                               { 4, 4, 57, "Minor 7" },
                               { 8, 4, 62, "Minor 7" },
                               { 12, 2, 55, "Minor 7" },
                               { 14, 2, 60, "Seventh" } };
    const std::vector<S> b = { { 0, 4, 55, "Minor 7" },
                               { 4, 4, 60, "Seventh" },
                               { 8, 4, 53, "Major 7" },
                               { 12, 2, 58, "Major 7" },
                               { 14, 2, 57, "Seventh" } };
    const std::vector<S> outro = {
        { 0, 4, 55, "Minor 7" }, { 4, 4, 60, "Seventh" }, { 8, 4, 53, "Major 7" }, { 12, 4, 62, "Minor 7" }
    };
    Notes notes;
    for (auto& s : which == 0 ? a : which == 1 ? b : outro)
        for (auto p : stamp (s.root, s.type))
            notes.push_back ({ s.start, s.length - 0.05, p, 0.6 });
    return toModel (notes);
}

/** The guitar: the verse chords' upper notes, an octave up, through the Piano Roll's Arpeggiate tool in
    eighth-note triplets. */
std::vector<model::Note> guitarPart()
{
    Notes chords;
    for (auto& c : progressionA)
        for (size_t i = 1; i < c.notes.size(); ++i)
            chords.push_back ({ c.start, c.length, c.notes[i] + 12, 0.6 });
    auto m = model::noteTools::arpeggiate (toModel (chords), beat / 3);
    for (size_t i = 0; i < m.size(); ++i)
    {
        m[i].velocity = (i % 3 == 0) ? 0.62f : 0.45f;
        m[i].pan = (float)(0.25 * std::sin ((double)i * 0.4));
    }
    juce::Random humanize (21);
    return model::noteTools::humanizeVelocity (std::move (m), 0.06f, humanize);
}

//==============================================================================
// Bass: 3x Osc's Sub Bass in Mono (slides): a note that overlaps the next slides into it.
Notes bassVerse()
{
    return {
        { 0, 1.5, 34, 0.9 },  { 1.75, 0.4, 34, 0.6 }, { 2.5, 1.0, 41, 0.75 },  { 3.5, 0.6, 32, 0.7 },
        { 4, 1.5, 33, 0.9 },  { 5.75, 0.4, 33, 0.6 }, { 6.5, 1.0, 40, 0.75 },  { 7.5, 0.6, 37, 0.7 },
        { 8, 1.5, 38, 0.9 },  { 9.75, 0.4, 38, 0.6 }, { 10.5, 1.0, 45, 0.75 }, { 11.5, 0.6, 36, 0.7 },
        { 12, 1.4, 31, 0.9 }, { 13.5, 0.4, 31, 0.6 }, { 14, 1.4, 36, 0.85 },   { 15.5, 0.45, 35, 0.65 },
    };
}

Notes bassHook()
{
    return {
        { 0, 1.5, 31, 0.9 },  { 1.75, 0.4, 31, 0.6 }, { 2.5, 1.0, 38, 0.75 },   { 3.5, 0.6, 35, 0.7 },
        { 4, 1.5, 36, 0.9 },  { 5.75, 0.4, 36, 0.6 }, { 6.5, 1.0, 43, 0.75 },   { 7.5, 0.6, 40, 0.7 },
        { 8, 1.5, 41, 0.9 },  { 9.75, 0.4, 41, 0.6 }, { 10.5, 1.0, 36, 0.75 },  { 11.5, 0.6, 35, 0.7 },
        { 12, 2.1, 34, 0.9 }, { 14, 1.4, 33, 0.85 },  { 15.5, 0.45, 40, 0.65 },
    };
}

Notes bassBreak()
{
    return {
        { 0, 4.1, 34, 0.85 }, { 4, 4.1, 33, 0.8 }, { 8, 4.1, 38, 0.85 }, { 12, 2.1, 31, 0.8 }, { 14, 1.9, 36, 0.8 }
    };
}

Notes bassOutro() { return { { 0, 3.6, 31, 0.85 }, { 4, 3.6, 36, 0.8 }, { 8, 3.6, 41, 0.8 }, { 12, 4.0, 38, 0.85 } }; }

//==============================================================================
// Melodies (make_audio.py sings the same ones: OOH_MELODY, HUM_MELODY).
Notes oohGuide()
{
    return { { 0.0, 1.75, 69 }, { 2.0, 1.5, 65 },  { 3.5, 0.5, 67 },  { 4.0, 1.5, 64 },   { 5.5, 2.25, 67 },
             { 8.0, 2.5, 69 },  { 10.5, 0.5, 67 }, { 11.0, 1.0, 72 }, { 12.0, 1.75, 70 }, { 14.0, 1.75, 69 } };
}

/** The flute's counter-melody over progression A; echo notes are softer through their per-note volume. */
Notes fluteCounter()
{
    return {
        { 0.5, 0.5, 69, 0.6 },    { 1.0, 0.5, 72, 0.65 },
        { 1.5, 1.5, 74, 0.75 },   { 3.5, 0.5, 72, 0.5, 0, 0, 0.6 },
        { 4.0, 1.0, 72, 0.7 },    { 5.0, 0.5, 69, 0.6 },
        { 5.5, 2.0, 67, 0.7 },    { 8.5, 0.5, 65, 0.6 },
        { 9.0, 0.5, 67, 0.62 },   { 9.5, 0.5, 69, 0.65 },
        { 10.0, 1.5, 72, 0.75 },  { 11.5, 0.5, 76, 0.55, 0, 0, 0.7 },
        { 12.0, 1.5, 74, 0.72 },  { 13.5, 0.5, 72, 0.55, 0, 0, 0.7 },
        { 14.0, 1.75, 70, 0.68 },
    };
}

/** The flute answering the hummed line, over progression B. */
Notes fluteAnswer()
{
    return {
        { 0.5, 0.5, 74, 0.62 },
        { 1.0, 0.5, 77, 0.66 },
        { 1.5, 1.0, 81, 0.78 },
        { 2.5, 0.5, 79, 0.6 },
        { 3.0, 1.0, 77, 0.68 },
        { 4.0, 0.75, 76, 0.7 },
        { 4.75, 0.25, 77, 0.5, 0, 0, 0.7 },
        { 5.0, 1.0, 76, 0.66 },
        { 6.0, 1.5, 72, 0.7 },
        { 8.5, 0.5, 69, 0.6 },
        { 9.0, 0.5, 72, 0.62 },
        { 9.5, 0.5, 76, 0.66 },
        { 10.0, 1.5, 79, 0.78 },
        { 11.5, 0.5, 77, 0.55, 0, 0, 0.7 },
        { 12.0, 1.5, 74, 0.72 },
        { 14.0, 1.9, 73, 0.7 },
    };
}

/** The answer again for the last bars, ending on D over the final Dm9 instead of C#. */
Notes fluteOutro()
{
    auto n = fluteAnswer();
    n.resize (n.size() - 2);
    n.push_back ({ 12.0, 1.0, 72, 0.66 });
    n.push_back ({ 13.0, 3.0, 74, 0.7 });
    return n;
}

//==============================================================================
// Drums, written on the Channel Rack's sixteenth steps (swing moves every second one) or on its
// triplet steps (six per beat). Hats lean left and right through their per-note pan.
L hit (double steps, int pad, double velocity, double pan = 0.0, double stepLength = 0.25)
{
    return { steps * stepLength, stepLength, pad, velocity, pan };
}

void hats (Notes& n, int barIndex, bool openAtEnd, double level = 1.0)
{
    static const double accents[] = { 0.55, 0.22, 0.38, 0.26 };
    for (int s = 0; s < 16; ++s)
    {
        if (openAtEnd && s >= 14)
            break;
        n.push_back (hit (barIndex * 16 + s, hat, accents[s % 4] * level, s % 2 == 0 ? -0.12 : 0.15));
    }
    if (openAtEnd)
        n.push_back (hit (barIndex * 16 + 14, openHat, 0.42 * level, 0.1));
}

Notes beatA()
{
    Notes n;
    for (int s : { 0, 10 })
        n.push_back (hit (s, kick, s == 0 ? 0.95 : 0.82));
    n.push_back (hit (7, kick, 0.5));
    for (int s : { 16, 18, 25, 27 })
        n.push_back (hit (s, kick, s == 16 ? 0.95 : s == 18 ? 0.55 : s == 25 ? 0.85 : 0.6));
    for (int s : { 4, 12, 20, 28 })
        n.push_back (hit (s, snare, 0.86));
    for (int s : { 6, 15, 23, 30 })
        n.push_back (hit (s, snare, 0.2));
    hats (n, 0, false);
    hats (n, 1, true);
    return n;
}

Notes beatB()
{
    Notes n;
    for (int s : { 0, 6, 10 })
        n.push_back (hit (s, kick, s == 0 ? 0.95 : s == 6 ? 0.6 : 0.85));
    for (int s : { 16, 19, 26 })
        n.push_back (hit (s, kick, s == 16 ? 0.95 : 0.7));
    for (int s : { 4, 12, 20, 28 })
        n.push_back (hit (s, snare, 0.86));
    for (int s : { 9, 14, 23, 31 })
        n.push_back (hit (s, rim, 0.38, 0.2));
    hats (n, 0, true);
    hats (n, 1, false);
    return n;
}

/** One bar on triplet steps: the beat, then a hat roll and a snare roll that grow into the next bar. */
Notes beatFill()
{
    Notes n;
    const double t = 1.0 / 6.0; // a triplet step, in beats
    n.push_back ({ 0, t, kick, 0.95 });
    n.push_back ({ 15 * t, t, kick, 0.8 });
    n.push_back ({ 6 * t, t, snare, 0.86 });
    n.push_back ({ 18 * t, t, snare, 0.8 });
    for (int s = 0; s < 18; s += 2)
        n.push_back ({ s * t, t, hat, s % 6 == 0 ? 0.5 : 0.3, s % 4 == 0 ? -0.15 : 0.15 });
    for (int s = 18; s < 24; ++s)
    {
        n.push_back ({ s * t, t, hat, 0.3 + 0.05 * (s - 18), -0.3 + 0.12 * (s - 18) });
        if (s >= 20)
            n.push_back ({ s * t, t, snare, 0.25 + 0.1 * (s - 20) });
    }
    return n;
}

/** Two bars of shaker on triplet steps, sweeping across through per-note pan. */
Notes shakerTriplets()
{
    Notes n;
    static const double accents[] = { 0.48, 0.18, 0.28, 0.42, 0.18, 0.28 };
    for (int s = 0; s < 48; ++s)
        n.push_back ({ s / 6.0, 1.0 / 6.0, shaker, accents[s % 6],
                       0.45 * std::sin (s * juce::MathConstants<double>::pi / 24.0) });
    return n;
}

Notes beatBreak()
{
    Notes n;
    for (int b = 0; b < 4; ++b)
    {
        const double o = b * 16;
        if (b % 2 == 0)
            n.push_back (hit (o, kick, 0.6));
        n.push_back (hit (o + 4, rim, 0.42, -0.1));
        n.push_back (hit (o + 12, rim, 0.46, -0.1));
        for (int s = 0; s < 16; s += 2)
            n.push_back (hit (o + s, shaker, s % 4 == 0 ? 0.32 : 0.2, 0.25));
        if (b == 3)
            for (int s : { 13, 14, 15 })
                n.push_back (hit (o + s, snare, 0.2 + 0.1 * (s - 13)));
    }
    return n;
}

Notes beatOutro()
{
    Notes n;
    for (int b = 0; b < 4; ++b)
    {
        const double o = b * 16, fade = 1.0 - 0.17 * b;
        n.push_back (hit (o, kick, 0.9 * fade));
        if (b < 2)
            n.push_back (hit (o + 10, kick, 0.8 * fade));
        n.push_back (hit (o + 4, b < 2 ? snare : rim, (b < 2 ? 0.8 : 0.45) * fade));
        n.push_back (hit (o + 12, b < 2 ? snare : rim, (b < 2 ? 0.8 : 0.45) * fade));
        Notes h;
        hats (h, 0, b == 1, fade);
        for (auto x : h)
        {
            x.start += o / 4;
            if (b < 3 || (int)std::lround (x.start * 4) % 2 == 0)
                n.push_back (x);
        }
    }
    n.push_back ({ 15.0, 1.0, openHat, 0.3, 0.1 });
    return n;
}

//==============================================================================
struct Builder : Song
{
    using Song::Song;

    void notes (int pattern, const Ch& ch, const std::vector<model::Note>& list)
    {
        juce::Array<juce::var> out;
        for (auto& n : list)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("start", (juce::int64)n.start);
            o->setProperty ("length", (juce::int64)n.length);
            o->setProperty ("pitch", n.pitch);
            o->setProperty ("velocity", n.velocity);
            if (std::abs (n.pan) > 0.001f)
                o->setProperty ("pan", n.pan);
            if (std::abs (n.finePitchCents) > 0.01f)
                o->setProperty ("finePitchCents", n.finePitchCents);
            if (! n.expressions.empty())
            {
                auto* e = new juce::DynamicObject();
                for (auto& [k, v] : n.expressions)
                    e->setProperty (k, v);
                o->setProperty ("expressions", juce::var (e));
            }
            out.add (juce::var (o));
        }
        c.run ("pattern.setNotes", object ({ { "pattern", pattern }, { "channel", ch.channel }, { "notes", out } }));
        noteCount += (int)list.size();
    }
    void notes (int pattern, const Ch& ch, const Notes& list) { notes (pattern, ch, toModel (list)); }

    int pattern (const juce::String& name, int bars, const juce::String& colour, double swing = 0.0,
                 bool triplets = false)
    {
        const auto id = addPattern (name, bars, colour);
        if (swing > 0)
            c.run ("pattern.setSwing", object ({ { "pattern", id }, { "swing", swing } }));
        if (triplets)
            c.run ("pattern.setTripletSteps", object ({ { "pattern", id }, { "triplets", true } }));
        return id;
    }

    int lane (const juce::String& name, const juce::String& colour, int under = 0)
    {
        const auto id = addLane (name);
        c.run ("lane.set", object ({ { "lane", id }, { "colour", colour } }));
        if (under > 0)
            c.run ("lane.set", object ({ { "lane", id }, { "group", under } }));
        return id;
    }

    /** The 0..1 value for a setting's text, by Spinus's one rule for typed settings (dsp/ParameterText.h): in
        the units the device shows ("450 ms", "4.5 kHz", "18%", "-16 dB"), a plain number in the setting's
        smallest unit (milliseconds, hertz, percent), a choice by its name ("Low pass"). */
    float knob (int device, const juce::String& parameterId, const juce::String& text) const
    {
        model::ParameterTarget target;
        target.kind = model::ParameterTarget::Kind::deviceParameter;
        target.device = model::DeviceId { (uint32_t)device };
        target.parameterId = parameterId;
        if (const auto v = session.parseTargetValue (target, text))
            return *v;
        // Another plugin's choice that does not say it is one (Surge XT Effects' "Tape"): by its name.
        if (auto* plugin = session.getAdapter().getDevicePlugin (target.device))
            for (int i = 0; i < plugin->getNumAutomatableParameters(); ++i)
                if (auto p = plugin->getAutomatableParameter (i); p != nullptr && p->paramID == parameterId)
                    for (int k = 0; k <= 4000; ++k)
                        if (const auto x = (float)k / 4000.0f;
                            p->valueToString (p->valueRange.convertFrom0to1 (x)).trim().equalsIgnoreCase (text.trim()))
                            return x;
        std::cerr << "No value " << text << " for " << parameterId << " on device " << device << "\n";
        std::exit (3);
    }

    void set (int device, std::initializer_list<std::pair<const char*, const char*>> values)
    {
        auto* o = new juce::DynamicObject();
        for (auto& [id, text] : values)
            o->setProperty (id, knob (device, id, text));
        c.run ("device.setParameters", object ({ { "device", device }, { "values", juce::var (o) } }));
    }

    template <typename T> T* processor (const Ch& ch) const
    {
        auto* plugin = dynamic_cast<tracktion::ExternalPlugin*> (
            session.getAdapter().getDevicePlugin (model::DeviceId { (uint32_t)ch.device }));
        auto* p = plugin != nullptr ? dynamic_cast<T*> (plugin->getAudioPluginInstance()) : nullptr;
        if (p == nullptr)
        {
            std::cerr << "No processor for channel " << ch.channel << "\n";
            std::exit (3);
        }
        return p;
    }

    /** Loads samples into a Sampler as dropping the files on its window does; `loop`: each zone loops all
        of itself while the key is held. */
    void loadSampler (const Ch& ch, const juce::Array<juce::File>& files, bool loop)
    {
        auto* sampler = processor<dsp::SamplerProcessor> (ch);
        if (auto r = sampler->loadFiles (files); r.failed())
        {
            std::cerr << "Sampler: " << r.getErrorMessage() << "\n";
            std::exit (3);
        }
        if (loop)
            for (int i = 0; i < (int)sampler->getZones().size(); ++i)
                sampler->updateZone (i,
                                     [] (dsp::SampleZone& z)
                                     {
                                         z.loop = true;
                                         z.loopStart = 0;
                                         z.loopEnd = z.getLength();
                                     });
        for (auto& z : sampler->getZones())
            std::cout << "  sampler zone " << z.name << ": root " << z.rootNote << ", keys " << z.lowKey << "-"
                      << z.highKey << ", " << z.getLength() << " samples" << (z.loop ? ", loops" : "") << "\n";
    }

    int audio (int laneId, const juce::File& file, int startBar, double bars, int channel, double gainDb,
               double offsetBars = 0.0, const juce::String& name = {})
    {
        auto params = object ({ { "lane", laneId },
                                { "file", file.getFullPathName() },
                                { "start", startBar * bar },
                                { "length", (juce::int64)std::llround (bars * (double)bar) },
                                { "gainDb", gainDb } });
        if (channel > 0)
            params.getDynamicObject()->setProperty ("channel", channel);
        if (name.isNotEmpty())
            params.getDynamicObject()->setProperty ("name", name);
        if (offsetBars > 0)
            params.getDynamicObject()->setProperty ("offset", (juce::int64)std::llround (offsetBars * (double)bar));
        const auto r = c.run ("clip.addAudio", params);
        lastAudioChannel = (int)r["channel"];
        ++audioClips;
        return (int)r["clip"];
    }

    int split (int clip, Ticks at)
    {
        const auto r = c.run ("clip.split", object ({ { "clip", clip }, { "at", at } }));
        return (int)r["clip"];
    }

    int lastAudioChannel = 0, audioClips = 0;
};

/** WAV from make_audio.py to FLAC (lossless, about half the size) for the project's audio folder. */
juce::File asFlac (const juce::File& wav, const juce::File& folder)
{
    auto& formats = dsp::SamplerProcessor::getFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (wav));
    if (reader == nullptr)
    {
        std::cerr << "Cannot read " << wav.getFullPathName() << " (run make_audio.py)\n";
        std::exit (3);
    }
    folder.createDirectory();
    const auto flac = folder.getChildFile (wav.getFileNameWithoutExtension() + ".flac");
    flac.deleteFile();
    juce::FlacAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream (flac.createOutputStream().release());
    auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                      .withSampleRate (reader->sampleRate)
                                                      .withNumChannels ((int)reader->numChannels)
                                                      .withBitsPerSample (16)
                                                      .withQualityOptionIndex (8));
    if (writer == nullptr)
        std::exit (3);
    writer->writeFromAudioReader (*reader, 0, reader->lengthInSamples);
    return flac;
}

//==============================================================================
int make (Session& session, ProjectDocument& document, const juce::File& folder, const juce::File& pluginFolder,
          const juce::File& audioFolder)
{
    if (folder.exists())
    {
        std::cerr << folder.getFullPathName() << " exists already: choose a new folder.\n";
        return 1;
    }
    if (! audioFolder.getChildFile ("samples/Rainy Keys_C5.wav").existsAsFile())
    {
        std::cerr << "--audio: a folder made by make_audio.py\n";
        return 1;
    }
    const auto started = Clock::now();
    Builder song (session, pluginFolder);
    auto& c = song.c;
    const auto samples = audioFolder.getChildFile ("samples");
    const auto flacs = audioFolder.getChildFile ("flac");

    c.run ("project.setTempo", object ({ { "bpm", bpm } }));
    c.run ("project.setKey", object ({ { "tonic", 2 }, { "scale", "minor" } })); // D minor

    const juce::String drumsColour = "#FFD9822B", keysColour = "#FF6FA8DC", padColour = "#FF8E7CC3",
                       guitarColour = "#FF93C47D", bassColour = "#FF5B6EE1", fluteColour = "#FFF6B26B",
                       vocalColour = "#FFE06666", textureColour = "#FF9E9E9E", fxColour = "#FFB4A7D6",
                       mixColour = "#FF7F8C8D";

    //--------------------------------------------------------------------------
    // Channels.
    const Ch drums = song.addChannel ("Dusty Drums", "spinus.drum-machine", {}, -7.0, 0.0, "Drums", drumsColour);
    const Ch keys = song.addChannel ("Rainy Keys", "spinus.sampler", {}, -7.5, 0.0, "Keys", keysColour);
    const Ch pad = song.addChannel ("Warm Pad", "bundled.surge-xt", {}, -5.0, 0.0, "Keys", padColour);
    const Ch guitar = song.addChannel ("Lo-Fi Guitar", "bundled.surge-xt", {}, 0.0, 0.25, "Keys", guitarColour);
    const Ch bass = song.addChannel ("Sub Bass", "spinus.3x-osc", "Sub Bass", -10.5, 0.0, "Bass", bassColour);
    const Ch flute = song.addChannel ("Dreamy Flute", "bundled.surge-xt", {}, -9.0, -0.15, "Lead", fluteColour);
    const Ch guide = [&]
    {
        // The melody Spinus Tune pulls the "ooh" vocal to: a channel without an instrument (it makes no sound).
        const auto r = c.run ("channel.add", object ({ { "name", "Tune Melody" }, { "insert", "master" } }));
        const Ch ch { (int)r["channel"], 0, 0 };
        c.run ("channel.set", object ({ { "channel", ch.channel }, { "group", "Vocals" }, { "colour", vocalColour } }));
        return ch;
    }();
    const Ch crackle = song.addChannel ("Vinyl Crackle", "spinus.sampler", {}, -12.0, 0.0, "Texture", textureColour);
    const Ch rain = song.addChannel ("Rain", "spinus.sampler", {}, -11.0, 0.0, "Texture", textureColour);

    // Audio tracks: the hummed takes, the "ooh" vocal and the piano effects.
    const auto audioTrack = [&] (const juce::String& name, double volumeDb, double pan, const juce::String& colour)
    {
        const auto r = c.run ("channel.add", object ({ { "name", name }, { "audioTrack", true } }));
        const Ch ch { (int)r["channel"], (int)r["insert"], 0 };
        c.run ("channel.set",
               object ({ { "channel", ch.channel }, { "volumeDb", volumeDb }, { "pan", pan }, { "colour", colour } }));
        c.run ("insert.set", object ({ { "insert", ch.insert }, { "name", name }, { "colour", colour } }));
        return ch;
    };
    const Ch hum = audioTrack ("Hum", -8.0, 0.1, vocalColour);
    const Ch ooh = audioTrack ("Ooh Vocal", -9.0, -0.05, vocalColour);
    const Ch keysFx = audioTrack ("Keys FX", -9.0, 0.0, fxColour);

    //--------------------------------------------------------------------------
    // Mixer: buses, send effects, insert effects.
    const int drumBus = song.addBus ("Drum Bus", { drums.insert }, drumsColour);
    const int musicBus =
        song.addBus ("Music Bus", { keys.insert, pad.insert, guitar.insert, flute.insert, keysFx.insert }, keysColour);
    const int vocalBus = song.addBus ("Vocal Bus", { hum.insert, ooh.insert }, vocalColour);
    const int room = song.addInsert ("Room", "#FF5FA0B0");
    const int hall = song.addInsert ("Hall", "#FF5F80C0");
    const int delay = song.addInsert ("Tape Delay", "#FF70B090");
    constexpr int master = 1;

    const int drumsEq = song.addEffect (drums.insert, "spinus.eq");
    const int drumBusComp = song.addEffect (drumBus, "spinus.compressor");
    const int drumBusFilter = song.addEffect (drumBus, "spinus.filter");
    const int keysFilter = song.addEffect (keys.insert, "spinus.filter");
    const int keysEq = song.addEffect (keys.insert, "spinus.eq");
    const int keysWidth = song.addEffect (keys.insert, "spinus.stereo-utility");
    const int bassMono = song.addEffect (bass.insert, "spinus.stereo-utility");
    const int bassComp = song.addEffect (bass.insert, "spinus.compressor");
    const int guitarEq = song.addEffect (guitar.insert, "spinus.eq");
    const int fluteEq = song.addEffect (flute.insert, "spinus.eq");
    const int crackleEq = song.addEffect (crackle.insert, "spinus.eq");
    const int crackleWidth = song.addEffect (crackle.insert, "spinus.stereo-utility");
    const int musicTape = song.addEffect (musicBus, "bundled.surge-xt-effects");
    const int musicFilter = song.addEffect (musicBus, "spinus.filter");
    const int musicDuck = song.addEffect (musicBus, "spinus.ducker");
    const int oohTune = song.addEffect (ooh.insert, "spinus.tune");
    const int oohEq = song.addEffect (ooh.insert, "spinus.eq");
    const int humEq = song.addEffect (hum.insert, "spinus.eq");
    const int vocalComp = song.addEffect (vocalBus, "spinus.compressor");
    const int roomVerb = song.addEffect (room, "bundled.dragonfly.room");
    const int hallVerb = song.addEffect (hall, "bundled.dragonfly.hall");
    const int hallEq = song.addEffect (hall, "spinus.eq");
    const int delayFx = song.addEffect (delay, "bundled.surge-xt-effects");
    const int delayFilter = song.addEffect (delay, "spinus.filter");
    const int masterEq = song.addEffect (master, "spinus.eq");
    const int masterComp = song.addEffect (master, "spinus.compressor");
    song.addEffect (master, "spinus.loudness-meter");
    const int masterLimiter = song.addEffect (master, "spinus.limiter");

    // The music breathes with the kick (the drum bus feeds the ducker's sidechain), and Spinus Tune
    // follows the melody channel.
    c.run ("insert.setSidechain", object ({ { "insert", musicBus }, { "source", drumBus } }));
    c.run ("insert.setNotesFrom", object ({ { "insert", ooh.insert }, { "channel", guide.channel } }));

    for (auto [from, level] : std::vector<std::pair<int, double>> {
             { drums.insert, -15 }, { keys.insert, -11 }, { guitar.insert, -12 }, { hum.insert, -14 } })
        song.send (from, room, level);
    for (auto [from, level] : std::vector<std::pair<int, double>> { { keys.insert, -16 },
                                                                    { pad.insert, -8 },
                                                                    { flute.insert, -9 },
                                                                    { hum.insert, -10 },
                                                                    { ooh.insert, -8 },
                                                                    { keysFx.insert, -6 } })
        song.send (from, hall, level);
    for (auto [from, level] :
         std::vector<std::pair<int, double>> { { flute.insert, -11 }, { ooh.insert, -13 }, { guitar.insert, -16 } })
        song.send (from, delay, level);
    c.run ("insert.set", object ({ { "insert", room }, { "volumeDb", -5.0 } }));
    c.run ("insert.set", object ({ { "insert", hall }, { "volumeDb", -4.0 } }));
    c.run ("insert.set", object ({ { "insert", delay }, { "volumeDb", -6.0 } }));

    // The devices exist now: their settings, as the plugins show them.
    song.syncDevices();
    song.set (drumsEq, { { "band1_on", "On" },
                         { "band1_freq", "35 Hz" },
                         { "band3_gain", "-2.0" },
                         { "band7_freq", "7.00 kHz" },
                         { "band7_gain", "-3.0" } });
    song.set (drumBusComp, { { "threshold", "-16" },
                             { "ratio", "3" },
                             { "attack", "20 ms" },
                             { "release", "120 ms" },
                             { "makeup", "2" } });
    song.set (drumBusFilter, { { "type", "Low pass" }, { "cutoff", "18000 Hz" }, { "resonance", "15%" } });
    song.set (keysFilter, { { "type", "Low pass" }, { "cutoff", "4500 Hz" }, { "resonance", "18%" } });
    song.set (keysEq, { { "band1_on", "On" },
                        { "band1_freq", "110 Hz" },
                        { "band3_freq", "300 Hz" },
                        { "band3_gain", "-2.5" },
                        { "band5_gain", "1.5" } });
    song.set (keysWidth, { { "width", "135%" } });
    song.set (bassMono, { { "mono", "On" } });
    song.set (bassComp, { { "threshold", "-20" }, { "ratio", "3" }, { "attack", "30 ms" }, { "makeup", "2" } });
    song.set (guitarEq, { { "band1_on", "On" }, { "band1_freq", "200 Hz" }, { "band7_gain", "-4.0" } });
    song.set (fluteEq, { { "band1_on", "On" }, { "band1_freq", "250 Hz" }, { "band6_gain", "-2.0" } });
    song.set (crackleEq, { { "band1_on", "On" }, { "band1_freq", "250 Hz" }, { "band7_gain", "-4.0" } });
    song.set (crackleWidth, { { "width", "60%" } });
    song.set (musicTape, { { "12", "Tape" } });
    song.set (musicFilter, { { "type", "Low pass" }, { "cutoff", "18000 Hz" }, { "resonance", "20%" } });
    song.set (musicDuck, { { "amount", "22%" }, { "release", "220 ms" } });
    song.set (oohEq, { { "band1_on", "On" }, { "band1_freq", "120 Hz" }, { "band6_gain", "2.0" } });
    song.set (humEq, { { "band1_on", "On" }, { "band1_freq", "90 Hz" }, { "band4_gain", "1.5" } });
    song.set (vocalComp, { { "threshold", "-20" }, { "ratio", "3" }, { "attack", "8 ms" }, { "makeup", "3" } });
    song.set (roomVerb, { { "0", "0" }, { "1", "20" }, { "3", "45" }, { "4", "14" }, { "7", "0.8" } });
    song.set (hallVerb,
              { { "0", "0" }, { "1", "10" }, { "2", "45" }, { "3", "30" }, { "15", "2.8" }, { "10", "6000" } });
    song.set (hallEq, { { "band1_on", "On" }, { "band1_freq", "200 Hz" } });
    song.set (delayFx, { { "12", "Delay" } });
    song.set (delayFilter, { { "type", "Low pass" }, { "cutoff", "3500 Hz" }, { "resonance", "10%" } });
    song.set (masterEq, { { "band1_on", "On" }, { "band1_freq", "28 Hz" }, { "band2_gain", "1.0" } });
    song.set (masterComp, { { "threshold", "-14" }, { "ratio", "2" }, { "attack", "30 ms" }, { "release", "200 ms" } });
    song.set (masterLimiter, { { "ceiling", "-1.2" }, { "input", "1.0" } });
    song.set (bass.device, { { "glide", "90 ms" } });
    juce::ignoreUnused (oohTune);

    // The instruments' own sounds.
    song.loadSampler (keys,
                      { samples.getChildFile ("Rainy Keys_C4.wav"), samples.getChildFile ("Rainy Keys_C5.wav"),
                        samples.getChildFile ("Rainy Keys_C6.wav") },
                      false);
    song.loadSampler (crackle, { samples.getChildFile ("Vinyl Crackle.wav") }, true);
    song.loadSampler (rain, { samples.getChildFile ("Rain on the Window.wav") }, true);
    {
        auto* kit = song.processor<dsp::DrumMachineProcessor> (drums);
        const std::pair<int, const char*> pads[] = { { kick, "Dusty Kick" }, { snare, "Dusty Snare" },
                                                     { hat, "Dusty Hat" },   { openHat, "Dusty Open Hat" },
                                                     { rim, "Dusty Rim" },   { shaker, "Dusty Shaker" } };
        for (auto [note, name] : pads)
            if (auto r = kit->loadPadFile (note - dsp::DrumMachineProcessor::firstNote,
                                           samples.getChildFile (juce::String (name) + ".wav"));
                r.failed())
            {
                std::cerr << "Drum Machine: " << r.getErrorMessage() << "\n";
                return 3;
            }
    }
    song.syncDevices();
    song.set (keys.device, { { "attack", "2 ms" }, { "release", "450 ms" }, { "velocity", "60%" } });
    for (auto& loop : { crackle, rain })
        song.set (loop.device, { { "attack", "40 ms" }, { "release", "900 ms" }, { "velocity", "0%" } });
    song.set (oohTune, { { "key", "D" }, { "scale", "Minor" }, { "retune", "35 ms" }, { "humanize", "10%" } });

    song.loadSurgePatch (pad, "Pads/Winter Warmer");
    song.loadSurgePatch (guitar, "Plucks/Lo-Fi Pluck");
    song.loadSurgePatch (flute, "Winds/Dreamy Flute");
    for (int i = 0; i < 40; ++i) // Surge XT finishes loading patches on its own thread
        settle (50);
    song.syncDevices();

    //--------------------------------------------------------------------------
    // Patterns.
    const int pBeatA = song.pattern ("Beat A", 2, drumsColour, 0.55);
    song.notes (pBeatA, drums, beatA());
    const int pBeatB = song.pattern ("Beat B", 2, drumsColour, 0.55);
    song.notes (pBeatB, drums, beatB());
    const int pFill = song.pattern ("Fill (triplets)", 1, drumsColour, 0.0, true);
    song.notes (pFill, drums, beatFill());
    const int pShaker = song.pattern ("Shaker Triplets", 2, drumsColour, 0.0, true);
    song.notes (pShaker, drums, shakerTriplets());
    const int pBreakBeat = song.pattern ("Break Rim", 4, drumsColour, 0.55);
    song.notes (pBreakBeat, drums, beatBreak());
    const int pOutroBeat = song.pattern ("Outro Beat", 4, drumsColour, 0.55);
    song.notes (pOutroBeat, drums, beatOutro());

    const int pKeysIntro = song.pattern ("Keys Intro", 4, keysColour);
    song.notes (pKeysIntro, keys, keysIntro());
    const int pKeysVerse = song.pattern ("Keys Verse", 4, keysColour);
    song.notes (pKeysVerse, keys, keysVerse());
    const int pKeysHook = song.pattern ("Keys Hook", 4, keysColour);
    song.notes (pKeysHook, keys, keysHook());
    const int pKeysOutro = song.pattern ("Keys Outro", 4, keysColour);
    song.notes (pKeysOutro, keys, keysOutro());

    const int pPadVerse = song.pattern ("Pad Verse", 4, padColour);
    song.notes (pPadVerse, pad, padPart (0));
    const int pPadHook = song.pattern ("Pad Hook", 4, padColour);
    song.notes (pPadHook, pad, padPart (1));
    const int pPadOutro = song.pattern ("Pad Outro", 4, padColour);
    song.notes (pPadOutro, pad, padPart (2));
    const int pGuitar = song.pattern ("Guitar Arp", 4, guitarColour);
    song.notes (pGuitar, guitar, guitarPart());

    const int pBassVerse = song.pattern ("Bass Verse", 4, bassColour);
    song.notes (pBassVerse, bass, bassVerse());
    const int pBassHook = song.pattern ("Bass Hook", 4, bassColour);
    song.notes (pBassHook, bass, bassHook());
    const int pBassBreak = song.pattern ("Bass Break", 4, bassColour);
    song.notes (pBassBreak, bass, bassBreak());
    const int pBassOutro = song.pattern ("Bass Outro", 4, bassColour);
    song.notes (pBassOutro, bass, bassOutro());

    const int pFluteCounter = song.pattern ("Flute Counter", 4, fluteColour);
    song.notes (pFluteCounter, flute, fluteCounter());
    const int pFluteAnswer = song.pattern ("Flute Answer", 4, fluteColour);
    song.notes (pFluteAnswer, flute, fluteAnswer());
    const int pFluteOutro = song.pattern ("Flute Outro", 4, fluteColour);
    song.notes (pFluteOutro, flute, fluteOutro());
    const int pGuide = song.pattern ("Ooh Melody", 4, vocalColour);
    song.notes (pGuide, guide, oohGuide());

    const int pCrackle = song.pattern ("Vinyl Loop", songBars, textureColour);
    song.notes (pCrackle, crackle, Notes { { 0, songBars * 4.0, 60, 0.8 } });
    const int pRain = song.pattern ("Rain Loop", 8, textureColour);
    song.notes (pRain, rain, Notes { { 0, 32.0, 60, 0.8 } });

    //--------------------------------------------------------------------------
    // The Playlist: tracks in order, each part's automation grouped under it.
    for (auto& s : sections)
        c.run ("marker.add", object ({ { "at", s.startBar * bar }, { "name", s.name } }));

    const int lDrums = song.lane ("Drums", drumsColour);
    const int laDrumFilter = song.lane ("Drum Filter", drumsColour, lDrums);
    const int lPerc = song.lane ("Percussion", drumsColour);
    const int lKeys = song.lane ("Keys", keysColour);
    const int laKeysFilter = song.lane ("Keys Filter", keysColour, lKeys);
    const int laWobble = song.lane ("Tape Wobble", keysColour, lKeys);
    const int lPad = song.lane ("Pad", padColour);
    const int laPadCut = song.lane ("Pad Cutoff", padColour, lPad);
    const int lGuitar = song.lane ("Guitar", guitarColour);
    const int lBass = song.lane ("Bass", bassColour);
    const int lFlute = song.lane ("Flute", fluteColour);
    const int laFlutePan = song.lane ("Flute Pan", fluteColour, lFlute);
    const int laDelay = song.lane ("Delay Throw", fluteColour, lFlute);

    // Takes: three tries at the hummed line on the Hum track, comped bar by bar below.
    const juce::File takes[] = { asFlac (audioFolder.getChildFile ("Hum Take 1.wav"), flacs),
                                 asFlac (audioFolder.getChildFile ("Hum Take 2.wav"), flacs),
                                 asFlac (audioFolder.getChildFile ("Hum Take 3.wav"), flacs) };
    std::vector<int> takeClips, takeLanes;
    for (auto& file : takes)
    {
        const auto r = c.run ("clip.addTake", object ({ { "channel", hum.channel },
                                                        { "file", file.getFullPathName() },
                                                        { "start", 20 * bar },
                                                        { "length", 4 * bar } }));
        takeClips.push_back ((int)r["clip"]);
        takeLanes.push_back ((int)r["lane"]);
        ++song.audioClips;
    }
    c.run ("lane.set", object ({ { "lane", takeLanes[0] }, { "name", "Hum (takes)" }, { "colour", vocalColour } }));
    for (size_t i = 1; i < takeLanes.size(); ++i)
        c.run ("lane.set", object ({ { "lane", takeLanes[i] }, { "colour", vocalColour } }));
    // Comping: cut every take at each bar, then choose a bar from each (Alt+click in the Playlist).
    std::vector<std::vector<int>> pieces (3);
    for (size_t t = 0; t < 3; ++t)
    {
        pieces[t].push_back (takeClips[t]);
        for (int b = 21; b < 24; ++b)
            pieces[t].push_back (song.split (pieces[t].back(), b * bar));
    }
    const int chosen[] = { 2, 0, 1, 2 }; // bar 1 from take 3, bar 2 from take 1, bar 3 from take 2, bar 4 from take 3
    for (int b = 0; b < 4; ++b)
        c.run ("clip.useTake", object ({ { "clip", pieces[(size_t)chosen[b]][(size_t)b] } }));

    const int lOoh = song.lane ("Ooh Vocal", vocalColour);
    const int lGuide = song.lane ("Tune Melody", vocalColour, lOoh);
    const int lKeysFx = song.lane ("Keys FX", fxColour);
    const int lTexture = song.lane ("Vinyl", textureColour);
    const int laCrackle = song.lane ("Vinyl Volume", textureColour, lTexture);
    const int lRain = song.lane ("Rain", textureColour);
    const int laRain = song.lane ("Rain Volume", textureColour, lRain);
    const int lMix = song.lane ("Mix", mixColour);
    const int laMusicFilter = song.lane ("Music Filter", mixColour, lMix);
    const int laHall = song.lane ("Hall Return", mixColour, lMix);
    const int laMaster = song.lane ("Master Fade", mixColour, lMix);

    // Intro 0-4
    song.place (lKeys, pKeysIntro, 0, 4);
    song.place (lTexture, pCrackle, 0, songBars);
    song.place (lRain, pRain, 0, 8);
    // Verse 1 4-20
    for (int b = 4; b < 20; b += 2)
        song.place (lDrums, b % 8 == 6 ? pBeatB : pBeatA, b, 2);
    for (int b = 4; b < 20; b += 4)
    {
        song.place (lKeys, pKeysVerse, b, 4);
        song.place (lBass, pBassVerse, b, 4);
    }
    song.place (lGuitar, pGuitar, 12, 4);
    song.place (lGuitar, pGuitar, 16, 4);
    song.place (lFlute, pFluteCounter, 16, 4);
    song.place (lPerc, pFill, 19, 1); // over the last bar of the beat
    // Hook 20-28
    for (int b = 20; b < 28; b += 2)
    {
        song.place (lDrums, b % 4 == 2 ? pBeatB : pBeatA, b, 2);
        song.place (lPerc, pShaker, b, 2);
    }
    for (int b = 20; b < 28; b += 4)
    {
        song.place (lKeys, pKeysHook, b, 4);
        song.place (lBass, pBassHook, b, 4);
        song.place (lPad, pPadHook, b, 4);
    }
    song.place (lFlute, pFluteAnswer, 24, 4);
    // Break 28-32
    song.place (lDrums, pBreakBeat, 28, 4);
    song.place (lKeys, pKeysIntro, 28, 4);
    song.place (lBass, pBassBreak, 28, 4);
    // Verse 2 32-44
    for (int b = 32; b < 44; b += 2)
        song.place (lDrums, b % 8 == 2 ? pBeatB : pBeatA, b, 2);
    song.place (lPerc, pFill, 39, 1);
    song.place (lPerc, pFill, 43, 1);
    for (int b = 32; b < 44; b += 4)
    {
        song.place (lKeys, pKeysVerse, b, 4);
        song.place (lBass, pBassVerse, b, 4);
        if (b >= 36)
            song.place (lPad, pPadVerse, b, 4);
    }
    song.place (lFlute, pFluteCounter, 36, 4);
    song.place (lGuitar, pGuitar, 40, 4);
    song.place (lGuide, pGuide, 32, 4);
    song.place (lGuide, pGuide, 40, 4);
    // Outro 44-52
    song.place (lDrums, pBeatA, 44, 2);
    song.place (lDrums, pBeatB, 46, 2);
    song.place (lDrums, pOutroBeat, 48, 4);
    song.place (lPerc, pShaker, 44, 2);
    song.place (lPerc, pShaker, 46, 2);
    song.place (lKeys, pKeysHook, 44, 4);
    song.place (lKeys, pKeysOutro, 48, 4);
    song.place (lBass, pBassHook, 44, 4);
    song.place (lBass, pBassOutro, 48, 4);
    song.place (lPad, pPadHook, 44, 4);
    song.place (lPad, pPadOutro, 48, 4);
    song.place (lFlute, pFluteOutro, 48, 4);
    song.place (lRain, pRain, 44, 8);

    // Audio clips: the "ooh" vocal twice (Spinus Tune corrects it as it plays), the hummed line again in the
    // outro (pieces of the takes), and the piano swell reversed, slowed down and an octave up.
    const auto oohFile = asFlac (audioFolder.getChildFile ("Ooh Vocal.wav"), flacs);
    song.audio (lOoh, oohFile, 32, 4, ooh.channel, 0.0);
    song.audio (lOoh, oohFile, 40, 4, ooh.channel, 0.0);
    {
        const auto first = song.audio (takeLanes[0], takes[0], 44, 2, hum.channel, -1.0);
        const auto second = song.audio (takeLanes[0], takes[2], 46, 2, hum.channel, -1.0, 2.0);
        c.run ("clip.setAudio", object ({ { "clip", second }, { "fadeOut", 2 * beat } }));
        juce::ignoreUnused (first);
    }
    const auto swell = asFlac (audioFolder.getChildFile ("Keys Swell.wav"), flacs);
    const int reversed = song.audio (lKeysFx, swell, 18, 2, keysFx.channel, -2.0);
    c.run ("clip.setAudio", object ({ { "clip", reversed }, { "reversed", true }, { "fadeIn", bar } }));
    const int slowed = song.audio (lKeysFx, swell, 28, 2, keysFx.channel, -1.0);
    c.run ("clip.setAudio",
           object ({ { "clip", slowed }, { "speed", 0.5 }, { "stretch", "resample" }, { "fit", true } }));
    const int shimmer = song.audio (lKeysFx, swell, 50, 2, keysFx.channel, -9.0);
    c.run ("clip.setAudio",
           object ({ { "clip", shimmer }, { "pitch", 12.0 }, { "stretch", "stretch" }, { "fadeOut", bar } }));

    //--------------------------------------------------------------------------
    // Automation clips.
    song.syncDevices();
    const auto v = [&] (int device, const char* id, const char* text) { return (double)song.knob (device, id, text); };

    // The keys' low-pass: opens through the intro, dips in the break, closes in the outro.
    const auto kf = [&] (const char* t) { return v (keysFilter, "cutoff", t); };
    song.automate (laKeysFilter, 0, 4, Song::parameter (keysFilter, "cutoff"),
                   { { 0, kf ("400 Hz"), 0.45 }, { 15.5, kf ("4500 Hz") }, { 16, kf ("4500 Hz") } });
    song.automate (
        laKeysFilter, 28, 4, Song::parameter (keysFilter, "cutoff"),
        { { 0, kf ("4500 Hz"), -0.5 }, { 4, kf ("900 Hz") }, { 12, kf ("700 Hz"), 0.5 }, { 16, kf ("4500 Hz") } });
    song.automate (laKeysFilter, 48, 4, Song::parameter (keysFilter, "cutoff"),
                   { { 0, kf ("4500 Hz"), -0.3 }, { 16, kf ("500 Hz") } });

    // Tape wobble: the Sampler's fine tune drifts slowly, like a worn cassette (deeper in the break and the
    // end), one automation clip per section.
    const auto fine = [&] (double cents)
    {
        const auto text = juce::String (juce::roundToInt (cents));
        return v (keys.device, "fine", text.toRawUTF8());
    };
    for (auto& s : sections)
    {
        const double depth = juce::String (s.name) == "Break" ? 22.0 : juce::String (s.name) == "Outro" ? 16.0 : 7.0;
        std::vector<P> points;
        for (int i = 0; i <= s.bars * 8; ++i)
        {
            const double beats = i * 0.5, seconds = (s.startBar * 4 + beats) * 60.0 / bpm;
            const double wow = std::sin (seconds * 2 * juce::MathConstants<double>::pi * 0.42) +
                               0.45 * std::sin (seconds * 2 * juce::MathConstants<double>::pi * 1.1 + 1.3);
            points.push_back ({ beats, fine (depth * wow / 1.45) });
        }
        song.automate (laWobble, s.startBar, s.bars, Song::parameter (keys.device, "fine"), points);
    }

    // The drums come back from under water at the start of the second verse.
    const auto df = [&] (const char* t) { return v (drumBusFilter, "cutoff", t); };
    song.automate (laDrumFilter, 32, 2, Song::parameter (drumBusFilter, "cutoff"),
                   { { 0, df ("500 Hz"), 0.55 }, { 7.9, df ("18000 Hz") }, { 8, df ("18000 Hz") } });
    song.automate (laDrumFilter, 50, 2, Song::parameter (drumBusFilter, "cutoff"),
                   { { 0, df ("18000 Hz"), -0.6 }, { 8, df ("800 Hz") } });

    // The music bus closes down in the break.
    const auto mf = [&] (const char* t) { return v (musicFilter, "cutoff", t); };
    song.automate (laMusicFilter, 27, 6, Song::parameter (musicFilter, "cutoff"),
                   { { 0, mf ("18000 Hz"), -0.6 },
                     { 4, mf ("1100 Hz") },
                     { 18, mf ("800 Hz"), 0.6 },
                     { 19.9, mf ("18000 Hz") },
                     { 24, mf ("18000 Hz") } });
    song.automate (laMusicFilter, 48, 4, Song::parameter (musicFilter, "cutoff"),
                   { { 0, mf ("18000 Hz"), -0.5 }, { 16, mf ("1500 Hz") } });

    // The pad's own cutoff (Surge XT) opens over the hooks.
    constexpr const char* surgeCutoff = "319"; // A Filter 1 Cutoff
    const auto padCut = song.currentValue (pad.device, surgeCutoff);
    song.automate (laPadCut, 20, 8, Song::parameter (pad.device, surgeCutoff),
                   { { 0, padCut - 0.15, 0.3 }, { 24, padCut + 0.05 }, { 32, padCut } });
    song.automate (laPadCut, 44, 8, Song::parameter (pad.device, surgeCutoff), { { 0, padCut }, { 32, padCut - 0.2 } });

    // The flute drifts across, and the delay throws its last notes.
    song.automate (laFlutePan, 24, 4, Song::channelPan (flute), { { 0, 0.4 }, { 8, 0.62 }, { 16, 0.38 } });
    song.automate (laFlutePan, 48, 4, Song::channelPan (flute), { { 0, 0.4 }, { 16, 0.66 } });
    const auto feedback = song.currentValue (delayFx, "2");
    song.automate (laDelay, 27, 1, Song::parameter (delayFx, "2"),
                   { { 0, feedback }, { 2, feedback + 0.25 }, { 3.9, feedback + 0.25 }, { 4, feedback } });
    song.automate (laDelay, 51, 1, Song::parameter (delayFx, "2"), { { 0, feedback }, { 2, feedback + 0.3 } });

    // Rain: in at the start, gone by the second bar of the beat; back for the end. The crackle swells in the break.
    song.automate (laRain, 0, 8, Song::channelVolume (rain),
                   { { 0, song.volumeValue (rain, "-11") },
                     { 16, song.volumeValue (rain, "-11"), -0.3 },
                     { 32, song.volumeValue (rain, "-50") } });
    song.automate (laRain, 44, 8, Song::channelVolume (rain),
                   { { 0, song.volumeValue (rain, "-45"), 0.3 },
                     { 24, song.volumeValue (rain, "-10") },
                     { 32, song.volumeValue (rain, "-9") } });
    song.automate (laCrackle, 28, 4, Song::channelVolume (crackle),
                   { { 0, song.volumeValue (crackle, "-12") },
                     { 4, song.volumeValue (crackle, "-6") },
                     { 12, song.volumeValue (crackle, "-6") },
                     { 16, song.volumeValue (crackle, "-12") } });

    // The hall swells in the break; the master fades in and out.
    song.automate (laHall, 28, 4, Song::insertVolume (hall),
                   { { 0, song.insertVolumeValue (hall, "-4") },
                     { 8, song.insertVolumeValue (hall, "0") },
                     { 15, song.insertVolumeValue (hall, "0") },
                     { 16, song.insertVolumeValue (hall, "-4") } });
    song.automate (
        laMaster, 0, 1, Song::insertVolume (master),
        { { 0, song.insertVolumeValue (master, "-24"), -0.5 }, { 4, song.insertVolumeValue (master, "0") } });
    song.automate (laMaster, 48, 4, Song::insertVolume (master),
                   { { 0, song.insertVolumeValue (master, "0") },
                     { 8, song.insertVolumeValue (master, "0"), 0.3 },
                     { 16, song.insertVolumeValue (master, "-40") } });
    juce::ignoreUnused (lMix);

    song.syncDevices();
    const auto built = msSince (started);

    const auto saved = document.saveAs (folder);
    if (saved.failed())
    {
        std::cerr << "Save failed: " << saved.getErrorMessage() << "\n";
        return 1;
    }
    showcase::makeStatesPortable (folder);

    const auto& project = session.getProject();
    int notes = 0, clips = 0, automation = 0;
    for (auto& p : project.patterns)
        for (auto& [channel, list] : p.notes)
            notes += (int)list.size();
    for (auto& clip : project.playlist.clips)
        (std::holds_alternative<model::AutomationClip> (clip.content) ? automation : clips)++;
    std::cout << "Made " << folder.getFullPathName() << " in " << built << " ms (" << c.count << " commands)\n"
              << "  channels " << project.channels.size() << ", inserts " << project.mixer.inserts.size() + 1
              << ", effects " << song.effects.size() << ", patterns " << project.patterns.size() << ", notes " << notes
              << ", lanes " << project.playlist.lanes.size() << ", pattern/audio clips " << clips << " (audio "
              << song.audioClips << "), automation clips " << automation << " (" << song.automationPoints
              << " points)\n"
              << "  Surge XT patches: " << song.surgePatches.joinIntoString (", ")
              << (song.surgeProblems.isEmpty() ? "" : " -- problems: " + song.surgeProblems.joinIntoString (", "))
              << "\n";
    for (auto& d : session.getAdapter().getMissingDevices())
        std::cout << "  MISSING device " << d.value << "\n";
    return 0;
}

//==============================================================================
int render (Session& session, ProjectDocument& document, const juce::File& folder, const juce::File& wav,
            const juce::File& stems)
{
    const auto opened = document.open (folder);
    if (opened.failed())
    {
        std::cerr << "Open failed: " << opened.getErrorMessage() << "\n";
        return 1;
    }
    session.getEdit().dispatchPendingUpdatesSynchronously();
    for (int i = 0; i < 60; ++i)
        settle (50);
    for (auto& d : session.getAdapter().getMissingDevices())
        std::cout << "  MISSING device " << d.value << "\n";
    if (juce::SystemStats::getEnvironmentVariable ("LOFI_PARAMETERS", {}).isNotEmpty())
    {
        const auto print = [&] (const juce::String& owner, model::DeviceId id)
        {
            std::cout << "== " << owner << ": device " << id.value << "\n";
            for (auto& [pid, name] : session.getDeviceParameters (id))
                if (! name.startsWith ("M") || ! name.contains (": -"))
                    std::cout << "  " << pid << " | " << name << " | " << session.getDeviceParameterValueText (id, pid)
                              << "\n";
        };
        for (auto& channel : session.getProject().channels)
            if (channel.instrument && channel.instrument->stockType.startsWith ("spinus."))
                print (channel.name, channel.instrument->id);
        for (auto& insert : session.getProject().mixer.inserts)
            for (auto& effect : insert.effects)
                print (insert.name, effect.device.id);
    }

    for (const bool asStems : { false, true })
    {
        if (asStems && stems == juce::File())
            break;
        engine::ExportSettings settings;
        settings.format = engine::ExportSettings::Format::wav24;
        settings.sampleRate = 48000.0;
        settings.tailSeconds = 4.0;
        settings.stems = asStems;
        std::optional<engine::ExportJob::Result> result;
        const auto start = Clock::now();
        auto job = session.createExportJob (asStems ? stems.getChildFile ("Mix.wav") : wav, settings,
                                            [&] (const engine::ExportJob::Result& r) { result = r; });
        job->start();
        while (! result)
            settle (20);
        job = nullptr;
        if (! result->ok)
        {
            std::cerr << "Export failed: " << result->error << "\n";
            return 1;
        }
        std::cout << "Exported in " << msSince (start) / 1000.0 << " s:\n";
        for (auto& f : result->files)
            std::cout << "  " << f.getFullPathName() << "\n";
    }
    return 0;
}
} // namespace lofi
