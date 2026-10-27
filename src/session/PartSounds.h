#pragma once

#include "Session.h"
#include "SoundDescriptors.h"
#include "SoundIndex.h"
#include "SynthMatch.h"

#include <map>
#include <set>
#include <optional>
#include <vector>

/** The Track creator's third step, sounds per part: for each part of the reference songs' style summary (bass,
    other, vocals, drums), the closest sounds this computer has, from the sound index (SoundIndex).

    1. **Targets** (targetsFromSummary): what each part needs, from the summary: the roles that fit (bass: bass;
       other: keys or pad when it plays chords, else lead, pluck or keys; vocals: lead; drums: drums), its
       register, whether it plays chords.
    2. **The stems measured** (measureNotes, measureHits): the reference songs' stems, kept in the analysis
       library, measured with the sound index's own descriptors over their notes (or drum hits), so a stem and a
       preset are compared like with like: attack, how the level holds, brightness in harmonics at the part's
       own register, noisiness.
    3. **Shortlist and rank** (choose): the sounds whose role fits, whose register covers the part's and that
       play chords when the part does; closest first, each with the reasons ("why").

    The Track creator's panel then refines the first few with closest preset's CLAP embeddings (when the AI is
    on), may tune the chosen synth toward the stem (out of process), sets effects and the mix toward the
    references, and adds it all to the project as one proposal (makeProposal: one undo step).

    Plain arithmetic, no model; any thread but the audio thread. */
namespace spinus::parts
{

/** A part of the song to write, on the user's list (the Sounds page): what it is called and what it plays. The
    song writer (spinus_ai.songwriter's ROLES) writes exactly the parts of the list, each by its role; two parts
    of one role get complementary material (a second chords part an arpeggio an octave up...). */
struct SongPart
{
    juce::String id;   // unique in the list ("chords", "chords-2")
    juce::String name; // as the user reads it ("Chords", "Rhythm guitar")
    juce::String role; // one of songRoles()

    bool operator== (const SongPart&) const = default;
};
/** drums, percussion, bass, sub, rhythm-guitar, acoustic-guitar, clean-guitar, lead-guitar, chords, piano, organ,
    pad, strings, choir, brass, woodwinds, arp, lead, counter, riser, impact (spinus_ai.songwriter.ROLES). */
const juce::StringArray& songRoles();
/** The role generic instrument words in an instrument's or preset's name say ("Steel Guitar": acoustic-guitar,
    "Fingered Bass": bass, "Grand Piano": piano), or empty. No vendor or product names: what the user marked
    (UserRoles) comes first where roles are used. */
juce::String roleFromName (const juce::String& name);
/** The parts a description names (its `described.parts`: "guitar, bass and drums"), none otherwise. */
std::vector<SongPart> describedParts (const juce::var& summary);
/** Suggest parts: "genre" (the described genre's usual parts), "sketch" or "full" (defaultParts). */
std::vector<SongPart> suggestedParts (const juce::String& which, const juce::var& summary);
/** How the page names a role: "Chords (rhythm)", "Arpeggio", "Riser (effect)"... */
juce::String roleName (const juce::String& role);
/** The list an arrangement proposes: "sketch" drums, bass, chords and lead; "full" those and a pad, an arpeggio,
    a counter-melody, a riser and impacts, with a sub bass when the summary's bass sits high enough. */
std::vector<SongPart> defaultParts (const juce::String& arrangement, const juce::var& summary);
/** A new part of a role for the list: an id and a name not taken yet ("Chords 2"). */
SongPart newPart (const std::vector<SongPart>& list, const juce::String& role);

/** A part of the summary and what it needs. */
struct Target
{
    juce::String part;                 // the stem: "bass", "other", "vocals", "drums" ("fx": an effect)
    juce::String id, name, role;       // the part of the list it is for (targetFor), or the stem's part and name
    bool measure = true;               // compared with the stem (the songs' own parts)
    juce::StringArray names;           // words a fitting sound's name has (a guitar part: "guitar", "metal"...)
    juce::StringArray roles;           // the roles that fit, the best first
    int low = 0, median = 0, high = 0; // MIDI notes (0: unknown)
    double notesPerBar = 0.0, voices = 1.0;
    bool chords = false; // the other instruments play more than one note at a time (voices over 1.3)
    double tempo = 0.0;  // the songs' median tempo (drum loops)
    int songs = 0;       // how many reference songs have the part
};

/** What a part of the list needs, from the summary: the roles of sound that fit, its register (a second part of
    a role an octave up), and the stem to compare with (only the first part of drums, bass, chords and lead). */
Target targetFor (const SongPart&, int instance, const juce::var& summary);

/** The parts of a style summary (`parts`), in the order bass, other, vocals, drums; parts no song has are
    left out. */
std::vector<Target> targetsFromSummary (const juce::var& summary);
/** How the panel names a part: "Bass", "Chords and melody" (other), "Lead (the vocals' tune)", "Drums". */
juce::String displayName (const juce::String& part);

//==============================================================================
// The stems measured.

/** A note of a stem, as the analysis library keeps Basic Pitch's notes (seconds). */
struct StemNote
{
    int pitch = 60;
    double start = 0.0, end = 0.0;
};
std::vector<StemNote> stemNotesFromVar (const juce::var&);

/** An audio file (a stem) read whole in mono at `rate` (at most `longest` seconds); empty if it cannot be read. */
juce::AudioBuffer<float> readMono (const juce::File&, double rate, double longest = 900.0);

/** A drum hit of a drum stem, as the library keeps them: its time and kind (kick, snare, closed_hat...). */
struct StemHit
{
    double time = 0.0;
    juce::String kind;
};
std::vector<StemHit> stemHitsFromVar (const juce::var&);

/** Measurements of a part's notes (or one drum's hits), from one or more songs. */
struct Measured
{
    std::vector<sounds::Descriptors> sounds; // one per note measured
    std::vector<double> harmonics;           // each note's brightness in harmonics (centroid / the note's frequency)
    std::vector<int> pitches;
    std::vector<double> held; // how long each note was held (seconds)

