#pragma once

// The showcase song itself ("Siskin", an original piece in A minor at 124 BPM), and the measurements.
// Included by ShowcaseSong.cpp after its helpers (Commands, object, settle).

namespace showcase
{
using Ticks = juce::int64;

constexpr double bpm = 124.0;
constexpr int songBars = 84;

struct Section
{
    const char* name;
    int startBar, bars;
};
constexpr Section sections[] = {
    { "Intro", 0, 8 },      { "Verse", 8, 16 },   { "Build", 24, 8 },   { "Drop", 32, 16 },
    { "Breakdown", 48, 8 }, { "Build 2", 56, 4 }, { "Drop 2", 60, 16 }, { "Outro", 76, 8 }
};

/** A note in beats (quarter notes): start, length, MIDI pitch (60 = C5 as Spinus names it), velocity. */
struct N
{
    double start, length;
    int pitch;
    double velocity = 0.8;
};
using Notes = std::vector<N>;

// The chords, one bar each: Am7 Fmaj7 C G (verse, drops) and Fmaj7 G Em7 Am (breakdown).
const std::vector<std::vector<int>> mainChords = {
    { 57, 60, 64, 67 }, { 53, 57, 60, 64 }, { 55, 60, 64, 67 }, { 55, 59, 62, 67 }
};
const std::vector<int> mainRoots = { 45, 41, 48, 43 }; // A2 F2 C3 G2
const std::vector<std::vector<int>> breakChords = {
    { 53, 57, 60, 64 }, { 55, 59, 62, 67 }, { 52, 55, 59, 62 }, { 57, 60, 64, 69 }
};
const std::vector<int> breakRoots = { 41, 43, 40, 45 };

enum Pad
{
    kick = 60,
    snare,
    clap,
    closedHat,
    openHat,
    lowTom,
    midTom,
    highTom,
    rim,
    cowbell,
    crash,
    shaker,
    kick2,
    snare2,
    ride,
    perc
};

class Song
{
public:
    Song (Session& s, const juce::File& pluginFolder) : session (s), c (s), plugins (pluginFolder) {}

    struct Channel
    {
        int channel = 0, insert = 0, device = 0;
    };

    //==========================================================================
    Channel addChannel (const juce::String& name, const juce::String& stockType, const juce::String& preset = {},
                        double volumeDb = 0.0, double pan = 0.0, const juce::String& group = {},
                        const juce::String& colour = {})
    {
        const auto r = c.run (
            "channel.add", object ({ { "name", name },
                                     { "instrument", object ({ { "kind", "stock" }, { "stockType", stockType } }) } }));
        Channel ch { (int)r["channel"], (int)r["insert"], (int)r["device"] };
        if (preset.isNotEmpty())
            c.run ("device.loadPreset", object ({ { "device", ch.device }, { "preset", preset } }));
        auto* set = new juce::DynamicObject();
        set->setProperty ("channel", ch.channel);
        set->setProperty ("volumeDb", volumeDb);
        set->setProperty ("pan", pan);
        if (group.isNotEmpty())
            set->setProperty ("group", group);
        if (colour.isNotEmpty())
            set->setProperty ("colour", colour);
        c.run ("channel.set", juce::var (set));
        c.run ("insert.set", object ({ { "insert", ch.insert }, { "name", name } }));
        if (colour.isNotEmpty())
            c.run ("insert.set", object ({ { "insert", ch.insert }, { "colour", colour } }));
        instruments.push_back ({ name, stockType, preset });
        return ch;
    }

    int addEffect (int insert, const juce::String& stockType)
    {
        const auto r = c.run ("insert.addEffect",
                              object ({ { "insert", insert },
                                        { "device", object ({ { "kind", "stock" }, { "stockType", stockType } }) } }));
        effects.push_back ({ insert, stockType });
        return (int)r["device"];
    }

    int addInsert (const juce::String& name, const juce::String& colour)
    {
        const auto id = (int)c.run ("insert.add", object ({ { "name", name } }))["insert"];
        c.run ("insert.set", object ({ { "insert", id }, { "colour", colour } }));
        return id;
    }

    int addBus (const juce::String& name, const std::vector<int>& inserts, const juce::String& colour)
    {
        juce::Array<juce::var> list;
        for (auto i : inserts)
            list.add (i);
        const auto id = (int)c.run ("insert.addBus", object ({ { "name", name }, { "inserts", list } }))["insert"];
        c.run ("insert.set", object ({ { "insert", id }, { "colour", colour } }));
        return id;
    }

    void send (int insert, int target, double levelDb)
    {
        c.run ("insert.setSend", object ({ { "insert", insert }, { "target", target }, { "levelDb", levelDb } }));
    }

    void syncDevices()
    {
        session.getEdit().dispatchPendingUpdatesSynchronously();
        settle (50);
    }

    model::ParameterTarget deviceTarget (int device, const juce::String& parameterId) const
    {
        model::ParameterTarget t;
        t.kind = model::ParameterTarget::Kind::deviceParameter;
        t.device = model::DeviceId { (uint32_t)device };
        t.parameterId = parameterId;
        return t;
    }

    /** A setting's 0..1 value for text as the plugin shows it ("1.2 kHz", "70"); the device must be loaded. */
    float valueFor (int device, const juce::String& parameterId, const juce::String& text) const
    {
        auto v = session.parseTargetValue (deviceTarget (device, parameterId), text);
        if (! v)
            if (auto* plugin = session.getAdapter().getDevicePlugin (model::DeviceId { (uint32_t)device }))
                for (int i = 0; i < plugin->getNumAutomatableParameters() && ! v; ++i)
                    if (auto p = plugin->getAutomatableParameter (i); p != nullptr && p->paramID == parameterId)
                        for (int k = 0; k <= 1000 && ! v; ++k) // a choice ("On", "High pass"): by its name
                            if (p->valueToString (p->valueRange.convertFrom0to1 ((float)k / 1000.0f))
                                    .trim()
                                    .equalsIgnoreCase (text))
                                v = (float)k / 1000.0f;
        if (! v)
        {
            std::cerr << "No value for " << text << " (device " << device << ", " << parameterId << ")\n";
            std::exit (3);
        }
        return *v;
    }

    /** The setting's current 0..1 value (a loaded preset's). */
    float currentValue (int device, const juce::String& parameterId) const
    {
        if (auto* plugin = session.getAdapter().getDevicePlugin (model::DeviceId { (uint32_t)device }))
            for (int i = 0; i < plugin->getNumAutomatableParameters(); ++i)
                if (auto p = plugin->getAutomatableParameter (i); p != nullptr && p->paramID == parameterId)
                    return p->valueRange.convertTo0to1 (p->getCurrentValue());
        std::cerr << "No parameter " << parameterId << " on device " << device << "\n";
        std::exit (3);
    }

    /** Settings by text, one command (one undo step). */
    void setTexts (int device, std::initializer_list<std::pair<const char*, const char*>> values)
    {
        auto* o = new juce::DynamicObject();
        for (auto& [id, text] : values)
            o->setProperty (id, valueFor (device, id, text));
        c.run ("device.setParameters", object ({ { "device", device }, { "values", juce::var (o) } }));
        if (juce::SystemStats::getEnvironmentVariable ("SHOWCASE_SHOW_SETTINGS", {}).isNotEmpty())
        {
            syncDevices();
            for (auto& [id, text] : values)
                std::cout << "  device " << device << " " << id << " = "
                          << session.getDeviceParameterValueText (model::DeviceId { (uint32_t)device }, id)
                          << " (wanted " << text << ")\n";
        }
    }

    void setValues (int device, std::initializer_list<std::pair<const char*, double>> values)
    {
        auto* o = new juce::DynamicObject();
        for (auto& [id, value] : values)
            o->setProperty (id, value);
        c.run ("device.setParameters", object ({ { "device", device }, { "values", juce::var (o) } }));
    }

    //==========================================================================
    int addPattern (const juce::String& name, int bars, const juce::String& colour)
    {
        const auto id = (int)c.run ("pattern.add", object ({ { "name", name }, { "length", bars * bar } }))["pattern"];
        c.run ("pattern.set", object ({ { "pattern", id }, { "colour", colour } }));
        patternIds.push_back (id);
        return id;
    }

    void setNotes (int pattern, const Channel& ch, const Notes& notes)
    {
        juce::Array<juce::var> list;
        for (auto& n : notes)
            list.add (object ({ { "start", (Ticks)std::llround (n.start * beat) },
                                { "length", std::max<Ticks> (1, (Ticks)std::llround (n.length * beat)) },
                                { "pitch", juce::jlimit (0, 127, n.pitch) },
                                { "velocity", juce::jlimit (0.0, 1.0, n.velocity) } }));
        c.run ("pattern.setNotes", object ({ { "pattern", pattern }, { "channel", ch.channel }, { "notes", list } }));
        noteCount += (int)notes.size();
    }

    int addLane (const juce::String& name) { return (int)c.run ("lane.add", object ({ { "name", name } }))["lane"]; }

    void place (int lane, int pattern, int startBar, int bars, int offsetBars = 0)
    {
        auto params = object (
            { { "lane", lane }, { "pattern", pattern }, { "start", startBar * bar }, { "length", bars * bar } });
        if (offsetBars > 0)
            params.getDynamicObject()->setProperty ("offset", offsetBars * bar);
        c.run ("clip.addPattern", params);
        ++patternClips;
    }

    /** An automation point: beats from the clip's start, 0..1 value, curve of the segment after it. */
    struct P
    {
        double beats;
        double value;
        double curve = 0.0;
    };

    void automate (int lane, int startBar, int bars, const juce::var& target, const std::vector<P>& points)
    {
        juce::Array<juce::var> list;
        for (auto& p : points)
            list.add (object ({ { "time", (Ticks)std::llround (p.beats * beat) },
                                { "value", juce::jlimit (0.0, 1.0, p.value) },
                                { "curve", juce::jlimit (-1.0, 1.0, p.curve) } }));
        c.run ("clip.addAutomation", object ({ { "lane", lane },
                                               { "start", startBar * bar },
                                               { "length", bars * bar },
                                               { "target", target },
                                               { "points", list } }));
        ++automationClips;
        automationPoints += (int)points.size();
    }

    static juce::var channelVolume (const Channel& ch)
    {
        return object ({ { "kind", "channelVolume" }, { "channel", ch.channel } });
    }
    static juce::var channelPan (const Channel& ch)
    {
        return object ({ { "kind", "channelPan" }, { "channel", ch.channel } });
    }
    static juce::var insertVolume (int insert) { return object ({ { "kind", "insertVolume" }, { "insert", insert } }); }
    static juce::var parameter (int device, const juce::String& id)
    {
        return object ({ { "kind", "deviceParameter" }, { "device", device }, { "parameterId", id } });
    }

    /** A channel volume's 0..1 automation value for a level in dB (as Spinus's volume faders map it). */
    float volumeValue (const Channel& ch, const juce::String& dbText) const
    {
        model::ParameterTarget t;
        t.kind = model::ParameterTarget::Kind::channelVolume;
        t.channel = model::ChannelId { (uint32_t)ch.channel };
        return session.parseTargetValue (t, dbText).value_or (0.8f);
    }
    float insertVolumeValue (int insert, const juce::String& dbText) const
    {
        model::ParameterTarget t;
        t.kind = model::ParameterTarget::Kind::insertVolume;
        t.insert = model::InsertId { (uint32_t)insert };
        return session.parseTargetValue (t, dbText).value_or (0.8f);
    }

