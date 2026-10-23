#pragma once

// "Sodium Lights": an original liquid drum & bass piece in F minor at 174 BPM, and its render.
// Included by DnbSong.cpp after its helpers and the showcase song's Song class.

namespace dnb
{
using showcase::N;
using showcase::Notes;
using showcase::Song;
using Ticks = juce::int64;

constexpr double bpm = 174.0;
constexpr const char* title = "Sodium Lights";

struct Section
{
    const char* name;
    int startBar, bars;
};
constexpr Section sections[] = { { "Intro", 0, 16 }, { "Drop", 16, 32 },   { "Breakdown", 48, 16 },
                                 { "Build", 64, 8 }, { "Drop 2", 72, 32 }, { "Outro", 104, 16 } };
constexpr int songBars = 120;

//==============================================================================
// Harmony: eight bars, one chord a bar (MIDI notes; 60 is C5 as Spinus names it).
//   Main:      Fm9 Fm9 | Dbmaj9 Dbmaj9 | Bbm9 Bbm9 | Csus4 C7(b9)
//   Breakdown: Dbmaj9 Dbmaj9 | Eb(add9) Eb(add9) | Cm7 Cm7 | Fm9 Fm9
using Chord = std::vector<int>;
const Chord fm9 { 56, 60, 63, 67 }, dbMaj9 { 53, 56, 60, 63 }, bbm9 { 49, 53, 56, 60 }, cSus { 53, 55, 58, 60 },
    c7b9 { 52, 55, 58, 61 }, ebAdd9 { 55, 58, 63, 65 }, cm7 { 51, 55, 58, 60 };
const std::vector<Chord> mainChords { fm9, fm9, dbMaj9, dbMaj9, bbm9, bbm9, cSus, c7b9 };
const std::vector<int> mainRoots { 41, 41, 37, 37, 34, 34, 36, 36 }; // F2 Db2 Bb1 C2
const std::vector<Chord> breakChords { dbMaj9, dbMaj9, ebAdd9, ebAdd9, cm7, cm7, fm9, fm9 };
const std::vector<int> breakRoots { 37, 37, 39, 39, 36, 36, 41, 41 };

Notes padChords (const std::vector<Chord>& chords, const std::vector<int>& roots, double velocity)
{
    Notes n;
    for (int b = 0; b < 8; b += 2) // two bars a chord (the C7(b9) bar changes the voicing only)
    {
        const bool split = chords[(size_t)b] != chords[(size_t)b + 1];
        for (int half = 0; half < (split ? 2 : 1); ++half)
        {
            const auto& chord = chords[(size_t)(b + half)];
            const double at = (b + half) * 4.0, length = split ? 3.95 : 7.95;
            for (auto p : chord)
                n.push_back ({ at, length, p + 12, velocity });
            n.push_back ({ at, length, roots[(size_t)b] + 24, velocity * 0.85 });
        }
    }
    return n;
}

/** Electric piano comping: the chord's hits with a liquid, laid-back syncopation. */
Notes rhodes (const std::vector<Chord>& chords, double velocity, std::mt19937& random, bool sparse = false)
{
    Notes n;
    std::uniform_real_distribution<double> human (-0.05, 0.05);
    const std::vector<std::pair<double, double>> busy { { 0, 1.4 }, { 1.5, 0.4 }, { 2.5, 1.3 }, { 3.75, 0.25 } };
    const std::vector<std::pair<double, double>> calm { { 0, 2.9 }, { 3, 0.9 } };
    for (int b = 0; b < 8; ++b)
        for (auto [at, length] : sparse ? calm : busy)
        {
            const double v = velocity + human (random) - (at > 0 ? 0.08 : 0.0);
            int i = 0;
            for (auto p : chords[(size_t)b])
                n.push_back ({ b * 4.0 + at + 0.012 * i++, length, p, v }); // a little strum
        }
    return n;
}

/** The bass line over eight bars of roots: the sub plays the long notes, the Reese adds octaves. */
Notes subLine (const std::vector<int>& roots)
{
    Notes n;
    for (int b = 0; b < 8; b += 2)
    {
        const double o = b * 4.0;
        const int r = roots[(size_t)b], r2 = roots[(size_t)b + 1];
        n.push_back ({ o, 2.5, r, 0.9 });
        n.push_back ({ o + 2.75, 1.0, r, 0.8 });
        n.push_back ({ o + 4.0, 2.25, r2, 0.85 });
        n.push_back ({ o + 6.5, 1.25, r2, 0.8 });
    }
    return n;
}

Notes reeseLine (const std::vector<int>& roots, bool busy)
{
    Notes n;
    for (int b = 0; b < 8; b += 2)
    {
        const double o = b * 4.0;
        const int r = roots[(size_t)b], r2 = roots[(size_t)b + 1];
        n.push_back ({ o, 2.5, r + 12, 0.85 });
        n.push_back ({ o + 2.75, 1.0, r + 12, 0.75 });
        n.push_back ({ o + 4.0, 1.5, r2 + 12, 0.8 });
        if (busy)
        {
            n.push_back ({ o + 5.5, 0.5, r2 + 24, 0.7 });
            n.push_back ({ o + 6.0, 0.5, r2 + 19, 0.7 });
            n.push_back ({ o + 6.5, 1.25, r2 + 12, 0.8 });
        }
        else
            n.push_back ({ o + 6.5, 1.25, r2 + 12, 0.75 });
    }
    return n;
}

Notes subPedal (const std::vector<int>& roots)
{
    Notes n;
    for (int b = 0; b < 8; b += 2)
        n.push_back ({ b * 4.0, 7.9, roots[(size_t)b], 0.6 });
    return n;
}

/** The lead's melody: two eight-bar phrases over the main chords (answer = false, then true). */
Notes leadPhrase (bool answer, int transpose = 0)
{
    Notes n {
        // Fm9
        { 0, 1.5, 72 },
        { 1.5, 0.5, 75 },
        { 2, 1.5, 77 },
        { 3.5, 0.5, 75 },
        { 4, 3, 72 },
        { 7, 0.5, 70 },
        { 7.5, 0.5, 72 },
        // Dbmaj9
        { 8, 1, 77 },
        { 9, 0.5, 75 },
        { 9.5, 1.5, 72 },
        { 11, 1, 68 },
        { 12, 3, 72 },
        { 15, 1, 75 },
        // Bbm9
        { 16, 1.5, 77 },
        { 17.5, 0.5, 80 },
        { 18, 2, 77 },
        { 20, 1, 73 },
        { 21, 1, 75 },
        { 22, 2, 72 },
    };
    if (! answer) // Csus4 - C7(b9): up to the leading tone, back to F
    {
        for (N x :
             { N { 24, 1, 72 }, N { 25, 1, 70 }, N { 26, 2, 67 }, N { 28, 1, 68 }, N { 29, 1, 67 }, N { 30, 2, 64 } })
            n.push_back (x);
    }
    else
    {
        n[4] = { 4, 2, 79 };
        n.push_back ({ 6, 1, 77 });
        n[10] = { 12, 2, 80 };
        n.push_back ({ 14, 1, 79 });
        for (N x : { N { 24, 1.5, 77 }, N { 25.5, 0.5, 75 }, N { 26, 1, 72 }, N { 27, 1, 70 }, N { 28, 1.5, 72 },
                     N { 29.5, 0.5, 70 }, N { 30, 2, 65 } })
            n.push_back (x);
    }
    for (auto& x : n)
    {
        x.pitch += transpose;
        x.velocity = 0.72 + (std::llround (x.start * 4.0) % 16 == 0 ? 0.08 : 0.0);
    }
    return n;
}

/** A quiet, spacious counter-melody for the breakdown: long notes on the chord's top. */
Notes leadBreakdown()
{
    return { { 0, 3, 72, .6 },   { 3, 1, 70, .55 }, { 4, 4, 68, .6 },  { 8, 3, 70, .6 },
             { 11, 1, 72, .55 }, { 12, 4, 75, .6 }, { 16, 2, 72, .6 }, { 18, 2, 70, .55 },
             { 20, 4, 67, .6 },  { 24, 2, 68, .6 }, { 26, 2, 72, .6 }, { 28, 4, 77, .65 } };
}

/** Sixteenth-note arpeggio over the chords (up through the chord and an octave, a liquid "pluck" figure). */
Notes pluckArp (const std::vector<Chord>& chords, double velocity)
{
    Notes n;
    const int order[] = { 0, 2, 4, 1, 3, 5, 2, 4 };
    for (int b = 0; b < 8; ++b)
    {
        const auto& c = chords[(size_t)b];
        const int tones[] = { c[0] + 12, c[1] + 12, c[2] + 12, c[3] + 12, c[0] + 24, c[1] + 24 };
        for (int s = 0; s < 16; ++s)
        {
            if (s % 8 == 7) // a breath at the end of each half bar
                continue;
            const double accent = s % 4 == 0 ? 0.12 : (s % 2 == 0 ? 0.04 : 0.0);
            n.push_back ({ b * 4.0 + s * 0.25, 0.2, tones[order[(b * 3 + s) % 8] % 6], velocity + accent });
        }
    }
    return n;
}

//==============================================================================
// The break. The loop (make_audio.py's BREAK) is cut at each hit: these are its hits' sixteenth steps, and
// slice i plays from key 60 + i. Patterns re-sequence the slices by naming which step's slice plays where.
constexpr int breakSteps[] = { 0,  2,  3,  4,  6,  7,  8,  9,  10, 12, 13, 14, 15,
                               16, 18, 20, 22, 23, 25, 26, 27, 28, 29, 30, 31 };
constexpr int breakKicks[] = { 0, 10, 16, 18, 26 }; // the steps with a kick (the kick layer doubles them)

int sliceKey (int sourceStep)
{
    for (int i = 0; i < (int)std::size (breakSteps); ++i)
        if (breakSteps[i] == sourceStep)
            return 60 + i;
    std::cerr << "No slice at step " << sourceStep << "\n";
    std::exit (3);
}

struct Hit
{
    double at;  // sixteenths from the pattern's start
    int source; // the step of the loop whose slice plays
    double velocity = 0.85;
};

/** The break's notes for these hits, and the kick layer's (the Drum Machine's kick under each kick slice). */
void breakNotes (const std::vector<Hit>& hits, Notes& slices, Notes& kicks)
{
    for (auto& h : hits)
    {
        slices.push_back ({ h.at * 0.25, 0.25, sliceKey (h.source), h.velocity });
        if (std::find (std::begin (breakKicks), std::end (breakKicks), h.source) != std::end (breakKicks))
            kicks.push_back ({ h.at * 0.25, 0.25, 60, std::min (1.0, h.velocity + 0.1) });
    }
}

/** Chop A: the first bar as recorded, the second re-ordered (a kick moved, the snare from bar one). */
std::vector<Hit> chopA()
{
    std::vector<Hit> h;
    for (auto s : breakSteps)
        if (s < 16)
            h.push_back ({ (double)s, s });
    for (auto [at, src] : std::vector<std::pair<int, int>> { { 16, 16 },
                                                             { 18, 18 },
                                                             { 20, 20 },
                                                             { 22, 6 },
                                                             { 23, 23 },
                                                             { 24, 8 },
                                                             { 25, 25 },
                                                             { 26, 26 },
                                                             { 27, 27 },
                                                             { 28, 28 },
                                                             { 29, 29 },
                                                             { 30, 12 },
                                                             { 31, 31 } })
        h.push_back ({ (double)at, src, src == 12 ? 0.6 : 0.85 });
    return h;
}

/** Chop B: a busier two-step variation with a doubled kick and a ghost-note answer. */
std::vector<Hit> chopB()
{
    std::vector<Hit> h;
    for (auto [at, src, v] : std::vector<std::tuple<int, int, double>> {
             { 0, 16, .9 },  { 2, 2, .8 },   { 3, 3, .7 },   { 4, 4, .9 },   { 6, 22, .8 },  { 7, 7, .8 },
             { 8, 8, .8 },   { 9, 9, .8 },   { 10, 10, .9 }, { 11, 18, .7 }, { 12, 28, .9 }, { 13, 13, .7 },
             { 14, 30, .8 }, { 15, 15, .8 }, { 16, 0, .9 },  { 18, 18, .8 }, { 20, 20, .9 }, { 21, 9, .7 },
             { 22, 22, .8 }, { 23, 23, .8 }, { 24, 8, .8 },  { 25, 25, .8 }, { 26, 26, .9 }, { 27, 27, .7 },
             { 28, 12, .9 }, { 29, 29, .8 }, { 30, 4, .55 }, { 31, 31, .8 } })
        h.push_back ({ (double)at, src, v });
    return h;
}

/** A fill: chop A's first bar, then a snare roll in thirty-seconds over the last beats. */
std::vector<Hit> fill()
{
    std::vector<Hit> h;
    for (auto& x : chopA())
        if (x.at < 16)
            h.push_back (x);
    for (auto [at, src, v] : std::vector<std::tuple<double, int, double>> { { 16, 16, .9 },
                                                                            { 18, 18, .8 },
                                                                            { 20, 20, .9 },
                                                                            { 22, 22, .8 },
                                                                            { 23, 23, .7 },
                                                                            { 24, 28, .8 },
                                                                            { 25, 29, .7 },
                                                                            { 26, 4, .7 },
                                                                            { 27, 12, .75 },
                                                                            { 28, 20, .8 },
                                                                            { 28.5, 7, .6 },
                                                                            { 29, 12, .8 },
                                                                            { 29.5, 7, .65 },
                                                                            { 30, 28, .85 },
                                                                            { 30.5, 15, .7 },
                                                                            { 31, 20, .9 },
                                                                            { 31.5, 15, .8 } })
        h.push_back ({ at, src, v });
    return h;
}

/** Half time for the breakdown: the kick on one, the snare on three. */
std::vector<Hit> halftime()
{
    std::vector<Hit> h;
    for (auto [at, src, v] : std::vector<std::tuple<int, int, double>> {
             { 0, 0, .8 },   { 2, 2, .6 },  { 4, 6, .6 },   { 6, 22, .6 },  { 7, 7, .5 },   { 8, 12, .8 },
             { 10, 10, .7 }, { 12, 8, .6 }, { 14, 14, .6 }, { 15, 15, .5 }, { 16, 16, .8 }, { 18, 18, .6 },
             { 20, 22, .6 }, { 22, 6, .6 }, { 24, 20, .8 }, { 26, 26, .7 }, { 27, 27, .5 }, { 28, 30, .6 },
             { 29, 29, .5 }, { 30, 2, .6 }, { 31, 31, .55 } })
        h.push_back ({ (double)at, src, v });
    return h;
}

/** The build's snare roll over eight bars: quarters, eighths, sixteenths, then thirty-seconds, rising. */
std::vector<Hit> snareRoll()
{
    std::vector<Hit> h;
    const auto add = [&] (int fromBar, int bars, double every)
    {
        for (double s = fromBar * 16.0; s < (fromBar + bars) * 16.0 - 1e-9; s += every)
        {
            const double rise = s / 128.0; // 0..1 over the build
            h.push_back ({ s, ((int)(s / every)) % 2 == 0 ? 12 : 20, 0.35 + 0.45 * rise });
        }
    };
    add (0, 2, 4.0);
    add (2, 2, 2.0);
    add (4, 2, 1.0);
    add (6, 1, 0.5);
    for (double s = 112.0; s < 124.0; s += 0.5) // the last bar: three beats of thirty-seconds, then silence
        h.push_back ({ s, ((int)(s * 2)) % 2 == 0 ? 12 : 20, 0.78 });
    return h;
}

// The Drum Machine's Synth Kit pads (C5 upwards).
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

Notes tops (bool full)
{
    Notes n;
    for (int s = 0; s < 32; ++s)
    {
        n.push_back ({ s * 0.25, 0.2, shaker, 0.25 + (s % 4 == 2 ? 0.2 : 0.0) + (s % 2 ? 0.0 : 0.05) });
        if (full && s % 2 == 0)
            n.push_back ({ s * 0.25, 0.25, ride, s % 4 == 2 ? 0.55 : 0.38 });
    }
    if (full)
        n.push_back ({ 7.5, 0.5, openHat, 0.45 });
    return n;
}

//==============================================================================
/** The 0..1 value for a setting's text, by Spinus's one rule for typed settings (dsp/ParameterText.h): in the
    units the device shows ("45%", "120 ms", "900 Hz", "-6 dB"), a plain number in the setting's smallest unit
    (percent, milliseconds, hertz), a choice by its name ("High pass", "On"). */
double valueAt (Song& song, int device, const juce::String& id, const juce::String& text)
{
    return song.valueFor (device, id, text);
}

/** Settings by the text the device shows, in one command (one undo step); prints what each now shows. */
void setTexts (Song& song, int device, std::initializer_list<std::pair<const char*, const char*>> values)
{
    auto* o = new juce::DynamicObject();
    for (auto& [id, text] : values)
        o->setProperty (id, valueAt (song, device, id, text));
    song.c.run ("device.setParameters", object ({ { "device", device }, { "values", juce::var (o) } }));
    song.syncDevices();
    for (auto& [id, text] : values)
        std::cout << "  device " << device << " " << id << " = "
                  << song.session.getDeviceParameterValueText (model::DeviceId { (uint32_t)device }, id) << " (wanted "
                  << text << ")\n";
}

/** Loads a patch from Surge XT's data folder (factory or third-party), as picking it in Surge XT's own browser
    does. */
void loadSurge (Song& song, const Song::Channel& ch, const juce::String& relativePath)
{
    const auto file = song.plugins.getChildFile ("SurgeXTData").getChildFile (relativePath + ".fxp");
    auto* plugin = dynamic_cast<tracktion::ExternalPlugin*> (
        song.session.getAdapter().getDevicePlugin (model::DeviceId { (uint32_t)ch.device }));
    auto* instance = plugin != nullptr ? plugin->getAudioPluginInstance() : nullptr;
    if (instance == nullptr || ! file.existsAsFile())
    {
        std::cerr << "Surge XT patch not loaded: " << relativePath << "\n";
        std::exit (3);
    }
    if (auto r = plugins::ClapPluginFormat::loadPreset (*instance, file); r.failed())
    {
        std::cerr << "Surge XT patch " << relativePath << ": " << r.getErrorMessage() << "\n";
        std::exit (3);
    }
    song.surgePatches.add (relativePath);
}

/** Loads the synthesised break into the Beat Slicer, as dropping it on its window does, and cuts it at its hits. */
void loadBreak (Song& song, const Song::Channel& ch, const juce::File& wav)
{
    auto* plugin = dynamic_cast<tracktion::ExternalPlugin*> (
        song.session.getAdapter().getDevicePlugin (model::DeviceId { (uint32_t)ch.device }));
    auto* slicer =
        plugin != nullptr ? dynamic_cast<dsp::BeatSlicerProcessor*> (plugin->getAudioPluginInstance()) : nullptr;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (wav));
    if (slicer == nullptr || reader == nullptr)
    {
        std::cerr << "The break could not be loaded into the Beat Slicer: " << wav.getFullPathName() << "\n";
        std::exit (3);
    }
    juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read (&audio, 0, (int)reader->lengthInSamples, 0, true, true);
    // Named as the file is, without this computer's path (the loop is kept inside the project).
    slicer->setLoop (std::move (audio), reader->sampleRate, wav.getFileNameWithoutExtension());
    slicer->setLoopBeats (8.0);
    const double samplesPerStep = reader->sampleRate * 60.0 / bpm / 4.0;
    std::vector<juce::int64> points;
    for (auto s : breakSteps)
        points.push_back ((juce::int64)std::llround (s * samplesPerStep));
    slicer->setSlicePoints (points);
    slicer->resetSliceKeys();
    if (slicer->getSliceNotes().size() != std::size (breakSteps))
    {
        std::cerr << "The Beat Slicer made " << slicer->getSliceNotes().size() << " slices, expected "
                  << std::size (breakSteps) << "\n";
        std::exit (3);
    }
}