    int count() const noexcept { return (int)sounds.size(); }
    void add (const Measured&);
    /** The median of each measurement. Decay and sustain only of the notes held 0.3 s or more. */
    sounds::Descriptors median() const;
    double medianHarmonics() const;
    int medianPitch() const;
    /** Whether enough notes were held long enough for their decay and sustain to say something. */
    bool knowsSustain() const;
};

/** A stem's notes measured: up to `most` notes (spread over the song) that start clear of the notes before them,
    each from its start to its end and half a second of ring (or the next note), with sounds::describe. Chords
    count as one note (starts 30 ms apart), measured at their lowest note. `mono` at `rate`. */
Measured measureNotes (const float* mono, int count, double rate, const std::vector<StemNote>&, int most = 40);
/** A drum stem's hits measured by drum: "kick", "snare" and "hat" (closed and open hats), up to `most` each. */
std::map<juce::String, Measured> measureHits (const float* mono, int count, double rate, const std::vector<StemHit>&,
                                              int most = 24);
/** The drum a hit's kind belongs to ("closed_hat" is a hat), or empty for the others (toms, cymbals...). */
juce::String drumOf (const juce::String& kind);

//==============================================================================
// Choosing.

/** A sound for a part: a preset of an instrument, a sample, drum samples, a drum kit or a drum loop. */
struct Choice
{
    enum class Kind
    {
        preset,      // an instrument's preset (or the instrument as it is: preset id "")
        sample,      // a one-shot played on keys (an 808 for a bass), in the Sampler
        drumSamples, // a kick, a snare and a hat sample on the Drum Machine's pads
        kit,         // an instrument that plays drums on its keys (the Drum Machine's own kit)
        loop         // a drum loop, played as audio
    };
    Kind kind = Kind::preset;
    juce::String instrument; // the index's key of the instrument (preset, kit)
    SoundSource source;      // the instrument
    juce::String preset;     // the preset's id (preset)
    juce::String version;    // the preset's version (closest preset's index knows presets by id and version)
    juce::String name;       // the preset's or the file's name
    juce::String category;   // the preset's
    juce::String role;       // its role guess
    juce::String roleFrom;   // "name" or "sound"
    juce::String file;       // the sample or the loop
    std::map<juce::String, juce::String> drums; // drumSamples: "kick", "snare", "hat" -> file
    std::map<juce::String, int> drumNotes;      // kit: the drum -> the note that plays it
    int low = 0, high = 0;                      // its register (preset)
    bool polyphonic = false;
    double tempo = 0.0, duration = 0.0; // loop
    double fundamental = 0.0;           // sample: its pitch (Hz; 0: unknown)
    double distance = 0.0;              // how far its measurements are from the stem's (lower is closer)
    bool compared = false;              // measured against the stem (false: no stem measurements to compare)
    double clap = -2.0;                 // closest preset's CLAP score (-1 to 1; -2: not compared)
    juce::StringArray why;              // the reasons, in plain words