    /** Loads one of Surge XT's factory patches into a Surge XT instrument, as picking it in Surge XT's own
        browser does (the plugin's state, saved with the project). */
    void loadSurgePatch (const Channel& ch, const juce::String& relativePath)
    {
        const auto file = plugins.getChildFile ("SurgeXTData/patches_factory").getChildFile (relativePath + ".fxp");
        auto* plugin = dynamic_cast<tracktion::ExternalPlugin*> (
            session.getAdapter().getDevicePlugin (model::DeviceId { (uint32_t)ch.device }));
        auto* instance = plugin != nullptr ? plugin->getAudioPluginInstance() : nullptr;
        if (instance == nullptr || ! file.existsAsFile())
        {
            std::cerr << "Surge XT patch not loaded: " << relativePath << "\n";
            surgeProblems.add (relativePath);
            return;
        }
        if (auto r = plugins::ClapPluginFormat::loadPreset (*instance, file); r.failed())
        {
            std::cerr << "Surge XT patch " << relativePath << ": " << r.getErrorMessage() << "\n";
            surgeProblems.add (relativePath);
        }
        surgePatches.add (relativePath);
    }

    Session& session;
    Commands c;
    juce::File plugins;
    struct Instrument
    {
        juce::String name, type, preset;
    };
    std::vector<Instrument> instruments;
    std::vector<std::pair<int, juce::String>> effects;
    std::vector<int> patternIds;
    int noteCount = 0, patternClips = 0, automationClips = 0, automationPoints = 0;
    juce::StringArray surgePatches, surgeProblems;
};

//==============================================================================
// The parts.

Notes chordsEachBar (const std::vector<std::vector<int>>& chords, int bars, double length, double velocity,
                     int transpose = 0)
{
    Notes n;
    for (int b = 0; b < bars; ++b)
        for (auto p : chords[(size_t)b % chords.size()])
            n.push_back ({ b * 4.0, length, p + transpose, velocity });
    return n;
}

Notes rhythmChords (const std::vector<std::vector<int>>& chords, int bars,
                    const std::vector<std::pair<double, double>>& hits, double velocity, std::mt19937& random,
                    int transpose = 0)
{
    Notes n;
    std::uniform_real_distribution<double> human (-0.06, 0.06);
    for (int b = 0; b < bars; ++b)
        for (auto [at, len] : hits)
        {
            const double v = velocity + human (random);
            for (auto p : chords[(size_t)b % chords.size()])
                n.push_back ({ b * 4.0 + at, len, p + transpose, v });
        }
    return n;
}

/** Sixteenth-note arpeggio over each bar's chord, going up and down `octaves`. */
Notes arpeggio (const std::vector<std::vector<int>>& chords, int bars, int transpose, int octaves, double velocity,
                double gate = 0.22, const std::vector<int>& order = {})
{
    Notes n;
    for (int b = 0; b < bars; ++b)
    {
        auto chord = chords[(size_t)b % chords.size()];
        std::vector<int> up;
        for (int o = 0; o < octaves; ++o)
            for (size_t i = 0; i < 3; ++i)
                up.push_back (chord[i] + 12 * o + transpose);
        std::vector<int> seq = up;
        for (int i = (int)up.size() - 2; i > 0; --i)
            seq.push_back (up[(size_t)i]);
        if (! order.empty())
        {
            seq.clear();
            for (auto i : order)
                seq.push_back (up[(size_t)i % up.size()]);
        }
        for (int s = 0; s < 16; ++s)
        {
            const double accent = s % 4 == 0 ? 0.15 : (s % 2 == 0 ? 0.05 : 0.0);
            n.push_back ({ b * 4.0 + s * 0.25, gate, seq[(size_t)(b * 16 + s) % seq.size()], velocity + accent });
        }
    }
    return n;
}

Notes leadA()
{
    // An original melody over Am F C G, twice (8 bars).
    return {
        { 0, 1.5, 76 },   { 1.5, .5, 74 },   { 2, .5, 72 },    { 2.5, .5, 74 },  { 3, 1, 76 },       { 4, 1, 72 },
        { 5, .5, 69 },    { 5.5, .5, 72 },   { 6, 1.5, 77 },   { 7.5, .5, 76 },  { 8, 1, 79 },       { 9, .5, 76 },
        { 9.5, .5, 79 },  { 10, 1, 76 },     { 11, .5, 74 },   { 11.5, .5, 72 }, { 12, 1.5, 74 },    { 13.5, .5, 71 },
        { 14, .5, 74 },   { 14.5, 1.5, 79 }, { 16, .5, 76 },   { 16.5, .5, 76 }, { 17, 1, 81 },      { 18, .5, 79 },
        { 18.5, .5, 76 }, { 19, .5, 74 },    { 19.5, .5, 76 }, { 20, .75, 77 },  { 20.75, .75, 76 }, { 21.5, .5, 72 },
        { 22, 1, 69 },    { 23, 1, 72 },     { 24, .5, 79 },   { 24.5, .5, 81 }, { 25, .5, 79 },     { 25.5, .5, 76 },
        { 26, 1, 79 },    { 27, 1, 84 },     { 28, 1.5, 83 },  { 29.5, .5, 81 }, { 30, .5, 79 },     { 30.5, .5, 74 },
        { 31, 1, 76 },
    };
}

/** Lead A with sixteenth-note pickups into its long notes, and accents. */
Notes leadB (int transpose)
{
    Notes n;
    int i = 0;
    for (auto note : leadA())
    {
        note.pitch += transpose;
        note.velocity = 0.75 + (i++ % 3 == 0 ? 0.15 : 0.0);
        if (note.length >= 1.0)
        {
            n.push_back ({ note.start, 0.25, note.pitch + 2, note.velocity - 0.1 });
            n.push_back ({ note.start + 0.25, note.length - 0.25, note.pitch, note.velocity });
        }
        else
            n.push_back (note);
    }
    return n;
}

Notes bellMelody()
{
    return {
        { 0, 2, 81, .7 },   { 2, 2, 84, .65 },  { 4, 2, 83, .7 },  { 6, 2, 86, .65 },
        { 8, 3, 79, .7 },   { 11, 1, 83, .6 },  { 12, 4, 81, .7 }, { 16, 1, 81, .7 },
        { 17, 1, 84, .65 }, { 18, 2, 88, .7 },  { 20, 2, 86, .7 }, { 22, 2, 83, .65 },
        { 24, 1, 79, .7 },  { 25, 1, 83, .65 }, { 26, 2, 88, .7 }, { 28, 4, 81, .75 },
    };
}

Notes fluteCounter()
{
    Notes n;
    for (int b = 0; b < 8; ++b)
    {
        const auto& chord = breakChords[(size_t)b % 4];
        n.push_back ({ b * 4.0 + 1.5, 1.0, chord[2] + 12, 0.6 });
        n.push_back ({ b * 4.0 + 2.5, 0.5, chord[1] + 12, 0.55 });
        n.push_back ({ b * 4.0 + 3.0, 1.0, chord[3] + 12, 0.6 });
    }
    return n;
}

Notes drumsIntro()
{
    Notes n;
    for (int b = 0; b < 4; ++b)
    {
        for (int e = 0; e < 8; ++e)
            n.push_back ({ b * 4.0 + e * 0.5, 0.25, closedHat, 0.3 + 0.03 * b + (e % 2 ? 0.0 : 0.12) });
        if (b >= 2)
            for (int s = 0; s < 16; ++s)
                n.push_back ({ b * 4.0 + s * 0.25, 0.2, shaker, 0.18 + (s % 4 == 2 ? 0.12 : 0.0) });
        n.push_back ({ b * 4.0 + 3.0, 0.25, rim, 0.5 });
    }
    n.push_back ({ 15.5, 0.5, openHat, 0.6 });
    return n;
}

Notes drumsVerse()
{
    Notes n;
    for (int b = 0; b < 4; ++b)
    {
        const double o = b * 4.0;
        n.push_back ({ o, 0.5, kick, 0.95 });
        n.push_back ({ o + 1.75, 0.25, kick, 0.6 });
        n.push_back ({ o + 2.5, 0.5, kick, 0.85 });
        if (b == 3)
            n.push_back ({ o + 3.25, 0.25, kick, 0.7 });
        n.push_back ({ o + 1, 0.5, snare, 0.9 });
        n.push_back ({ o + 3, 0.5, snare, 0.92 });
        n.push_back ({ o + 1.75, 0.25, snare2, 0.25 });
        n.push_back ({ o + 3.75, 0.25, snare2, 0.3 });
        for (int s = 0; s < 8; ++s)
            if (s != 7)
                n.push_back ({ o + s * 0.5, 0.25, closedHat, s % 2 ? 0.45 : 0.62 });
        n.push_back ({ o + 3.5, 0.5, openHat, 0.55 });
    }
    return n;
}

Notes drumsBuild()
{
    // Eight bars: quarter kicks, and a snare roll that doubles every two bars, getting louder.
    Notes n;
    for (int b = 0; b < 8; ++b)
    {
        for (int q = 0; q < 4; ++q)
            n.push_back ({ b * 4.0 + q, 0.5, kick, 0.8 + 0.02 * b });
        const double division = b < 2 ? 1.0 : b < 4 ? 0.5 : b < 6 ? 0.25 : b < 7 ? 0.25 : 0.125;
        for (double t = 0; t < 4.0 - 1e-9; t += division)
        {
            const double progress = (b * 4.0 + t) / 32.0;
            n.push_back ({ b * 4.0 + t, std::min (division, 0.25), snare, 0.35 + 0.6 * progress });
        }
        if (b < 4)
            for (int e = 0; e < 8; ++e)
                n.push_back ({ b * 4.0 + e * 0.5, 0.25, closedHat, 0.4 });
    }
    n.push_back ({ 0, 2, crash, 0.7 });
    n.push_back ({ 16, 2, crash, 0.6 });
    return n;
}

Notes drumsDrop (bool fill)
{
    Notes n;
    for (int b = 0; b < 4; ++b)
    {
        const double o = b * 4.0;
        const bool filling = fill && b == 3;
        for (int q = 0; q < 4; ++q)
            if (! (filling && q == 3))
                n.push_back ({ o + q, 0.5, kick, q == 0 ? 1.0 : 0.92 });
        n.push_back ({ o + 1, 0.5, clap, 0.9 });
        n.push_back ({ o + 3, 0.5, clap, 0.92 });
        n.push_back ({ o + 1, 0.5, snare2, 0.55 });
        n.push_back ({ o + 3, 0.5, snare2, 0.55 });
        for (int s = 0; s < 16; ++s)
        {
            if (filling && s >= 8)
                break;
            static const double accents[] = { 0.42, 0.3, 0.68, 0.3 };
            if (s % 4 != 2)
                n.push_back ({ o + s * 0.25, 0.2, closedHat, accents[s % 4] });
        }
        for (int q = 0; q < 4; ++q)
            if (! (filling && q >= 2))
                n.push_back ({ o + q + 0.5, 0.4, openHat, 0.5 });
        if (b >= 2 && ! filling)
            for (int q = 0; q < 4; ++q)
                n.push_back ({ o + q, 0.5, ride, 0.4 });
        n.push_back ({ o + 0.75, 0.25, perc, 0.45 });
        n.push_back ({ o + 2.25, 0.25, perc, 0.4 });
        if (filling)
        {
            static const int toms[] = { highTom, highTom, midTom, midTom, midTom, lowTom, lowTom, lowTom };
            for (int s = 0; s < 8; ++s)
                n.push_back ({ o + 2 + s * 0.25, 0.25, toms[s], 0.6 + s * 0.04 });
            n.push_back ({ o + 3.5, 0.25, snare, 0.9 });
            n.push_back ({ o + 3.75, 0.25, snare, 1.0 });
        }
    }
    n.push_back ({ 0, 2, crash, 0.85 });
    return n;
}

Notes drumsOutro()
{
    Notes n;
    for (int b = 0; b < 4; ++b)
    {
        const double o = b * 4.0;
        n.push_back ({ o, 0.5, kick, 0.85 - 0.08 * b });
        n.push_back ({ o + 2, 0.5, kick, 0.75 - 0.08 * b });
        n.push_back ({ o + 3, 0.5, snare, 0.6 - 0.06 * b });
        for (int e = 0; e < 8; ++e)
            n.push_back ({ o + e * 0.5, 0.25, closedHat, 0.4 - 0.04 * b });
        n.push_back ({ o + 1, 0.5, ride, 0.35 });
    }
    return n;
}

Notes fillSnare()
{
    Notes n { { 0, 0.5, kick, 0.9 } };
    for (int s = 0; s < 16; ++s)
        n.push_back ({ s * 0.25, 0.25, s < 8 ? snare2 : snare, 0.3 + s * 0.04 });
    return n;
}

Notes fillToms()
{
    Notes n;
    static const int toms[] = { highTom, highTom, highTom, midTom, midTom, midTom, lowTom, lowTom };
    for (int s = 0; s < 16; ++s)
        n.push_back ({ s * 0.25, 0.25, toms[s / 2], 0.45 + s * 0.03 });
    n.push_back ({ 3.75, 0.25, crash, 0.5 });
    return n;
}

Notes percGroove()
{
    Notes n;
    for (int b = 0; b < 4; ++b)
    {
        const double o = b * 4.0;
        for (int s : { 3, 6, 10, 13 })
            n.push_back ({ o + s * 0.25, 0.25, cowbell, 0.35 + (s == 10 ? 0.15 : 0.0) });
        for (int s : { 2, 7, 11, 15 })
            n.push_back ({ o + s * 0.25, 0.25, rim, 0.4 });
        for (int s = 0; s < 16; ++s)
            n.push_back ({ o + s * 0.25, 0.2, shaker, 0.18 + (s % 2 ? 0.0 : 0.1) });
        n.push_back ({ o + 1.25, 0.25, perc, 0.5 });
        n.push_back ({ o + 3.5, 0.25, perc, 0.45 });
        if (b == 3)
            for (int s : { 12, 13, 14, 15 })
                n.push_back ({ o + s * 0.25, 0.25, s < 14 ? midTom : lowTom, 0.5 });
    }
    return n;
}

Notes pluckBass()
{
    Notes n;
    for (int b = 0; b < 4; ++b)
    {
        const int r = mainRoots[(size_t)b];
        const double o = b * 4.0;
        const std::pair<int, int> steps[] = {
            { 0, 0 }, { 3, 0 }, { 6, 12 }, { 8, 0 }, { 11, 0 }, { 12, 7 }, { 14, 12 }
        };
        for (auto [s, interval] : steps)
            n.push_back ({ o + s * 0.25, s == 0 || s == 8 ? 0.5 : 0.25, r + interval, s % 8 == 0 ? 0.9 : 0.7 });
    }
    return n;
}

Notes subBass()
{
    Notes n;
    for (int b = 0; b < 4; ++b)
    {
        const int r = mainRoots[(size_t)b] - 12;
        n.push_back ({ b * 4.0, 2.4, r, 0.9 });
        n.push_back ({ b * 4.0 + 2.5, 1.4, r, 0.85 });
    }
    return n;
}

Notes reeseBass()
{
    Notes n;
    for (int b = 0; b < 4; ++b)
    {
        const int r = mainRoots[(size_t)b];
        const double o = b * 4.0;
        n.push_back ({ o, 1.9, r, 0.9 });
        n.push_back ({ o + 2, 0.9, r, 0.8 });
        n.push_back ({ o + 3, 0.45, r + 12, 0.85 });
        n.push_back ({ o + 3.5, 0.45, r, 0.8 });
    }
    return n;
}

Notes acidLine()
{
    // Sixteenths from A minor, accents and a few tied notes for glides; fixed seed, the same each time.
    std::mt19937 random (124);
    static const int scale[] = { 0, 3, 5, 7, 10, 12, 15 };
    Notes n;
    for (int b = 0; b < 4; ++b)
    {
        const int r = mainRoots[(size_t)b];
        for (int s = 0; s < 16; ++s)
        {
            if (random() % 7 == 0 && s % 4 != 0)
                continue; // a rest
            const int pitch = s % 4 == 0 ? r : r + scale[random() % 7];
            const bool tie = random() % 5 == 0;
            const bool accent = s % 4 == 0 || random() % 4 == 0;
            n.push_back ({ b * 4.0 + s * 0.25, tie ? 0.5 : 0.18, pitch, accent ? 0.95 : 0.6 });
        }
    }
    return n;
}

Notes bassBuild()
{
    Notes n;
    for (int b = 0; b < 4; ++b)
        for (int e = 0; e < (b < 3 ? 8 : 16); ++e)
        {
            const double d = b < 3 ? 0.5 : 0.25;
            n.push_back ({ b * 4.0 + e * d, d * 0.8, b < 3 ? 45 : 45 + e, 0.7 + 0.02 * e });
        }
    return n;
}

Notes sweepPadIntro()
{
    Notes n;
    for (auto p : { 45, 57, 60, 64, 71 })
        n.push_back ({ 0, 16, p, 0.7 });
    for (auto p : { 41, 57, 60, 64, 69 })
        n.push_back ({ 16, 8, p, 0.7 });
    for (auto p : { 43, 55, 59, 62, 67 })
        n.push_back ({ 24, 8, p, 0.72 });
    return n;
}

Notes softKeysOutro()
{
    Notes n;
    for (int b = 0; b < 4; ++b)
    {
        const auto& chord = mainChords[(size_t)b];
        for (int q = 0; q < 8; ++q)
            n.push_back ({ b * 4.0 + q * 0.5, 0.9, chord[(size_t)(q % 4)] + (q >= 4 ? 12 : 0), 0.5 - 0.05 * b });
    }
    return n;
}

Notes riser (int bars) { return { { 0, bars * 4.0 - 0.05, 60, 0.9 } }; }

Notes zapHits() { return { { 0, 1, 84, 0.9 }, { 0, 1, 72, 0.8 }, { 2.5, 0.5, 79, 0.5 } }; }

Notes thumpLayer()
{
    Notes n;
    for (int b = 0; b < 4; ++b)
        for (int q = 0; q < 4; ++q)
            n.push_back ({ b * 4.0 + q, 0.4, 36, 0.85 });
    return n;
}

Notes hatLayer()
{
    Notes n;
    for (int b = 0; b < 4; ++b)
        for (int s = 1; s < 16; s += 2)
            n.push_back ({ b * 4.0 + s * 0.25, 0.1, 84, s % 4 == 3 ? 0.45 : 0.3 });
    return n;
}

//==============================================================================
/** Makes the saved plugin states fit to share (the song ships with Spinus as its demo, demo/): each names
    the plugin file it was loaded from (Tracktion's "filename", a path on this computer, kept as just the file's
    name: Spinus finds the plugin from the project, not from the state), and Surge XT's also names the patch
    file it loaded last (its lastLoadedPath, for its patch browser; emptied). Nothing else changes. */
void makeStatesPortable (const juce::File& folder)
{
    for (const auto& entry : juce::RangedDirectoryIterator (folder.getChildFile ("plugin-states"), false, "*.bin"))
    {
        const auto file = entry.getFile();
        juce::MemoryBlock data;
        if (! file.loadFileAsData (data))
            continue;
        auto tree = juce::ValueTree::readFromData (data.getData(), data.getSize());
        if (! tree.isValid())
            continue;

        if (const auto path = tree["filename"].toString(); juce::File::isAbsolutePath (path))
            tree.setProperty ("filename", juce::File (path).getFileName(), nullptr);

        // Surge XT's chunk: "sub3", the XML's size (little-endian), wavetable sizes (32 bytes in all), the XML.
        juce::MemoryBlock chunk;
        if (tree.hasProperty ("state") && chunk.fromBase64Encoding (tree["state"].toString()) &&
            chunk.getSize() > 32 && std::memcmp (chunk.getData(), "sub3", 4) == 0)
        {
            const auto* bytes = static_cast<const char*> (chunk.getData());
            const auto xmlSize = (size_t)juce::ByteOrder::littleEndianInt (bytes + 4);
            if (32 + xmlSize <= chunk.getSize())
            {
                std::string xml (bytes + 32, xmlSize);
                const std::string tag = "<lastLoadedPath v=\"";
                if (const auto at = xml.find (tag); at != std::string::npos)
                    if (const auto end = xml.find ('"', at + tag.size()); end != std::string::npos)
                    {
                        xml.erase (at + tag.size(), end - (at + tag.size()));
                        juce::MemoryOutputStream out;
                        out.write (bytes, 4);
                        out.writeInt ((int)xml.size()); // little-endian
                        out.write (bytes + 8, 24);
                        out.write (xml.data(), xml.size());
                        out.write (bytes + 32 + xmlSize, chunk.getSize() - 32 - xmlSize);
                        tree.setProperty ("state", out.getMemoryBlock().toBase64Encoding(), nullptr);
                    }
            }
        }

        juce::MemoryOutputStream out;
        tree.writeToStream (out);
        file.replaceWithData (out.getData(), out.getDataSize());
    }
}

int make (Session& session, ProjectDocument& document, const juce::File& folder, const juce::File& pluginFolder)
{
    if (folder.exists())
    {
        std::cerr << folder.getFullPathName() << " exists already: choose a new folder.\n";
        return 1;
    }
    const auto started = Clock::now();
    Song song (session, pluginFolder);
    auto& c = song.c;
    std::mt19937 random (2026);

    c.run ("project.setTempo", object ({ { "bpm", bpm } }));
    c.run ("project.setKey", object ({ { "tonic", 9 }, { "scale", "minor" } }));

    // Colours by part.
    const juce::String drumsColour = "#FFE0603F", bassColour = "#FF3F7FE0", chordsColour = "#FF8F5FD0",
                       leadColour = "#FFE0B03F", arpColour = "#FF3FC0A0", fxColour = "#FFB0B0B0",
                       breakColour = "#FFD070A0";

    //--------------------------------------------------------------------------
    // Channels: Spinus's Drum Machine, 3x Osc with its factory presets, Surge XT with its factory patches.
    using Ch = Song::Channel;
    const Ch drums = song.addChannel ("Drums", "spinus.drum-machine", {}, -7.0, 0.0, "Drums", drumsColour);
    const Ch percussion = song.addChannel ("Percussion", "spinus.drum-machine", {}, -6.0, 0.15, "Drums", drumsColour);
    const Ch thump = song.addChannel ("Thump", "spinus.3x-osc", "Thump", -8.0, 0.0, "Drums", drumsColour);
    const Ch hat = song.addChannel ("Hat Layer", "spinus.3x-osc", "Hat", -10.0, 0.3, "Drums", drumsColour);
    const Ch sub = song.addChannel ("Sub Bass", "spinus.3x-osc", "Sub Bass", -11.0, 0.0, "Bass", bassColour);
    const Ch reese = song.addChannel ("Reese Bass", "spinus.3x-osc", "Reese Bass", -7.0, 0.0, "Bass", bassColour);
    const Ch acid = song.addChannel ("Acid Bass", "spinus.3x-osc", "Acid Bass", -11.0, 0.0, "Bass", bassColour);
    const Ch bassPluck = song.addChannel ("Pluck Bass", "spinus.3x-osc", "Pluck Bass", -5.0, 0.0, "Bass", bassColour);
    const Ch pad = song.addChannel ("Warm Pad (Surge XT)", "bundled.surge-xt", {}, -7.0, 0.0, "Chords", chordsColour);
    const Ch keys = song.addChannel ("EP Keys (Surge XT)", "bundled.surge-xt", {}, -4.0, -0.1, "Chords", chordsColour);
    const Ch poly =
        song.addChannel ("Poly Chords (Surge XT)", "bundled.surge-xt", {}, -8.0, 0.0, "Chords", chordsColour);
    const Ch brass =
        song.addChannel ("Synth Brass", "spinus.3x-osc", "Synth Brass", -14.0, -0.15, "Chords", chordsColour);
    const Ch sweepPad = song.addChannel ("Sweep Pad", "spinus.3x-osc", "Sweep Pad", -11.0, 0.0, "Chords", chordsColour);
    const Ch strings = song.addChannel ("String Pad", "spinus.3x-osc", "String Pad", -10.0, 0.0, "Chords", breakColour);
    const Ch choir = song.addChannel ("Choir Pad", "spinus.3x-osc", "Choir Pad", -12.0, 0.0, "Chords", breakColour);
    const Ch organ = song.addChannel ("Organ", "spinus.3x-osc", "Organ", -10.0, 0.1, "Chords", chordsColour);
    const Ch softKeys = song.addChannel ("Soft Keys", "spinus.3x-osc", "Soft Keys", -5.0, 0.0, "Chords", chordsColour);
    const Ch lead = song.addChannel ("Lead (Surge XT)", "bundled.surge-xt", {}, -8.0, 0.0, "Leads", leadColour);
    const Ch sawLead = song.addChannel ("Saw Lead", "spinus.3x-osc", "Saw Lead", -14.0, 0.0, "Leads", leadColour);
    const Ch bell = song.addChannel ("Bell", "spinus.3x-osc", "Bell", -5.0, 0.2, "Leads", breakColour);
    const Ch flute = song.addChannel ("Flute Lead", "spinus.3x-osc", "Flute Lead", -6.0, -0.2, "Leads", breakColour);
    const Ch arp = song.addChannel ("Pluck Arp (Surge XT)", "bundled.surge-xt", {}, -12.0, 0.0, "Arps", arpColour);
    const Ch chip = song.addChannel ("Chip Arp", "spinus.3x-osc", "Chip Square", -13.0, 0.25, "Arps", arpColour);
    const Ch pluck = song.addChannel ("Pluck", "spinus.3x-osc", "Pluck", -8.0, -0.25, "Arps", arpColour);
    const Ch noiseRiser = song.addChannel ("Noise Riser", "spinus.3x-osc", "Noise Riser", -14.0, 0.0, "FX", fxColour);
    const Ch zap = song.addChannel ("Zap", "spinus.3x-osc", "Zap", -13.0, 0.0, "FX", fxColour);
    const Ch space = song.addChannel ("Space FX (Surge XT)", "bundled.surge-xt", {}, -12.0, 0.0, "FX", fxColour);

    //--------------------------------------------------------------------------
    // Mixer: buses, send effects and insert effects.
    const int drumsBus =
        song.addBus ("Drums Bus", { drums.insert, percussion.insert, thump.insert, hat.insert }, drumsColour);
    const int bassBus =
        song.addBus ("Bass Bus", { sub.insert, reese.insert, acid.insert, bassPluck.insert }, bassColour);
    const int synthBus =
        song.addBus ("Synth Bus",
                     { pad.insert, keys.insert, poly.insert, brass.insert, sweepPad.insert, strings.insert,
                       choir.insert, organ.insert, softKeys.insert, arp.insert, chip.insert, pluck.insert },
                     chordsColour);
    const int leadBus =
        song.addBus ("Lead Bus", { lead.insert, sawLead.insert, bell.insert, flute.insert }, leadColour);
    const int fxBus = song.addBus ("FX Bus", { noiseRiser.insert, zap.insert, space.insert }, fxColour);
    const int hall = song.addInsert ("Hall Reverb", "#FF5F9FD0");
    const int plate = song.addInsert ("Plate Reverb", "#FF5FB0D0");
    const int room = song.addInsert ("Room Reverb", "#FF5FC0C0");
    const int delay = song.addInsert ("Delay", "#FF70D0B0");
    constexpr int master = 1;

    // Insert effects.
    const int drumsEq = song.addEffect (drumsBus, "spinus.eq");
    const int drumsComp = song.addEffect (drumsBus, "spinus.compressor");
    const int bassDucker = song.addEffect (bassBus, "spinus.ducker");
    const int bassComp = song.addEffect (bassBus, "spinus.compressor");
    const int bassEq = song.addEffect (bassBus, "spinus.eq");
    const int synthFilter = song.addEffect (synthBus, "spinus.filter");
    const int synthDucker = song.addEffect (synthBus, "spinus.ducker");
    const int synthGlue = song.addEffect (synthBus, "spinus.compressor");
    const int leadEq = song.addEffect (leadBus, "spinus.eq");
    const int leadComp = song.addEffect (lead.insert, "spinus.compressor");
    const int arpFilter = song.addEffect (arp.insert, "spinus.filter");
    const int padWidth = song.addEffect (pad.insert, "spinus.stereo-utility");
    const int keysEarly = song.addEffect (keys.insert, "bundled.dragonfly.early");
    const int acidFilter = song.addEffect (acid.insert, "spinus.filter");
    const int fxGate = song.addEffect (fxBus, "spinus.gate");
    const int hallReverb = song.addEffect (hall, "bundled.dragonfly.hall");
    const int hallEq = song.addEffect (hall, "spinus.eq");
    const int plateReverb = song.addEffect (plate, "bundled.dragonfly.plate");
    const int roomReverb = song.addEffect (room, "bundled.dragonfly.room");
    const int delayFx = song.addEffect (delay, "bundled.surge-xt-effects");
    const int delayFilter = song.addEffect (delay, "spinus.filter");
    // The master's chain: EQ, glue compressor, loudness meter, limiter (every bus plays into the master).
    const int masterEq = song.addEffect (master, "spinus.eq");
    const int masterComp = song.addEffect (master, "spinus.compressor");
    song.addEffect (master, "spinus.loudness-meter");
    const int masterLimiter = song.addEffect (master, "spinus.limiter");
    juce::ignoreUnused (fxGate);

    // Sidechains: the bass and the synths duck under the Thump kick layer (it plays in the drops).
    c.run ("insert.setSidechain", object ({ { "insert", bassBus }, { "source", thump.insert } }));
    c.run ("insert.setSidechain", object ({ { "insert", synthBus }, { "source", thump.insert } }));

    // Sends.
    for (auto [from, level] : std::vector<std::pair<int, double>> { { pad.insert, -10 },
                                                                    { lead.insert, -12 },
                                                                    { bell.insert, -6 },
                                                                    { flute.insert, -8 },
                                                                    { strings.insert, -9 },
                                                                    { choir.insert, -8 },
                                                                    { arp.insert, -14 },
                                                                    { keys.insert, -14 },
                                                                    { pluck.insert, -12 },
                                                                    { zap.insert, -8 },
                                                                    { space.insert, -6 },
                                                                    { noiseRiser.insert, -10 } })
        song.send (from, hall, level);
    for (auto [from, level] : std::vector<std::pair<int, double>> {
             { drums.insert, -16 }, { percussion.insert, -12 }, { brass.insert, -14 } })
        song.send (from, plate, level);
    song.send (keys.insert, room, -12);
    song.send (percussion.insert, room, -14);
    for (auto [from, level] : std::vector<std::pair<int, double>> { { lead.insert, -14 },
                                                                    { arp.insert, -16 },
                                                                    { bell.insert, -12 },
                                                                    { pluck.insert, -14 },
                                                                    { zap.insert, -10 } })
        song.send (from, delay, level);
    c.run ("insert.set", object ({ { "insert", hall }, { "volumeDb", -4.0 } }));
    c.run ("insert.set", object ({ { "insert", plate }, { "volumeDb", -6.0 } }));
    c.run ("insert.set", object ({ { "insert", room }, { "volumeDb", -6.0 } }));
    c.run ("insert.set", object ({ { "insert", delay }, { "volumeDb", -3.0 } }));
    c.run ("insert.set", object ({ { "insert", synthBus }, { "stereoWidth", 1.2 } }));
    c.run ("insert.set", object ({ { "insert", master }, { "volumeDb", -1.0 } }));

    // The devices exist now: the effects' settings (by the values the plugins show), and Surge XT's patches.
    song.syncDevices();
    song.setTexts (
        drumsEq, { { "band1_on", "On" }, { "band1_freq", "30 Hz" }, { "band2_gain", "2.0" }, { "band7_gain", "1.5" } });
    song.setTexts (drumsComp, { { "threshold", "-14 dB" },
                                { "ratio", "3:1" },
                                { "attack", "15 ms" },
                                { "release", "100 ms" },
                                { "makeup", "2 dB" } });
    song.setTexts (bassDucker, { { "amount", "60%" }, { "release", "180 ms" } });
    song.setTexts (bassComp, { { "threshold", "-18 dB" },
                               { "ratio", "3:1" },
                               { "attack", "10 ms" },
                               { "release", "100 ms" },
                               { "detector", "RMS" } });
    song.setTexts (bassEq, { { "band1_on", "On" }, { "band1_freq", "28 Hz" }, { "band4_gain", "-2.5" } });
    song.setTexts (synthFilter, { { "type", "High pass" }, { "cutoff", "20 Hz" }, { "resonance", "15%" } });
    song.setTexts (synthDucker, { { "amount", "40%" }, { "release", "200 ms" } });
    song.setTexts (synthGlue, { { "threshold", "-14 dB" },
                                { "ratio", "1.5:1" },
                                { "knee", "12 dB" },
                                { "attack", "30 ms" },
                                { "release", "200 ms" },
                                { "detector", "RMS" } });
    song.setTexts (leadEq, { { "band1_on", "On" }, { "band1_freq", "120 Hz" } });
    song.setTexts (leadComp, { { "threshold", "-16 dB" }, { "ratio", "3:1" }, { "attack", "5 ms" } });
    song.setTexts (arpFilter, { { "cutoff", "6000 Hz" }, { "resonance", "35%" } });
    song.setTexts (acidFilter, { { "type", "Low pass" }, { "cutoff", "2500 Hz" }, { "resonance", "45%" } });
    song.setTexts (hallReverb, { { "0", "0" }, { "1", "15" }, { "2", "70" }, { "3", "32" }, { "15", "2.6" } });
    song.setTexts (hallEq, { { "band1_on", "On" }, { "band1_freq", "180 Hz" } });
    song.setTexts (plateReverb, { { "0", "0" }, { "1", "80" }, { "5", "1.4" } });
    song.setTexts (roomReverb, { { "0", "0" }, { "1", "30" }, { "3", "60" } });
    song.setTexts (delayFilter, { { "type", "Low pass" }, { "cutoff", "4500 Hz" }, { "resonance", "10%" } });
    song.setTexts (masterEq, { { "band1_on", "On" }, { "band1_freq", "25 Hz" }, { "band7_gain", "1.0" } });
    song.setTexts (masterComp, { { "threshold", "-10 dB" },
                                 { "ratio", "2:1" },
                                 { "attack", "30 ms" },
                                 { "release", "200 ms" },
                                 { "detector", "RMS" } });
    song.setTexts (masterLimiter, { { "ceiling", "-1.0" }, { "input", "5.0" } });
    juce::ignoreUnused (padWidth, keysEarly, plateReverb);

    song.loadSurgePatch (pad, "Pads/MKS-70 Warm Pad");
    song.loadSurgePatch (keys, "Keys/DX EP");
    song.loadSurgePatch (poly, "Polysynths/Anthemish 1");
    song.loadSurgePatch (lead, "Leads/Classic Lead 1");
    song.loadSurgePatch (arp, "Plucks/Diamonds");
    song.loadSurgePatch (space, "FX/Space Adventure 1");
    for (int i = 0; i < 40; ++i) // Surge XT finishes loading patches on its own thread
        settle (50);
    song.syncDevices();

    //--------------------------------------------------------------------------
    // Patterns.
    const int pDrumsIntro = song.addPattern ("Drums Intro", 4, drumsColour);
    song.setNotes (pDrumsIntro, drums, drumsIntro());
    const int pDrumsVerse = song.addPattern ("Drums Verse", 4, drumsColour);
    song.setNotes (pDrumsVerse, drums, drumsVerse());
    c.run ("pattern.setSwing", object ({ { "pattern", pDrumsVerse }, { "swing", 0.18 } }));
    const int pDrumsBuild = song.addPattern ("Drums Build", 8, drumsColour);
    song.setNotes (pDrumsBuild, drums, drumsBuild());
    const int pDrumsDrop = song.addPattern ("Drums Drop", 4, drumsColour);
    song.setNotes (pDrumsDrop, drums, drumsDrop (false));
    song.setNotes (pDrumsDrop, thump, thumpLayer());
    const int pDrumsDropFill = song.addPattern ("Drums Drop Fill", 4, drumsColour);
    song.setNotes (pDrumsDropFill, drums, drumsDrop (true));
    song.setNotes (pDrumsDropFill, thump, thumpLayer());
    const int pDrumsOutro = song.addPattern ("Drums Outro", 4, drumsColour);
    song.setNotes (pDrumsOutro, drums, drumsOutro());
    const int pFillSnare = song.addPattern ("Fill Snare", 1, drumsColour);
    song.setNotes (pFillSnare, drums, fillSnare());
    const int pFillToms = song.addPattern ("Fill Toms", 1, drumsColour);
    song.setNotes (pFillToms, percussion, fillToms());
    const int pPerc = song.addPattern ("Percussion Groove", 4, drumsColour);
    song.setNotes (pPerc, percussion, percGroove());
    c.run ("pattern.setSwing", object ({ { "pattern", pPerc }, { "swing", 0.25 } }));
    const int pHats = song.addPattern ("Hat Layer", 4, drumsColour);
    song.setNotes (pHats, hat, hatLayer());

    const int pPluckBass = song.addPattern ("Pluck Bass", 4, bassColour);
    song.setNotes (pPluckBass, bassPluck, pluckBass());
    const int pSub = song.addPattern ("Sub Bass", 4, bassColour);
    song.setNotes (pSub, sub, subBass());
    const int pReese = song.addPattern ("Reese Bass", 4, bassColour);
    song.setNotes (pReese, reese, reeseBass());
    const int pAcid = song.addPattern ("Acid Line", 4, bassColour);
    song.setNotes (pAcid, acid, acidLine());
    const int pBassBuild = song.addPattern ("Bass Build", 4, bassColour);
    song.setNotes (pBassBuild, sub, bassBuild());

    const int pPad = song.addPattern ("Pad Chords", 4, chordsColour);
    song.setNotes (pPad, pad, chordsEachBar (mainChords, 4, 3.95, 0.65));
    const int pKeys = song.addPattern ("EP Comping", 4, chordsColour);
    song.setNotes (
        pKeys, keys,
        rhythmChords (mainChords, 4, { { 0, 1.4 }, { 1.5, 0.45 }, { 2.5, 0.9 }, { 3.5, 0.45 } }, 0.6, random));
    const int pPoly = song.addPattern ("Poly Stabs", 4, chordsColour);
    song.setNotes (
        pPoly, poly,
        rhythmChords (mainChords, 4, { { 0.5, 0.35 }, { 1.5, 0.35 }, { 2.5, 0.35 }, { 3.5, 0.35 } }, 0.7, random, 12));
    const int pBrass = song.addPattern ("Brass Stabs", 4, chordsColour);
    song.setNotes (pBrass, brass,
                   rhythmChords (mainChords, 4, { { 0, 0.6 }, { 0.75, 0.4 }, { 1.5, 0.4 }, { 3, 0.4 } }, 0.7, random));
    const int pSweep = song.addPattern ("Sweep Pad Intro", 8, chordsColour);
    song.setNotes (pSweep, sweepPad, sweepPadIntro());
    const int pStrings = song.addPattern ("Strings Breakdown", 8, breakColour);
    song.setNotes (pStrings, strings, chordsEachBar (breakChords, 8, 3.95, 0.6));
    const int pChoirBreak = song.addPattern ("Choir Breakdown", 8, breakColour);
    song.setNotes (pChoirBreak, choir, chordsEachBar (breakChords, 8, 3.95, 0.5, 12));
    const int pChoir = song.addPattern ("Choir Drop", 4, breakColour);
    song.setNotes (pChoir, choir, chordsEachBar (mainChords, 4, 3.95, 0.55, 12));
    const int pOrgan = song.addPattern ("Organ Outro", 4, chordsColour);
    song.setNotes (pOrgan, organ, chordsEachBar (mainChords, 4, 3.95, 0.5));
    const int pSoftKeys = song.addPattern ("Soft Keys Outro", 4, chordsColour);
    song.setNotes (pSoftKeys, softKeys, softKeysOutro());

    const int pLeadA = song.addPattern ("Lead A", 8, leadColour);
    song.setNotes (pLeadA, lead, leadA());
    const int pLeadB = song.addPattern ("Lead B", 8, leadColour);
    song.setNotes (pLeadB, lead, leadB (0));
    song.setNotes (pLeadB, sawLead, leadB (-12));
    const int pBell = song.addPattern ("Bell Melody", 8, breakColour);
    song.setNotes (pBell, bell, bellMelody());
    song.setNotes (pBell, flute, fluteCounter());

    const int pArp = song.addPattern ("Arp", 4, arpColour);
    song.setNotes (pArp, arp, arpeggio (mainChords, 4, 12, 2, 0.6));
    const int pChip = song.addPattern ("Chip Arp", 4, arpColour);
    song.setNotes (pChip, chip, arpeggio (mainChords, 4, 24, 1, 0.55, 0.12, { 0, 2, 1, 2 }));
    const int pPluck = song.addPattern ("Pluck Arp", 4, arpColour);
    song.setNotes (pPluck, pluck, arpeggio (mainChords, 4, 12, 2, 0.6, 0.4, { 0, 4, 2, 4, 1, 5, 2, 5 }));

    const int pRiser8 = song.addPattern ("Riser 8 bars", 8, fxColour);
    song.setNotes (pRiser8, noiseRiser, riser (8));
    const int pRiser4 = song.addPattern ("Riser 4 bars", 4, fxColour);
    song.setNotes (pRiser4, noiseRiser, riser (4));
    const int pZap = song.addPattern ("Zap Hits", 1, fxColour);
    song.setNotes (pZap, zap, zapHits());
    const int pSpace = song.addPattern ("Space FX", 4, fxColour);
    song.setNotes (pSpace, space, { { 0, 8, 57, 0.6 }, { 8, 6, 64, 0.5 } });

    //--------------------------------------------------------------------------
    // The Playlist.
    const int lDrums = song.addLane ("Drums"), lFills = song.addLane ("Fills"), lPerc = song.addLane ("Percussion"),
              lBass = song.addLane ("Bass"), lSub = song.addLane ("Sub"), lChords = song.addLane ("Chords"),
              lPad = song.addLane ("Pads"), lLead = song.addLane ("Lead"), lArp = song.addLane ("Arp"),
              lArp2 = song.addLane ("Arp 2"), lBreak = song.addLane ("Breakdown"), lFx = song.addLane ("FX"),
              lImpacts = song.addLane ("Impacts (audio)");

    for (auto& s : sections)
        c.run ("marker.add", object ({ { "at", s.startBar * bar }, { "name", s.name } }));

    // Intro 0-8
    song.place (lPad, pSweep, 0, 8);
    song.place (lArp, pArp, 0, 4);
    song.place (lArp, pArp, 4, 4);
    song.place (lDrums, pDrumsIntro, 4, 4);
    song.place (lFx, pSpace, 0, 4);
    // Verse 8-24
    for (int b = 8; b < 24; b += 4)
    {
        song.place (lDrums, pDrumsVerse, b, 4);
        song.place (lBass, pPluckBass, b, 4);
        song.place (lChords, pKeys, b, 4);
        song.place (lPad, pPad, b, 4);
    }
    song.place (lArp2, pPluck, 8, 4);
    song.place (lArp2, pPluck, 12, 4);
    song.place (lArp, pArp, 16, 4);
    song.place (lArp, pArp, 20, 4);
    song.place (lPerc, pPerc, 16, 4);
    song.place (lPerc, pPerc, 20, 4);
    song.place (lSub, pSub, 16, 4);
    song.place (lSub, pSub, 20, 4);
    song.place (lFills, pFillSnare, 23, 1);
    // Build 24-32
    song.place (lDrums, pDrumsBuild, 24, 8);
    song.place (lSub, pBassBuild, 24, 4);
    song.place (lSub, pBassBuild, 28, 4);
    song.place (lArp, pArp, 24, 4);
    song.place (lArp, pArp, 28, 4);
    song.place (lPad, pPad, 24, 4);
    song.place (lPad, pPad, 28, 4);
    song.place (lFx, pRiser8, 24, 8);
    // Drop 32-48
    for (int b = 32; b < 48; b += 4)
    {
        song.place (lDrums, b == 44 ? pDrumsDropFill : pDrumsDrop, b, 4);
        song.place (lBass, pReese, b, 4);
        song.place (lSub, pSub, b, 4);
        song.place (lChords, pPoly, b, 4);
        song.place (lArp, pArp, b, 4);
        if (b >= 36)
            song.place (lPerc, pPerc, b, 4);
        if (b >= 40)
        {
            song.place (lPad, pBrass, b, 4);
            song.place (lArp2, pHats, b, 4);
        }
    }
    song.place (lLead, pLeadA, 32, 8);
    song.place (lLead, pLeadA, 40, 8);
    song.place (lFx, pZap, 32, 1);
    // Breakdown 48-56
    song.place (lBreak, pBell, 48, 8);
    song.place (lPad, pStrings, 48, 8);
    song.place (lChords, pChoirBreak, 52, 4, 4); // its second half
    song.place (lFx, pZap, 48, 1);
    song.place (lFx, pSpace, 50, 4);
    song.place (lFills, pFillToms, 55, 1);
    // Build 2 56-60: the second half of the build
    song.place (lDrums, pDrumsBuild, 56, 4, 4);
    song.place (lSub, pBassBuild, 56, 4);
    song.place (lArp, pArp, 56, 4);
    song.place (lFx, pRiser4, 56, 4);
    song.place (lPad, pPad, 56, 4);
    // Drop 2 60-76
    for (int b = 60; b < 76; b += 4)
    {
        song.place (lDrums, b == 72 ? pDrumsDropFill : pDrumsDrop, b, 4);
        song.place (lBass, b % 8 == 4 ? pReese : pAcid, b, 4);
        song.place (lSub, pSub, b, 4);
        song.place (lChords, pPoly, b, 4);
        song.place (lArp, pArp, b, 4);
        song.place (lPerc, pPerc, b, 4);
        song.place (lBreak, pChoir, b, 4);
        song.place (lArp2, b >= 64 ? pChip : pHats, b, 4);
        if (b >= 68)
            song.place (lPad, pBrass, b, 4);
        else
            song.place (lPad, pPad, b, 4);
    }
    song.place (lLead, pLeadB, 60, 8);
    song.place (lLead, pLeadB, 68, 8);
    song.place (lFx, pZap, 60, 1);
    // Outro 76-84
    song.place (lDrums, pDrumsOutro, 76, 4);
    song.place (lDrums, pDrumsOutro, 80, 4);
    song.place (lPad, pPad, 76, 4);
    song.place (lPad, pPad, 80, 4);
    song.place (lChords, pSoftKeys, 76, 4);
    song.place (lChords, pOrgan, 80, 4);
    song.place (lArp, pArp, 76, 4);
    song.place (lArp, pArp, 80, 4);
    song.place (lFx, pZap, 76, 1);
    song.place (lFills, pFillToms, 79, 1);

    // Audio clips: an impact on each drop, a reverse swell before it (original sounds, made for this song).
    const auto audio = juce::File (juce::SystemStats::getEnvironmentVariable ("SHOWCASE_AUDIO", {}));
    int audioClips = 0;
    if (audio.isDirectory())
    {
        const auto impact = audio.getChildFile ("Showcase Impact.wav").getFullPathName();
        const auto swell = audio.getChildFile ("Showcase Reverse Swell.wav").getFullPathName();
        const auto first = c.run ("clip.addAudio", object ({ { "lane", lImpacts },
                                                             { "file", impact },
                                                             { "start", 32 * bar },
                                                             { "length", 2 * bar },
                                                             { "name", "Impact" },
                                                             { "gainDb", -6.0 } }));
        const auto impactChannel = (int)first["channel"];
        const auto second = c.run ("clip.addAudio", object ({ { "lane", lImpacts },
                                                              { "file", swell },
                                                              { "start", 30 * bar },
                                                              { "length", 2 * bar },
                                                              { "name", "Reverse Swell" },
                                                              { "gainDb", -8.0 } }));
        const auto swellChannel = (int)second["channel"];
        c.run ("clip.addAudio", object ({ { "lane", lImpacts },
                                          { "file", impact },
                                          { "start", 60 * bar },
                                          { "length", 2 * bar },
                                          { "channel", impactChannel },
                                          { "gainDb", -6.0 } }));
        c.run ("clip.addAudio", object ({ { "lane", lImpacts },
                                          { "file", swell },
                                          { "start", 58 * bar },
                                          { "length", 2 * bar },
                                          { "channel", swellChannel },
                                          { "gainDb", -8.0 } }));
        c.run ("clip.addAudio", object ({ { "lane", lImpacts },
                                          { "file", swell },
                                          { "start", 46 * bar },
                                          { "length", 2 * bar },
                                          { "channel", swellChannel },
                                          { "gainDb", -11.0 },
                                          { "fadeIn", bar } }));
        audioClips = 5;
        for (auto id : { impactChannel, swellChannel })
            for (auto& ch : session.getProject().channels)
                if ((int)ch.id.value == id)
                {
                    c.run ("channel.set", object ({ { "channel", id }, { "group", "FX" } }));
                    c.run ("channel.route", object ({ { "channel", id }, { "insert", fxBus } }));
                    song.send (fxBus, hall, -14);
                }
    }
    else
        std::cerr << "SHOWCASE_AUDIO not set: no audio clips.\n";

    //--------------------------------------------------------------------------
    // Automation clips.
    using P = Song::P;
    song.syncDevices();
    auto lane = [&] (const juce::String& name) { return song.addLane (name); };
    const auto hz = [&] (int device, const char* id, const char* text)
    { return (double)song.valueFor (device, id, text); };

    // Arp filter: opens over the intro, again over the build, closes in the outro.
    const int laArp = lane ("Auto: Arp Filter Cutoff");
    song.automate (laArp, 0, 8, Song::parameter (arpFilter, "cutoff"),
                   { { 0, hz (arpFilter, "cutoff", "250 Hz"), 0.4 }, { 32, hz (arpFilter, "cutoff", "6000 Hz") } });
    song.automate (laArp, 24, 8, Song::parameter (arpFilter, "cutoff"),
                   { { 0, hz (arpFilter, "cutoff", "600 Hz"), 0.5 },
                     { 31.9, hz (arpFilter, "cutoff", "14000 Hz") },
                     { 32, hz (arpFilter, "cutoff", "6000 Hz") } });
    song.automate (laArp, 76, 8, Song::parameter (arpFilter, "cutoff"),
                   { { 0, hz (arpFilter, "cutoff", "6000 Hz"), -0.4 }, { 32, hz (arpFilter, "cutoff", "180 Hz") } });
    const int laArpRes = lane ("Auto: Arp Filter Resonance");
    song.automate (laArpRes, 24, 8, Song::parameter (arpFilter, "resonance"),
                   { { 0, hz (arpFilter, "resonance", "20%") },
                     { 31.9, hz (arpFilter, "resonance", "70%") },
                     { 32, hz (arpFilter, "resonance", "35%") } });

    // The synth bus's high-pass rises through each build and drops back on the downbeat.
    const int laHpf = lane ("Auto: Synth Bus High-pass");
    song.automate (laHpf, 24, 8, Song::parameter (synthFilter, "cutoff"),
                   { { 0, hz (synthFilter, "cutoff", "20 Hz"), 0.6 },
                     { 31.9, hz (synthFilter, "cutoff", "1200 Hz") },
                     { 32, hz (synthFilter, "cutoff", "20 Hz") } });
    song.automate (laHpf, 56, 4, Song::parameter (synthFilter, "cutoff"),
                   { { 0, hz (synthFilter, "cutoff", "20 Hz"), 0.6 },
                     { 15.9, hz (synthFilter, "cutoff", "1500 Hz") },
                     { 16, hz (synthFilter, "cutoff", "20 Hz") } });

    // The noise riser: louder and brighter to each drop.
    const int laRiserVol = lane ("Auto: Riser Volume");
    song.automate (laRiserVol, 24, 8, Song::channelVolume (noiseRiser),
                   { { 0, song.volumeValue (noiseRiser, "-40"), 0.5 }, { 32, song.volumeValue (noiseRiser, "-8") } });
    song.automate (laRiserVol, 56, 4, Song::channelVolume (noiseRiser),
                   { { 0, song.volumeValue (noiseRiser, "-30"), 0.5 }, { 16, song.volumeValue (noiseRiser, "-6") } });
    const int laRiserCut = lane ("Auto: Riser Cutoff (3x Osc)");
    const auto riserCut = [&] (const char* t) { return hz (noiseRiser.device, "cutoff", t); };
    song.automate (laRiserCut, 24, 8, Song::parameter (noiseRiser.device, "cutoff"),
                   { { 0, riserCut ("300 Hz"), 0.5 }, { 32, riserCut ("16000 Hz") } });
    song.automate (laRiserCut, 56, 4, Song::parameter (noiseRiser.device, "cutoff"),
                   { { 0, riserCut ("500 Hz"), 0.5 }, { 16, riserCut ("16000 Hz") } });

    // Master: fades in and out.
    const int laMaster = lane ("Auto: Master Volume");
    song.automate (
        laMaster, 0, 2, Song::insertVolume (master),
        { { 0, song.insertVolumeValue (master, "-30"), -0.5 }, { 8, song.insertVolumeValue (master, "-1") } });
    song.automate (
        laMaster, 80, 4, Song::insertVolume (master),
        { { 0, song.insertVolumeValue (master, "-1"), 0.3 }, { 16, song.insertVolumeValue (master, "-60") } });

    // Drums bus: hats fade in at the end of the intro; the drums thin out in the outro.
    const int laDrumBus = lane ("Auto: Drums Bus Volume");
    song.automate (laDrumBus, 4, 4, Song::insertVolume (drumsBus),
                   { { 0, song.insertVolumeValue (drumsBus, "-24") }, { 16, song.insertVolumeValue (drumsBus, "0") } });
    song.automate (laDrumBus, 78, 6, Song::insertVolume (drumsBus),
                   { { 0, song.insertVolumeValue (drumsBus, "0") }, { 24, song.insertVolumeValue (drumsBus, "-20") } });

    // Lead pan moves, and an auto-pan on the arp (a sine, sixteen points a bar).
    const int laLeadPan = lane ("Auto: Lead Pan");
    song.automate (laLeadPan, 32, 16, Song::channelPan (lead),
                   { { 0, 0.5 }, { 16, 0.35 }, { 32, 0.65 }, { 48, 0.4 }, { 64, 0.5 } });
    song.automate (laLeadPan, 60, 16, Song::channelPan (lead),
                   { { 0, 0.5 }, { 8, 0.3 }, { 24, 0.7 }, { 40, 0.3 }, { 56, 0.7 }, { 64, 0.5 } });
    const int laArpPan = lane ("Auto: Arp Auto-pan");
    for (int start : { 32, 60 })
    {
        std::vector<P> points;
        for (int i = 0; i <= 16 * 16; ++i)
            points.push_back ({ i * 0.25, 0.5 + 0.35 * std::sin (i * juce::MathConstants<double>::twoPi / 16.0) });
        song.automate (laArpPan, start, 16, Song::channelPan (arp), points);
    }

    // The hall reverb: its return swells in the breakdown, with a longer decay.
    const int laHall = lane ("Auto: Hall Return");
    song.automate (laHall, 46, 12, Song::insertVolume (hall),
                   { { 0, song.insertVolumeValue (hall, "-4") },
                     { 8, song.insertVolumeValue (hall, "0"), 0.3 },
                     { 40, song.insertVolumeValue (hall, "1") },
                     { 48, song.insertVolumeValue (hall, "-4") } });
    const int laHallDecay = lane ("Auto: Hall Decay");
    song.automate (laHallDecay, 48, 8, Song::parameter (hallReverb, "15"),
                   { { 0, hz (hallReverb, "15", "2.6") },
                     { 16, hz (hallReverb, "15", "6") },
                     { 31, hz (hallReverb, "15", "6") },
                     { 32, hz (hallReverb, "15", "2.6") } });

    // The delay: more feedback in the breakdown, and a throw at the end of the first drop.
    const int laDelay = lane ("Auto: Delay Feedback");
    const auto feedback = song.currentValue (delayFx, "2");
    song.automate (laDelay, 46, 2, Song::parameter (delayFx, "2"),
                   { { 0, feedback }, { 4, feedback + 0.25 }, { 7.9, feedback + 0.25 }, { 8, feedback } });
    song.automate (laDelay, 48, 8, Song::parameter (delayFx, "2"),
                   { { 0, feedback }, { 24, feedback + 0.2 }, { 31.9, feedback + 0.2 }, { 32, feedback } });
    const int laDelayCut = lane ("Auto: Delay Tone");
    song.automate (laDelayCut, 48, 8, Song::parameter (delayFilter, "cutoff"),
                   { { 0, hz (delayFilter, "cutoff", "1500 Hz") }, { 32, hz (delayFilter, "cutoff", "9000 Hz") } });

    // Surge XT: filter cutoffs of the lead, the chords and the pad, from their patches' own values.
    constexpr const char* surgeCutoff = "319"; // A Filter 1 Cutoff
    const auto leadCut = song.currentValue (lead.device, surgeCutoff);
    const int laLeadCut = lane ("Auto: Surge Lead Cutoff");
    song.automate (laLeadCut, 32, 16, Song::parameter (lead.device, surgeCutoff),
                   { { 0, leadCut - 0.12 }, { 32, leadCut }, { 56, leadCut + 0.05 }, { 64, leadCut } });
    song.automate (laLeadCut, 60, 16, Song::parameter (lead.device, surgeCutoff),
                   { { 0, leadCut - 0.15, 0.4 }, { 16, leadCut + 0.08 }, { 48, leadCut + 0.08 }, { 64, leadCut } });
    const auto polyCut = song.currentValue (poly.device, surgeCutoff);
    const int laPolyCut = lane ("Auto: Surge Poly Cutoff");
    song.automate (laPolyCut, 32, 16, Song::parameter (poly.device, surgeCutoff),
                   { { 0, polyCut - 0.2, 0.3 }, { 64, polyCut } });
    song.automate (laPolyCut, 60, 16, Song::parameter (poly.device, surgeCutoff),
                   { { 0, polyCut - 0.1 }, { 32, polyCut + 0.05 }, { 64, polyCut - 0.05 } });
    const auto padCut = song.currentValue (pad.device, surgeCutoff);
    const int laPadCut = lane ("Auto: Surge Pad Cutoff");
    song.automate (laPadCut, 8, 16, Song::parameter (pad.device, surgeCutoff),
                   { { 0, padCut - 0.2, 0.3 }, { 64, padCut } });
    song.automate (laPadCut, 76, 8, Song::parameter (pad.device, surgeCutoff),
                   { { 0, padCut, -0.3 }, { 32, padCut - 0.3 } });
    const int laArpVol = lane ("Auto: Surge Arp Volume");
    const auto arpVolume = song.currentValue (arp.device, "237"); // A Volume
    song.automate (laArpVol, 16, 8, Song::parameter (arp.device, "237"),
                   { { 0, arpVolume - 0.25 }, { 32, arpVolume } });

    // 3x Osc filters: a wobbling Reese, a squelching acid line, a sweep pad that opens.
    const int laReese = lane ("Auto: Reese Cutoff (3x Osc)");
    for (int start : { 32, 64, 72 })
    {
        const int bars = start == 32 ? 16 : 4;
        std::vector<P> points;
        for (int i = 0; i <= bars * 4; ++i)
            points.push_back (
                { i * 1.0, i % 2 == 0 ? hz (reese.device, "cutoff", "400 Hz") : hz (reese.device, "cutoff", "2200 Hz"),
                  i % 2 == 0 ? 0.5 : -0.5 });
        song.automate (laReese, start, bars, Song::parameter (reese.device, "cutoff"), points);
    }
    const int laAcid = lane ("Auto: Acid Cutoff (3x Osc)");
    song.automate (laAcid, 60, 4, Song::parameter (acid.device, "cutoff"),
                   { { 0, hz (acid.device, "cutoff", "300 Hz"), 0.4 }, { 16, hz (acid.device, "cutoff", "3000 Hz") } });
    song.automate (laAcid, 68, 4, Song::parameter (acid.device, "cutoff"),
                   { { 0, hz (acid.device, "cutoff", "500 Hz") },
                     { 8, hz (acid.device, "cutoff", "4000 Hz") },
                     { 12, hz (acid.device, "cutoff", "800 Hz") },
                     { 16, hz (acid.device, "cutoff", "5000 Hz") } });
    const int laAcidRes = lane ("Auto: Acid Resonance (3x Osc)");
    song.automate (laAcidRes, 60, 16, Song::parameter (acid.device, "resonance"),
                   { { 0, hz (acid.device, "resonance", "30%") },
                     { 32, hz (acid.device, "resonance", "75%") },
                     { 64, hz (acid.device, "resonance", "50%") } });
    const int laAcidFilter = lane ("Auto: Acid Insert Filter");
    song.automate (laAcidFilter, 60, 16, Song::parameter (acidFilter, "cutoff"),
                   { { 0, hz (acidFilter, "cutoff", "900 Hz") },
                     { 16, hz (acidFilter, "cutoff", "4000 Hz") },
                     { 32, hz (acidFilter, "cutoff", "1200 Hz") },
                     { 48, hz (acidFilter, "cutoff", "6000 Hz") },
                     { 64, hz (acidFilter, "cutoff", "2500 Hz") } });
    const int laSweep = lane ("Auto: Sweep Pad Cutoff (3x Osc)");
    song.automate (
        laSweep, 0, 8, Song::parameter (sweepPad.device, "cutoff"),
        { { 0, hz (sweepPad.device, "cutoff", "400 Hz"), 0.3 }, { 32, hz (sweepPad.device, "cutoff", "8000 Hz") } });

    // The breakdown: a wider pad, strings and bell moving across, choir swells.
    const int laWidth = lane ("Auto: Pad Width");
    song.automate (laWidth, 48, 8, Song::parameter (padWidth, "width"),
                   { { 0, hz (padWidth, "width", "100%") }, { 32, hz (padWidth, "width", "190%") } });
    const int laStrings = lane ("Auto: Strings Pan");
    song.automate (laStrings, 48, 8, Song::channelPan (strings), { { 0, 0.3 }, { 16, 0.7 }, { 32, 0.3 } });
    const int laBell = lane ("Auto: Bell Pan");
    song.automate (laBell, 48, 8, Song::channelPan (bell), { { 0, 0.75 }, { 16, 0.25 }, { 32, 0.75 } });
    const int laChoir = lane ("Auto: Choir Volume");
    song.automate (laChoir, 52, 4, Song::channelVolume (choir),
                   { { 0, song.volumeValue (choir, "-40"), 0.4 }, { 16, song.volumeValue (choir, "-12") } });
    song.automate (laChoir, 60, 16, Song::channelVolume (choir),
                   { { 0, song.volumeValue (choir, "-24") },
                     { 32, song.volumeValue (choir, "-12") },
                     { 64, song.volumeValue (choir, "-14") } });

    // Verse: an EP volume ride; percussion and hats move.
    const int laKeys = lane ("Auto: EP Volume");
    song.automate (laKeys, 8, 16, Song::channelVolume (keys),
                   { { 0, song.volumeValue (keys, "-10") },
                     { 16, song.volumeValue (keys, "-4") },
                     { 48, song.volumeValue (keys, "-4") },
                     { 64, song.volumeValue (keys, "-7") } });
    const int laPerc = lane ("Auto: Percussion Pan");
    song.automate (laPerc, 16, 8, Song::channelPan (percussion), { { 0, 0.4 }, { 16, 0.7 }, { 32, 0.4 } });
    const int laHatPan = lane ("Auto: Closed Hat Pan (Drum Machine)");
    {
        std::vector<P> points;
        for (int i = 0; i <= 32; ++i)
            points.push_back ({ i * 2.0, i % 2 == 0 ? 0.4 : 0.6 });
        song.automate (laHatPan, 32, 16, Song::parameter (drums.device, "pad4_pan"), points);
    }

    // Master EQ: brighter through the first build; the synth bus ducks harder in the second drop.
    const int laMasterEq = lane ("Auto: Master EQ High Shelf");
    song.automate (laMasterEq, 24, 8, Song::parameter (masterEq, "band7_gain"),
                   { { 0, hz (masterEq, "band7_gain", "1.0") },
                     { 31.9, hz (masterEq, "band7_gain", "4.0") },
                     { 32, hz (masterEq, "band7_gain", "1.0") } });
    const int laDuck = lane ("Auto: Synth Ducker Amount");
    song.automate (laDuck, 60, 16, Song::parameter (synthDucker, "amount"),
                   { { 0, hz (synthDucker, "amount", "40%") },
                     { 32, hz (synthDucker, "amount", "65%") },
                     { 64, hz (synthDucker, "amount", "65%") } });
    const int laPlate = lane ("Auto: Plate Wet");
    song.automate (laPlate, 60, 16, Song::parameter (plateReverb, "1"),
                   { { 0, hz (plateReverb, "1", "60") }, { 64, hz (plateReverb, "1", "95") } });
    const int laLeadComp = lane ("Auto: Lead Compressor Threshold");
    song.automate (laLeadComp, 60, 16, Song::parameter (leadComp, "threshold"),
                   { { 0, hz (leadComp, "threshold", "-16") }, { 64, hz (leadComp, "threshold", "-24") } });
    const int laKeysEarly = lane ("Auto: EP Early Reflections Wet");
    song.automate (laKeysEarly, 8, 16, Song::parameter (keysEarly, "wet level"),
                   { { 0, song.currentValue (keysEarly, "wet level") },
                     { 64, song.currentValue (keysEarly, "wet level") * 0.8 } });
    const int laSpace = lane ("Auto: Space FX Pan");
    song.automate (laSpace, 0, 4, Song::channelPan (space), { { 0, 0.2 }, { 16, 0.8 } });
    song.automate (laSpace, 50, 4, Song::channelPan (space), { { 0, 0.8 }, { 16, 0.2 } });

    song.syncDevices();
    const auto built = msSince (started);

    // Saved as a normal project.
    const auto saveStart = Clock::now();
    const auto saved = document.saveAs (folder);
    const auto saveMs = msSince (saveStart);
    if (saved.failed())
    {
        std::cerr << "Save failed: " << saved.getErrorMessage() << "\n";
        return 1;
    }
    makeStatesPortable (folder);

    const auto& project = session.getProject();
    int notes = 0, clips = 0, automation = 0, played = 0;
    for (auto& clip : project.playlist.clips)
        if (auto* pc = std::get_if<model::PatternClip> (&clip.content))
            if (auto* p = project.findPattern (pc->pattern))
                for (auto& [channel, list] : p->notes)
                    for (auto& n : list)
                        if (n.start >= pc->offset && n.start < pc->offset + clip.length)
                            ++played;
    for (auto& p : project.patterns)
        for (auto& [channel, list] : p.notes)
            notes += (int)list.size();
    for (auto& clip : project.playlist.clips)
        (std::holds_alternative<model::AutomationClip> (clip.content) ? automation : clips)++;
    std::cout << "Made " << folder.getFullPathName() << " in " << built << " ms (" << c.count << " commands); saved in "
              << saveMs << " ms\n"
              << "  channels " << project.channels.size() << ", inserts " << project.mixer.inserts.size() + 1
              << ", effects " << song.effects.size() << ", patterns " << project.patterns.size() << ", notes " << notes
              << " (" << played << " played in the song)"
              << ", lanes " << project.playlist.lanes.size() << ", pattern/audio clips " << clips << " (audio "
              << audioClips << "), automation clips " << automation << " (" << song.automationPoints << " points)\n"
              << "  Surge XT patches: " << song.surgePatches.joinIntoString (", ")
              << (song.surgeProblems.isEmpty() ? "" : " -- problems: " + song.surgeProblems.joinIntoString (", "))
              << "\n";
    for (auto& i : song.instruments)
        std::cout << "  instrument: " << i.name << " = " << i.type
                  << (i.preset.isNotEmpty() ? " / " + i.preset : juce::String()) << "\n";
    for (auto& [insert, type] : song.effects)
        std::cout << "  effect on insert " << insert << ": " << type << "\n";
    for (auto& d : session.getAdapter().getMissingDevices())
        std::cout << "  MISSING device " << d.value << "\n";
    return 0;
}

//==============================================================================
// Measurements.

struct Stats
{
    float peak = 0;
    double rms = 0;
    int64_t clipped = 0;
};

Stats statsOf (const juce::AudioBuffer<float>& audio, int64_t from, int64_t to)
{
    Stats s;
    double sum = 0;
    int64_t count = 0;
    to = std::min<int64_t> (to, audio.getNumSamples());
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        for (auto i = from; i < to; ++i)
        {
            const auto x = audio.getSample (ch, (int)i);
            s.peak = std::max (s.peak, std::abs (x));
            sum += (double)x * x;
            ++count;
            if (std::abs (x) >= 0.999f)
                ++s.clipped;
        }
    s.rms = count > 0 ? std::sqrt (sum / (double)count) : 0.0;
    return s;
}

juce::String db (double x) { return x > 0 ? juce::String (juce::Decibels::gainToDecibels (x), 1) + " dB" : "-inf"; }

int measure (Session& session, ProjectDocument& document, EngineHost& host, const juce::File& folder,
             const juce::File& wav, const juce::File& saveCopy)
{
    // Opening the project: reading it, and until every device is made.
    auto start = Clock::now();
    const auto opened = document.open (folder);
    const auto openMs = msSince (start);
    if (opened.failed())
    {
        std::cerr << "Open failed: " << opened.getErrorMessage() << "\n";
        return 1;
    }
    session.getEdit().dispatchPendingUpdatesSynchronously();
    settle (1);
    const auto loadedMs = msSince (start);

    // Experiments: commands from a JSON file ([{"command": ..., "parameters": {...}}]) before measuring.
    if (const auto file = juce::SystemStats::getEnvironmentVariable ("SHOWCASE_COMMANDS", {}); file.isNotEmpty())
    {
        Commands extra (session);
        const auto list = juce::JSON::parse (juce::File (file));
        for (auto& step : *list.getArray())
            extra.run (step["command"].toString(), step["parameters"]);
        session.getEdit().dispatchPendingUpdatesSynchronously();
        settle (50);
        std::cout << "Ran " << extra.count << " commands from " << file << "\n";
    }
    if (juce::SystemStats::getEnvironmentVariable ("SHOWCASE_DESCRIBE", {}).isNotEmpty())
        std::cout << session.getAdapter().describe() << "\n";
    std::cout << "Project opened in " << openMs << " ms; devices ready after " << loadedMs << " ms\n";
    for (auto& d : session.getAdapter().getMissingDevices())
        std::cout << "  MISSING device " << d.value << "\n";

    // The main window, and every pattern opened in the Piano Roll (each with the first of its channels).
    ui::MainView view { session, &document };
    view.setSize (1600, 1000);
    start = Clock::now();
    settle (5);
    view.createComponentSnapshot (view.getLocalBounds());
    std::cout << "Main window drawn in " << msSince (start) << " ms\n";
    double worst = 0, total = 0;
    juce::String worstName;
    const auto project = session.getProject();
    for (auto& p : project.patterns)
    {
        if (p.notes.empty())
            continue;
        const auto channel = p.notes.begin()->first;
        start = Clock::now();
        session.setCurrentPattern (p.id);
        session.setSelectedChannel (channel);
        view.getWorkspace().showPanel ("pianoRoll");
        settle (1);
        if (auto* roll = dynamic_cast<ui::PianoRoll*> (view.getWorkspace().getPanel ("pianoRoll")))
        {
            roll->handleUpdateNowIfNeeded();
            roll->createComponentSnapshot (roll->getLocalBounds());
        }
        view.createComponentSnapshot (view.getLocalBounds());
        const auto ms = msSince (start);
        size_t notes = 0;
        for (auto& [ch, list] : p.notes)
            notes += list.size();
        std::cout << "  open pattern \"" << p.name << "\" (" << notes << " notes): " << ms << " ms\n";
        total += ms;
        if (ms > worst)
        {
            worst = ms;
            worstName = p.name;
        }
    }
    std::cout << "Opening patterns: " << total / (double)project.patterns.size() << " ms on average, worst " << worst
              << " ms (" << worstName << ")\n";

    // The Playlist and Mixer drawn with everything on them.
    for (auto* panel : { "playlist", "mixer", "channelRack" })
    {
        start = Clock::now();
        view.getWorkspace().showPanel (panel);
        settle (1);
        view.createComponentSnapshot (view.getLocalBounds());
        std::cout << "Panel " << panel << " shown and drawn in " << msSince (start) << " ms\n";
    }

    // An edit and its undo (one note moved in the lead pattern).
    for (auto& p : project.patterns)
        if (p.name == "Lead A")
        {
            auto notes = p.notes.begin()->second;
            juce::Array<juce::var> list;
            for (auto& n : notes)
                list.add (object ({ { "start", (juce::int64)n.start + (list.isEmpty() ? 240 : 0) },
                                    { "length", (juce::int64)n.length },
                                    { "pitch", n.pitch },
                                    { "velocity", n.velocity } }));
            start = Clock::now();
            Commands (session).run ("pattern.setNotes", object ({ { "pattern", (int)p.id.value },
                                                                  { "channel", (int)p.notes.begin()->first.value },
                                                                  { "notes", list } }));
            settle (1);
            const auto editMs = msSince (start);
            start = Clock::now();
            session.getHistory().undo();
            settle (1);
            std::cout << "Edit (move a note): " << editMs << " ms; undo: " << msSince (start) << " ms\n";
        }

    if (saveCopy != juce::File())
    {
        start = Clock::now();
        const auto autosaved = document.autosave();
        std::cout << "Autosave: " << msSince (start) << " ms"
                  << (autosaved.failed() ? " FAILED " + autosaved.getErrorMessage() : "") << "\n";
        start = Clock::now();
        const auto saved = document.saveAs (saveCopy);
        std::cout << "Save As (copy): " << msSince (start) << " ms"
                  << (saved.failed() ? " FAILED " + saved.getErrorMessage() : "") << "\n";
        start = Clock::now();
        const auto again = document.save();
        std::cout << "Save (again): " << msSince (start) << " ms"
                  << (again.failed() ? " FAILED " + again.getErrorMessage() : "") << "\n";
    }

    // Export (offline render) of the whole song.
    const auto songSeconds = (double)engine::ExportJob::getSongEnd (session.getProject()) / 960.0 * 60.0 / bpm;
    if (wav != juce::File())
    {
        engine::ExportSettings settings;
        settings.format = engine::ExportSettings::Format::wav24;
        settings.sampleRate = 48000.0;
        settings.tailSeconds = 3.0;
        std::optional<engine::ExportJob::Result> result;
        start = Clock::now();
        auto job = session.createExportJob (wav, settings, [&] (const engine::ExportJob::Result& r) { result = r; });
        job->start();
        while (! result)
            settle (20);
        const auto exportMs = msSince (start);
        job = nullptr;
        if (! result->ok)
        {
            std::cerr << "Export failed: " << result->error << "\n";
            return 1;
        }
        std::cout << "Export: " << songSeconds << " s of song in " << exportMs / 1000.0 << " s ("
                  << songSeconds / (exportMs / 1000.0) << "x real time)\n";

        // Stems (one file per insert), to check the balance: SHOWCASE_STEMS=<folder>.
        if (const auto stems = juce::SystemStats::getEnvironmentVariable ("SHOWCASE_STEMS", {}); stems.isNotEmpty())
        {
            auto stemSettings = settings;
            stemSettings.stems = true;
            std::optional<engine::ExportJob::Result> stemResult;
            const auto stemStart = Clock::now();
            auto stemJob = session.createExportJob (juce::File (stems).getChildFile ("Mix.wav"), stemSettings,
                                                    [&] (const engine::ExportJob::Result& r) { stemResult = r; });
            stemJob->start();
            while (! stemResult)
                settle (20);
            stemJob = nullptr;
            std::cout << "Stems export: " << stemResult->files.size() << " files in " << msSince (stemStart) / 1000.0
                      << " s" << (stemResult->ok ? "" : " FAILED " + stemResult->error) << "\n";
            juce::AudioFormatManager stemFormats;
            stemFormats.registerBasicFormats();
            for (auto& f : stemResult->files)
                if (std::unique_ptr<juce::AudioFormatReader> r (stemFormats.createReaderFor (f)); r != nullptr)
                {
                    juce::AudioBuffer<float> a ((int)r->numChannels, (int)r->lengthInSamples);
                    r->read (&a, 0, (int)r->lengthInSamples, 0, true, true);
                    const auto st = statsOf (a, 0, a.getNumSamples());
                    std::cout << "  stem " << f.getFileName() << ": peak " << db (st.peak) << ", RMS " << db (st.rms)
                              << "\n";
                }
        }

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (result->files.front()));
        if (reader == nullptr)
        {
            std::cerr << "Cannot read " << result->files.front().getFullPathName() << "\n";
            return 1;
        }
        juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->lengthInSamples);
        reader->read (&audio, 0, (int)reader->lengthInSamples, 0, true, true);
        const auto rate = reader->sampleRate;
        const auto secondsPerBar = 4.0 * 60.0 / bpm;
        std::cout << "Rendered file: " << result->files.front().getFullPathName() << ", "
                  << reader->lengthInSamples / rate << " s\n";
        for (auto& s : sections)
        {
            const auto st = statsOf (audio, (int64_t)(s.startBar * secondsPerBar * rate),
                                     (int64_t)((s.startBar + s.bars) * secondsPerBar * rate));
            std::cout << "  " << s.name << " (bars " << s.startBar + 1 << "-" << s.startBar + s.bars << "): peak "
                      << db (st.peak) << ", RMS " << db (st.rms) << ", clipped samples " << st.clipped << "\n";
        }
        const auto all = statsOf (audio, 0, audio.getNumSamples());
        std::cout << "  Whole song: peak " << db (all.peak) << ", RMS " << db (all.rms) << ", clipped samples "
                  << all.clipped << "\n";
    }

    // Playback as the audio thread runs it: the whole song, as fast as it goes (not paced), 128-sample blocks.
    {
        session.setMode (Session::Mode::song);
        session.getEdit().dispatchPendingUpdatesSynchronously();
        test::RealtimeRunner runner (host.getEngine(), { .sampleRate = 48000.0,
                                                         .blockSize = 256,
                                                         .outputChannels = 2,
                                                         .maxSeconds = 2.0 * songSeconds + 120.0,
                                                         .paceInRealTime = false });
        runner.start();
        runner.runUntil (0.5);
        session.getEdit().getTransport().setPosition (tracktion::TimePosition());
        const auto playedFrom = runner.getSamplesProcessed();
        session.play();
        start = Clock::now();
        const auto from = (size_t)(playedFrom / 256);
        // Unpaced, the audio thread never waits: it runs on while the message thread works.
        runner.runUntil ((double)playedFrom / 48000.0 + songSeconds);
        const auto wallMs = msSince (start);
        session.stop();
        runner.stop();

        // What playback sounded like, by section (from where play() was called).
        {
            const auto recorded = runner.getNumRecordedSamples();
            juce::AudioBuffer<float> heard (2, (int)recorded);
            for (int ch = 0; ch < 2; ++ch)
                heard.copyFrom (ch, 0, runner.getChannel (ch).data(), (int)recorded);
            const auto rate = runner.getParams().sampleRate;
            auto offset = playedFrom; // where the first sound is (the transport starts a little after play())
            while (offset < recorded && std::abs (heard.getSample (0, (int)offset)) < 1.0e-4f)
                ++offset;
            std::cout << "  playback started " << (double)(offset - playedFrom) / rate * 1000.0 << " ms after play()\n";
            const auto secondsPerBar = 4.0 * 60.0 / bpm;
            for (auto& s : sections)
            {
                const auto st = statsOf (heard, offset + (int64_t)(s.startBar * secondsPerBar * rate),
                                         offset + (int64_t)((s.startBar + s.bars) * secondsPerBar * rate));
                std::cout << "  played " << s.name << ": peak " << db (st.peak) << ", RMS " << db (st.rms)
                          << ", clipped samples " << st.clipped << "\n";
            }
        }
        const auto& times = runner.getBlockTimesMs();
        std::vector<float> played (times.begin() + (long)from, times.end());
        std::sort (played.begin(), played.end());
        double sum = 0;
        for (auto t : played)
            sum += t;
        const auto period = runner.getBlockPeriodMs();
        const auto mean = played.empty() ? 0.0 : sum / (double)played.size();
        const auto p99 = played.empty() ? 0.0f : played[(size_t)((double)played.size() * 0.99)];
        const auto worstBlock = played.empty() ? 0.0f : played.back();
        int over = 0;
        for (auto t : played)
            if (t > period)
                ++over;
        std::cout << "Playback (256-sample blocks at 48 kHz, " << period << " ms each, " << played.size()
                  << " blocks): mean " << mean << " ms (" << 100.0 * mean / period
                  << "% of real time), 99th percentile " << p99 << " ms (" << 100.0 * p99 / period << "%), worst "
                  << worstBlock << " ms; " << over << " blocks over the period; song played in " << wallMs / 1000.0
                  << " s of wall time (" << songSeconds / (wallMs / 1000.0)
                  << "x real time); message thread stalled at most " << runner.getMaxMessageThreadStallMs() << " ms\n";
    }

    // Real time, as a sound card plays it: 20 seconds of the second drop (the busiest part), 128-sample blocks.
    {
        test::RealtimeRunner runner (host.getEngine(), { .sampleRate = 48000.0,
                                                         .blockSize = 128,
                                                         .outputChannels = 2,
                                                         .maxSeconds = 30.0,
                                                         .paceInRealTime = true });
        runner.start();
        runner.runUntil (0.5);
        session.getEdit().getTransport().setPosition (tracktion::TimePosition::fromSeconds (60 * 4 * 60.0 / bpm));
        const auto from = runner.getBlockTimesMs().size();
        session.play();
        runner.runUntil (20.5);
        session.stop();
        runner.stop();
        const auto& times = runner.getBlockTimesMs();
        std::vector<float> played (times.begin() + (long)from, times.end());
        std::sort (played.begin(), played.end());
        double sum = 0;
        for (auto t : played)
            sum += t;
        const auto period = runner.getBlockPeriodMs();
        const auto mean = played.empty() ? 0.0 : sum / (double)played.size();
        std::cout << "Real-time playback of Drop 2 (128-sample blocks, " << period << " ms each, " << played.size()
                  << " blocks): mean " << mean << " ms (" << 100.0 * mean / period << "%), 99th percentile "
                  << played[(size_t)((double)played.size() * 0.99)] << " ms, worst " << played.back() << " ms; "
                  << runner.getNumOverrunBlocks() << " blocks over the period (dropouts)\n";
    }
    return 0;
}
} // namespace showcase