int make (Session& session, ProjectDocument& document, const juce::File& folder, const juce::File& pluginFolder)
{
    if (folder.exists())
    {
        std::cerr << folder.getFullPathName() << " exists already: choose a new folder.\n";
        return 1;
    }
    const auto audio = juce::File (juce::SystemStats::getEnvironmentVariable ("DNB_AUDIO", {}));
    if (! audio.getChildFile ("Sodium Break 174bpm.wav").existsAsFile())
    {
        std::cerr << "Set DNB_AUDIO to the folder make_audio.py wrote.\n";
        return 1;
    }
    Song song (session, pluginFolder);
    auto& c = song.c;
    std::mt19937 random (174);

    c.run ("project.setTempo", object ({ { "bpm", bpm } }));
    c.run ("project.setKey", object ({ { "tonic", 5 }, { "scale", "minor" } })); // F minor

    // Colours: sodium-lamp orange drums, night blues and purples for the music.
    const juce::String drumsColour = "#FFF0903C", bassColour = "#FF3A5BD9", padColour = "#FF8A5CD6",
                       keysColour = "#FF3FB8A8", leadColour = "#FFF2C14E", fxColour = "#FFA0A4AE",
                       sendColour = "#FF5F9FD0";

    //--------------------------------------------------------------------------
    // Channels.
    using Ch = Song::Channel;
    const Ch brk = song.addChannel ("Break (Beat Slicer)", "spinus.beat-slicer", {}, -4.0, 0.0, "Drums", drumsColour);
    const Ch kickLayer = song.addChannel ("Kick Layer", "spinus.drum-machine", {}, -8.0, 0.0, "Drums", drumsColour);
    const Ch topsCh = song.addChannel ("Tops", "spinus.drum-machine", {}, -12.0, 0.2, "Drums", drumsColour);
    const Ch sub = song.addChannel ("Sub", "spinus.3x-osc", "Sub Bass", -11.0, 0.0, "Bass", bassColour);
    const Ch reese = song.addChannel ("Reese (Surge XT)", "bundled.surge-xt", {}, -8.0, 0.0, "Bass", bassColour);
    const Ch pad = song.addChannel ("Pad (Surge XT)", "bundled.surge-xt", {}, -12.0, 0.0, "Music", padColour);
    const Ch keys = song.addChannel ("Rhodes (Surge XT)", "bundled.surge-xt", {}, -9.0, -0.1, "Music", keysColour);
    const Ch pluck = song.addChannel ("Pluck Arp (Surge XT)", "bundled.surge-xt", {}, -16.0, 0.15, "Music", keysColour);
    const Ch lead = song.addChannel ("Lead (Surge XT)", "bundled.surge-xt", {}, -13.0, 0.0, "Lead", leadColour);
    const Ch riser = song.addChannel ("Riser", "spinus.3x-osc", "Noise Riser", -19.0, 0.0, "FX", fxColour);

    //--------------------------------------------------------------------------
    // Mixer: buses, send effects, insert effects, sidechains.
    const int drumsBus = song.addBus ("Drums Bus", { brk.insert, kickLayer.insert, topsCh.insert }, drumsColour);
    const int bassBus = song.addBus ("Bass Bus", { sub.insert, reese.insert }, bassColour);
    const int musicBus = song.addBus ("Music Bus", { pad.insert, keys.insert, pluck.insert }, padColour);
    const int fxBus = song.addBus ("FX Bus", { riser.insert }, fxColour);
    const int hall = song.addInsert ("Hall Reverb", sendColour);
    const int plate = song.addInsert ("Plate Reverb", sendColour);
    const int delay = song.addInsert ("Delay", sendColour);
    constexpr int master = 1;

    const int breakFilter = song.addEffect (brk.insert, "spinus.filter");
    const int breakEq = song.addEffect (brk.insert, "spinus.eq");
    const int kickEq = song.addEffect (kickLayer.insert, "spinus.eq");
    const int drumsComp = song.addEffect (drumsBus, "spinus.compressor");
    const int drumsClip = song.addEffect (drumsBus, "bundled.peakeater");
    const int reeseFilter = song.addEffect (reese.insert, "spinus.filter");
    const int bassDucker = song.addEffect (bassBus, "spinus.ducker");
    const int bassEq = song.addEffect (bassBus, "spinus.eq");
    const int musicEq = song.addEffect (musicBus, "spinus.eq");
    const int musicFilter = song.addEffect (musicBus, "spinus.filter");
    const int musicDucker = song.addEffect (musicBus, "spinus.ducker");
    const int padWidth = song.addEffect (pad.insert, "spinus.stereo-utility");
    const int leadComp = song.addEffect (lead.insert, "spinus.compressor");
    const int hallReverb = song.addEffect (hall, "bundled.dragonfly.hall");
    const int hallEq = song.addEffect (hall, "spinus.eq");
    const int plateReverb = song.addEffect (plate, "bundled.dragonfly.plate");
    const int delayFx = song.addEffect (delay, "bundled.surge-xt-effects");
    const int delayFilter = song.addEffect (delay, "spinus.filter");
    // The master: EQ, glue compressor, loudness meter, limiter.
    const int masterEq = song.addEffect (master, "spinus.eq");
    const int masterComp = song.addEffect (master, "spinus.compressor");
    song.addEffect (master, "spinus.loudness-meter");
    const int masterLimiter = song.addEffect (master, "spinus.limiter");

    // The bass and the music duck under the kick layer (it plays in the drops only).
    c.run ("insert.setSidechain", object ({ { "insert", bassBus }, { "source", kickLayer.insert } }));
    c.run ("insert.setSidechain", object ({ { "insert", musicBus }, { "source", kickLayer.insert } }));

    // Sends.
    for (auto [from, level] : std::vector<std::pair<int, double>> { { pad.insert, -12 },
                                                                    { keys.insert, -10 },
                                                                    { pluck.insert, -9 },
                                                                    { lead.insert, -8 },
                                                                    { riser.insert, -8 } })
        song.send (from, hall, level);
    song.send (brk.insert, plate, -16);
    song.send (keys.insert, plate, -18);
    for (auto [from, level] :
         std::vector<std::pair<int, double>> { { lead.insert, -11 }, { pluck.insert, -12 }, { keys.insert, -20 } })
        song.send (from, delay, level);
    c.run ("insert.set", object ({ { "insert", hall }, { "volumeDb", -5.0 } }));
    c.run ("insert.set", object ({ { "insert", plate }, { "volumeDb", -8.0 } }));
    c.run ("insert.set", object ({ { "insert", delay }, { "volumeDb", -9.0 } }));
    c.run ("insert.set", object ({ { "insert", musicBus }, { "stereoWidth", 1.15 } }));

    // Settings (by the values the devices show).
    song.syncDevices();
    setTexts (song, breakFilter, { { "type", "Low pass" }, { "cutoff", "20000 Hz" }, { "resonance", "10%" } });
    setTexts (song, breakEq, { { "band1_on", "On" }, { "band1_freq", "60 Hz" }, { "band7_gain", "3.0" } });
    setTexts (song, kickEq, { { "band1_on", "On" }, { "band1_freq", "30 Hz" }, { "band6_gain", "-4.0" } });
    setTexts (song, drumsComp,
              { { "threshold", "-16" }, { "ratio", "3" }, { "attack", "20" }, { "release", "90" }, { "makeup", "2" } });
    setTexts (song, reeseFilter, { { "type", "Low pass" }, { "cutoff", "900 Hz" }, { "resonance", "25%" } });
    setTexts (song, bassDucker, { { "amount", "45%" }, { "release", "120 ms" } });
    setTexts (song, bassEq, { { "band1_on", "On" }, { "band1_freq", "28 Hz" } });
    setTexts (song, musicEq, { { "band1_on", "On" }, { "band1_freq", "140 Hz" }, { "band7_gain", "1.5" } });
    setTexts (song, musicFilter, { { "type", "High pass" }, { "cutoff", "20 Hz" }, { "resonance", "15%" } });
    setTexts (song, musicDucker, { { "amount", "20%" }, { "release", "140 ms" } });
    setTexts (song, leadComp, { { "threshold", "-18" }, { "ratio", "3" }, { "attack", "8" } });
    setTexts (song, hallReverb, { { "0", "0" }, { "1", "10" }, { "2", "80" }, { "3", "36" }, { "15", "3.2" } });
    setTexts (song, hallEq, { { "band1_on", "On" }, { "band1_freq", "220 Hz" } });
    setTexts (song, plateReverb, { { "0", "0" }, { "1", "80" }, { "5", "0.9" } });
    setTexts (song, delayFilter, { { "type", "Low pass" }, { "cutoff", "3500 Hz" }, { "resonance", "10%" } });
    setTexts (song, masterEq, { { "band1_on", "On" }, { "band1_freq", "25 Hz" } });
    setTexts (song, masterComp, { { "threshold", "-12" }, { "ratio", "2" }, { "attack", "30" }, { "release", "150" } });
    // The song's true peak: the Limiter holds it at the ceiling (BS.1770 meters and ffmpeg read -2.3 dBTP).
    setTexts (song, masterLimiter, { { "ceiling", "-2.3" }, { "input", "0.5" } });
    setTexts (song, drumsClip, { { "0", "1.5" }, { "2", "-1.0" } }); // clips the snare's peaks a little
    setTexts (song, kickLayer.device, { { "pad1_decay", "220 ms" }, { "pad1_tune", "-2 st" } });
    setTexts (song, topsCh.device, { { "pad15_volume", "-5.0 dB" } });
    setTexts (song, padWidth, { { "width", "130%" } });
    juce::ignoreUnused (delayFx);

    // Copy to insert: the music bus's EQ (low cut and air), with its settings, onto the lead's insert, as the
    // Mixer's "Copy to insert" does.
    {
        const model::DeviceId id { (uint32_t)musicEq };
        model::Device copy = *session.getProject().findDevice (id);
        for (auto& [key, value] : session.getAdapter().readParameters (id))
            copy.parameters[key] = value;
        c.run ("insert.addEffect", object ({ { "insert", lead.insert }, { "device", model::deviceToJson (copy) } }));
        song.effects.push_back ({ lead.insert, "spinus.eq (copied)" });
    }

    loadSurge (song, reese, "patches_3rdparty/Giana Brotherz/Basses/Deep Jungler");
    loadSurge (song, pad, "patches_3rdparty/Cybersoda/Pads/Lush");
    loadSurge (song, keys, "patches_3rdparty/Bluelight/Keys/Treated Rhodes");
    loadSurge (song, pluck, "patches_factory/Plucks/Nice Pluck 2");
    loadSurge (song, lead, "patches_factory/Leads/Sine Lead");
    for (int i = 0; i < 40; ++i) // Surge XT finishes loading patches on its own thread
        settle (50);
    song.syncDevices();
    loadBreak (song, brk, audio.getChildFile ("Sodium Break 174bpm.wav"));

    //--------------------------------------------------------------------------
    // Patterns.
    // The break as recorded, written by the Beat Slicer's "Slices to pattern" into a two-bar pattern.
    const int pBreak = song.addPattern ("Break Original", 2, drumsColour);
    session.setCurrentPattern (model::PatternId { (uint32_t)pBreak });
    if (auto r = slicesToPattern (session, model::ChannelId { (uint32_t)brk.channel }); r.failed())
    {
        std::cerr << "Slices to pattern: " << r.getErrorMessage() << "\n";
        return 1;
    }
    ++c.count;
    const auto drumPattern = [&] (const juce::String& name, int bars, const std::vector<Hit>& hits, bool withKick)
    {
        const int id = song.addPattern (name, bars, drumsColour);
        Notes slices, kicks;
        breakNotes (hits, slices, kicks);
        song.setNotes (id, brk, slices);
        if (withKick)
            song.setNotes (id, kickLayer, kicks);
        return id;
    };
    const int pChopA = drumPattern ("Break Chop A", 2, chopA(), true);
    const int pChopB = drumPattern ("Break Chop B", 2, chopB(), true);
    const int pFill = drumPattern ("Break Fill", 2, fill(), true);
    const int pHalf = drumPattern ("Break Half Time", 2, halftime(), false);
    const int pRoll = drumPattern ("Snare Roll", 8, snareRoll(), false);
    c.run ("pattern.setSwing", object ({ { "pattern", pHalf }, { "swing", 0.12 } }));
    const int pTops = song.addPattern ("Tops", 2, drumsColour);
    song.setNotes (pTops, topsCh, tops (true));
    const int pShaker = song.addPattern ("Shaker", 2, drumsColour);
    song.setNotes (pShaker, topsCh, tops (false));
    c.run ("pattern.setSwing", object ({ { "pattern", pShaker }, { "swing", 0.15 } }));
    const int pCrash = song.addPattern ("Crash", 1, drumsColour);
    song.setNotes (pCrash, topsCh, { { 0, 4, crash, 0.8 } });

    const int pBass = song.addPattern ("Bass", 8, bassColour);
    song.setNotes (pBass, sub, subLine (mainRoots));
    song.setNotes (pBass, reese, reeseLine (mainRoots, false));
    const int pBass2 = song.addPattern ("Bass Busy", 8, bassColour);
    song.setNotes (pBass2, sub, subLine (mainRoots));
    song.setNotes (pBass2, reese, reeseLine (mainRoots, true));
    const int pSubBreak = song.addPattern ("Sub Breakdown", 8, bassColour);
    song.setNotes (pSubBreak, sub, subPedal (breakRoots));
    const int pSubMain = song.addPattern ("Sub Long", 8, bassColour);
    song.setNotes (pSubMain, sub, subPedal (mainRoots));

    const int pPad = song.addPattern ("Pad", 8, padColour);
    song.setNotes (pPad, pad, padChords (mainChords, mainRoots, 0.62));
    const int pPadBreak = song.addPattern ("Pad Breakdown", 8, padColour);
    song.setNotes (pPadBreak, pad, padChords (breakChords, breakRoots, 0.6));
    const int pKeys = song.addPattern ("Rhodes", 8, keysColour);
    song.setNotes (pKeys, keys, rhodes (mainChords, 0.62, random));
    const int pKeysBreak = song.addPattern ("Rhodes Breakdown", 8, keysColour);
    song.setNotes (pKeysBreak, keys, rhodes (breakChords, 0.55, random, true));
    const int pKeysIntro = song.addPattern ("Rhodes Intro", 8, keysColour);
    song.setNotes (pKeysIntro, keys, rhodes (mainChords, 0.5, random, true));
    const int pArp = song.addPattern ("Pluck Arp", 8, keysColour);
    song.setNotes (pArp, pluck, pluckArp (mainChords, 0.55));

    const int pLeadA = song.addPattern ("Lead A", 8, leadColour);
    song.setNotes (pLeadA, lead, leadPhrase (false));
    const int pLeadB = song.addPattern ("Lead B", 8, leadColour);
    song.setNotes (pLeadB, lead, leadPhrase (true));
    const int pLeadHigh = song.addPattern ("Lead B High", 8, leadColour);
    song.setNotes (pLeadHigh, lead, leadPhrase (true, 12));
    const int pLeadBreak = song.addPattern ("Lead Breakdown", 8, leadColour);
    song.setNotes (pLeadBreak, lead, leadBreakdown());

    const int pRiser8 = song.addPattern ("Riser 8 bars", 8, fxColour);
    song.setNotes (pRiser8, riser, { { 0, 32, 60, 0.8 } });
    const int pRiser4 = song.addPattern ("Riser 4 bars", 4, fxColour);
    song.setNotes (pRiser4, riser, { { 0, 16, 60, 0.7 } });

    //--------------------------------------------------------------------------
    // The Playlist: tracks in groups (a group's first track holds the others), markers per section.
    const auto colourLane = [&] (int lane, const juce::String& colour)
    { c.run ("lane.set", object ({ { "lane", lane }, { "colour", colour } })); };
    const auto groupUnder = [&] (int lane, int parent)
    { c.run ("lane.set", object ({ { "lane", lane }, { "group", parent } })); };

    const int lDrums = song.addLane ("Drums"), lBreak = song.addLane ("Break"), lTops = song.addLane ("Tops"),
              lCrash = song.addLane ("Crashes"), laBreakFilter = song.addLane ("Break Filter");
    const int lBassGroup = song.addLane ("Bass"), lBass = song.addLane ("Bass Line"),
              laReese = song.addLane ("Reese Cutoff"), laReeseFilter = song.addLane ("Reese Filter"),
              laSub = song.addLane ("Sub Volume");
    const int lMusic = song.addLane ("Music"), lPad = song.addLane ("Pad"), lKeys = song.addLane ("Rhodes"),
              lArp = song.addLane ("Pluck Arp"), laMusicHp = song.addLane ("Music HP");
    const int lLeadGroup = song.addLane ("Lead"), lLead = song.addLane ("Lead Melody"),
              laLead = song.addLane ("Lead Volume");
    const int lFx = song.addLane ("FX"), lRiser = song.addLane ("Risers"), lImpacts = song.addLane ("Impacts (audio)"),
              lCrackle = song.addLane ("Crackle (audio)"), laHall = song.addLane ("Hall Decay");
    for (auto [group, colour, members] : std::vector<std::tuple<int, juce::String, std::vector<int>>> {
             { lDrums, drumsColour, { lBreak, lTops, lCrash, laBreakFilter } },
             { lBassGroup, bassColour, { lBass, laReese, laReeseFilter, laSub } },
             { lMusic, padColour, { lPad, lKeys, lArp, laMusicHp } },
             { lLeadGroup, leadColour, { lLead, laLead } },
             { lFx, fxColour, { lRiser, lImpacts, lCrackle, laHall } } })
    {
        colourLane (group, colour);
        for (auto m : members)
        {
            colourLane (m, colour);
            groupUnder (m, group);
        }
    }

    for (auto& s : sections)
        c.run ("marker.add", object ({ { "at", s.startBar * bar }, { "name", s.name } }));

    const auto drumsFor = [&] (int from, int to)
    {
        for (int b = from; b < to; b += 2)
        {
            const int i = ((b - from) / 2) % 4;
            song.place (lBreak, i == 3 ? pFill : (i == 1 ? pChopB : pChopA), b, 2);
            song.place (lTops, pTops, b, 2);
        }
    };

    // Intro 0-16: the pad and crackle, then the Rhodes and the break behind a closed filter.
    song.place (lPad, pPad, 0, 8);
    song.place (lPad, pPad, 8, 8);
    song.place (lKeys, pKeysIntro, 8, 8);
    for (int b = 8; b < 16; b += 2)
        song.place (lBreak, pBreak, b, 2);
    song.place (lTops, pShaker, 12, 2);
    song.place (lTops, pShaker, 14, 2);
    song.place (lBass, pSubMain, 8, 8);
    song.place (lRiser, pRiser4, 12, 4);
    // Drop 16-48.
    drumsFor (16, 48);
    for (int b = 16; b < 48; b += 8)
    {
        song.place (lBass, b >= 32 ? pBass2 : pBass, b, 8);
        song.place (lPad, pPad, b, 8);
        song.place (lKeys, pKeys, b, 8);
    }
    song.place (lLead, pLeadA, 16, 8);
    song.place (lLead, pLeadB, 24, 8);
    song.place (lLead, pLeadA, 32, 8);
    song.place (lLead, pLeadB, 40, 8);
    song.place (lArp, pArp, 32, 8);
    song.place (lArp, pArp, 40, 8);
    song.place (lCrash, pCrash, 16, 1);
    song.place (lCrash, pCrash, 32, 1);
    // Breakdown 48-64: the drums drop out; half time from bar 56.
    song.place (lPad, pPadBreak, 48, 8);
    song.place (lPad, pPadBreak, 56, 8);
    song.place (lKeys, pKeysBreak, 48, 8);
    song.place (lKeys, pKeysBreak, 56, 8);
    song.place (lBass, pSubBreak, 48, 8);
    song.place (lBass, pSubBreak, 56, 8);
    song.place (lLead, pLeadBreak, 56, 8);
    for (int b = 56; b < 64; b += 2)
    {
        song.place (lBreak, pHalf, b, 2);
        song.place (lTops, pShaker, b, 2);
    }
    song.place (lCrash, pCrash, 48, 1);
    // Build 64-72: the snare roll, the riser, the music thinning out through a high pass.
    song.place (lBreak, pRoll, 64, 8);
    song.place (lPad, pPad, 64, 8);
    song.place (lKeys, pKeysIntro, 64, 8);
    song.place (lArp, pArp, 64, 8);
    song.place (lBass, pSubMain, 64, 8);
    song.place (lRiser, pRiser8, 64, 8);
    // Drop 2 72-104: everything, the lead an octave up for its second half.
    drumsFor (72, 104);
    for (int b = 72; b < 104; b += 8)
    {
        song.place (lBass, b % 16 == 8 ? pBass2 : pBass, b, 8);
        song.place (lPad, pPad, b, 8);
        song.place (lKeys, pKeys, b, 8);
        song.place (lArp, pArp, b, 8);
    }
    song.place (lLead, pLeadA, 72, 8);
    song.place (lLead, pLeadB, 80, 8);
    song.place (lLead, pLeadA, 88, 8);
    song.place (lLead, pLeadHigh, 96, 8);
    song.place (lCrash, pCrash, 72, 1);
    song.place (lCrash, pCrash, 88, 1);
    // Outro 104-120: the drums for eight bars behind a closing filter, then the pad, Rhodes and crackle.
    for (int b = 104; b < 112; b += 2)
    {
        song.place (lBreak, b == 110 ? pFill : pChopA, b, 2);
        song.place (lTops, pShaker, b, 2);
    }
    song.place (lBass, pSubMain, 104, 8);
    song.place (lPad, pPad, 104, 8);
    song.place (lPad, pPad, 112, 8);
    song.place (lKeys, pKeys, 104, 8);
    song.place (lKeys, pKeysIntro, 112, 8);
    song.place (lArp, pArp, 104, 8);
    song.place (lCrash, pCrash, 104, 1);

    // Audio clips: crackle in the quiet parts, an impact on each drop and a reversed cymbal into it.
    {
        const auto file = [&] (const char* name) { return audio.getChildFile (name).getFullPathName(); };
        int crackleChannel = 0, impactChannel = 0, reverseChannel = 0;
        const auto clip = [&] (const char* name, const char* channelName, int& channel, int startBar, int bars,
                               double gainDb, Ticks fadeIn = 0, Ticks fadeOut = 0)
        {
            auto params = object ({ { "lane", juce::String (name).contains ("Crackle") ? lCrackle : lImpacts },
                                    { "file", file (name) },
                                    { "start", startBar * bar },
                                    { "length", bars * bar },
                                    { "gainDb", gainDb } });
            auto* o = params.getDynamicObject();
            if (channel != 0)
                o->setProperty ("channel", channel);
            else
                o->setProperty ("name", channelName);
            if (fadeIn > 0)
                o->setProperty ("fadeIn", fadeIn);
            if (fadeOut > 0)
                o->setProperty ("fadeOut", fadeOut);
            const auto r = c.run ("clip.addAudio", params);
            if (channel == 0)
                channel = (int)r["channel"];
        };
        for (int b : { 0, 8, 48, 56, 104, 112 })
            clip ("Sodium Crackle.wav", "Crackle", crackleChannel, b, 8, -4.0, b == 0 ? 2 * bar : 0,
                  b == 112 ? 4 * bar : 0);
        for (int b : { 16, 72 })
            clip ("Sodium Impact.wav", "Impact", impactChannel, b, 2, -5.0);
        for (int b : { 14, 70 })
            clip ("Sodium Reverse Cymbal.wav", "Reverse Cymbal", reverseChannel, b, 2, -6.0);
        // Each sound has its own insert (made with its channel); they play into the FX bus.
        for (auto id : { crackleChannel, impactChannel, reverseChannel })
        {
            c.run ("channel.set", object ({ { "channel", id }, { "group", "FX" }, { "colour", fxColour } }));
            int insert = 0;
            for (auto& ch : session.getProject().channels)
                if ((int)ch.id.value == id)
                    insert = (int)ch.targetInsert.value;
            c.run ("insert.setOutput", object ({ { "insert", insert }, { "target", fxBus } }));
            c.run ("insert.set", object ({ { "insert", insert }, { "colour", fxColour } }));
            if (id == impactChannel)
                song.send (insert, hall, -10);
        }
    }

    //--------------------------------------------------------------------------
    // Automation clips.
    using P = Song::P;
    song.syncDevices();
    const auto v = [&] (int device, const char* id, const char* text) { return valueAt (song, device, id, text); };

    // The break opens up through the intro, and closes again in the outro.
    song.automate (laBreakFilter, 8, 8, Song::parameter (breakFilter, "cutoff"),
                   { { 0, v (breakFilter, "cutoff", "250 Hz"), 0.4 }, { 30, v (breakFilter, "cutoff", "20000 Hz") } });
    song.automate (laBreakFilter, 104, 8, Song::parameter (breakFilter, "cutoff"),
                   { { 0, v (breakFilter, "cutoff", "20000 Hz"), -0.4 }, { 32, v (breakFilter, "cutoff", "400 Hz") } });

    // The Reese breathes: its filter opens and closes over each eight bars, wider in the second drop.
    for (auto [start, bars, low, high] : std::vector<std::tuple<int, int, const char*, const char*>> {
             { 16, 32, "450 Hz", "1600 Hz" }, { 72, 32, "500 Hz", "3200 Hz" } })
    {
        std::vector<P> points;
        for (int b = 0; b < bars; b += 8)
        {
            points.push_back ({ b * 4.0, v (reeseFilter, "cutoff", low), 0.3 });
            points.push_back ({ b * 4.0 + 12, v (reeseFilter, "cutoff", high), -0.3 });
            points.push_back ({ b * 4.0 + 16, v (reeseFilter, "cutoff", high), 0.3 });
            points.push_back ({ b * 4.0 + 30, v (reeseFilter, "cutoff", low) });
        }
        song.automate (laReeseFilter, start, bars, Song::parameter (reeseFilter, "cutoff"), points);
    }
    // ... and Surge XT's own filter cutoff in the Reese patch (its scene A filter 1, the parameter at position 319)
    // moves slowly against it, a bar's worth of growl at the end of every four.
    {
        const double base = song.currentValue (reese.device, "319");
        for (int start : { 16, 72 })
        {
            std::vector<P> points;
            for (int b = 0; b < 32; b += 4)
            {
                points.push_back ({ b * 4.0, base });
                points.push_back ({ b * 4.0 + 12, base, 0.4 });
                points.push_back ({ b * 4.0 + 14, juce::jmin (1.0, base + (start == 72 ? 0.12 : 0.08)), -0.4 });
                points.push_back ({ b * 4.0 + 16, base });
            }
            song.automate (laReese, start, 32, Song::parameter (reese.device, "319"), points);
        }
    }

    // The build: the music bus thins out through a rising high pass, back to full on the drop.
    song.automate (laMusicHp, 64, 8, Song::parameter (musicFilter, "cutoff"),
                   { { 0, v (musicFilter, "cutoff", "20 Hz"), 0.5 },
                     { 31.75, v (musicFilter, "cutoff", "900 Hz") },
                     { 32, v (musicFilter, "cutoff", "20 Hz") } });

    // The sub steps back for the breakdown, and comes back in the build.
    song.automate (laSub, 48, 24, Song::channelVolume (sub),
                   { { 0, song.volumeValue (sub, "-11 dB") },
                     { 4, song.volumeValue (sub, "-17 dB") },
                     { 60, song.volumeValue (sub, "-17 dB") },
                     { 64, song.volumeValue (sub, "-11 dB") } });

    // The lead fades in at the first drop.
    song.automate (laLead, 16, 4, Song::channelVolume (lead),
                   { { 0, song.volumeValue (lead, "-24 dB") }, { 8, song.volumeValue (lead, "-13 dB") } });

    // The hall gets bigger for the breakdown.
    song.automate (laHall, 48, 16, Song::parameter (hallReverb, "15"),
                   { { 0, v (hallReverb, "15", "3.2") },
                     { 16, v (hallReverb, "15", "5.5"), -0.3 },
                     { 60, v (hallReverb, "15", "5.5") },
                     { 64, v (hallReverb, "15", "3.2") } });

    //--------------------------------------------------------------------------
    // The Launcher: one scene per section, for jamming the song's parts live.
    {
        c.run ("launcher.setQuantize", object ({ { "quantize", "bar" } }));
        std::vector<int> tracks;
        for (auto [name, colour] : std::vector<std::pair<const char*, juce::String>> { { "Drums", drumsColour },
                                                                                       { "Tops", drumsColour },
                                                                                       { "Bass", bassColour },
                                                                                       { "Pad", padColour },
                                                                                       { "Keys", keysColour },
                                                                                       { "Lead", leadColour } })
        {
            const auto id = (int)c.run ("launcher.addTrack", object ({ { "name", name } }))["track"];
            c.run ("launcher.setTrack", object ({ { "track", id }, { "colour", colour } }));
            tracks.push_back (id);
        }
        // Per scene: the pattern for each launcher track (0: empty).
        const std::vector<std::pair<const char*, std::vector<int>>> scenes {
            { "Intro", { pBreak, pShaker, pSubMain, pPad, pKeysIntro, 0 } },
            { "Drop", { pChopA, pTops, pBass, pPad, pKeys, pLeadA } },
            { "Drop B", { pChopB, pTops, pBass2, pPad, pArp, pLeadB } },
            { "Breakdown", { pHalf, pShaker, pSubBreak, pPadBreak, pKeysBreak, pLeadBreak } },
            { "Build", { pRoll, 0, pSubMain, pPad, pArp, 0 } },
            { "Drop 2", { pChopA, pTops, pBass2, pPad, pArp, pLeadHigh } },
            { "Outro", { pChopA, pShaker, 0, pPad, pKeysIntro, 0 } },
        };
        for (auto& [name, patterns] : scenes)
        {
            const auto scene = (int)c.run ("launcher.addScene", object ({ { "name", name } }))["scene"];
            for (size_t i = 0; i < patterns.size(); ++i)
                if (patterns[i] != 0)
                    c.run ("launcher.addPattern",
                           object ({ { "track", tracks[i] }, { "scene", scene }, { "pattern", patterns[i] } }));
        }
    }

    // Freeze the heaviest channel, the pad (Surge XT with a big unison patch): it plays from a recording.
    c.run ("channel.freeze", object ({ { "channel", pad.channel } }));
    session.setCurrentPattern (model::PatternId { (uint32_t)pChopA });

    song.syncDevices();
    for (int i = 0; i < 100; ++i) // the freeze records in the background
        settle (50);

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
    std::cout << "Made " << folder.getFullPathName() << " (" << c.count << " commands)\n"
              << "  channels " << project.channels.size() << ", inserts " << project.mixer.inserts.size() + 1
              << ", patterns " << project.patterns.size() << ", notes " << notes << ", lanes "
              << project.playlist.lanes.size() << ", clips " << clips << ", automation clips " << automation << " ("
              << song.automationPoints << " points), launcher " << project.launcher.tracks.size() << "x"
              << project.launcher.scenes.size() << " with " << project.launcher.clips.size() << " clips\n"
              << "  Surge XT patches: " << song.surgePatches.joinIntoString (", ") << "\n";
    for (auto& d : session.getAdapter().getMissingDevices())
        std::cout << "  MISSING device " << d.value << "\n";
    return 0;
}