    /** "Surge XT: Reese Bass", "Sample: 808 Boom.wav", "Drum samples: Kick 01, Snare 3, Hat 2"... */
    juce::String title() const;
    /** The reasons in one line. */
    juce::String reasons() const { return why.joinIntoString ("; "); }
    /** The instrument for its channel, as channel.add takes it: the instrument itself, the Sampler (sample) or
        the Drum Machine (drum samples). */
    juce::var deviceJson() const;
    /** A sample's own note (from its pitch; C2 when unknown). */
    int rootNote() const;
};

/** The part's measured stems (empty when there are none to compare with) and, for the drums, its hits. */
struct Reference
{
    Measured notes;
    std::map<juce::String, Measured> hits;
};

/** The closest sounds of the index for the part, closest first, at most `most`. When nothing fits all the
    rules (no preset of that role covering the register), the closest that fit fewer of them, saying so. */
std::vector<Choice> choose (const Target&, const Reference&, const SoundIndex&, int most = 5);

/** Varied suggestions: `candidates` (closest first) put in an order whose first few come from different
    instruments (then different preset families), the instruments in `avoid` (the other parts') later, and
    near ones shuffled by `seed` (another seed, other suggestions). At most `most`. */
std::vector<Choice> vary (std::vector<Choice> candidates, const std::set<juce::String>& avoid, juce::uint32 seed,
                          int most);

/** How far a preset's measurements are from the stem's (scaled so that 1 is "clearly different"), and the
    reasons in words. Exposed for tests. */
double presetDistance (const juce::var& sound, const Measured& stem, juce::StringArray* why = nullptr);
double hitDistance (const sounds::Descriptors& sample, const sounds::Descriptors& stem);

//==============================================================================
// Out of Spinus's process (spinus-plugin-scanner --sound-index: SoundRender's "render" and "tune" modes), so a
// plugin that crashes or hangs takes only the helper down. Blocking: run them on a thread of their own.

/** The helper's request for an instrument (its "mode" set by the caller). */
juce::var instrumentRequest (const SoundSource&);

/** Plays the items (SoundRender's render mode: {id, preset, settings, notes, tail, output}, {id, cut}, {id, mix})
    and returns each item's {id, output} or {id, problem}; `problem` says why none came (the helper crashed,
    hung or the instrument did not load). */
juce::Array<juce::var> renderInHelper (const SoundIndexer::Options&, const SoundSource&,
                                       const juce::Array<juce::var>& items, const std::function<bool()>& shouldStop,
                                       juce::String& problem);

/** Synth match in the helper: the instrument with the preset (an id; empty for its own sound) tuned toward the
    excerpt of `target` (an audio file: start, length) playing `notes` (from the excerpt's start), for up to
    `tries`. `progress` gets (tries done, best distance) as it goes. None when it could not be tuned (`problem`
    says why: no settings to tune, a crash, a hang, a stop). */
std::optional<SynthMatchResult> tuneInHelper (const SoundIndexer::Options&, const SoundSource&,
                                              const juce::String& preset, const juce::File& target, double start,
                                              double length, const std::vector<OfflineInstrument::Note>& notes,
                                              int tries, const std::function<bool()>& shouldStop,
                                              const std::function<void (int, double)>& progress, juce::String& problem);

//==============================================================================
// Into the project.

/** Notes for a part's channel to play, so the sound can be heard at once (milestone 4 writes the real ones):
    a short phrase in the part's register (bass: roots, other: chords or a line, lead: a tune), or a beat
    (drums: kick, snare and hats from the summary's most played steps). Times in seconds at `tempo`. */
std::vector<OfflineInstrument::Note> auditionPhrase (const Target&, const Choice&, double tempo,
                                                     const juce::var& summary = {});

/** What the user adds: a sound per part, its tuning and effects, and the mix. */
struct Plan
{
    struct Part
    {
        Target target; // its id, name and role: the part of the list
        Choice choice;
        std::optional<SynthMatchResult> tuned;
        juce::Array<juce::var> effects; // spinus_ai.mixplan items for its insert (eq, compressor, gate)
        double volumeDb = 0.0;          // its insert's fader
    };
    std::vector<Part> parts;
    juce::Array<juce::var> master; // items for the master (limiter, eq)
    double tempo = 120.0;
    juce::var summary;
};

/** The proposal ({summary, steps}) that adds the plan to the project through commands: per part a channel
    (with its own insert) named "<Part> (Track creator)", in the Channel Rack group "Track creator", its
    tuned settings (device.setParameters), its effects (Spinus's EQ, Compressor, Gate) and fader, the master's
    items, and one pattern "Track creator: sounds" with every part's audition phrase. Presets and samples are
    put into the new instruments afterwards (loadSounds), as the Analyzer does. ai::Proposal::prepare checks
    it, and applying it is one undo step. */
juce::var makeProposal (const Plan&);

/** After the proposal is applied (message thread): the chosen presets and samples into the new channels'
    instruments (`channels`, one per part of the plan, in order), as closest preset and the Analyzer do: a
    preset through the plugin's own way of loading presets, samples onto the Drum Machine's pads or into the
    Sampler; then the tuned settings on top. Undoing the proposal takes the channels away with them. */
juce::Result loadSounds (Session&, const Plan&, const std::vector<model::ChannelId>& channels);

/** The steps (appended to `steps`) for one mixplan item on the insert that `insert` names (an insert ID or a
    "$n.insert" reference): Spinus's own EQ, Compressor, Limiter and Gate with their settings as text. */
void addItemSteps (juce::Array<juce::var>& steps, const juce::var& item, const juce::var& insert);

} // namespace spinus::parts
