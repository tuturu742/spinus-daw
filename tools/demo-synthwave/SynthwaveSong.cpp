// "Neon Coast": the synthwave demo song (an original piece in D minor at 104 BPM, with a 6/8 bridge and a
// last chorus a tone up in E minor), made only through the Command API (each change an undoable command, as
// the UI makes them) on Spinus's own instruments and effects and the free open-source plugins that exist for
// both Linux and Windows (Surge XT, Surge XT Effects, OB-Xf, Dragonfly Reverb, PeakEater), then saved as a
// normal project. The project made by --make (without its rendered .wav) ships with Spinus as demo/Neon Coast
// (demo/README.md says how to make it again).
//
//   spinus_demo_synthwave --plugins <folder> --probe
//   spinus_demo_synthwave --plugins <folder> --make <project folder>
//   spinus_demo_synthwave --plugins <folder> --render <project folder> --wav <file> [--stems <folder>]
//   spinus_demo_synthwave --plugins <folder> --shots <project folder> --out <folder>   (pictures of its panels)
//
// <folder> holds the free plugins as Spinus downloads them (Surge XT.clap with SurgeXTData, OB-Xf.clap with
// its Patches folder beside it, peakeater.clap, the Dragonfly reverbs). Run it with HOME set to a scratch
// folder: Tracktion keeps its settings under ~/.config.
// Environment: SYNTHWAVE_AUDIO=<folder> with the song's three sound effects (made by `python3 make_audio.py
// <folder>`, which needs numpy); without it the song has no audio clips.

// The showcase song's tool (tools/showcase-song) has the helpers: Commands, object, settle, makeDevices,
// and showcase::Song (channels, effects, patterns, clips, automation, Surge XT patches) and
// showcase::makeStatesPortable. Its own main() is renamed so that this file has its own.
#define main spinusShowcaseSongMain
#include "ShowcaseSong.cpp"
#undef main

#include "model/NoteTools.h"
#include "session/InstrumentPresets.h"

