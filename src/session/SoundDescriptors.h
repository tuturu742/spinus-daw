#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <functional>
#include <vector>

/** How a sound sounds, in a few plain numbers, for the Track creator's sound index (session/SoundIndex):
    measured on an instrument preset playing test notes, or on a sample file. The same measurements can be
    made of a reference song's stems, so a part of the style summary can be matched with the closest sound
    the user owns.

    Everything here is plain arithmetic on audio in memory (JUCE's FFT): no model, nothing downloaded, any
    thread but the audio thread (it allocates). */
namespace spinus::sounds
{

/** Change when what is measured changes: every sound is measured again. */
inline constexpr int descriptorsVersion = 1;

/** One sound's measurements. Times in seconds; a one-shot (a sample, a drum) has no key held, so no sustain
    or release. */
struct Descriptors
{
    double attack = 0;             // from the sound starting (40 dB under its peak) to 90 % of its peak
    double onset = 0;              // its level 30 ms after it starts, relative to the peak: high for a hit
    double decay = 0;              // from the peak until 10 dB under it (while the key is held; the held time if never)
    double sustain = 0;            // the level just before the key is released, relative to the peak (0 to 1)
    double release = 0;            // from the key's release until 40 dB under the peak
    double length = 0;             // from the start until it finally stays 40 dB under the peak
    double brightness = 0;         // the spectral centroid, Hz
    double harmonicBrightness = 0; // brightness / fundamental (0 when not pitched)
    double noisiness = 0;          // spectral flatness: 0 for tones, 1 for white noise
    double pitched = 0;            // how clearly it has one pitch (YIN's clarity, 0 to 1)
    double fundamental = 0;        // Hz (0: no clear pitch)
    double low = 0, high = 0;      // share of the power under 150 Hz and over 5 kHz
    double peakDb = -100;          // the loudest 10 ms, dBFS
};

juce::var toVar (const Descriptors&);
Descriptors descriptorsFromVar (const juce::var&);

//==============================================================================
// The measurements (tests).

struct Envelope
{
    double start = 0; // seconds from the beginning to where the sound starts
    double attack = 0, onset = 0, decay = 0, sustain = 0, release = 0, length = 0;
    double peak = 0; // linear
};
/** `held`: how long the key was held (seconds from the beginning); 0 or less for a one-shot. */
Envelope measureEnvelope (const float*, int count, double rate, double held);

struct Spectrum
{
    double centroid = 0, flatness = 0, low = 0, high = 0;
};
/** The spectrum's shape from `from` to `to` seconds (power-weighted over 2048-sample frames). */
Spectrum measureSpectrum (const float*, int count, double rate, double from, double to);

struct Pitch
{
    double frequency = 0, clarity = 0;
};
/** The pitch (YIN, 30 Hz to 2 kHz) from `from` to `to` seconds: the median of a few frames that have one,
    and their mean clarity. */
Pitch detectPitch (const float*, int count, double rate, double from, double to);

/** Pitch classes (C = 0) from `from` to `to` seconds, summing to 1 (all zero for silence). */
std::array<double, 12> chroma (const float*, int count, double rate, double from, double to);

/** Seconds of the onsets (spectral flux peaks). */
std::vector<double> findOnsets (const float*, int count, double rate);

/** All of the above for one sound. */
Descriptors describe (const float*, int count, double rate, double held);

//==============================================================================
// Instruments.

/** The roles a sound can play in a song (the role guess). */
inline const juce::StringArray& roles()
{
    static const juce::StringArray all { "bass", "lead", "pad", "pluck", "keys", "drums", "fx" };
    return all;
}

struct Note
{
    int note = 60;
    double start = 0.0, length = 1.0; // seconds
    float velocity = 0.8f;
};
/** Plays notes from silence and returns the sound (mono), `tail` seconds after the last note ends. */
using Player = std::function<juce::AudioBuffer<float> (const std::vector<Note>&, double tail)>;

/** The test notes an instrument plays: C3 held a second (with two seconds to ring out), short notes
    from C1 to C7 (its register), the chord C4 E4 G4 (polyphony) and, for a drum kit, every note from B1 to
    D#3 and from C4 to D#5, each with 1.2 s to ring (General MIDI's drums, and Spinus's Drum Machine's pads). */
struct TestNotes
{
    static constexpr int held = 48;
    static constexpr double hold = 1.0, ringOut = 2.0;
    static constexpr std::array<int, 7> probes { 24, 36, 48, 60, 72, 84, 96 };
    static constexpr double probeHold = 0.25, probeTail = 0.15;
    static constexpr std::array<int, 3> chord { 60, 64, 67 };
    static constexpr double chordHold = 0.6;
};

struct InstrumentSound
{
    Descriptors sound; // of the held C3
    bool silent = false;
    bool invalid = false;     // it made invalid audio (not a number, infinite or absurdly loud): silence there
    bool tracksPitch = false; // its notes are in tune with the keys played (an octave off counts)
    bool polyphonic = false;  // plays the chord's three notes
    int low = 0, high = 0;    // the lowest and highest test note that sounds (in tune, when it tracks pitch)
    juce::String role;        // the guess: one of roles(), or "none" (silent)
    juce::String soundRole;   // what the sound alone suggests
    juce::String roleFrom;    // "name" (its name or category says) or "sound"
    std::vector<std::pair<int, juce::String>> drumMap; // a drum kit: the class of each note that sounds
};

InstrumentSound describeInstrument (const Player&, double rate, const juce::String& name, const juce::String& category);
juce::var toVar (const InstrumentSound&);

/** The role a preset's name or category names ("Basses/Deep Sub" is bass), or empty. */
juce::String roleFromName (const juce::String& name, const juce::String& category);
/** The role the measurements suggest. */
juce::String roleFromSound (const InstrumentSound&);

//==============================================================================
// Samples.

/** The classes of a one-shot sample. */
inline const juce::StringArray& hitClasses()
{
    static const juce::StringArray all { "kick", "snare", "clap", "hat", "cymbal", "tom", "perc", "bass", "other" };
    return all;
}

/** What a one-shot's start looks like, besides its descriptors. */
struct HitShape
{
    int bursts = 1;        // separate bursts in its first 80 ms (a clap has several)
    double pitchDrop = 0;  // semitones its pitch falls from 15 ms to 120 ms after the start (a kick's sweep)
    double startPitch = 0; // Hz at 15 ms (0: none)
    double laterPitch = 0; // Hz at 120 ms (0: none)
};
HitShape measureHit (const float*, int count, double rate);
juce::String classifyHit (const Descriptors&, const HitShape&);

struct SampleSound
{
    Descriptors sound;
    double duration = 0;
    juce::String kind;                  // "one-shot", "loop" or "long" (over a minute: a song, not a sample)
    juce::String sampleClass;           // a one-shot: one of hitClasses(); a loop: drums, bass, melodic, other
    juce::String soundClass, classFrom; // what the sound alone says; "name" or "sound"
    double tempo = 0, bars = 0;         // a loop (0: not found)
    juce::String tempoFrom;             // "name", "length" (whole bars at the onsets' tempo) or "onsets"
    juce::String key, keyFrom;          // "A minor"; "name" or "sound"
};

/** `mono` at `rate`; the file's name (and its folder's) can name the class, tempo and key. */
SampleSound describeSample (const juce::AudioBuffer<float>& mono, double rate, const juce::File& file);
juce::var toVar (const SampleSound&);

/** The pieces (tests). */
juce::String classFromName (const juce::File&);
double tempoFromName (const juce::String&);
juce::String keyFromName (const juce::String&);
/** Tempo from the onsets' regularity alone, and the loop's tempo: whole bars of 4/4 at about that tempo. */
double tempoFromOnsets (const float*, int count, double rate);
juce::String keyOf (const std::array<double, 12>& chroma, double* fit = nullptr);

} // namespace spinus::sounds