//==============================================================================
/** Renders the saved song (as File > Export does) and prints its level by section. */
int render (Session& session, ProjectDocument& document, const juce::File& folder, const juce::File& wav)
{
    if (const auto opened = document.open (folder); opened.failed())
    {
        std::cerr << "Open failed: " << opened.getErrorMessage() << "\n";
        return 1;
    }
    session.getEdit().dispatchPendingUpdatesSynchronously();
    for (int i = 0; i < 40; ++i)
        settle (50);
    for (auto& d : session.getAdapter().getMissingDevices())
        std::cout << "  MISSING device " << d.value << "\n";

    const auto exportTo = [&] (const juce::File& file, bool stems)
    {
        engine::ExportSettings settings;
        settings.format = engine::ExportSettings::Format::wav24;
        settings.sampleRate = 48000.0;
        settings.tailSeconds = 3.0;
        settings.stems = stems;
        std::optional<engine::ExportJob::Result> result;
        auto job = session.createExportJob (file, settings, [&] (const engine::ExportJob::Result& r) { result = r; });
        job->start();
        while (! result)
            settle (20);
        job = nullptr;
        if (! result->ok)
            std::cerr << "Export failed: " << result->error << "\n";
        return *result;
    };
    const auto result = exportTo (wav, false);
    if (! result.ok)
        return 1;
    if (const auto stems = juce::SystemStats::getEnvironmentVariable ("DNB_STEMS", {}); stems.isNotEmpty())
        exportTo (juce::File (stems).getChildFile ("Mix.wav"), true);

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (result.files.front()));
    juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read (&audio, 0, (int)reader->lengthInSamples, 0, true, true);
    const auto rate = reader->sampleRate;
    const auto secondsPerBar = 4.0 * 60.0 / bpm;
    std::cout << "Rendered " << result.files.front().getFullPathName() << ", " << reader->lengthInSamples / rate
              << " s\n";
    for (auto& s : sections)
    {
        const auto st = showcase::statsOf (audio, (int64_t)(s.startBar * secondsPerBar * rate),
                                           (int64_t)((s.startBar + s.bars) * secondsPerBar * rate));
        std::cout << "  " << s.name << ": peak " << showcase::db (st.peak) << ", RMS " << showcase::db (st.rms)
                  << ", clipped " << st.clipped << "\n";
    }
    return 0;
}
//==============================================================================
/** Pictures of the opened song's Playlist, Channel Rack, Mixer and Launcher (drawn off screen), to check
    that it looks tidy. */
int shots (Session& session, ProjectDocument& document, const juce::File& folder, const juce::File& out)
{
    if (const auto opened = document.open (folder); opened.failed())
    {
        std::cerr << "Open failed: " << opened.getErrorMessage() << "\n";
        return 1;
    }
    session.getEdit().dispatchPendingUpdatesSynchronously();
    settle (500);
    ui::MainView view { session, &document };
    view.setSize (1600, 1000);
    out.createDirectory();
    for (auto& id : view.getWorkspace().getPanelIds())
    {
        view.getWorkspace().showPanel (id);
        settle (50);
        auto* panel = view.getWorkspace().getPanel (id);
        if (panel == nullptr)
            continue;
        panel->setBounds (0, 0, 1600, 1000);
        settle (50);
        const auto image = panel->createComponentSnapshot (panel->getLocalBounds());
        juce::FileOutputStream stream (out.getChildFile (id + ".png"));
        stream.setPosition (0);
        stream.truncate();
        juce::PNGImageFormat().writeImageToStream (image, stream);
        std::cout << "Wrote " << out.getChildFile (id + ".png").getFullPathName() << "\n";
    }
    return 0;
}
} // namespace dnb