namespace
{
namespace synthwave
{
using showcase::N;
using showcase::Notes;
using Ch = showcase::Song::Channel;
using P = showcase::Song::P;
using Song = showcase::Song;

constexpr double bpm = 104.0;
constexpr juce::int64 eighth68Bar = 2880; // one 6/8 bar in ticks

struct Section
{
    const char* name;
    int startBar, bars;
};
constexpr Section sections[] = { { "Intro", 0, 8 },
                                 { "Verse 1", 8, 8 },
                                 { "Pre-Chorus", 16, 4 },
                                 { "Chorus 1", 20, 8 },
                                 { "Verse 2", 28, 8 },
                                 { "Pre-Chorus 2", 36, 4 },
                                 { "Chorus 2", 40, 8 },
                                 { "Bridge (6/8)", 48, 6 },
                                 { "Solo", 54, 8 },
                                 { "Lift", 62, 2 },
                                 { "Last Chorus (E minor)", 64, 8 },
                                 { "Outro", 72, 8 } };
constexpr int songBars = 80;

//==============================================================================
// Harmony. Each chord: a pad voicing (around middle C) and the bass root (octave 2).
struct Chord
{
    std::vector<int> pad;
    int root;
};
const Chord Dm { { 57, 62, 65, 69 }, 38 }, Bb { { 58, 62, 65, 70 }, 34 }, F { { 57, 60, 65, 69 }, 41 },
    C { { 55, 60, 64, 67 }, 36 }, Gm7 { { 55, 58, 62, 65 }, 43 }, Bbmaj7 { { 57, 62, 65, 70 }, 34 },
    Csus4 { { 55, 60, 65, 67 }, 36 }, A { { 57, 61, 64, 69 }, 33 }, Asus4 { { 57, 62, 64, 69 }, 33 },
    DmOverC { { 57, 60, 62, 65 }, 36 }, B { { 59, 63, 66, 71 }, 35 };

Chord up (const Chord& c, int semitones)
{
    Chord t = c;
    for (auto& p : t.pad)
        p += semitones;
    t.root += semitones;
    return t;
}
using Progression = std::vector<Chord>;

const Progression verseChords { Dm, Bb, F, C, Dm, Bb, F, C };                     // i VI III VII
const Progression preChords { Gm7, Bbmaj7, C, Csus4 };                            // iv VI VII
const Progression chorusChords { Bb, F, C, Dm, Bb, F, C, A };                     // VI III VII i, ending on V
const Progression bridgeChords { Gm7, Bbmaj7, Dm, DmOverC, Bbmaj7, C, Asus4, A }; // one 6/8 bar each
const Progression liftChords { A, B };                                            // up a tone: B is E minor's V
const Progression outroChords { up (Dm, 2), up (Bb, 2), up (F, 2), up (C, 2),
                                up (Dm, 2), up (Bb, 2), up (C, 2), up (Dm, 2) }; // Em C G D Em C D Em

Progression transposed (const Progression& p, int semitones)
{
    Progression t;
    for (auto& c : p)
        t.push_back (up (c, semitones));
    return t;
}

//==============================================================================
// Notes: Spinus's model notes for the Piano Roll's own tools (arpeggiate, strum, chop), and back.
std::vector<model::Note> toModel (const Notes& notes)
{
    std::vector<model::Note> out;
    for (auto& n : notes)
    {
        model::Note m;
        m.start = (model::Ticks)std::llround (n.start * (double)beat);
        m.length = std::max<model::Ticks> (1, (model::Ticks)std::llround (n.length * (double)beat));
        m.pitch = n.pitch;
        m.velocity = (float)n.velocity;
        out.push_back (m);
    }
    return out;
}

Notes fromModel (const std::vector<model::Note>& notes)
{
    Notes out;
    for (auto& m : notes)
        out.push_back (
            { (double)m.start / (double)beat, (double)m.length / (double)beat, m.pitch, (double)m.velocity });
    return out;
}

Notes shifted (Notes notes, double beats, int semitones = 0)
{
    for (auto& n : notes)
    {
        n.start += beats;
        n.pitch += semitones;
    }
    return notes;
}

Notes& operator+= (Notes& a, const Notes& b)
{
    a.insert (a.end(), b.begin(), b.end());
    return a;
}

/** Block chords, one per bar (`beatsPerChord` beats each). */
Notes blockChords (const Progression& p, double beatsPerChord, double velocity, int transpose = 0, double gate = 0.97)
{
    Notes n;
    for (size_t i = 0; i < p.size(); ++i)
        for (auto pitch : p[i].pad)
            n.push_back ({ (double)i * beatsPerChord, beatsPerChord * gate, pitch + transpose, velocity });
    return n;
}

/** The arpeggiated bass: root and octave as one chord per bar, turned into sixteenths by the Piano Roll's
    Arpeggiate tool, then made a little staccato with accents on the beat (the pumping 80s bass). */
Notes arpBass (const Progression& p, double beatsPerChord = 4.0, double step = 0.25, double velocity = 0.75)
{
    Notes chords;
    for (size_t i = 0; i < p.size(); ++i)
    {
        chords.push_back ({ (double)i * beatsPerChord, beatsPerChord, p[i].root, velocity });
        chords.push_back ({ (double)i * beatsPerChord, beatsPerChord, p[i].root + 12, velocity });
    }
    auto notes = fromModel (model::noteTools::arpeggiate (toModel (chords), (model::Ticks)std::llround (step * beat)));
    for (auto& n : notes)
    {
        n.length = step * 0.8;
        const auto inBeat = std::fmod (n.start, 1.0);
        n.velocity = inBeat < 0.01 ? velocity + 0.15 : (std::abs (inBeat - 0.5) < 0.01 ? velocity : velocity - 0.12);
    }
    return notes;
}

/** A sparkling arpeggio: the pad chord an octave up (and its top note another octave up), in sixteenths,
    through the Arpeggiate tool. */
Notes pluckArp (const Progression& p, double velocity, int transpose = 12)
{
    Notes chords;
    for (size_t i = 0; i < p.size(); ++i)
    {
        for (auto pitch : p[i].pad)
            chords.push_back ({ (double)i * 4.0, 4.0, pitch + transpose, velocity });
        chords.push_back ({ (double)i * 4.0, 4.0, p[i].pad[1] + transpose + 12, velocity });
    }
    auto notes = fromModel (model::noteTools::arpeggiate (toModel (chords), step));
    for (auto& n : notes)
    {
        n.length = 0.2;
        n.velocity = std::fmod (n.start, 1.0) < 0.01 ? velocity + 0.12 : velocity;
    }
    return notes;
}

/** Chord stabs in a rhythm (start and length in beats, each bar), strummed a little by the Strum tool. */
Notes stabs (const Progression& p, const std::vector<std::pair<double, double>>& rhythm, double velocity,
             int transpose = 0)
{
    Notes n;
    for (size_t i = 0; i < p.size(); ++i)
        for (auto [at, length] : rhythm)
            for (auto pitch : p[i].pad)
                n.push_back (
                    { (double)i * 4.0 + at, length, pitch + transpose, at < 0.01 ? velocity + 0.1 : velocity });
    return fromModel (model::noteTools::strum (toModel (n), 12, true));
}

Notes subBass (const Progression& p, double beatsPerChord = 4.0, double velocity = 0.8)
{
    Notes n;
    for (size_t i = 0; i < p.size(); ++i)
        n.push_back ({ (double)i * beatsPerChord, beatsPerChord - 0.1, p[i].root, velocity });
    return n;
}

//==============================================================================
// Melodies (beats from the pattern's start). D minor; the last chorus plays the chorus a tone up.

/** The chorus hook, over Bb F C Dm | Bb F C A. */
Notes chorusLead (int transpose = 0)
{
    Notes n = {
        // Bb
        { 0, 0.5, 69 },
        { 0.5, 0.5, 70 },
        { 1, 1.5, 74 },
        { 2.5, 0.5, 72 },
        { 3, 1, 74 },
        // F
        { 4, 2.5, 77, 0.9 },
        { 6.5, 0.5, 76 },
        { 7, 1, 74 },
        // C
        { 8, 1.5, 76 },
        { 9.5, 0.5, 72 },
        { 10, 1, 67 },
        { 11, 0.5, 72 },
        { 11.5, 0.5, 74 },
        // Dm
        { 12, 3, 74, 0.85 },
        { 15, 0.5, 69 },
        { 15.5, 0.5, 72 },
        // Bb
        { 16, 0.5, 69 },
        { 16.5, 0.5, 70 },
        { 17, 1.5, 74 },
        { 18.5, 0.5, 72 },
        { 19, 1, 74 },
        // F
        { 20, 1.5, 77, 0.9 },
        { 21.5, 0.5, 79 },
        { 22, 2, 81, 0.95 },
        // C
        { 24, 1, 79 },
        { 25, 1, 76 },
        { 26, 1, 72 },
        { 27, 1, 76 },
        // A (V)
        { 28, 2, 76, 0.9 },
        { 30, 1.9, 73, 0.85 },
    };
    return shifted (n, 0, transpose);
}

/** The verse melody (a soft square lead), over Dm Bb F C twice. */
Notes verseLead()
{
    return {
        { 0.5, 0.5, 69 },  { 1, 1, 74 },      { 2, 0.5, 72 },   { 2.5, 1.5, 69 },  // Dm
        { 4.5, 0.5, 65 },  { 5, 0.5, 67 },    { 5.5, 1.5, 69 }, { 7, 1, 65 },      // Bb
        { 8.5, 0.5, 72 },  { 9, 1, 69 },      { 10, 0.5, 67 },  { 10.5, 1.5, 65 }, // F
        { 12, 1, 64 },     { 13, 1, 67 },     { 14, 2, 64 },                       // C
        { 16.5, 0.5, 69 }, { 17, 1, 74 },     { 18, 0.5, 72 },  { 18.5, 1.5, 69 }, // Dm
        { 20.5, 0.5, 74 }, { 21, 1, 77 },     { 22, 0.5, 76 },  { 22.5, 1.5, 74 }, // Bb
        { 24, 1.5, 72 },   { 25.5, 0.5, 69 }, { 26, 1, 72 },    { 27, 1, 74 },     // F
        { 28, 2, 76 },     { 30, 2, 67 },                                          // C
    };
}

/** The solo (OB-Xf), over Dm Bb F C twice: long bent notes, then faster runs. */
Notes soloLead()
{
    return {
        // Dm
        { 0, 1, 81, 0.9 },
        { 1, 0.5, 79 },
        { 1.5, 0.5, 77 },
        { 2, 1, 76 },
        { 3, 0.5, 77 },
        { 3.5, 0.5, 81 },
        // Bb
        { 4, 2, 82, 0.95 },
        { 6, 0.5, 81 },
        { 6.5, 0.5, 79 },
        { 7, 1, 77 },
        // F
        { 8, 0.25, 72 },
        { 8.25, 0.25, 74 },
        { 8.5, 0.25, 76 },
        { 8.75, 0.25, 77 },
        { 9, 1, 81 },
        { 10, 0.5, 84, 0.95 },
        { 10.5, 1.5, 81 },
        // C
        { 12, 1.5, 79 },
        { 13.5, 0.5, 76 },
        { 14, 2, 72 },
        // Dm
        { 16, 0.5, 74 },
        { 16.5, 0.5, 77 },
        { 17, 0.5, 81 },
        { 17.5, 0.5, 86, 0.95 },
        { 18, 2, 84 },
        // Bb
        { 20, 1, 86, 0.95 },
        { 21, 1, 84 },
        { 22, 1, 82 },
        { 23, 1, 81 },
        // F: a run up and down
        { 24, 0.25, 77 },
        { 24.25, 0.25, 79 },
        { 24.5, 0.25, 81 },
        { 24.75, 0.25, 84 },
        { 25, 0.25, 81 },
        { 25.25, 0.25, 79 },
        { 25.5, 0.25, 77 },
        { 25.75, 0.25, 76 },
        { 26, 2, 77, 0.9 },
        // C
        { 28, 1, 79 },
        { 29, 0.5, 81 },
        { 29.5, 0.5, 79 },
        { 30, 2, 76, 0.9 },
    };
}

/** The bridge's bell melody, in 6/8 (three beats a bar), over the bridge chords. */
Notes bridgeBell()
{
    return {
        { 0, 1.5, 74 },  { 1.5, 0.5, 72 },  { 2, 1, 70 },     // Gm7
        { 3, 1.5, 69 },  { 4.5, 1.5, 65 },                    // Bbmaj7
        { 6, 1, 69 },    { 7, 0.5, 70 },    { 7.5, 1.5, 72 }, // Dm
        { 9, 3, 69 },                                         // Dm/C
        { 12, 1.5, 74 }, { 13.5, 0.5, 77 }, { 14, 1, 76 },    // Bbmaj7
        { 15, 1.5, 72 }, { 16.5, 1.5, 67 },                   // C
        { 18, 1.5, 69 }, { 19.5, 1.5, 74 },                   // Asus4
        { 21, 3, 73 },                                        // A
    };
}

//==============================================================================
// Drums. The three Drum Machines (Spinus Synth Kit): Kick plays pad 1, Gated Snare pads 2-3 (snare, clap),
// Hats & Toms the rest. Pads by note: 60 kick, 61 snare, 62 clap, 63 closed hat, 64 open hat, 65-67 toms,
// 70 crash, 71 shaker.
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
    crash = 70,
    shaker
};

struct Beat
{
    Notes kicks, snares, hats, thump;
};

void backbeat (Notes& s, double barStart, double velocity = 0.85)
{
    for (double b : { 1.0, 3.0 })
    {
        s.push_back ({ barStart + b, 0.25, snare, velocity });
        s.push_back ({ barStart + b, 0.25, clap, velocity - 0.15 });
    }
}

void eighthHats (Notes& h, double barStart, double velocity = 0.7)
{
    for (int e = 0; e < 8; ++e)
        h.push_back ({ barStart + e * 0.5, 0.2, closedHat, e % 2 == 0 ? velocity : velocity - 0.2 });
}

void tomFill (Beat& b, double from, double beats)
{
    const int toms[] = { highTom, highTom, midTom, midTom, lowTom, lowTom, lowTom, lowTom };
    const int steps = (int)(beats * 4);
    for (int i = 0; i < steps; ++i)
        b.hats.push_back ({ from + i * 0.25, 0.25, toms[(i * 8) / steps], 0.6 + 0.4 * i / steps });
}

Beat beatIntro()
{
    Beat b;
    for (int bar = 0; bar < 4; ++bar)
    {
        eighthHats (b.hats, bar * 4.0, 0.4 + 0.05 * bar);
        for (int e = 0; e < 8; ++e)
            b.hats.push_back ({ bar * 4.0 + e * 0.5 + 0.25, 0.2, shaker, 0.3 });
    }
    b.snares.push_back ({ 15.5, 0.25, snare, 0.7 });
    b.snares.push_back ({ 15.75, 0.25, snare, 0.85 });
    return b;
}

Beat beatVerse()
{
    Beat b;
    for (int bar = 0; bar < 4; ++bar)
    {
        const double s = bar * 4.0;
        b.kicks.push_back ({ s, 0.25, kick, 0.95 });
        b.kicks.push_back ({ s + 2, 0.25, kick, 0.9 });
        if (bar % 2 == 1)
            b.kicks.push_back ({ s + 2.5, 0.25, kick, 0.75 });
        backbeat (b.snares, s);
        eighthHats (b.hats, s);
    }
    b.hats.push_back ({ 15.5, 0.5, openHat, 0.5 });
    return b;
}

Beat beatChorus (bool fill)
{
    Beat b;
    for (int bar = 0; bar < 4; ++bar)
    {
        const double s = bar * 4.0;
        const bool filling = fill && bar == 3;
        for (int q = 0; q < 4; ++q)
        {
            b.kicks.push_back ({ s + q, 0.25, kick, q == 0 ? 1.0 : 0.92 });
            b.thump.push_back ({ s + q, 0.4, 36, 0.85 });
        }
        backbeat (b.snares, s, 0.95);
        for (int q = 0; q < (filling ? 2 : 4); ++q)
        {
            b.hats.push_back ({ s + q, 0.2, closedHat, 0.5 });
            b.hats.push_back ({ s + q + 0.25, 0.2, closedHat, 0.3 });
            b.hats.push_back ({ s + q + 0.5, 0.4, openHat, 0.45 });
            b.hats.push_back ({ s + q + 0.75, 0.2, closedHat, 0.35 });
        }
        if (filling)
            tomFill (b, s + 2, 2);
    }
    b.hats.push_back ({ 0, 2, crash, 0.8 });
    return b;
}

Beat beatPre()
{
    Beat b;
    for (int bar = 0; bar < 4; ++bar)
    {
        const double s = bar * 4.0;
        for (int q = 0; q < 4; q += (bar < 2 ? 2 : 1))
            b.kicks.push_back ({ s + q, 0.25, kick, 0.9 });
        if (bar < 3)
        {
            backbeat (b.snares, s, 0.8);
            eighthHats (b.hats, s, 0.5);
        }
    }
    // The last bar: a snare roll that grows, eighths then sixteenths.
    for (int i = 0; i < 4; ++i)
        b.snares.push_back ({ 12 + i * 0.5, 0.25, snare, 0.5 + 0.05 * i });
    for (int i = 0; i < 8; ++i)
        b.snares.push_back ({ 14 + i * 0.25, 0.25, snare, 0.7 + 0.035 * i });
    return b;
}

Beat beatLift()
{
    Beat b;
    for (int q = 0; q < 8; ++q)
        b.kicks.push_back ({ q * 1.0, 0.25, kick, 0.9 });
    for (int i = 0; i < 8; ++i)
        b.snares.push_back ({ i * 0.5, 0.25, snare, 0.55 + 0.03 * i });
    for (int i = 0; i < 8; ++i)
        b.snares.push_back ({ 4 + i * 0.25, 0.25, snare, 0.8 + 0.02 * i });
    tomFill (b, 6, 2);
    return b;
}

/** The bridge, in 6/8: a half-time feel, the gated snare on the fourth eighth of each bar. */
Beat beatBridge()
{
    Beat b;
    for (int bar = 0; bar < 8; ++bar)
    {
        const double s = bar * 3.0;
        b.kicks.push_back ({ s, 0.25, kick, 0.9 });
        if (bar % 2 == 1)
            b.kicks.push_back ({ s + 2.5, 0.25, kick, 0.7 });
        b.snares.push_back ({ s + 1.5, 0.25, snare, 0.9 });
        b.snares.push_back ({ s + 1.5, 0.25, clap, 0.7 });
        for (int e = 0; e < 6; ++e)
        {
            b.hats.push_back ({ s + e * 0.5, 0.2, closedHat, e % 3 == 0 ? 0.45 : 0.3 });
            b.hats.push_back ({ s + e * 0.5 + 0.25, 0.2, shaker, 0.25 });
        }
    }
    b.hats.push_back ({ 0, 2, crash, 0.6 });
    tomFill (b, 22.5, 1.5);
    return b;
}

Beat beatOutro()
{
    Beat b;
    for (int bar = 0; bar < 4; ++bar)
    {
        const double s = bar * 4.0;
        b.kicks.push_back ({ s, 0.25, kick, 0.9 });
        b.kicks.push_back ({ s + 2, 0.25, kick, 0.85 });
        backbeat (b.snares, s, 0.75);
        eighthHats (b.hats, s, 0.45);
    }
    b.hats.push_back ({ 0, 2, crash, 0.7 });
    return b;
}

//==============================================================================
/** Loads one of OB-Xf's factory patches (from the Patches folder beside OB-Xf.clap, as Spinus downloads it):
    OB-Xf does not load presets for a host, so its patch is given as the plugin's state, as Spinus's closest
    preset does (InstrumentPresets). */
bool loadObxfPatch (Session& session, const Ch& ch, const juce::File& plugins, const juce::String& relativePath)
{
    const auto file = plugins.getChildFile ("Patches").getChildFile (relativePath + ".fxp");
    auto* plugin = dynamic_cast<tracktion::ExternalPlugin*> (
        session.getAdapter().getDevicePlugin (model::DeviceId { (uint32_t)ch.device }));
    auto* instance = plugin != nullptr ? plugin->getAudioPluginInstance() : nullptr;
    juce::MemoryBlock data;
    if (instance == nullptr || ! file.loadFileAsData (data))
    {
        std::cerr << "OB-Xf patch not loaded: " << relativePath << "\n";
        return false;
    }
    InstrumentPreset preset;
    preset.kind = InstrumentPreset::Kind::state;
    preset.name = file.getFileNameWithoutExtension();
    preset.state = InstrumentPresets::obxfStateFromPatch (data);
    if (const auto r = InstrumentPresets::load (*instance, preset); r.failed() || preset.state.isEmpty())
    {
        std::cerr << "OB-Xf patch " << relativePath << ": " << r.getErrorMessage() << "\n";
        return false;
    }
    return true;
}

int make (Session& session, ProjectDocument& document, const juce::File& folder, const juce::File& pluginFolder)
{
    if (folder.exists())
    {
        std::cerr << folder.getFullPathName() << " exists already: choose a new folder.\n";
        return 1;
    }
    showcase::Song song (session, pluginFolder);
    auto& c = song.c;

    c.run ("project.setTempo", object ({ { "bpm", bpm } }));
    c.run ("project.setKey", object ({ { "tonic", 2 }, { "scale", "minor" } })); // D minor (the Piano Roll's scale)

    // Colours by part: neon pink drums, cyan bass, purple synths, orange leads, grey effects.
    const juce::String drumsColour = "#FFFF3FA4", bassColour = "#FF2FD5E8", synthColour = "#FF9B5CFF",
                       padColour = "#FF6E59D9", arpColour = "#FF3F8CFF", leadColour = "#FFFF9A3C",
                       soloColour = "#FFFFD23F", fxColour = "#FFA0A8B8", returnColour = "#FF4FB6A8";

    //--------------------------------------------------------------------------
    // Channels.
    const Ch kickCh = song.addChannel ("Kick", "spinus.drum-machine", {}, -3.0, 0.0, "Drums", drumsColour);
    const Ch snareCh = song.addChannel ("Gated Snare", "spinus.drum-machine", {}, -5.0, 0.0, "Drums", drumsColour);
    const Ch hatsCh = song.addChannel ("Hats & Toms", "spinus.drum-machine", {}, -4.0, 0.1, "Drums", drumsColour);
    const Ch thumpCh = song.addChannel ("Kick Thump", "spinus.3x-osc", "Thump", -7.0, 0.0, "Drums", drumsColour);
    const Ch arpBassCh = song.addChannel ("Arp Bass", "spinus.3x-osc", "Pluck Bass", -5.0, 0.0, "Bass", bassColour);
    const Ch subCh = song.addChannel ("Sub Bass", "spinus.3x-osc", "Sub Bass", -12.0, 0.0, "Bass", bassColour);
    const Ch padCh = song.addChannel ("Poly Pad (OB-Xf)", "bundled.ob-xf", {}, -8.0, 0.0, "Synths", padColour);
    const Ch warmPadCh = song.addChannel ("Warm Pad", "spinus.3x-osc", "Warm Pad", -15.0, 0.0, "Synths", padColour);
    const Ch stringsCh = song.addChannel ("String Pad", "spinus.3x-osc", "String Pad", -14.0, 0.0, "Synths", padColour);
    const Ch jupiterCh =
        song.addChannel ("Jupiter Chords (Surge XT)", "bundled.surge-xt", {}, -12.0, -0.15, "Synths", synthColour);
    const Ch brassCh =
        song.addChannel ("Jump Brass (Surge XT)", "bundled.surge-xt", {}, -12.0, 0.1, "Synths", synthColour);
    const Ch arpCh = song.addChannel ("Pluck Arp", "spinus.3x-osc", "Pluck", -12.0, 0.0, "Synths", arpColour);
    const Ch bellCh = song.addChannel ("Bell", "spinus.3x-osc", "Bell", -11.0, 0.2, "Synths", arpColour);
    const Ch leadCh = song.addChannel ("Lead (Surge XT)", "bundled.surge-xt", {}, -11.0, 0.0, "Leads", leadColour);
    const Ch verseLeadCh =
        song.addChannel ("Verse Lead", "spinus.3x-osc", "Square Lead", -14.0, -0.1, "Leads", leadColour);
    const Ch soloCh = song.addChannel ("Solo Lead (OB-Xf)", "bundled.ob-xf", {}, -9.0, 0.0, "Leads", soloColour);
    const Ch riserCh = song.addChannel ("Noise Riser", "spinus.3x-osc", "Noise Riser", -16.0, 0.0, "FX", fxColour);

    //--------------------------------------------------------------------------
    // Mixer: buses, returns, effects.
    const int drumsBus =
        song.addBus ("Drums Bus", { kickCh.insert, snareCh.insert, hatsCh.insert, thumpCh.insert }, drumsColour);
    const int bassBus = song.addBus ("Bass Bus", { arpBassCh.insert, subCh.insert }, bassColour);
    const int synthBus = song.addBus ("Synth Bus",
                                      { padCh.insert, warmPadCh.insert, stringsCh.insert, jupiterCh.insert,
                                        brassCh.insert, arpCh.insert, bellCh.insert },
                                      synthColour);
    const int leadBus = song.addBus ("Lead Bus", { leadCh.insert, verseLeadCh.insert, soloCh.insert }, leadColour);
    const int fxBus = song.addBus ("FX Bus", { riserCh.insert }, fxColour);
    const int gatedVerb = song.addInsert ("Gated Reverb", returnColour);
    const int hall = song.addInsert ("Hall Reverb", returnColour);
    const int delay = song.addInsert ("Delay", returnColour);
    const int throwVerb = song.addInsert ("Reverb Throw", returnColour);
    constexpr int master = 1;

    // The gated snare: the snare's plate reverb, cut off by a gate that opens on the dry snare (its sidechain).
    const int snareEq = song.addEffect (snareCh.insert, "spinus.eq");
    const int gatedPlate = song.addEffect (gatedVerb, "bundled.dragonfly.plate");
    const int gate = song.addEffect (gatedVerb, "spinus.gate");
    const int gatedEq = song.addEffect (gatedVerb, "spinus.eq");
    c.run ("insert.setSidechain", object ({ { "insert", gatedVerb }, { "source", snareCh.insert } }));
    // Drums: EQ and glue compression.
    const int drumsEq = song.addEffect (drumsBus, "spinus.eq");
    const int drumsComp = song.addEffect (drumsBus, "spinus.compressor");
    const int drumsClip = song.addEffect (drumsBus, "bundled.peakeater"); // PeakEater shaves the loudest peaks
    // Bass: a compressor on the kick's sidechain (the pumping bass), and an EQ.
    const int bassComp = song.addEffect (bassBus, "spinus.compressor");
    const int bassEq = song.addEffect (bassBus, "spinus.eq");
    c.run ("insert.setSidechain", object ({ { "insert", bassBus }, { "source", kickCh.insert } }));
    // Synths: the filter that sweeps open into each chorus, a ducker on the kick, an EQ; the pad wide.
    const int synthFilter = song.addEffect (synthBus, "spinus.filter");
    const int synthDucker = song.addEffect (synthBus, "spinus.ducker");
    const int synthEq = song.addEffect (synthBus, "spinus.eq");
    c.run ("insert.setSidechain", object ({ { "insert", synthBus }, { "source", kickCh.insert } }));
    const int padWidth = song.addEffect (padCh.insert, "spinus.stereo-utility");
    const int arpFilter = song.addEffect (arpCh.insert, "spinus.filter");
    // Leads: EQ, compressor.
    const int leadEq = song.addEffect (leadBus, "spinus.eq");
    const int leadComp = song.addEffect (leadBus, "spinus.compressor");
    // Returns.
    const int hallReverb = song.addEffect (hall, "bundled.dragonfly.hall");
    const int hallEq = song.addEffect (hall, "spinus.eq");
    const int delayFx = song.addEffect (delay, "bundled.surge-xt-effects");
    const int delayFilter = song.addEffect (delay, "spinus.filter");
    const int throwReverb = song.addEffect (throwVerb, "bundled.dragonfly.hall");
    const int fxEq = song.addEffect (fxBus, "spinus.eq");
    // Master: EQ, glue compressor, loudness meter, limiter.
    const int masterEq = song.addEffect (master, "spinus.eq");
    const int masterComp = song.addEffect (master, "spinus.compressor");
    song.addEffect (master, "spinus.loudness-meter");
    const int masterLimiter = song.addEffect (master, "spinus.limiter");

    // Sends.
    song.send (snareCh.insert, gatedVerb, 0.0);
    song.send (snareCh.insert, throwVerb, 0.0);
    for (auto [from, level] : std::vector<std::pair<int, double>> { { padCh.insert, -12 },
                                                                    { stringsCh.insert, -8 },
                                                                    { warmPadCh.insert, -10 },
                                                                    { jupiterCh.insert, -12 },
                                                                    { brassCh.insert, -14 },
                                                                    { arpCh.insert, -12 },
                                                                    { bellCh.insert, -6 },
                                                                    { leadCh.insert, -10 },
                                                                    { verseLeadCh.insert, -9 },
                                                                    { soloCh.insert, -9 },
                                                                    { hatsCh.insert, -18 },
                                                                    { riserCh.insert, -8 } })
        song.send (from, hall, level);
    for (auto [from, level] : std::vector<std::pair<int, double>> { { leadCh.insert, -12 },
                                                                    { soloCh.insert, -10 },
                                                                    { arpCh.insert, -14 },
                                                                    { bellCh.insert, -12 },
                                                                    { verseLeadCh.insert, -12 } })
        song.send (from, delay, level);
    song.send (leadCh.insert, throwVerb, 0.0);
    song.send (soloCh.insert, throwVerb, 0.0);
    c.run ("insert.set", object ({ { "insert", gatedVerb }, { "volumeDb", -3.0 } }));
    c.run ("insert.set", object ({ { "insert", hall }, { "volumeDb", -5.0 } }));
    c.run ("insert.set", object ({ { "insert", delay }, { "volumeDb", -2.0 } }));
    c.run ("insert.set", object ({ { "insert", throwVerb }, { "volumeDb", -60.0 } })); // opened by automation
    c.run ("insert.set", object ({ { "insert", synthBus }, { "stereoWidth", 1.15 } }));

    // Settings, by the values the devices show (a percentage that the device keeps as 0 to 1 is given as
    // 0 to 1: "0.35" for 35%), each checked by what the device shows afterwards.
    song.syncDevices();
    const auto setTexts = [&] (int device, std::initializer_list<std::pair<const char*, const char*>> values)
    {
        song.setTexts (device, values);
        for (auto& [id, text] : values)
            std::cout << "  set " << device << " " << id << " = " << text << " -> "
                      << session.getDeviceParameterValueText (model::DeviceId { (uint32_t)device }, id) << "\n";
    };
    setTexts (snareEq,
              { { "band1_on", "On" }, { "band1_freq", "120 Hz" }, { "band3_gain", "2.0" }, { "band7_gain", "2.5" } });
    setTexts (gatedPlate, { { "0", "0" }, { "1", "100" }, { "5", "2.2" }, { "6", "200" }, { "7", "11000" } });
    setTexts (gate, { { "sidechain", "Sidechain" },
                      { "threshold", "-34" },
                      { "hold", "190" },
                      { "release", "45" },
                      { "attack", "0.5" } });
    setTexts (gatedEq, { { "band1_on", "On" }, { "band1_freq", "180 Hz" } });
    setTexts (drumsEq,
              { { "band1_on", "On" }, { "band1_freq", "30 Hz" }, { "band2_gain", "2.0" }, { "band7_gain", "1.5" } });
    setTexts (
        drumsComp,
        { { "threshold", "-14" }, { "ratio", "3" }, { "attack", "20" }, { "release", "120" }, { "makeup", "2" } });
    setTexts (bassComp, { { "sidechain", "Sidechain" },
                          { "threshold", "-30" },
                          { "ratio", "6" },
                          { "attack", "2" },
                          { "release", "140" } });
    setTexts (bassEq, { { "band1_on", "On" }, { "band1_freq", "32 Hz" }, { "band4_gain", "-2.0" } });
    setTexts (synthFilter, { { "type", "Low pass" }, { "cutoff", "20000 Hz" }, { "resonance", "25%" } });
    setTexts (synthDucker, { { "amount", "30%" }, { "release", "220 ms" } });
    setTexts (synthEq, { { "band1_on", "On" }, { "band1_freq", "150 Hz" } });
    setTexts (padWidth, { { "width", "140%" } });
    setTexts (arpFilter, { { "type", "Low pass" }, { "cutoff", "5000 Hz" }, { "resonance", "30%" } });
    setTexts (drumsClip, { { "0", "1.5" }, { "1", "-1.5" } });
    setTexts (leadEq, { { "band1_on", "On" }, { "band1_freq", "200 Hz" }, { "band6_gain", "1.5" } });
    setTexts (leadComp, { { "threshold", "-18" }, { "ratio", "3" }, { "attack", "8" }, { "release", "150" } });
    setTexts (hallReverb, { { "0", "0" }, { "1", "10" }, { "2", "90" }, { "3", "40" }, { "15", "3.4" } });
    setTexts (hallEq, { { "band1_on", "On" }, { "band1_freq", "250 Hz" } });
    setTexts (delayFilter, { { "type", "Low pass" }, { "cutoff", "4000 Hz" }, { "resonance", "10%" } });
    setTexts (throwReverb, { { "0", "0" }, { "1", "5" }, { "2", "100" }, { "3", "50" }, { "15", "6" } });
    setTexts (fxEq, { { "band1_on", "On" }, { "band1_freq", "120 Hz" } });
    setTexts (masterEq, { { "band1_on", "On" }, { "band1_freq", "25 Hz" }, { "band7_gain", "1.0" } });
    setTexts (masterComp, { { "threshold", "-12" }, { "ratio", "2" }, { "attack", "30" }, { "release", "200" } });
    setTexts (masterLimiter, { { "ceiling", "-2.3" }, { "input", "1.5" }, { "true_peak", "On" } });

    // The plugins' sounds: Surge XT and OB-Xf factory patches.
    song.loadSurgePatch (jupiterCh, "Polysynths/Jupiter-8");
    song.loadSurgePatch (brassCh, "Brass/OB-8 Jump");
    song.loadSurgePatch (leadCh, "Leads/Classic Lead 2");
    for (int i = 0; i < 40; ++i) // Surge XT finishes loading patches on its own thread
        settle (50);
    const bool padLoaded = loadObxfPatch (session, padCh, pluginFolder, "Pads/Dreaming Anew");
    const bool soloLoaded = loadObxfPatch (session, soloCh, pluginFolder, "Leads/Street Hawk");
    settle (200);
    song.syncDevices();
    // OB-Xf's vibrato: the solo's wheel-free vibrato comes from LFO 1 on both oscillators' pitch, slow and light.
    if (soloLoaded)
        song.setTexts (soloCh.device, { { "6", "12.0" } }); // a little glide between the solo's notes

    //--------------------------------------------------------------------------
    // Patterns. Most hold one part; the beats hold the three drum machines and the kick thump together.
    auto pattern = [&] (const juce::String& name, int bars, const juce::String& colour)
    { return song.addPattern (name, bars, colour); };
    auto fit = [&] (int p, juce::int64 ticks)
    { c.run ("pattern.setLength", object ({ { "pattern", p }, { "length", ticks } })); };
    auto beatPattern = [&] (const juce::String& name, int bars, const Beat& b)
    {
        const int p = pattern (name, bars, drumsColour);
        if (! b.kicks.empty())
            song.setNotes (p, kickCh, b.kicks);
        if (! b.snares.empty())
            song.setNotes (p, snareCh, b.snares);
        if (! b.hats.empty())
            song.setNotes (p, hatsCh, b.hats);
        if (! b.thump.empty())
            song.setNotes (p, thumpCh, b.thump);
        fit (p, bars * bar);
        return p;
    };
    auto onePart =
        [&] (const juce::String& name, int bars, const juce::String& colour, const Ch& ch, const Notes& notes)
    {
        const int p = pattern (name, bars, colour);
        song.setNotes (p, ch, notes);
        fit (p, bars * bar);
        return p;
    };

    const int pBeatIntro = beatPattern ("Beat Intro", 4, beatIntro());
    const int pBeatVerse = beatPattern ("Beat Verse", 4, beatVerse());
    const int pBeatPre = beatPattern ("Beat Pre-Chorus", 4, beatPre());
    const int pBeatChorus = beatPattern ("Beat Chorus", 4, beatChorus (false));
    const int pBeatChorusFill = beatPattern ("Beat Chorus Fill", 4, beatChorus (true));
    const int pBeatLift = beatPattern ("Beat Lift", 2, beatLift());
    const int pBeatOutro = beatPattern ("Beat Outro", 4, beatOutro());
    c.run ("pattern.setSwing", object ({ { "pattern", pBeatIntro }, { "swing", 0.12 } }));

    const int pBassVerse = onePart ("Arp Bass Verse", 8, bassColour, arpBassCh, arpBass (verseChords));
    const int pBassPre = onePart ("Arp Bass Pre-Chorus", 4, bassColour, arpBassCh, arpBass (preChords));
    const int pBassChorus = onePart ("Arp Bass Chorus", 8, bassColour, arpBassCh, arpBass (chorusChords, 4, 0.25, 0.8));
    const int pBassLift = onePart ("Arp Bass Lift", 2, bassColour, arpBassCh, arpBass (liftChords, 4, 0.25, 0.8));
    const int pBassOutro = onePart ("Arp Bass Outro", 8, bassColour, arpBassCh, arpBass (outroChords, 4, 0.25, 0.7));
    const int pSubChorus = onePart ("Sub Chorus", 8, bassColour, subCh, subBass (chorusChords));
    const int pSubVerse = onePart ("Sub Verse", 8, bassColour, subCh, subBass (verseChords));

    const int pPadIntro = onePart ("Pad Intro", 8, padColour, padCh, blockChords (verseChords, 4, 0.6));
    const int pPadVerse = pattern ("Pads Verse", 8, padColour);
    song.setNotes (pPadVerse, padCh, blockChords (verseChords, 4, 0.55));
    song.setNotes (pPadVerse, warmPadCh, blockChords (verseChords, 4, 0.5, -12));
    fit (pPadVerse, 8 * bar);
    const int pPadPre = onePart ("Pad Pre-Chorus", 4, padColour, padCh, blockChords (preChords, 4, 0.65));
    const int pPadChorus = pattern ("Pads Chorus", 8, padColour);
    song.setNotes (pPadChorus, padCh, blockChords (chorusChords, 4, 0.7));
    song.setNotes (pPadChorus, stringsCh, blockChords (chorusChords, 4, 0.55, 12));
    fit (pPadChorus, 8 * bar);
    const int pPadLift = onePart ("Pad Lift", 2, padColour, padCh, blockChords (liftChords, 4, 0.7));
    const int pPadOutro = pattern ("Pads Outro", 8, padColour);
    song.setNotes (pPadOutro, padCh, blockChords (outroChords, 4, 0.55));
    song.setNotes (pPadOutro, warmPadCh, blockChords (outroChords, 4, 0.45, -12));
    fit (pPadOutro, 8 * bar);

    const std::vector<std::pair<double, double>> jumpRhythm {
        { 0, 0.35 }, { 0.75, 0.35 }, { 1.5, 0.9 }, { 3, 0.35 }, { 3.5, 0.35 }
    };
    const int pBrassChorus =
        onePart ("Jump Brass Chorus", 8, synthColour, brassCh, stabs (chorusChords, jumpRhythm, 0.75));
    const int pJupiterVerse = onePart ("Jupiter Verse", 8, synthColour, jupiterCh,
                                       stabs (verseChords, { { 0, 1.4 }, { 1.5, 0.4 }, { 2.5, 1.4 } }, 0.55));
    const int pJupiterSolo =
        onePart ("Jupiter Solo", 8, synthColour, jupiterCh,
                 stabs (verseChords, { { 0, 0.4 }, { 0.75, 0.4 }, { 2, 0.4 }, { 2.75, 1.0 } }, 0.65));

    const int pArpIntro = onePart ("Arp Intro", 4, arpColour, arpCh, pluckArp ({ Dm, Bb, F, C }, 0.55));
    const int pArpVerse = onePart ("Arp Verse", 8, arpColour, arpCh, pluckArp (verseChords, 0.55));
    const int pArpPre = onePart ("Arp Pre-Chorus", 4, arpColour, arpCh, pluckArp (preChords, 0.6));
    const int pArpChorus = onePart ("Arp Chorus", 8, arpColour, arpCh, pluckArp (chorusChords, 0.6));
    const int pArpLift = onePart ("Arp Lift", 2, arpColour, arpCh, pluckArp (liftChords, 0.65));
    const int pArpOutro = onePart ("Arp Outro", 8, arpColour, arpCh, pluckArp (outroChords, 0.5));

    const int pLeadChorus = onePart ("Lead Chorus", 8, leadColour, leadCh, chorusLead());
    const int pVerseLead = onePart ("Verse Lead", 8, leadColour, verseLeadCh, verseLead());
    const int pSolo = onePart ("Solo", 8, soloColour, soloCh, soloLead());
    const int pBellOutro = onePart ("Bell Outro", 8, arpColour, bellCh, shifted (chorusLead (2), 0, -12));

    const int pRiser4 = onePart ("Riser 4 bars", 4, fxColour, riserCh, { { 0, 15.9, 60, 0.9 } });
    const int pRiser2 = onePart ("Riser 2 bars", 2, fxColour, riserCh, { { 0, 7.9, 60, 0.9 } });

    // The bridge: its own time signature, 6/8 (eight bars of 6/8 = six of the song's 4/4 bars).
    auto pattern68 = [&] (const juce::String& name, const juce::String& colour)
    {
        const int p = pattern (name, 6, colour);
        c.run ("pattern.setTimeSignature", object ({ { "pattern", p }, { "numerator", 6 }, { "denominator", 8 } }));
        return p;
    };
    const int pBridgeBeat = pattern68 ("Beat Bridge (6/8)", drumsColour);
    {
        const auto b = beatBridge();
        song.setNotes (pBridgeBeat, kickCh, b.kicks);
        song.setNotes (pBridgeBeat, snareCh, b.snares);
        song.setNotes (pBridgeBeat, hatsCh, b.hats);
    }
    const int pBridgePads = pattern68 ("Pads Bridge (6/8)", padColour);
    song.setNotes (pBridgePads, padCh, blockChords (bridgeChords, 3, 0.6));
    song.setNotes (pBridgePads, stringsCh, blockChords (bridgeChords, 3, 0.6, 12));
    const int pBridgeBass = pattern68 ("Bass Bridge (6/8)", bassColour);
    song.setNotes (pBridgeBass, subCh, subBass (bridgeChords, 3));
    song.setNotes (pBridgeBass, arpBassCh, arpBass (bridgeChords, 3, 0.5, 0.65)); // eighths: the 6/8 lilt
    const int pBridgeBell = pattern68 ("Bell Bridge (6/8)", arpColour);
    song.setNotes (pBridgeBell, bellCh, bridgeBell());
    for (int p : { pBridgeBeat, pBridgePads, pBridgeBass, pBridgeBell })
        fit (p, 8 * eighth68Bar);

    //--------------------------------------------------------------------------
    // The Playlist: tracks in folding groups, each automation track (named for what it moves) under the track
    // it changes.
    struct Lane
    {
        int id;
    };
    auto lane = [&] (const juce::String& name, const juce::String& colour, int group = 0)
    {
        const int id = song.addLane (name);
        auto params = object ({ { "lane", id }, { "colour", colour } });
        if (group != 0)
            params.getDynamicObject()->setProperty ("group", group);
        c.run ("lane.set", params);
        return id;
    };
    const int lDrums = lane ("Drums", drumsColour);
    const int laGate = lane ("Gate Hold", drumsColour, lDrums);
    const int laDrumsVol = lane ("Drums Fade", drumsColour, lDrums);
    const int lBass = lane ("Arp Bass", bassColour);
    const int lSub = lane ("Sub Bass", bassColour, lBass);
    const int laBassCut = lane ("Bass Cutoff", bassColour, lBass);
    const int lPads = lane ("Pads", padColour);
    const int laSynthFilter = lane ("Synth Sweep", padColour, lPads);
    const int laPadWidth = lane ("Pad Width", padColour, lPads);
    const int lChords = lane ("Chords", synthColour, lPads);
    const int lArp = lane ("Arp", arpColour, lPads);
    const int laArpCut = lane ("Arp Cutoff", arpColour, lPads);
    const int laArpPan = lane ("Arp Pan", arpColour, lPads);
    const int lBell = lane ("Bell", arpColour, lPads);
    const int lLead = lane ("Lead", leadColour);
    const int laSwoop = lane ("Lead Pitch", leadColour, lLead);
    const int laLeadCut = lane ("Lead Cutoff", leadColour, lLead);
    const int lVerseLead = lane ("Verse Lead", leadColour, lLead);
    const int lSolo = lane ("Solo", soloColour, lLead);
    const int laSoloPitch = lane ("Solo Bends", soloColour, lLead);
    const int laSoloCut = lane ("Solo Cutoff", soloColour, lLead);
    const int laThrow = lane ("Verb Throws", returnColour, lLead);
    const int lFx = lane ("FX", fxColour);
    const int laRiser = lane ("Riser Volume", fxColour, lFx);
    const int laRiserCut = lane ("Riser Cutoff", fxColour, lFx);
    const int lWaves = lane ("Ocean Waves", fxColour, lFx);
    const int lHits = lane ("Cymbals & Hits", fxColour, lFx);
    const int laMaster = lane ("Master Fades", "#FFE0E0E0");
    const int laMasterEq = lane ("Master Air", "#FFE0E0E0", laMaster);
    const int laDelay = lane ("Delay Return", returnColour, laMaster);

    for (auto& s : sections)
        c.run ("marker.add", object ({ { "at", s.startBar * bar }, { "name", s.name } }));

    auto place = [&] (int l, int p, int startBar, int bars) { song.place (l, p, startBar, bars); };
    auto placeClip = [&] (int l, int p, int startBar, int bars)
    {
        return (int)c.run (
            "clip.addPattern",
            object (
                { { "lane", l }, { "pattern", p }, { "start", startBar * bar }, { "length", bars * bar } }))["clip"];
    };

    // Intro 0-8: waves, the pad, then the arp opening and the hats.
    place (lPads, pPadIntro, 0, 8);
    place (lArp, pArpIntro, 4, 4);
    place (lDrums, pBeatIntro, 4, 4);
    // Verse 1 8-16.
    place (lDrums, pBeatVerse, 8, 4);
    place (lDrums, pBeatVerse, 12, 4);
    place (lBass, pBassVerse, 8, 8);
    place (lPads, pPadVerse, 8, 8);
    place (lVerseLead, pVerseLead, 8, 8);
    // Pre-chorus 16-20 (and again 36-40).
    for (int at : { 16, 36 })
    {
        place (lDrums, pBeatPre, at, 4);
        place (lBass, pBassPre, at, 4);
        place (lPads, pPadPre, at, 4);
        place (lArp, pArpPre, at, 4);
        place (lFx, pRiser4, at, 4);
    }
    // Choruses 20-28, 40-48.
    for (int at : { 20, 40 })
    {
        place (lDrums, pBeatChorus, at, 4);
        place (lDrums, pBeatChorusFill, at + 4, 4);
        place (lBass, pBassChorus, at, 8);
        place (lSub, pSubChorus, at, 8);
        place (lPads, pPadChorus, at, 8);
        place (lChords, pBrassChorus, at, 8);
        place (lArp, pArpChorus, at, 8);
        place (lLead, pLeadChorus, at, 8);
    }
    // Verse 2 28-36: the verse with the arp and Jupiter chords; its lead clip made unique and varied.
    place (lDrums, pBeatVerse, 28, 4);
    place (lDrums, pBeatVerse, 32, 4);
    place (lBass, pBassVerse, 28, 8);
    place (lSub, pSubVerse, 28, 8);
    place (lPads, pPadVerse, 28, 8);
    place (lChords, pJupiterVerse, 28, 8);
    place (lArp, pArpVerse, 28, 8);
    {
        const int clip = placeClip (lVerseLead, pVerseLead, 28, 8);
        const int unique = (int)c.run ("clip.makeUnique", object ({ { "clip", clip } }))["pattern"];
        c.run ("pattern.set", object ({ { "pattern", unique }, { "name", "Verse Lead 2" } }));
        // The second time: the last four bars an octave up, with pickups into each bar.
        auto notes = verseLead();
        for (auto& n : notes)
            if (n.start >= 16)
                n.pitch += 12;
        notes +=
            Notes { { 3.5, 0.5, 72, 0.6 }, { 7.5, 0.5, 69, 0.6 }, { 11.5, 0.25, 62, 0.6 }, { 11.75, 0.25, 64, 0.6 } };
        song.setNotes (unique, verseLeadCh, notes);
        fit (unique, 8 * bar);
    }
    // Bridge 48-54, in 6/8.
    for (auto [l, p] : std::vector<std::pair<int, int>> {
             { lDrums, pBridgeBeat }, { lPads, pBridgePads }, { lBass, pBridgeBass }, { lBell, pBridgeBell } })
        place (l, p, 48, 6);
    // Solo 54-62.
    place (lDrums, pBeatChorus, 54, 4);
    place (lDrums, pBeatChorusFill, 58, 4);
    place (lBass, pBassVerse, 54, 8);
    place (lSub, pSubVerse, 54, 8);
    place (lPads, pPadVerse, 54, 8);
    place (lChords, pJupiterSolo, 54, 8);
    place (lSolo, pSolo, 54, 8);
    // Lift 62-64: A then B, to the new key.
    place (lDrums, pBeatLift, 62, 2);
    place (lBass, pBassLift, 62, 2);
    place (lPads, pPadLift, 62, 2);
    place (lArp, pArpLift, 62, 2);
    place (lFx, pRiser2, 62, 2);
    // Last chorus 64-72, a tone up: each chorus clip made unique, and its copy moved up to E minor.
    place (lDrums, pBeatChorus, 64, 4);
    place (lDrums, pBeatChorusFill, 68, 4);
    {
        struct Up
        {
            int lane, pattern;
            std::vector<std::pair<Ch, Notes>> parts;
        };
        const Progression lastChords = transposed (chorusChords, 2);
        Progression lastWithB = lastChords;
        lastWithB.back() = B;
        const std::vector<Up> ups {
            { lBass, pBassChorus, { { arpBassCh, arpBass (lastWithB, 4, 0.25, 0.8) } } },
            { lSub, pSubChorus, { { subCh, subBass (lastWithB) } } },
            { lPads,
              pPadChorus,
              { { padCh, blockChords (lastWithB, 4, 0.7) }, { stringsCh, blockChords (lastWithB, 4, 0.55, 12) } } },
            { lChords, pBrassChorus, { { brassCh, stabs (lastWithB, jumpRhythm, 0.8) } } },
            { lArp, pArpChorus, { { arpCh, pluckArp (lastWithB, 0.62) } } },
            { lLead,
              pLeadChorus,
              { { leadCh,
                  []
                  {
                      auto n = chorusLead (2);
                      n.back().pitch = 75;
                      return n;
                  }() } } },
        };
        for (auto& u : ups)
        {
            const int clip = placeClip (u.lane, u.pattern, 64, 8);
            const int unique = (int)c.run ("clip.makeUnique", object ({ { "clip", clip } }))["pattern"];
            for (auto& p : session.getProject().patterns)
                if ((int)p.id.value == u.pattern)
                    c.run ("pattern.set", object ({ { "pattern", unique }, { "name", p.name + " (E minor)" } }));
            for (auto& [ch, notes] : u.parts)
                song.setNotes (unique, ch, notes);
            fit (unique, 8 * bar);
        }
        // The verse lead doubles the hook an octave down in the last chorus.
        place (lVerseLead,
               onePart ("Lead Double (E minor)", 8, leadColour, verseLeadCh, shifted (chorusLead (2), 0, -12)), 64, 8);
    }
    // Outro 72-80, in E minor.
    place (lDrums, pBeatOutro, 72, 4);
    place (lDrums, pBeatIntro, 76, 4);
    place (lBass, pBassOutro, 72, 8);
    place (lPads, pPadOutro, 72, 8);
    place (lArp, pArpOutro, 72, 8);
    place (lBell, pBellOutro, 72, 8);

    // Audio clips (original sounds, made by make_audio.py): ocean waves, reverse cymbals, gated impacts.
    const auto audio = juce::File (juce::SystemStats::getEnvironmentVariable ("SYNTHWAVE_AUDIO", {}));
    int audioClips = 0;
    if (audio.isDirectory())
    {
        auto addAudio = [&] (int l, const juce::String& file, double startBar, double bars, int channel, double gainDb,
                             double fadeInBars = 0, double fadeOutBars = 0)
        {
            auto params = object ({ { "lane", l },
                                    { "file", audio.getChildFile (file).getFullPathName() },
                                    { "start", (juce::int64)std::llround (startBar * bar) },
                                    { "length", (juce::int64)std::llround (bars * bar) },
                                    { "gainDb", gainDb } });
            auto* o = params.getDynamicObject();
            if (channel != 0)
                o->setProperty ("channel", channel);
            else
                o->setProperty ("name", juce::File (file).getFileNameWithoutExtension());
            if (fadeInBars > 0)
                o->setProperty ("fadeIn", (juce::int64)std::llround (fadeInBars * bar));
            if (fadeOutBars > 0)
                o->setProperty ("fadeOut", (juce::int64)std::llround (fadeOutBars * bar));
            ++audioClips;
            return (int)c.run ("clip.addAudio", params)["channel"];
        };
        const int waves = addAudio (lWaves, "Ocean Waves.wav", 0, 4, 0, -8.0, 2);
        addAudio (lWaves, "Ocean Waves.wav", 4, 4, waves, -8.0, 0, 3);
        addAudio (lWaves, "Ocean Waves.wav", 48, 4, waves, -14.0, 2);
        addAudio (lWaves, "Ocean Waves.wav", 72, 4, waves, -10.0, 2);
        addAudio (lWaves, "Ocean Waves.wav", 76, 4, waves, -8.0, 0, 3);
        const int cymbal = addAudio (lHits, "Reverse Cymbal.wav", 7, 1, 0, -10.0);
        for (int at : { 19, 39, 47, 63, 71 })
            addAudio (lHits, "Reverse Cymbal.wav", at, 1, cymbal, -8.0);
        const int impact = addAudio (lHits, "Gated Impact.wav", 20, 0.75, 0, -9.0);
        for (int at : { 40, 54, 64 })
            addAudio (lHits, "Gated Impact.wav", at, 0.75, impact, -9.0);
        for (auto [id, name] : std::vector<std::pair<int, const char*>> {
                 { waves, "Ocean Waves" }, { cymbal, "Reverse Cymbal" }, { impact, "Gated Impact" } })
        {
            c.run ("channel.set",
                   object ({ { "channel", id }, { "name", name }, { "group", "FX" }, { "colour", fxColour } }));
            c.run ("channel.route", object ({ { "channel", id }, { "insert", fxBus } }));
        }
        song.send (fxBus, hall, -16);
    }
    else
        std::cerr << "SYNTHWAVE_AUDIO not set: no audio clips.\n";

    //--------------------------------------------------------------------------
    // Automation clips.
    song.syncDevices();
    const auto val = [&] (int device, const char* id, const char* text)
    { return (double)song.valueFor (device, id, text); };

    // The synth bus's low-pass sweeps open over each pre-chorus and the lift, and is wide open on the downbeat.
    for (auto [at, bars] : std::vector<std::pair<int, int>> { { 16, 4 }, { 36, 4 }, { 62, 2 } })
        song.automate (laSynthFilter, at, bars, Song::parameter (synthFilter, "cutoff"),
                       { { 0, val (synthFilter, "cutoff", "500 Hz"), 0.55 },
                         { bars * 4.0 - 0.05, val (synthFilter, "cutoff", "20000 Hz") } });
    // ... closes in the outro.
    song.automate (
        laSynthFilter, 72, 8, Song::parameter (synthFilter, "cutoff"),
        { { 0, val (synthFilter, "cutoff", "20000 Hz"), -0.5 }, { 32, val (synthFilter, "cutoff", "700 Hz") } });

    // The arp's filter opens through the intro and the pre-chorus; a slow auto-pan in the choruses.
    song.automate (laArpCut, 4, 4, Song::parameter (arpFilter, "cutoff"),
                   { { 0, val (arpFilter, "cutoff", "300 Hz"), 0.4 }, { 16, val (arpFilter, "cutoff", "5000 Hz") } });
    for (int at : { 16, 36 })
        song.automate (
            laArpCut, at, 4, Song::parameter (arpFilter, "cutoff"),
            { { 0, val (arpFilter, "cutoff", "1200 Hz"), 0.4 }, { 16, val (arpFilter, "cutoff", "9000 Hz") } });
    song.automate (laArpCut, 72, 8, Song::parameter (arpFilter, "cutoff"),
                   { { 0, val (arpFilter, "cutoff", "5000 Hz"), -0.3 }, { 32, val (arpFilter, "cutoff", "400 Hz") } });
    for (int at : { 20, 40, 64 })
    {
        std::vector<P> points;
        for (int i = 0; i <= 8 * 8; ++i)
            points.push_back ({ i * 0.5, 0.5 + 0.3 * std::sin (i * juce::MathConstants<double>::twoPi / 16.0) });
        song.automate (laArpPan, at, 8, Song::channelPan (arpCh), points);
    }

    // The arp bass: its filter opens over the first verse.
    song.automate (
        laBassCut, 8, 8, Song::parameter (arpBassCh.device, "cutoff"),
        { { 0, val (arpBassCh.device, "cutoff", "120 Hz"), 0.3 }, { 32, val (arpBassCh.device, "cutoff", "260 Hz") } });

    // The pad: wider in the choruses.
    for (int at : { 20, 40, 64 })
        song.automate (laPadWidth, at, 8, Song::parameter (padWidth, "width"),
                       { { 0, val (padWidth, "width", "140%") },
                         { 2, val (padWidth, "width", "190%") },
                         { 30, val (padWidth, "width", "190%") },
                         { 32, val (padWidth, "width", "140%") } });

    // The gated reverb: a longer hold in the bridge (a bigger, slower snare), back for the solo.
    song.automate (laGate, 48, 6, Song::parameter (gate, "hold"),
                   { { 0, val (gate, "hold", "190") },
                     { 1, val (gate, "hold", "420") },
                     { 23, val (gate, "hold", "420") },
                     { 24, val (gate, "hold", "190") } });
    song.automate (
        laDrumsVol, 4, 4, Song::insertVolume (drumsBus),
        { { 0, song.insertVolumeValue (drumsBus, "-18"), 0.3 }, { 16, song.insertVolumeValue (drumsBus, "0") } });

    // The lead: swoops into its long notes (Surge XT's scene pitch from below, a pitch bend), and vibrato on
    // the held notes (the pitch moving a quarter-tone either side, five times a second).
    const auto pitchValue = [&] (double semitones)
    { return val (leadCh.device, "229", juce::String (semitones, 2).toRawUTF8()); };
    const double zero = pitchValue (0.0);
    std::cout << "  lead pitch: 0 -> " << zero << ", -2 -> " << pitchValue (-2.0) << ", 0.25 -> " << pitchValue (0.25)
              << "\n";
    for (int at : { 20, 40, 64 })
    {
        std::vector<P> points { { 0, zero } };
        for (auto& n : chorusLead())
            if (n.length >= 1.5)
            {
                const bool swoop = juce::approximatelyEqual (n.start, 4.0) ||
                                   juce::approximatelyEqual (n.start, 12.0) || juce::approximatelyEqual (n.start, 22.0);
                if (swoop)
                {
                    points.push_back ({ n.start - 0.02, zero });
                    points.push_back ({ n.start, pitchValue (-2.0), 0.6 });
                    points.push_back ({ n.start + 0.35, zero });
                }
                // Delayed vibrato: from half a beat into the note to just before its end.
                for (double t = n.start + 0.5; t < n.start + n.length - 0.1; t += 1.0 / 12.0)
                    points.push_back (
                        { t, pitchValue (0.25 * std::sin ((t - n.start - 0.5) * juce::MathConstants<double>::twoPi *
                                                          5.0 * 60.0 / bpm)) });
                points.push_back ({ n.start + n.length - 0.05, zero });
            }
        points.push_back ({ 32, zero });
        std::sort (points.begin(), points.end(), [] (const P& a, const P& b) { return a.beats < b.beats; });
        song.automate (laSwoop, at, 8, Song::parameter (leadCh.device, "229"), points);
    }
    const auto leadCut = song.currentValue (leadCh.device, "319");
    for (int at : { 20, 40, 64 })
        song.automate (laLeadCut, at, 8, Song::parameter (leadCh.device, "319"),
                       { { 0, leadCut - 0.08, 0.3 }, { 16, leadCut }, { 28, leadCut + 0.06 }, { 32, leadCut } });

    // The solo: OB-Xf's oscillator pitch bent up into the first notes of phrases, and its filter opening.
    if (soloLoaded)
    {
        const auto bend = [&] (const char* text) { return val (soloCh.device, "11", text); };
        std::cout << "  solo bend: 0 -> " << bend ("0.00") << ", -1 -> " << bend ("-1.00") << ", -2 -> "
                  << bend ("-2.00") << "\n";
        std::vector<P> points { { 0, bend ("-2.00") }, { 0.4, bend ("0.00"), 0.0 } };
        for (double at : { 4.0, 16.0, 20.0 })
        {
            points.push_back ({ at - 0.02, bend ("0.00") });
            points.push_back ({ at, bend ("-1.00"), 0.5 });
            points.push_back ({ at + 0.3, bend ("0.00") });
        }
        points.push_back ({ 32, bend ("0.00") });
        song.automate (laSoloPitch, 54, 8, Song::parameter (soloCh.device, "11"), points);
        const auto soloCut = song.currentValue (soloCh.device, "40");
        song.automate (laSoloCut, 54, 8, Song::parameter (soloCh.device, "40"),
                       { { 0, soloCut - 0.12, 0.3 }, { 24, soloCut + 0.05 }, { 32, soloCut } });
    }

    // Reverb throws: the throw reverb's return opens for a moment on the ends of phrases.
    for (double at : { 27.0, 47.0, 61.0, 71.0 })
    {
        const int barAt = (int)at;
        song.automate (laThrow, barAt, 1, Song::insertVolume (throwVerb),
                       { { 0, song.insertVolumeValue (throwVerb, "-60") },
                         { 2, song.insertVolumeValue (throwVerb, "-11") },
                         { 3.9, song.insertVolumeValue (throwVerb, "-11") },
                         { 4, song.insertVolumeValue (throwVerb, "-60") } });
    }
    // ... and the bridge's whole gated snare and bell washed in it.
    song.automate (laThrow, 48, 6, Song::insertVolume (throwVerb),
                   { { 0, song.insertVolumeValue (throwVerb, "-60") },
                     { 4, song.insertVolumeValue (throwVerb, "-14"), 0.3 },
                     { 22, song.insertVolumeValue (throwVerb, "-10") },
                     { 24, song.insertVolumeValue (throwVerb, "-60") } });
    song.automate (laDelay, 48, 6, Song::insertVolume (delay),
                   { { 0, song.insertVolumeValue (delay, "-2") },
                     { 12, song.insertVolumeValue (delay, "1") },
                     { 24, song.insertVolumeValue (delay, "-2") } });

    // The risers: louder and brighter to each downbeat.
    for (auto [at, bars] : std::vector<std::pair<int, int>> { { 16, 4 }, { 36, 4 }, { 62, 2 } })
    {
        song.automate (
            laRiser, at, bars, Song::channelVolume (riserCh),
            { { 0, song.volumeValue (riserCh, "-40"), 0.5 }, { bars * 4.0, song.volumeValue (riserCh, "-12") } });
        song.automate (laRiserCut, at, bars, Song::parameter (riserCh.device, "cutoff"),
                       { { 0, val (riserCh.device, "cutoff", "300 Hz"), 0.5 },
                         { bars * 4.0, val (riserCh.device, "cutoff", "14000 Hz") } });
    }

    // The master: a fade in, a little more air in the last chorus, a fade out.
    song.automate (
        laMaster, 0, 2, Song::insertVolume (master),
        { { 0, song.insertVolumeValue (master, "-24"), -0.4 }, { 8, song.insertVolumeValue (master, "0") } });
    song.automate (
        laMaster, 76, 4, Song::insertVolume (master),
        { { 0, song.insertVolumeValue (master, "0"), 0.3 }, { 16, song.insertVolumeValue (master, "-50") } });
    song.automate (laMasterEq, 64, 8, Song::parameter (masterEq, "band7_gain"),
                   { { 0, val (masterEq, "band7_gain", "1.0") },
                     { 2, val (masterEq, "band7_gain", "2.5") },
                     { 30, val (masterEq, "band7_gain", "2.5") },
                     { 32, val (masterEq, "band7_gain", "1.0") } });

    // Fold the FX group, to show it folded (its clips show faintly on the FX track).
    c.run ("lane.set", object ({ { "lane", lFx }, { "collapsed", true } }));
    juce::ignoreUnused (padLoaded, synthDucker, leadEq, leadComp, hallEq, delayFx, fxEq, gatedEq, drumsEq, drumsComp,
                        bassEq, synthEq, hallReverb, masterComp, masterLimiter, gatedPlate, throwReverb, delayFilter,
                        bassComp, drumsClip, snareEq);
    song.syncDevices();

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
    std::cout << "Made " << folder.getFullPathName() << " (" << c.count << " commands): channels "
              << project.channels.size() << ", inserts " << project.mixer.inserts.size() + 1 << ", effects "
              << song.effects.size() << ", patterns " << project.patterns.size() << ", notes " << notes << ", lanes "
              << project.playlist.lanes.size() << ", clips " << clips << " (audio " << audioClips
              << "), automation clips " << automation << " (" << song.automationPoints << " points)\n"
              << "  Surge XT patches: " << song.surgePatches.joinIntoString (", ")
              << (song.surgeProblems.isEmpty() ? "" : " -- problems: " + song.surgeProblems.joinIntoString (", "))
              << "\n  OB-Xf patches: " << (padLoaded ? "Pads/Dreaming Anew" : "PAD NOT LOADED") << ", "
              << (soloLoaded ? "Leads/Street Hawk" : "SOLO NOT LOADED") << "\n";
    for (auto& d : session.getAdapter().getMissingDevices())
        std::cout << "  MISSING device " << d.value << "\n";
    return 0;
}

//==============================================================================
/** Opens the project as a user would and exports it (WAV, 48 kHz), with stems if asked; prints each section's
    peak and RMS (the loudness is measured from the file afterwards). */
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
    settle (500);
    for (auto& d : session.getAdapter().getMissingDevices())
        std::cout << "  MISSING device " << d.value << "\n";

    auto exportTo = [&] (const juce::File& file, bool asStems) -> std::optional<engine::ExportJob::Result>
    {
        engine::ExportSettings settings;
        settings.format = engine::ExportSettings::Format::wav24;
        settings.sampleRate = 48000.0;
        settings.tailSeconds = 3.0;
        settings.stems = asStems;
        std::optional<engine::ExportJob::Result> result;
        auto job = session.createExportJob (file, settings, [&] (const engine::ExportJob::Result& r) { result = r; });
        job->start();
        while (! result)
            settle (20);
        return result;
    };
    const auto start = Clock::now();
    const auto result = exportTo (wav, false);
    if (! result || ! result->ok)
    {
        std::cerr << "Export failed: " << (result ? result->error : juce::String()) << "\n";
        return 1;
    }
    std::cout << "Exported " << result->files.front().getFullPathName() << " in " << msSince (start) / 1000.0 << " s\n";
    if (stems != juce::File())
    {
        stems.createDirectory();
        const auto s = exportTo (stems.getChildFile ("Mix.wav"), true);
        std::cout << "Stems: " << (s ? (int)s->files.size() : 0) << " files\n";
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (result->files.front()));
    if (reader == nullptr)
        return 1;
    juce::AudioBuffer<float> audio ((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read (&audio, 0, (int)reader->lengthInSamples, 0, true, true);
    const auto rate = reader->sampleRate;
    const auto secondsPerBar = 4.0 * 60.0 / bpm;
    for (auto& s : sections)
    {
        const auto st = showcase::statsOf (audio, (int64_t)(s.startBar * secondsPerBar * rate),
                                           (int64_t)((s.startBar + s.bars) * secondsPerBar * rate));
        std::cout << "  " << s.name << " (bars " << s.startBar + 1 << "-" << s.startBar + s.bars << "): peak "
                  << showcase::db (st.peak) << ", RMS " << showcase::db (st.rms) << "\n";
    }
    return 0;
}

//==============================================================================
/** Pictures of the main window with one panel filling the workspace (Playlist, Channel Rack, Mixer, Piano
    Roll), drawn offscreen as the window draws them: to check that the project is tidy. */
int shots (Session& session, ProjectDocument& document, const juce::File& folder, const juce::File& out)
{
    if (const auto opened = document.open (folder); opened.failed())
    {
        std::cerr << "Open failed: " << opened.getErrorMessage() << "\n";
        return 1;
    }
    session.getEdit().dispatchPendingUpdatesSynchronously();
    settle (300);
    out.createDirectory();
    ui::MainView view { session, &document };
    view.setSize (1800, 1100);
    settle (50);
    for (auto& p : session.getProject().patterns)
        if (p.name == "Lead Chorus")
        {
            session.setCurrentPattern (p.id);
            session.setSelectedChannel (p.notes.begin()->first);
        }
    for (auto* id : { "playlist", "channelRack", "mixer", "pianoRoll" })
    {
        juce::ValueTree layout ("LAYOUT"), panel ("PANEL");
        panel.setProperty ("id", id, nullptr);
        panel.setProperty ("visible", true, nullptr);
        panel.setProperty ("detached", false, nullptr);
        panel.setProperty ("x", 0.0f, nullptr);
        panel.setProperty ("y", 0.0f, nullptr);
        panel.setProperty ("w", 1.0f, nullptr);
        panel.setProperty ("h", 1.0f, nullptr);
        layout.appendChild (panel, nullptr);
        view.getWorkspace().setLayout (layout);
        view.getWorkspace().showPanel (id);
        for (int i = 0; i < 10; ++i)
            settle (30);
        const auto image = view.createComponentSnapshot (view.getLocalBounds());
        const auto file = out.getChildFile (juce::String (id) + ".png");
        file.deleteFile();
        juce::FileOutputStream stream (file);
        juce::PNGImageFormat().writeImageToStream (image, stream);
        std::cout << "Wrote " << file.getFullPathName() << "\n";
    }
    return 0;
}

//==============================================================================
void probe (Session& session)
{
    Commands c (session);
    std::vector<std::pair<juce::String, int>> devices;
    for (auto* type : { "spinus.3x-osc", "spinus.drum-machine", "bundled.surge-xt", "bundled.ob-xf" })
        devices.emplace_back (
            type, (int)c.run ("channel.add",
                              object ({ { "name", type },
                                        { "instrument",
                                          object ({ { "kind", "stock" }, { "stockType", type } }) } }))["device"]);
    const auto insert = (int)c.run ("insert.add", object ({ { "name", "FX" } }))["insert"];
    for (auto* type : { "spinus.filter", "spinus.eq", "spinus.compressor", "spinus.limiter", "spinus.ducker",
                        "spinus.stereo-utility", "spinus.gate", "spinus.loudness-meter", "bundled.dragonfly.hall",
                        "bundled.dragonfly.plate", "bundled.surge-xt-effects", "bundled.peakeater" })
        devices.emplace_back (
            type,
            (int)c.run ("insert.addEffect",
                        object ({ { "insert", insert },
                                  { "device", object ({ { "kind", "stock" }, { "stockType", type } }) } }))["device"]);
    session.getEdit().dispatchPendingUpdatesSynchronously();
    settle (800);
    for (auto& [type, id] : devices)
    {
        const auto parameters = session.getDeviceParameters (model::DeviceId { (uint32_t)id });
        std::cout << "== " << type << " (device " << id << "): " << parameters.size() << " parameters\n";
        for (auto& [pid, name] : parameters)
            std::cout << "  " << pid << " | " << name << " | "
                      << session.getDeviceParameterValueText (model::DeviceId { (uint32_t)id }, pid) << "\n";
    }
}
} // namespace synthwave
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::StringArray args (argv, argc);
    const auto valueOf = [&] (const char* name) -> juce::String
    {
        const auto i = args.indexOf (name);
        return i >= 0 && i + 1 < args.size() ? args[i + 1] : juce::String();
    };
    const auto file = [&] (const char* name)
    {
        const auto v = valueOf (name);
        return v.isEmpty() ? juce::File() : juce::File::getCurrentWorkingDirectory().getChildFile (v);
    };
    if (args.contains ("--help") || valueOf ("--plugins").isEmpty())
    {
        std::cout << "spinus_demo_synthwave --plugins <folder> (--probe | --make <project folder> | --render <project "
                     "folder> --wav <file> [--stems <folder>] | --shots <project folder> --out <folder>)\n";
        return args.contains ("--help") ? 0 : 1;
    }
    const auto plugins = file ("--plugins");

    EngineHost::setSettingsName ("Siskin Tool");
    int status = 0;
    {
        EngineHost host ({ .openAudioDevice = false });
        Session session (host.getEngine(), makeDevices (host.getEngine(), plugins));
        session.getDeviceList().setBundledFolders ({ plugins });
        const auto settings =
            juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("spinus-synthwave-settings");
        ProjectDocument document (session, settings);
        if (args.contains ("--probe"))
            synthwave::probe (session);
        else if (valueOf ("--make").isNotEmpty())
            status = synthwave::make (session, document, file ("--make"), plugins);
        else if (valueOf ("--shots").isNotEmpty())
            status = synthwave::shots (session, document, file ("--shots"), file ("--out"));
        else if (valueOf ("--render").isNotEmpty())
            status = synthwave::render (session, document, file ("--render"), file ("--wav"), file ("--stems"));
        settle (200);
    }
    return status;
}
